/*
 * local_proxy.c — Reverse proxy HTTPS locale per SSHPad.
 *
 * Per ogni host SSH con LocalForward:
 *   1. Genera un certificato TLS con mkcert
 *   2. Aggiunge 127.0.0.1 <host_alias> a /etc/hosts (via pkexec)
 *   3. Ascolta su porta 443 (fallback 8443) con SNI routing
 *   4. Proxa le richieste HTTPS a localhost:<prima_porta_forward>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "local_proxy.h"
#include "config_parser.h"
#include "sse.h"

#define LP_MAX_VHOSTS    64
#define LP_BUF_SIZE      16384
#define LP_BACKLOG       16
#define LP_POLL_TIMEOUT  1000   /* ms */

/* -------------------------------------------------------------------------
 * Strutture interne
 * ---------------------------------------------------------------------- */

typedef struct {
    char     hostname[256];
    int      backend_port;
    char     cert_path[512];
    char     key_path[512];
    SSL_CTX *ssl_ctx;
} lp_vhost_t;

struct local_proxy {
    int               listen_fd;
    int               port;          /* porta effettiva (443 o 8443) */
    volatile int      running;
    pthread_t         accept_thread;
    lp_vhost_t        vhosts[LP_MAX_VHOSTS];
    int               num_vhosts;
    SSL_CTX          *dispatcher_ctx;
    sse_broadcaster_t *sse;
    char              cert_dir[512];
};

typedef struct {
    local_proxy_t *lp;
    int            client_fd;
} conn_arg_t;

/* -------------------------------------------------------------------------
 * Helper: scrivi tutti i byte
 * ---------------------------------------------------------------------- */

static int write_all(int fd, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, (size_t)(len - total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (int)n;
    }
    return 0;
}

static int ssl_write_all(SSL *ssl, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = SSL_write(ssl, buf + total, len - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Helper: mkdir -p ricorsivo
 * ---------------------------------------------------------------------- */

static void mkdirs(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* -------------------------------------------------------------------------
 * mkcert: verifica e generazione certificati
 * ---------------------------------------------------------------------- */

int lp_check_mkcert(void)
{
    /* Verifica che il binario mkcert sia nel PATH */
    FILE *fp = popen("which mkcert 2>/dev/null", "r");
    if (!fp) return -1;

    char buf[256];
    int found = (fgets(buf, sizeof(buf), fp) != NULL);
    int status = pclose(fp);

    return (found && status == 0) ? 0 : -1;
}

static int ensure_mkcert_ca(void)
{
    /* Verifica se la CA locale è già installata */
    char ca_path[512];
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (data_home && data_home[0])
        snprintf(ca_path, sizeof(ca_path), "%s/mkcert/rootCA.pem", data_home);
    else if (home && home[0])
        snprintf(ca_path, sizeof(ca_path), "%s/.local/share/mkcert/rootCA.pem", home);
    else
        return -1;

    if (access(ca_path, R_OK) == 0)
        return 0;

    /* CA non installata: esegui mkcert -install */
    fprintf(stderr, "local_proxy: installazione CA locale mkcert...\n");
    int rc = system("mkcert -install 2>&1");
    return (rc == 0) ? 0 : -1;
}

static int ensure_cert_dir(char *dir_out, size_t dir_size)
{
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");

    if (data_home && data_home[0])
        snprintf(dir_out, dir_size, "%s/sshpad/certs", data_home);
    else if (home && home[0])
        snprintf(dir_out, dir_size, "%s/.local/share/sshpad/certs", home);
    else
        return -1;

    mkdirs(dir_out);
    return (access(dir_out, W_OK) == 0) ? 0 : -1;
}

static int generate_cert(const char *hostname, const char *cert_dir,
                          char *cert_out, char *key_out, size_t path_size)
{
    snprintf(cert_out, path_size, "%s/%s.pem", cert_dir, hostname);
    snprintf(key_out,  path_size, "%s/%s-key.pem", cert_dir, hostname);

    /* Se già esistono, non rigenerare */
    if (access(cert_out, R_OK) == 0 && access(key_out, R_OK) == 0)
        return 0;

    fprintf(stderr, "local_proxy: generazione certificato per '%s'...\n",
            hostname);

    /* mkcert genera cert valido per hostname, localhost e 127.0.0.1 */
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        execlp("mkcert", "mkcert",
               "-cert-file", cert_out,
               "-key-file", key_out,
               hostname, "localhost", "127.0.0.1",
               NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * /etc/hosts: aggiunta entry mancanti
 * ---------------------------------------------------------------------- */

static int host_entry_exists(const char *hostname)
{
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* Ignora commenti */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n') continue;

        /* Cerca 127.0.0.1 e il nostro hostname sulla stessa riga */
        if (strstr(line, "127.0.0.1") && strstr(line, hostname)) {
            /* Verifica che hostname sia una parola intera */
            char *pos = strstr(line, hostname);
            size_t hlen = strlen(hostname);
            char before = (pos > line) ? *(pos - 1) : ' ';
            char after  = pos[hlen];

            if ((before == ' ' || before == '\t') &&
                (after == ' ' || after == '\t' || after == '\n' ||
                 after == '\r' || after == '\0')) {
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

static int ensure_hosts_entries(lp_vhost_t *vhosts, int num_vhosts)
{
    /* Trova entry mancanti */
    char entries[4096] = "";
    char *ptr = entries;
    int remaining = (int)sizeof(entries);
    int any_missing = 0;

    for (int i = 0; i < num_vhosts; i++) {
        if (host_entry_exists(vhosts[i].hostname))
            continue;

        if (!any_missing) {
            int n = snprintf(ptr, (size_t)remaining,
                             "\n# --- SSHPad managed entries ---\n");
            ptr += n;
            remaining -= n;
            any_missing = 1;
        }

        int n = snprintf(ptr, (size_t)remaining,
                         "127.0.0.1 %s\n", vhosts[i].hostname);
        ptr += n;
        remaining -= n;
    }

    if (!any_missing)
        return 0;

    fprintf(stderr, "local_proxy: aggiunta entry in /etc/hosts...\n");

    /* Scrivi in un file temporaneo */
    char tmpfile[] = "/tmp/sshpad-hosts-XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) return -1;

    write_all(fd, entries, (int)strlen(entries));
    close(fd);

    /* Usa pkexec per appendere a /etc/hosts */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "pkexec sh -c 'cat \"%s\" >> /etc/hosts' 2>/dev/null", tmpfile);
    int rc = system(cmd);
    unlink(tmpfile);

    if (rc != 0) {
        fprintf(stderr,
                "local_proxy: impossibile aggiornare /etc/hosts.\n"
                "Aggiungi manualmente le seguenti righe a /etc/hosts:\n%s",
                entries);
        return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * SSL: setup contesto per-vhost e callback SNI
 * ---------------------------------------------------------------------- */

static SSL_CTX *create_vhost_ctx(const char *cert_path, const char *key_path)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_path) != 1) {
        fprintf(stderr, "local_proxy: errore caricamento cert: %s\n", cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "local_proxy: errore caricamento key: %s\n", key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

static int sni_callback(SSL *ssl, int *al, void *arg)
{
    (void)al;
    local_proxy_t *lp = (local_proxy_t *)arg;
    const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    if (!servername)
        return SSL_TLSEXT_ERR_NOACK;

    for (int i = 0; i < lp->num_vhosts; i++) {
        if (strcasecmp(lp->vhosts[i].hostname, servername) == 0) {
            SSL_set_SSL_CTX(ssl, lp->vhosts[i].ssl_ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }

    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

/* -------------------------------------------------------------------------
 * Proxy: data pipe bidirezionale TLS <-> backend TCP
 * ---------------------------------------------------------------------- */

static void pipe_data(SSL *ssl, int backend_fd)
{
    char buf[LP_BUF_SIZE];
    int ssl_fd = SSL_get_fd(ssl);

    for (;;) {
        /* Se SSL ha dati bufferizzati internamente, leggili subito */
        if (SSL_pending(ssl) > 0) {
            int n = SSL_read(ssl, buf, (int)sizeof(buf));
            if (n <= 0) break;
            if (write_all(backend_fd, buf, n) != 0) break;
            continue;
        }

        struct pollfd fds[2];
        fds[0].fd      = ssl_fd;
        fds[0].events  = POLLIN;
        fds[0].revents = 0;
        fds[1].fd      = backend_fd;
        fds[1].events  = POLLIN;
        fds[1].revents = 0;

        int ret = poll(fds, 2, 30000);
        if (ret <= 0) break;

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        /* Client TLS → Backend */
        if (fds[0].revents & POLLIN) {
            int n = SSL_read(ssl, buf, (int)sizeof(buf));
            if (n <= 0) break;
            if (write_all(backend_fd, buf, n) != 0) break;
        }

        /* Backend → Client TLS */
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(backend_fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (ssl_write_all(ssl, buf, (int)n) != 0) break;
        }
    }
}

/* -------------------------------------------------------------------------
 * Proxy: handler per singola connessione (thread)
 * ---------------------------------------------------------------------- */

static int find_backend_port(local_proxy_t *lp, const char *servername)
{
    if (!servername) return -1;

    for (int i = 0; i < lp->num_vhosts; i++) {
        if (strcasecmp(lp->vhosts[i].hostname, servername) == 0)
            return lp->vhosts[i].backend_port;
    }
    return -1;
}

static int connect_backend(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *connection_handler(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;
    local_proxy_t *lp = ca->lp;
    int client_fd = ca->client_fd;
    free(ca);

    SSL *ssl = SSL_new(lp->dispatcher_ctx);
    if (!ssl) {
        close(client_fd);
        return NULL;
    }

    SSL_set_fd(ssl, client_fd);

    /* Handshake TLS (il callback SNI seleziona il contesto corretto) */
    if (SSL_accept(ssl) != 1) {
        unsigned long err = ERR_peek_last_error();
        /* Non loggare errori di connessione chiusa dal client */
        if (ERR_GET_REASON(err) != SSL_R_PEER_DID_NOT_RETURN_A_CERTIFICATE)
            fprintf(stderr, "local_proxy: TLS handshake fallito\n");
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    /* Trova il backend dalla SNI */
    const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    int backend_port = find_backend_port(lp, servername);

    if (backend_port < 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    /* Connetti al backend locale */
    int backend_fd = connect_backend(backend_port);
    if (backend_fd < 0) {
        /* Tunnel non attivo: rispondi con 502 */
        const char *resp =
            "HTTP/1.1 502 Bad Gateway\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 30\r\n"
            "Connection: close\r\n\r\n"
            "Tunnel non attivo per questo host";
        SSL_write(ssl, resp, (int)strlen(resp));
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(client_fd);
        return NULL;
    }

    /* Pipe bidirezionale TLS ↔ backend */
    pipe_data(ssl, backend_fd);

    /* Cleanup */
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(client_fd);
    close(backend_fd);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Proxy: accept loop (thread principale)
 * ---------------------------------------------------------------------- */

static void *accept_loop(void *arg)
{
    local_proxy_t *lp = (local_proxy_t *)arg;

    while (lp->running) {
        struct pollfd pfd;
        pfd.fd      = lp->listen_fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, LP_POLL_TIMEOUT);
        if (ret <= 0) continue;

        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int client_fd = accept(lp->listen_fd,
                               (struct sockaddr *)&client_addr, &addrlen);
        if (client_fd < 0) continue;

        conn_arg_t *ca = malloc(sizeof(*ca));
        if (!ca) {
            close(client_fd);
            continue;
        }
        ca->lp        = lp;
        ca->client_fd = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_handler, ca) != 0) {
            free(ca);
            close(client_fd);
        } else {
            pthread_detach(tid);
        }
    }

    return NULL;
}

/* -------------------------------------------------------------------------
 * API pubblica
 * ---------------------------------------------------------------------- */

local_proxy_t *lp_create(const ssh_host_t *hosts, int num_hosts,
                          sse_broadcaster_t *sse)
{
    /* Assicura che la CA mkcert sia installata */
    if (ensure_mkcert_ca() != 0) {
        fprintf(stderr, "local_proxy: impossibile installare CA mkcert\n");
        return NULL;
    }

    local_proxy_t *lp = calloc(1, sizeof(*lp));
    if (!lp) return NULL;

    lp->listen_fd = -1;
    lp->sse       = sse;

    /* Prepara directory certificati */
    if (ensure_cert_dir(lp->cert_dir, sizeof(lp->cert_dir)) != 0) {
        fprintf(stderr, "local_proxy: impossibile creare directory certificati\n");
        free(lp);
        return NULL;
    }

    /* Inizializza OpenSSL */
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    /* Per ogni host con LocalForward, crea un vhost */
    for (int i = 0; i < num_hosts && lp->num_vhosts < LP_MAX_VHOSTS; i++) {
        const ssh_host_t *h = &hosts[i];

        if (h->num_local_forward == 0)
            continue;

        lp_vhost_t *vh = &lp->vhosts[lp->num_vhosts];

        /* Usa l'alias Host come hostname per SNI e /etc/hosts */
        snprintf(vh->hostname, sizeof(vh->hostname), "%s", h->name);

        /* Backend: prima porta di LocalForward */
        vh->backend_port = h->local_forward[0].bind_port;

        /* Genera certificato */
        if (generate_cert(vh->hostname, lp->cert_dir,
                          vh->cert_path, vh->key_path,
                          sizeof(vh->cert_path)) != 0) {
            fprintf(stderr, "local_proxy: skip '%s' (cert generation failed)\n",
                    vh->hostname);
            continue;
        }

        /* Crea SSL_CTX per questo vhost */
        vh->ssl_ctx = create_vhost_ctx(vh->cert_path, vh->key_path);
        if (!vh->ssl_ctx) {
            fprintf(stderr, "local_proxy: skip '%s' (SSL_CTX failed)\n",
                    vh->hostname);
            continue;
        }

        fprintf(stderr, "local_proxy: vhost '%s' -> localhost:%d\n",
                vh->hostname, vh->backend_port);
        lp->num_vhosts++;
    }

    if (lp->num_vhosts == 0) {
        fprintf(stderr, "local_proxy: nessun host con LocalForward trovato\n");
        free(lp);
        return NULL;
    }

    /* Aggiorna /etc/hosts */
    ensure_hosts_entries(lp->vhosts, lp->num_vhosts);

    /* Crea dispatcher SSL_CTX con SNI callback */
    lp->dispatcher_ctx = SSL_CTX_new(TLS_server_method());
    if (!lp->dispatcher_ctx) {
        fprintf(stderr, "local_proxy: SSL_CTX_new fallito\n");
        lp_free(lp);
        return NULL;
    }

    SSL_CTX_set_min_proto_version(lp->dispatcher_ctx, TLS1_2_VERSION);

    /* Carica il cert del primo vhost come default (necessario per SSL_accept) */
    SSL_CTX_use_certificate_chain_file(lp->dispatcher_ctx,
                                        lp->vhosts[0].cert_path);
    SSL_CTX_use_PrivateKey_file(lp->dispatcher_ctx,
                                 lp->vhosts[0].key_path, SSL_FILETYPE_PEM);

    /* Registra callback SNI per routing multi-host */
    SSL_CTX_set_tlsext_servername_callback(lp->dispatcher_ctx, sni_callback);
    SSL_CTX_set_tlsext_servername_arg(lp->dispatcher_ctx, lp);

    return lp;
}

int lp_start(local_proxy_t *lp)
{
    if (!lp) return -1;

    /* Ignora SIGPIPE per evitare crash su write a socket chiuso */
    signal(SIGPIPE, SIG_IGN);

    /* Crea socket listener */
    lp->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (lp->listen_fd < 0) {
        perror("local_proxy: socket");
        return -1;
    }

    int optval = 1;
    setsockopt(lp->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               &optval, sizeof(optval));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    /* Tenta porta 443, poi fallback a 8443 */
    int ports[] = { 443, 8443 };
    int bound = 0;

    for (int i = 0; i < 2; i++) {
        addr.sin_port = htons((uint16_t)ports[i]);

        if (bind(lp->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            lp->port = ports[i];
            bound = 1;
            break;
        }

        if (i == 0 && errno == EACCES) {
            fprintf(stderr,
                    "local_proxy: porta 443 richiede privilegi. "
                    "Fallback a 8443.\n"
                    "Per usare 443 esegui: "
                    "sudo setcap 'cap_net_bind_service=+ep' ./sshpad\n");
        }
    }

    if (!bound) {
        perror("local_proxy: bind");
        close(lp->listen_fd);
        lp->listen_fd = -1;
        return -1;
    }

    if (listen(lp->listen_fd, LP_BACKLOG) != 0) {
        perror("local_proxy: listen");
        close(lp->listen_fd);
        lp->listen_fd = -1;
        return -1;
    }

    fprintf(stderr, "local_proxy: HTTPS proxy attivo su 127.0.0.1:%d\n",
            lp->port);

    /* Notifica via SSE */
    if (lp->sse) {
        char json[256];
        snprintf(json, sizeof(json),
                 "{\"port\":%d,\"numVhosts\":%d}",
                 lp->port, lp->num_vhosts);
        sse_broadcast(lp->sse, "proxy_started", json);
    }

    /* Avvia thread accept */
    lp->running = 1;
    if (pthread_create(&lp->accept_thread, NULL, accept_loop, lp) != 0) {
        perror("local_proxy: pthread_create");
        close(lp->listen_fd);
        lp->listen_fd = -1;
        return -1;
    }

    return 0;
}

void lp_stop(local_proxy_t *lp)
{
    if (!lp || !lp->running) return;

    lp->running = 0;

    /* Chiudi il socket listener per svegliare il poll */
    if (lp->listen_fd >= 0) {
        close(lp->listen_fd);
        lp->listen_fd = -1;
    }

    pthread_join(lp->accept_thread, NULL);
}

void lp_free(local_proxy_t *lp)
{
    if (!lp) return;

    if (lp->listen_fd >= 0)
        close(lp->listen_fd);

    /* Libera SSL_CTX per-vhost */
    for (int i = 0; i < lp->num_vhosts; i++) {
        if (lp->vhosts[i].ssl_ctx)
            SSL_CTX_free(lp->vhosts[i].ssl_ctx);
    }

    /* Libera dispatcher context */
    if (lp->dispatcher_ctx)
        SSL_CTX_free(lp->dispatcher_ctx);

    free(lp);
}
