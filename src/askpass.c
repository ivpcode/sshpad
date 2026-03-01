/*
 * askpass.c — Meccanismo SSH_ASKPASS per SSHPad.
 *
 * Flusso completo:
 *
 *  1. askpass_init() crea /tmp/sshpad-askpass-XXXXXX, uno script shell
 *     eseguibile che SSH chiama al posto di un dialog grafico.
 *
 *  2. I processi SSH vengono avviati con:
 *       SSH_ASKPASS=<script_path>
 *       SSH_ASKPASS_REQUIRE=force
 *     così SSH invoca lo script invece di leggere dal terminale.
 *
 *  3. Lo script genera un UUID locale, poi fa:
 *       GET http://127.0.0.1:<port>/api/internal/askpass?id=UUID&prompt=<enc>
 *     Questa richiesta rimane pendente finché l'utente non inserisce la
 *     password nella UI; il server risponde con la password in chiaro.
 *
 *  4. Sul lato server:
 *     - Il gestore di /api/internal/askpass chiama askpass_wait_for_password().
 *     - La funzione registra la richiesta nella tabella globale e attende
 *       su una condizione POSIX con timeout di 120 secondi.
 *     - Quando la UI invia POST /api/password il gestore chiama
 *       askpass_deliver_password() che sveglia il thread in attesa.
 *
 *  5. askpass_cleanup() rimuove lo script temporaneo.
 *
 * Il codice è C11, thread-safe e compilabile con:
 *   -std=c11 -Wall -Wextra -Wpedantic -pthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "askpass.h"

/* --------------------------------------------------------------------------
 * Tabella delle richieste di password in volo.
 * Dimensionata per gestire un numero ragionevole di tunnel concorrenti.
 * -------------------------------------------------------------------------- */
#define MAX_ASKPASS_REQUESTS 16
#define ASKPASS_TIMEOUT_SEC  120

typedef struct {
    char            request_id[64];  /* UUID che identifica la richiesta    */
    char            password[512];   /* Password consegnata dalla UI        */
    int             ready;           /* 1 = la password è disponibile       */
    int             in_use;          /* 1 = slot occupato                   */
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} password_request_t;

/* Tabella globale protetta da un mutex di livello superiore per le operazioni
 * di allocazione/rilascio degli slot. */
static password_request_t  g_requests[MAX_ASKPASS_REQUESTS];
static pthread_mutex_t     g_table_mutex = PTHREAD_MUTEX_INITIALIZER;
static int                 g_table_init  = 0; /* 1 dopo la prima inizializzazione */

/* --------------------------------------------------------------------------
 * init_table — inizializza le strutture dati la prima volta.
 * Chiamata internamente; non rientrante senza g_table_mutex.
 * -------------------------------------------------------------------------- */
static void init_table(void)
{
    if (g_table_init)
        return;

    for (int i = 0; i < MAX_ASKPASS_REQUESTS; i++) {
        memset(&g_requests[i], 0, sizeof(g_requests[i]));
        pthread_mutex_init(&g_requests[i].mutex, NULL);
        pthread_cond_init(&g_requests[i].cond, NULL);
        g_requests[i].in_use = 0;
    }
    g_table_init = 1;
}

/* --------------------------------------------------------------------------
 * alloc_slot — trova uno slot libero nella tabella.
 * Deve essere chiamata con g_table_mutex acquisito.
 * Ritorna il puntatore allo slot oppure NULL se la tabella è piena.
 * -------------------------------------------------------------------------- */
static password_request_t *alloc_slot(const char *request_id)
{
    for (int i = 0; i < MAX_ASKPASS_REQUESTS; i++) {
        if (!g_requests[i].in_use) {
            /* Azzera solo i campi dati, NON i mutex/cond che sono già stati
             * inizializzati da init_table() e devono restare validi tra un
             * utilizzo e l'altro dello slot. */
            memset(g_requests[i].request_id, 0,
                   sizeof(g_requests[i].request_id));
            memset(g_requests[i].password, 0,
                   sizeof(g_requests[i].password));
            strncpy(g_requests[i].request_id, request_id,
                    sizeof(g_requests[i].request_id) - 1);
            g_requests[i].ready  = 0;
            g_requests[i].in_use = 1;
            return &g_requests[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * find_slot — trova lo slot corrispondente a request_id.
 * Deve essere chiamata con g_table_mutex acquisito.
 * -------------------------------------------------------------------------- */
static password_request_t *find_slot(const char *request_id)
{
    for (int i = 0; i < MAX_ASKPASS_REQUESTS; i++) {
        if (g_requests[i].in_use &&
            strncmp(g_requests[i].request_id, request_id,
                    sizeof(g_requests[i].request_id)) == 0) {
            return &g_requests[i];
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * askpass_init
 *
 * Crea uno script shell temporaneo in /tmp.  Lo script:
 *   1. Genera un UUID leggendo /dev/urandom.
 *   2. Codifica il prompt SSH come query-string minimale (solo i caratteri
 *      più comuni; spazi diventano %20, i caratteri speciali più frequenti
 *      vengono sostituiti con la loro codifica percentuale).
 *   3. Esegue una GET bloccante con `curl` o `wget` verso il server HTTP
 *      locale di SSHPad e stampa la risposta su stdout (la password).
 *
 * Lo script usa `curl` se disponibile, altrimenti ricade su `wget`.
 * In assenza di entrambi ritorna una stringa vuota (SSH fallirà).
 *
 * Parametri:
 *   askpass_path_out — buffer di almeno 512 byte; riceve il path dello script.
 *   server_port      — porta del server HTTP locale (es. 54321).
 *
 * Ritorna 0 in caso di successo, -1 in caso di errore.
 * -------------------------------------------------------------------------- */
int askpass_init(char *askpass_path_out, int server_port)
{
    if (!askpass_path_out || server_port <= 0 || server_port > 65535)
        return -1;

    /* Inizializza la tabella se necessario. */
    pthread_mutex_lock(&g_table_mutex);
    init_table();
    pthread_mutex_unlock(&g_table_mutex);

    /* Crea il file temporaneo con mkstemp per evitare race condition. */
    char tmpl[] = "/tmp/sshpad-askpass-XXXXXX";
    int  fd     = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }

    /* Scrive lo script shell nel file.
     *
     * Dettagli implementativi:
     * - Il prompt SSH arriva come primo argomento ($1).
     * - L'UUID viene generato leggendo 16 byte da /dev/urandom e
     *   formattandoli in esadecimale tramite `od` o `xxd` (tool standard).
     * - La codifica del prompt usa `printf '%s' "$1" | od -An -tx1 -v`
     *   per trasformarlo carattere per carattere in %XX; la shell loop
     *   è volutamente semplice e compatibile con sh POSIX.
     * - Il timeout di curl/wget è impostato a ASKPASS_TIMEOUT_SEC + 10
     *   per essere leggermente più lungo del timeout lato server.
     */
    int curl_timeout = ASKPASS_TIMEOUT_SEC + 10;

    /* Lo script è scritto come un'unica stringa per semplicità.
     * Le variabili shell sono espanse dal C (server_port, curl_timeout). */
    char script[4096];
    int  slen = snprintf(script, sizeof(script),
        "#!/bin/sh\n"
        "# sshpad-askpass: generato automaticamente — non modificare.\n"
        "\n"
        "PROMPT=\"${1:-Password: }\"\n"
        "PORT=%d\n"
        "\n"
        "# Genera un UUID v4 grezzo da /dev/urandom.\n"
        "UUID=$(od -An -N16 -tx1 /dev/urandom 2>/dev/null \\\n"
        "        | tr -d ' \\n' \\\n"
        "        | sed 's/\\(........\\)\\(....\\)\\(....\\)\\(....\\)\\(............\\)/\\1-\\2-\\3-\\4-\\5/')\n"
        "\n"
        "# Codifica percent-encoding minimale del prompt (spazi e simboli comuni).\n"
        "encode_prompt() {\n"
        "    printf '%%s' \"$1\" | od -An -tx1 -v | tr ' ' '\\n' | grep -v '^$' | while read -r HEX; do\n"
        "        case \"$HEX\" in\n"
        "            20) printf '%%%%20' ;;\n"
        "            3a) printf '%%%%3A' ;;\n"
        "            2f) printf '%%%%2F' ;;\n"
        "            3f) printf '%%%%3F' ;;\n"
        "            26) printf '%%%%26' ;;\n"
        "            3d) printf '%%%%3D' ;;\n"
        "            2b) printf '%%%%2B' ;;\n"
        "            *)  printf \"$(printf '\\\\x%%s' \"$HEX\")\" ;;\n"
        "        esac\n"
        "    done\n"
        "}\n"
        "\n"
        "ENC_PROMPT=$(encode_prompt \"$PROMPT\")\n"
        "URL=\"http://127.0.0.1:${PORT}/api/internal/askpass?id=${UUID}&prompt=${ENC_PROMPT}\"\n"
        "\n"
        "# Tenta prima con curl, poi con wget.\n"
        "if command -v curl >/dev/null 2>&1; then\n"
        "    curl -sf --max-time %d \"$URL\"\n"
        "elif command -v wget >/dev/null 2>&1; then\n"
        "    wget -qO- --timeout=%d \"$URL\"\n"
        "else\n"
        "    printf '' # Nessun client HTTP disponibile.\n"
        "fi\n",
        server_port, curl_timeout, curl_timeout);

    if (slen <= 0 || (size_t)slen >= sizeof(script)) {
        close(fd);
        unlink(tmpl);
        return -1;
    }

    /* Scrive lo script nel file temporaneo. */
    ssize_t written = 0;
    ssize_t remaining = slen;
    const char *ptr = script;
    while (remaining > 0) {
        written = write(fd, ptr, (size_t)remaining);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            perror("write askpass script");
            close(fd);
            unlink(tmpl);
            return -1;
        }
        ptr += written;
        remaining -= written;
    }
    close(fd);

    /* Rende lo script eseguibile (rwxr-xr-x). */
    if (chmod(tmpl, 0755) != 0) {
        perror("chmod askpass script");
        unlink(tmpl);
        return -1;
    }

    strncpy(askpass_path_out, tmpl, 511);
    askpass_path_out[511] = '\0';
    return 0;
}

/* --------------------------------------------------------------------------
 * askpass_deliver_password
 *
 * Chiamata dal gestore di POST /api/password nella UI: trova la richiesta
 * corrispondente a request_id, copia la password e sveglia il thread che
 * era in attesa in askpass_wait_for_password().
 *
 * Ritorna 0 in caso di successo, -1 se la richiesta non è stata trovata.
 * -------------------------------------------------------------------------- */
int askpass_deliver_password(const char *request_id, const char *password)
{
    if (!request_id || !request_id[0] || !password)
        return -1;

    pthread_mutex_lock(&g_table_mutex);
    password_request_t *req = find_slot(request_id);
    pthread_mutex_unlock(&g_table_mutex);

    if (!req) {
        fprintf(stderr, "askpass: richiesta non trovata: %s\n", request_id);
        return -1;
    }

    pthread_mutex_lock(&req->mutex);
    strncpy(req->password, password, sizeof(req->password) - 1);
    req->password[sizeof(req->password) - 1] = '\0';
    req->ready = 1;
    pthread_cond_signal(&req->cond);
    pthread_mutex_unlock(&req->mutex);

    return 0;
}

/* --------------------------------------------------------------------------
 * askpass_wait_for_password
 *
 * Chiamata dal gestore HTTP di GET /api/internal/askpass.
 * Registra una nuova richiesta nella tabella, poi attende fino a
 * ASKPASS_TIMEOUT_SEC secondi che askpass_deliver_password() segnali
 * l'arrivo della password.
 *
 * Ritorna una stringa allocata con malloc (da liberare con free) contenente
 * la password, oppure NULL in caso di timeout o errore.
 *
 * La funzione è bloccante e deve essere chiamata da un thread separato
 * (tipicamente quello del worker MHD) per non bloccare il loop GTK.
 * -------------------------------------------------------------------------- */
char *askpass_wait_for_password(const char *request_id)
{
    if (!request_id || !request_id[0])
        return NULL;

    /* Assicura che la tabella sia inizializzata. */
    pthread_mutex_lock(&g_table_mutex);
    init_table();
    password_request_t *req = alloc_slot(request_id);
    pthread_mutex_unlock(&g_table_mutex);

    if (!req) {
        fprintf(stderr, "askpass: tabella piena, richiesta rifiutata: %s\n",
                request_id);
        return NULL;
    }

    /* Calcola il tempo di scadenza assoluto per pthread_cond_timedwait. */
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        /* Fallback: usa time() con perdita di precisione sub-secondo. */
        deadline.tv_sec  = (time_t)time(NULL) + ASKPASS_TIMEOUT_SEC;
        deadline.tv_nsec = 0;
    } else {
        deadline.tv_sec += ASKPASS_TIMEOUT_SEC;
    }

    char *result = NULL;

    pthread_mutex_lock(&req->mutex);

    /* Attende la password con timeout. */
    while (!req->ready) {
        int rc = pthread_cond_timedwait(&req->cond, &req->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            fprintf(stderr, "askpass: timeout per la richiesta %s\n",
                    request_id);
            break;
        }
        /* Ignora EINTR e altri errori transitori; controlla ready al prossimo
         * giro del loop. */
    }

    if (req->ready && req->password[0] != '\0') {
        result = strdup(req->password);
        /* Azzera la password dalla memoria il prima possibile. */
        volatile char *p = req->password;
        for (size_t i = 0; i < sizeof(req->password); i++)
            p[i] = '\0';
    }

    pthread_mutex_unlock(&req->mutex);

    /* Rilascia lo slot nella tabella. */
    pthread_mutex_lock(&g_table_mutex);
    req->in_use = 0;
    pthread_mutex_unlock(&g_table_mutex);

    return result;
}

/* --------------------------------------------------------------------------
 * askpass_cleanup
 *
 * Rimuove lo script askpass temporaneo dal filesystem.
 * Chiamata alla chiusura dell'applicazione o al teardown del tunnel.
 * -------------------------------------------------------------------------- */
void askpass_cleanup(const char *askpass_path)
{
    if (!askpass_path || !askpass_path[0])
        return;

    if (unlink(askpass_path) != 0 && errno != ENOENT) {
        perror("askpass_cleanup: unlink");
    }
}
