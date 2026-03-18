#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <ctype.h>
#include <json-c/json.h>

#include "process_manager.h"
#include "sse.h"
#include "config_parser.h"

#define MAX_TUNNELS 64

/* -------------------------------------------------------------------------
 * Strutture interne
 * ---------------------------------------------------------------------- */

typedef struct process_manager process_manager_t; /* forward per tunnel_entry_t */

typedef struct {
    char              host[128];
    pid_t             pid;
    int               active;
    int               stderr_fd;   /* pipe per leggere stderr di SSH */
    process_manager_t *pm; /* puntatore al manager (per il thread monitor) */
} tunnel_entry_t;

struct process_manager {
    sse_broadcaster_t *sse;
    pthread_mutex_t    mutex;
    tunnel_entry_t     tunnels[MAX_TUNNELS];
    int                num_tunnels;
    char               askpass_path[512];
};

/* Wrapper passato al thread monitor per portare sia pm che entry */
typedef struct {
    process_manager_t *pm;
    tunnel_entry_t    *entry;
} monitor_arg_t;

/* -------------------------------------------------------------------------
 * Validazione alias host: solo alfanumerici, trattini, underscore, punti;
 * la stringa non deve essere vuota.
 * ---------------------------------------------------------------------- */
static int is_valid_host_alias(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;

    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char)*p) &&
            *p != '-' && *p != '_' && *p != '.')
            return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------
 * Thread monitor: aspetta la terminazione del processo SSH e notifica via
 * SSE l'evento "tunnel_status" con status "inactive".
 * ---------------------------------------------------------------------- */
static void broadcast_status_msg(process_manager_t *pm, const char *host,
                                  const char *status_str, int exit_code,
                                  const char *message)
{
    struct json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "host",     json_object_new_string(host));
    json_object_object_add(jobj, "status",   json_object_new_string(status_str));
    json_object_object_add(jobj, "exitCode", json_object_new_int(exit_code));
    if (message)
        json_object_object_add(jobj, "message", json_object_new_string(message));
    sse_broadcast(pm->sse, "tunnel_status", json_object_to_json_string(jobj));
    json_object_put(jobj);
}

/*
 * read_stderr: legge tutto il contenuto disponibile dalla pipe stderr di SSH.
 * Ritorna una stringa allocata con malloc (o NULL). Il chiamante deve fare free().
 */
static char *read_stderr(int fd)
{
    if (fd < 0) return NULL;

    char buf[2048];
    ssize_t total = 0;
    ssize_t n;

    /* Leggi tutto ciò che è disponibile (la pipe è stata chiusa dal figlio) */
    while ((n = read(fd, buf + total,
                     sizeof(buf) - 1 - (size_t)total)) > 0) {
        total += n;
        if (total >= (ssize_t)(sizeof(buf) - 1)) break;
    }

    if (total <= 0) return NULL;

    buf[total] = '\0';

    /* Rimuovi trailing newlines */
    while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
        buf[--total] = '\0';

    return strdup(buf);
}

static void *tunnel_monitor_thread(void *arg)
{
    monitor_arg_t *marg  = (monitor_arg_t *)arg;
    process_manager_t *pm = marg->pm;
    tunnel_entry_t    *entry = marg->entry;
    free(marg);

    /*
     * Polling: controlla ogni 500ms se SSH è ancora vivo.
     * ConnectTimeout è 10s, quindi aspettiamo fino a 12s prima di
     * dichiarare "active". Se SSH muore prima → errore.
     */
    #define PROBE_INTERVAL_US  500000   /* 500 ms */
    #define PROBE_MAX_CHECKS   24       /* 24 × 500ms = 12 secondi */

    int declared_active = 0;
    int wstatus = 0;
    pid_t ret_pid = 0;

    for (int check = 0; check < PROBE_MAX_CHECKS; check++) {
        usleep(PROBE_INTERVAL_US);

        ret_pid = waitpid(entry->pid, &wstatus, WNOHANG);
        if (ret_pid != 0) {
            /* Il processo è terminato durante la fase di connessione */
            goto process_exited;
        }
    }

    /* SSH è sopravvissuto 12 secondi → la connessione è riuscita */
    declared_active = 1;
    broadcast_status_msg(pm, entry->host, "active", 0, NULL);

    /* Ora attendi la terminazione (bloccante) */
    ret_pid = waitpid(entry->pid, &wstatus, 0);

process_exited:
    ;
    int exit_code = -1;
    if (ret_pid > 0) {
        if (WIFEXITED(wstatus))
            exit_code = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
            exit_code = -(int)WTERMSIG(wstatus);
    }

    /* Leggi l'eventuale messaggio di errore da stderr */
    char *errmsg = read_stderr(entry->stderr_fd);
    if (entry->stderr_fd >= 0) {
        close(entry->stderr_fd);
        entry->stderr_fd = -1;
    }

    pthread_mutex_lock(&pm->mutex);
    entry->active = 0;
    pthread_mutex_unlock(&pm->mutex);

    if (exit_code != 0 && !declared_active) {
        /* SSH è morto durante la connessione → errore */
        broadcast_status_msg(pm, entry->host, "error", exit_code, errmsg);
        /* Dopo un breve ritardo, manda anche "inactive" per resettare la UI */
        usleep(100000);
    }

    broadcast_status_msg(pm, entry->host, "inactive", exit_code,
                          exit_code != 0 ? errmsg : NULL);
    free(errmsg);
    return NULL;
}

/* -------------------------------------------------------------------------
 * process_manager_create
 * ---------------------------------------------------------------------- */
process_manager_t *process_manager_create(sse_broadcaster_t *sse,
                                           const char *askpass_path)
{
    process_manager_t *pm = calloc(1, sizeof(*pm));
    if (!pm)
        return NULL;

    pm->sse = sse;
    if (askpass_path)
        snprintf(pm->askpass_path, sizeof(pm->askpass_path), "%s", askpass_path);

    if (pthread_mutex_init(&pm->mutex, NULL) != 0) {
        free(pm);
        return NULL;
    }

    return pm;
}

/* -------------------------------------------------------------------------
 * process_manager_start_tunnel
 *
 * Avvia un tunnel SSH usando l'alias Host come destinazione, così SSH
 * legge direttamente ~/.ssh/config e applica tutte le direttive
 * (HostName, User, Port, LocalForward, RemoteForward, DynamicForward, ecc.).
 *
 * Nel figlio: execvp("ssh", argv) + _exit(127).
 * Nel padre: registra il tunnel, invia SSE "starting", lancia thread monitor.
 *
 * Ritorna 0 in caso di successo, -1 in caso di errore.
 * ---------------------------------------------------------------------- */
int process_manager_start_tunnel(process_manager_t *pm, const ssh_host_t *host)
{
    if (!pm || !host)
        return -1;

    if (!is_valid_host_alias(host->name)) {
        fprintf(stderr, "process_manager: alias host non valido: '%s'\n",
                host->name);
        return -1;
    }

    pthread_mutex_lock(&pm->mutex);

    /* Controlla che non ci sia già un tunnel attivo per questo host */
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (pm->tunnels[i].active &&
            strcmp(pm->tunnels[i].host, host->name) == 0) {
            pthread_mutex_unlock(&pm->mutex);
            fprintf(stderr, "process_manager: tunnel già attivo per '%s'\n",
                    host->name);
            return -1;
        }
    }

    /* Cerca uno slot inattivo da riutilizzare */
    int slot = -1;
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (!pm->tunnels[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (pm->num_tunnels >= MAX_TUNNELS) {
            pthread_mutex_unlock(&pm->mutex);
            fprintf(stderr, "process_manager: raggiunto MAX_TUNNELS (%d)\n",
                    MAX_TUNNELS);
            return -1;
        }
        slot = pm->num_tunnels++;
    }

    pthread_mutex_unlock(&pm->mutex);

    /*
     * Costruzione argv per ssh.
     *
     * Usiamo l'alias Host (host->name) come destinazione, così SSH legge
     * direttamente ~/.ssh/config e applica HostName, User, Port,
     * IdentityFile, ProxyJump, LocalForward, RemoteForward, DynamicForward
     * e tutte le altre direttive. Noi aggiungiamo solo -N (no shell) e
     * le opzioni di robustezza che non sono tipicamente nel config utente.
     */
    char *argv[] = {
        "ssh",
        "-N",
        "-o", "ExitOnForwardFailure=yes",
        "-o", "ConnectTimeout=10",
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        (char *)host->name,
        NULL
    };

    /* Pipe per catturare stderr di SSH (messaggi di errore) */
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stderr_pipe) != 0) {
        perror("pipe");
        /* Non fatale: proseguiamo senza cattura stderr */
        stderr_pipe[0] = stderr_pipe[1] = -1;
    }

    /* Fork */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        if (stderr_pipe[0] >= 0) { close(stderr_pipe[0]); close(stderr_pipe[1]); }
        return -1;
    }

    if (pid == 0) {
        /* Figlio: stacca dal terminale così SSH non può leggere la
         * password da stdin e sarà costretto a usare SSH_ASKPASS. */
        setsid();

        /* Redirect stderr sulla pipe */
        if (stderr_pipe[1] >= 0) {
            close(stderr_pipe[0]); /* chiudi lato lettura nel figlio */
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        }

        /* Imposta lo script askpass come handler per le richieste password */
        if (pm->askpass_path[0] != '\0') {
            setenv("SSH_ASKPASS", pm->askpass_path, 1);
            setenv("SSH_ASKPASS_REQUIRE", "force", 1);
            /* DISPLAY serve perché SSH usa SSH_ASKPASS solo se è settato */
            if (!getenv("DISPLAY"))
                setenv("DISPLAY", ":0", 0);
        }

        /* Chiudi stdin per evitare che SSH tenti di leggere dal terminale */
        close(STDIN_FILENO);

        execvp("ssh", argv);
        _exit(127);
    }

    /* Padre: chiudi il lato scrittura della pipe */
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);

    /* Padre: registra il tunnel */
    {
        tunnel_entry_t *entry;
        monitor_arg_t  *marg;
        pthread_t       tid;

        pthread_mutex_lock(&pm->mutex);
        entry = &pm->tunnels[slot];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->host, sizeof(entry->host), "%s", host->name);
        entry->pid       = pid;
        entry->active    = 1;
        entry->stderr_fd = stderr_pipe[0]; /* lato lettura della pipe */
        entry->pm        = pm;
        pthread_mutex_unlock(&pm->mutex);

        /* Broadcast SSE: stato "starting" */
        {
            struct json_object *jobj = json_object_new_object();
            json_object_object_add(jobj, "host",
                                   json_object_new_string(host->name));
            json_object_object_add(jobj, "status",
                                   json_object_new_string("starting"));
            json_object_object_add(jobj, "pid",
                                   json_object_new_int((int)pid));

            const char *json_str = json_object_to_json_string(jobj);
            sse_broadcast(pm->sse, "tunnel_status", json_str);
            json_object_put(jobj);
        }

        /* Lancia il thread monitor */
        marg = malloc(sizeof(*marg));
        if (!marg) {
            /* Non fatale: il tunnel gira comunque, solo non avremo notifica SSE */
            fprintf(stderr,
                    "process_manager: impossibile allocare monitor_arg\n");
            return 0;
        }
        marg->pm    = pm;
        marg->entry = entry;

        if (pthread_create(&tid, NULL, tunnel_monitor_thread, marg) != 0) {
            free(marg);
            fprintf(stderr, "process_manager: pthread_create fallito\n");
        } else {
            pthread_detach(tid);
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * process_manager_stop_tunnel
 *
 * Trova il tunnel attivo con il nome dato e invia SIGTERM.
 * Ritorna 0 se trovato, -1 altrimenti.
 * ---------------------------------------------------------------------- */
int process_manager_stop_tunnel(process_manager_t *pm, const char *host_name)
{
    if (!pm || !host_name)
        return -1;

    pthread_mutex_lock(&pm->mutex);

    int found = 0;
    for (int i = 0; i < pm->num_tunnels; i++) {
        tunnel_entry_t *e = &pm->tunnels[i];
        if (e->active && strcmp(e->host, host_name) == 0) {
            /* Kill dell'intero process group (SSH + askpass + curl).
             * setsid() nel figlio ha creato un nuovo process group
             * con PGID == PID di SSH, quindi -pid killa tutto il gruppo. */
            kill(-(e->pid), SIGTERM);
            found = 1;
            break;
        }
    }

    pthread_mutex_unlock(&pm->mutex);
    return found ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * process_manager_get_tunnel_status
 *
 * Ritorna "active" se il tunnel esiste ed è attivo, "inactive" altrimenti.
 * ---------------------------------------------------------------------- */
const char *process_manager_get_tunnel_status(process_manager_t *pm,
                                               const char *host_name)
{
    if (!pm || !host_name)
        return "inactive";

    pthread_mutex_lock(&pm->mutex);

    const char *status = "inactive";
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (pm->tunnels[i].active &&
            strcmp(pm->tunnels[i].host, host_name) == 0) {
            status = "active";
            break;
        }
    }

    pthread_mutex_unlock(&pm->mutex);
    return status;
}

/* -------------------------------------------------------------------------
 * process_manager_kill_all
 *
 * Invia SIGTERM a tutti i tunnel attivi, attende 500 ms, poi invia SIGKILL
 * ai processi ancora in vita.
 * ---------------------------------------------------------------------- */
void process_manager_kill_all(process_manager_t *pm)
{
    if (!pm)
        return;

    pthread_mutex_lock(&pm->mutex);

    /* Prima passata: SIGTERM all'intero process group */
    for (int i = 0; i < pm->num_tunnels; i++) {
        tunnel_entry_t *e = &pm->tunnels[i];
        if (e->active && e->pid > 0)
            kill(-(e->pid), SIGTERM);
    }

    pthread_mutex_unlock(&pm->mutex);

    /* Attendi che i processi terminino */
    usleep(500000); /* 500 ms */

    pthread_mutex_lock(&pm->mutex);

    /* Seconda passata: SIGKILL ai sopravvissuti */
    for (int i = 0; i < pm->num_tunnels; i++) {
        tunnel_entry_t *e = &pm->tunnels[i];
        if (!e->active || e->pid <= 0)
            continue;

        /* Controlla se il processo è ancora in vita con waitpid non-bloccante */
        int   wstatus = 0;
        pid_t r = waitpid(e->pid, &wstatus, WNOHANG);
        if (r == 0) {
            /* Ancora in vita: SIGKILL all'intero group */
            kill(-(e->pid), SIGKILL);
            waitpid(e->pid, &wstatus, 0); /* raccoglie il zombie */
        }
        e->active = 0;
    }

    pthread_mutex_unlock(&pm->mutex);
}

/* -------------------------------------------------------------------------
 * process_manager_free
 * ---------------------------------------------------------------------- */
void process_manager_free(process_manager_t *pm)
{
    if (!pm)
        return;

    pthread_mutex_destroy(&pm->mutex);
    free(pm);
}
