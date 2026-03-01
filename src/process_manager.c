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
    process_manager_t *pm; /* puntatore al manager (per il thread monitor) */
} tunnel_entry_t;

struct process_manager {
    sse_broadcaster_t *sse;
    pthread_mutex_t    mutex;
    tunnel_entry_t     tunnels[MAX_TUNNELS];
    int                num_tunnels;
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
static void broadcast_status(process_manager_t *pm, const char *host,
                              const char *status_str, int exit_code)
{
    struct json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "host",     json_object_new_string(host));
    json_object_object_add(jobj, "status",   json_object_new_string(status_str));
    json_object_object_add(jobj, "exitCode", json_object_new_int(exit_code));
    sse_broadcast(pm->sse, "tunnel_status", json_object_to_json_string(jobj));
    json_object_put(jobj);
}

static void *tunnel_monitor_thread(void *arg)
{
    monitor_arg_t *marg  = (monitor_arg_t *)arg;
    process_manager_t *pm = marg->pm;
    tunnel_entry_t    *entry = marg->entry;
    free(marg);

    /* Dopo 2 secondi, se il processo è ancora vivo → "active" */
    usleep(2000000);
    int probe = waitpid(entry->pid, NULL, WNOHANG);
    if (probe == 0) {
        /* Ancora in vita */
        broadcast_status(pm, entry->host, "active", 0);
    }

    int   status   = 0;
    pid_t ret_pid  = waitpid(entry->pid, &status, 0);

    int exit_code = -1;
    if (ret_pid > 0) {
        if (WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            exit_code = -(int)WTERMSIG(status); /* negativo = segnale */
    }

    pthread_mutex_lock(&pm->mutex);
    entry->active = 0;
    pthread_mutex_unlock(&pm->mutex);

    broadcast_status(pm, entry->host, "inactive", exit_code);
    return NULL;
}

/* -------------------------------------------------------------------------
 * process_manager_create
 * ---------------------------------------------------------------------- */
process_manager_t *process_manager_create(sse_broadcaster_t *sse)
{
    process_manager_t *pm = calloc(1, sizeof(*pm));
    if (!pm)
        return NULL;

    pm->sse = sse;

    if (pthread_mutex_init(&pm->mutex, NULL) != 0) {
        free(pm);
        return NULL;
    }

    return pm;
}

/* -------------------------------------------------------------------------
 * process_manager_start_tunnel
 *
 * Costruisce l'argv per ssh e poi esegue fork/exec.
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

    if (pm->num_tunnels >= MAX_TUNNELS) {
        pthread_mutex_unlock(&pm->mutex);
        fprintf(stderr, "process_manager: raggiunto MAX_TUNNELS (%d)\n",
                MAX_TUNNELS);
        return -1;
    }

    pthread_mutex_unlock(&pm->mutex);

    /*
     * Costruzione argv per ssh.
     * Slot massimi: "ssh" + opzioni fisse (6) + port (-p N, 2) +
     * identity (-i path, 2) + ProxyJump (-J host, 2) +
     * -L/-R per forward (MAX_FORWARDS * 2 ciascuno) +
     * -D per dynamic (MAX_FORWARDS * 2) +
     * host name + NULL
     */
    int max_args = 1 /* ssh */
                 + 6 /* -N -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 */
                 + 2 /* -p port */
                 + 2 /* -l user */
                 + 2 /* -i identity */
                 + 2 /* -J proxyjump */
                 + host->num_local_forward   * 2
                 + host->num_remote_forward  * 2
                 + host->num_dynamic_forward * 2
                 + 1 /* hostname/alias */
                 + 1 /* NULL sentinel */;

    char **argv = calloc((size_t)max_args, sizeof(char *));
    if (!argv)
        return -1;

    /*
     * Buffer di supporto per le stringhe dei forward.
     * Alloca un array di stringhe da liberare dopo exec/errore.
     */
    int   nfwd_bufs = host->num_local_forward
                    + host->num_remote_forward
                    + host->num_dynamic_forward;
    char **fwd_bufs = calloc((size_t)(nfwd_bufs + 1), sizeof(char *));
    if (!fwd_bufs) {
        free(argv);
        return -1;
    }
    int buf_idx = 0;

    int argc = 0;
    argv[argc++] = "ssh";

    /* Modalità no-shell (tunnel only) */
    argv[argc++] = "-N";

    /* Opzioni di robustezza */
    argv[argc++] = "-o";
    argv[argc++] = "ExitOnForwardFailure=yes";
    argv[argc++] = "-o";
    argv[argc++] = "ServerAliveInterval=30";
    argv[argc++] = "-o";
    argv[argc++] = "ServerAliveCountMax=3";

    /* Porta SSH non standard */
    if (host->port > 0 && host->port != 22) {
        argv[argc++] = "-p";

        char *port_str = malloc(16);
        if (!port_str) goto oom;
        fwd_bufs[buf_idx++] = port_str;
        snprintf(port_str, 16, "%d", host->port);
        argv[argc++] = port_str;
    }

    /* Utente */
    if (host->user[0] != '\0') {
        argv[argc++] = "-l";
        argv[argc++] = (char *)host->user; /* punta direttamente alla struct */
    }

    /* Identity file */
    if (host->identity_file[0] != '\0') {
        argv[argc++] = "-i";
        argv[argc++] = (char *)host->identity_file;
    }

    /* ProxyJump */
    if (host->proxy_jump[0] != '\0') {
        argv[argc++] = "-J";
        argv[argc++] = (char *)host->proxy_jump;
    }

    /* Forward locali: -L [bind_addr:]bind_port:remote_host:remote_port */
    for (int i = 0; i < host->num_local_forward; i++) {
        const forward_rule_t *r = &host->local_forward[i];
        char *s = malloc(512);
        if (!s) goto oom;
        fwd_bufs[buf_idx++] = s;

        if (r->bind_addr[0] != '\0')
            snprintf(s, 512, "%s:%d:%s:%d",
                     r->bind_addr, r->bind_port,
                     r->remote_host, r->remote_port);
        else
            snprintf(s, 512, "%d:%s:%d",
                     r->bind_port, r->remote_host, r->remote_port);

        argv[argc++] = "-L";
        argv[argc++] = s;
    }

    /* Forward remoti: -R [bind_addr:]bind_port:remote_host:remote_port */
    for (int i = 0; i < host->num_remote_forward; i++) {
        const forward_rule_t *r = &host->remote_forward[i];
        char *s = malloc(512);
        if (!s) goto oom;
        fwd_bufs[buf_idx++] = s;

        if (r->bind_addr[0] != '\0')
            snprintf(s, 512, "%s:%d:%s:%d",
                     r->bind_addr, r->bind_port,
                     r->remote_host, r->remote_port);
        else
            snprintf(s, 512, "%d:%s:%d",
                     r->bind_port, r->remote_host, r->remote_port);

        argv[argc++] = "-R";
        argv[argc++] = s;
    }

    /* Forward dinamici (SOCKS): -D [bind_addr:]bind_port */
    for (int i = 0; i < host->num_dynamic_forward; i++) {
        const dynamic_rule_t *r = &host->dynamic_forward[i];
        char *s = malloc(128);
        if (!s) goto oom;
        fwd_bufs[buf_idx++] = s;

        if (r->bind_addr[0] != '\0')
            snprintf(s, 128, "%s:%d", r->bind_addr, r->bind_port);
        else
            snprintf(s, 128, "%d", r->bind_port);

        argv[argc++] = "-D";
        argv[argc++] = s;
    }

    /*
     * Hostname di destinazione: se hostname esplicitamente specificato
     * usiamo quello, altrimenti usiamo il name (che ssh risolverà via
     * ~/.ssh/config o DNS).
     */
    if (host->hostname[0] != '\0')
        argv[argc++] = (char *)host->hostname;
    else
        argv[argc++] = (char *)host->name;

    argv[argc] = NULL;

    /* Fork */
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        goto cleanup;
    }

    if (pid == 0) {
        /* Figlio: esegui ssh */
        execvp("ssh", argv);
        _exit(127);
    }

    /* Padre */

    /* Libera i buffer dei forward (il figlio ne ha già una copia via exec) */
    for (int i = 0; i < buf_idx; i++)
        free(fwd_bufs[i]);
    free(fwd_bufs);
    free(argv);

    /* Registra il tunnel */
    {
        tunnel_entry_t *entry;
        monitor_arg_t  *marg;
        pthread_t       tid;

        pthread_mutex_lock(&pm->mutex);
        entry = &pm->tunnels[pm->num_tunnels];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->host, sizeof(entry->host), "%s", host->name);
        entry->pid    = pid;
        entry->active = 1;
        entry->pm     = pm;
        pm->num_tunnels++;
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

oom:
    fprintf(stderr, "process_manager: out of memory durante build argv\n");

cleanup:
    for (int i = 0; i < buf_idx; i++)
        free(fwd_bufs[i]);
    free(fwd_bufs);
    free(argv);
    return -1;
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
            kill(e->pid, SIGTERM);
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

    /* Prima passata: SIGTERM */
    for (int i = 0; i < pm->num_tunnels; i++) {
        tunnel_entry_t *e = &pm->tunnels[i];
        if (e->active && e->pid > 0)
            kill(e->pid, SIGTERM);
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
            /* Ancora in vita */
            kill(e->pid, SIGKILL);
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
