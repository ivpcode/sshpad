# SSHPad — Documentazione Tecnica

## Panoramica

**SSHPad** è un SSH connection manager desktop scritto interamente in **C**, con UI in **HTML/CSS/JS** renderizzata tramite **WebKitGTK**. Un singolo eseguibile che:

- Avvia un HTTP server embedded su una **porta libera** di `127.0.0.1` (scelta automaticamente)
- Apre una **finestra GTK con WebKitGTK** che punta a `http://127.0.0.1:<port>`
- Parsa `~/.ssh/config` e mostra le connessioni come card interattive
- Gestisce tunnel SSH (local/remote/dynamic) tramite `fork/exec` del binario `ssh`
- Lancia terminali nativi dell'OS con la connessione SSH preconfigurata
- Gestisce il prompt password via dialog modale HTML + meccanismo `SSH_ASKPASS`

Nessuna dipendenza Node/npm. Il frontend HTML/JS è **embedded nel binario C** come stringa o file nella directory dell'eseguibile.

---

## Architettura

```
┌──────────────────────────────────────────────────────────┐
│                    Processo sshpad                        │
│                                                          │
│  ┌────────────────────┐     ┌─────────────────────────┐  │
│  │   GTK+ Window      │     │  HTTP Server (MHD)      │  │
│  │                    │     │  127.0.0.1:<auto_port>   │  │
│  │  ┌──────────────┐  │     │                         │  │
│  │  │  WebKitGTK   │──┼────►│  GET /              → index.html  │
│  │  │  WebView     │  │ HTTP│  GET /api/hosts     → JSON        │
│  │  │              │◄─┼─────│  GET /api/events    → SSE         │
│  │  │  (HTML/JS)   │  │     │  POST /api/tunnel/* → gestione    │
│  │  │              │  │     │  POST /api/terminal → fork+exec   │
│  │  │              │  │     │  POST /api/password → stdin ssh   │
│  │  └──────────────┘  │     └────────────┬────────────┘  │
│  └────────────────────┘                  │               │
│                                          │               │
│  ┌───────────────────────────────────────┴────────────┐  │
│  │               Core C                               │  │
│  │                                                    │  │
│  │  ┌──────────────┐  ┌───────────┐  ┌────────────┐  │  │
│  │  │ SSH Config   │  │ Process   │  │ SSE Event  │  │  │
│  │  │ Parser       │  │ Manager   │  │ Broadcaster│  │  │
│  │  └──────────────┘  └─────┬─────┘  └────────────┘  │  │
│  │                          │                         │  │
│  └──────────────────────────┼─────────────────────────┘  │
└─────────────────────────────┼────────────────────────────┘
                              │ fork/exec
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
     ┌──────────────┐ ┌──────────────┐ ┌────────────────┐
     │ ssh -N -L .. │ │ ssh -N -D .. │ │ gnome-terminal │
     │ (tunnel)     │ │ (SOCKS)      │ │ -- ssh host    │
     └──────────────┘ └──────────────┘ └────────────────┘
```

### Flusso di Avvio

```
main()
  │
  ├─ 1. Trova porta libera su 127.0.0.1 (bind + getsockname)
  │
  ├─ 2. Avvia HTTP server (libmicrohttpd) su quella porta
  │     └─ Thread separato (MHD_USE_INTERNAL_POLLING_THREAD)
  │
  ├─ 3. Inizializza GTK
  │
  ├─ 4. Crea GtkWindow + WebKitWebView
  │     └─ webkit_web_view_load_uri("http://127.0.0.1:<port>")
  │
  ├─ 5. Entra nel GTK main loop (gtk_main / g_application_run)
  │
  └─ 6. Alla chiusura finestra: stop MHD, kill processi figli, exit
```

### Iniezione Dinamica della Porta

La porta viene scelta a runtime e deve essere nota al JavaScript. Tre approcci possibili (in ordine di semplicità):

**Approccio 1 — Endpoint `/api/config` (consigliato):**
Il JS non ha bisogno della porta: usa URL relativi (`/api/hosts`, `/api/events`). Siccome WebKitGTK carica `http://127.0.0.1:<port>`, tutte le fetch relative puntano automaticamente al server corretto.

> Questo è l'approccio più pulito: **il JS non deve conoscere la porta**.

**Approccio 2 — Template HTML:**
Il server genera `index.html` al volo, iniettando la porta:
```c
// In risposta a GET /
snprintf(html, sizeof(html), html_template, port); // %d nel template
```

**Approccio 3 — WebKit JavaScript injection:**
```c
char js[128];
snprintf(js, sizeof(js), "window.SSHPAD_PORT = %d;", port);
webkit_web_view_evaluate_javascript(webview, js, -1, NULL, NULL, NULL, NULL, NULL);
```

---

## Stack Tecnologico

| Componente      | Tecnologia                           | Note                                        |
|-----------------|--------------------------------------|---------------------------------------------|
| Linguaggio      | **C** (C11)                          | Unico linguaggio backend                    |
| Finestra        | **GTK 4** + **WebKitGTK 6.0**       | Finestra nativa con webview embedded        |
| HTTP server     | **libmicrohttpd**                    | Embedded, thread-safe, supporta SSE         |
| JSON            | **json-c** oppure **cJSON**          | Serializzazione risposte API                |
| Frontend        | **HTML/CSS/JS** vanilla              | Nessun framework, nessun bundler            |
| SSH             | **Binario `ssh` di sistema**         | fork/exec, nessuna lib SSH                  |
| Terminale       | **Nativo OS** (gnome-terminal, etc.) | Lanciato via fork/exec                      |

### Dipendenze di Sistema (Debian/Ubuntu)

```bash
sudo apt install \
  build-essential \
  libgtk-4-dev \
  libwebkitgtk-6.0-dev \
  libmicrohttpd-dev \
  libjson-c-dev
```

### Dipendenze (Fedora)

```bash
sudo dnf install \
  gtk4-devel \
  webkitgtk6.0-devel \
  libmicrohttpd-devel \
  json-c-devel
```

### Dipendenze (Arch)

```bash
sudo pacman -S gtk4 webkitgtk-6.0 libmicrohttpd json-c
```

---

## Struttura Progetto

```
sshpad/
├── src/
│   ├── main.c                  # Entry point: porta libera → MHD → GTK → WebKit
│   ├── http_server.c/.h        # Setup libmicrohttpd, routing, handler
│   ├── config_parser.c/.h      # Parser ~/.ssh/config
│   ├── process_manager.c/.h    # fork/exec tunnel + terminali, monitoraggio
│   ├── sse.c/.h                # SSE broadcaster (lista client, mutex)
│   ├── terminal_launch.c/.h    # Detect + lancio terminale nativo
│   ├── askpass.c/.h            # Generazione helper SSH_ASKPASS
│   ├── port_finder.c/.h        # Trova porta libera su 127.0.0.1
│   └── util.c/.h               # Base64, UUID, string helpers
│
├── ui/
│   ├── index.html              # Pagina principale
│   ├── style.css               # Stile UI
│   └── app.js                  # Logica frontend (SSE, fetch, DOM)
│
├── Makefile
├── CMakeLists.txt
├── README.md
└── LICENSE
```

---

## Codice C — Moduli Principali

### `main.c`

```c
#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include "http_server.h"
#include "port_finder.h"
#include "config_parser.h"
#include "process_manager.h"
#include "sse.h"

typedef struct {
    int                port;
    struct MHD_Daemon *httpd;
    sse_broadcaster_t *sse;
    process_manager_t *pm;
    ssh_host_t        *hosts;
    int                num_hosts;
} app_context_t;

static void on_activate(GtkApplication *gtkapp, gpointer user_data) {
    app_context_t *ctx = (app_context_t *)user_data;

    // Finestra
    GtkWidget *window = gtk_application_window_new(gtkapp);
    gtk_window_set_title(GTK_WINDOW(window), "SSHPad");
    gtk_window_set_default_size(GTK_WINDOW(window), 960, 680);

    // WebView
    WebKitWebView *webview = WEBKIT_WEB_VIEW(webkit_web_view_new());

    // Disabilita cache per sviluppo (opzionale)
    WebKitSettings *settings = webkit_web_view_get_settings(webview);
    webkit_settings_set_enable_developer_extras(settings, TRUE);

    // Carica UI dal server locale
    char uri[64];
    snprintf(uri, sizeof(uri), "http://127.0.0.1:%d", ctx->port);
    webkit_web_view_load_uri(webview, uri);

    gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(webview));
    gtk_window_present(GTK_WINDOW(window));
}

static void cleanup(app_context_t *ctx) {
    if (ctx->httpd) MHD_stop_daemon(ctx->httpd);
    process_manager_kill_all(ctx->pm);
    process_manager_free(ctx->pm);
    sse_broadcaster_free(ctx->sse);
    ssh_hosts_free(ctx->hosts, ctx->num_hosts);
}

int main(int argc, char *argv[]) {
    app_context_t ctx = {0};

    // 1. Trova porta libera
    ctx.port = find_free_port("127.0.0.1");
    if (ctx.port < 0) {
        fprintf(stderr, "Impossibile trovare una porta libera\n");
        return 1;
    }
    printf("Porta selezionata: %d\n", ctx.port);

    // 2. Parsa SSH config
    ctx.hosts = parse_ssh_config(NULL, &ctx.num_hosts);  // NULL = default path

    // 3. Inizializza SSE e process manager
    ctx.sse = sse_broadcaster_create();
    ctx.pm = process_manager_create(ctx.sse);

    // 4. Avvia HTTP server
    ctx.httpd = http_server_start(ctx.port, &ctx);
    if (!ctx.httpd) {
        fprintf(stderr, "Impossibile avviare HTTP server\n");
        return 1;
    }

    // 5. GTK Application
    GtkApplication *gtkapp = gtk_application_new("io.github.sshpad",
                                                  G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtkapp, "activate", G_CALLBACK(on_activate), &ctx);

    int status = g_application_run(G_APPLICATION(gtkapp), argc, argv);

    // 6. Cleanup
    g_object_unref(gtkapp);
    cleanup(&ctx);

    return status;
}
```

### `port_finder.c`

```c
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "port_finder.h"

int find_free_port(const char *bind_addr) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),  // 0 = lascia scegliere all'OS
    };
    inet_pton(AF_INET, bind_addr, &addr.sin_addr);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &len) < 0) {
        close(sock);
        return -1;
    }

    int port = ntohs(addr.sin_port);
    close(sock);

    return port;
}
```

### `http_server.c`

```c
#include <microhttpd.h>
#include <string.h>
#include <stdio.h>
#include <json-c/json.h>
#include "http_server.h"
#include "sse.h"
#include "config_parser.h"
#include "process_manager.h"

// ------------------------------------------------------------------
// Utilità: lettura body POST (MHD accumula in più chiamate)
// ------------------------------------------------------------------

typedef struct {
    char *data;
    size_t size;
} post_data_t;

// ------------------------------------------------------------------
// Serve file statici dalla directory ui/
// ------------------------------------------------------------------

static const char *get_content_type(const char *path) {
    if (strstr(path, ".html")) return "text/html; charset=utf-8";
    if (strstr(path, ".css"))  return "text/css; charset=utf-8";
    if (strstr(path, ".js"))   return "application/javascript; charset=utf-8";
    if (strstr(path, ".svg"))  return "image/svg+xml";
    if (strstr(path, ".png"))  return "image/png";
    return "application/octet-stream";
}

static enum MHD_Result serve_static(struct MHD_Connection *conn, const char *url) {
    char filepath[512];

    if (strcmp(url, "/") == 0)
        snprintf(filepath, sizeof(filepath), "ui/index.html");
    else
        snprintf(filepath, sizeof(filepath), "ui%s", url);  // es. "ui/style.css"

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *not_found = "404 Not Found";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(not_found), (void *)not_found, MHD_RESPMEM_PERSISTENT);
        return MHD_queue_response(conn, 404, resp);
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *content = malloc(fsize);
    fread(content, 1, fsize, f);
    fclose(f);

    struct MHD_Response *resp = MHD_create_response_from_buffer(
        fsize, content, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", get_content_type(filepath));
    enum MHD_Result ret = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return ret;
}

// ------------------------------------------------------------------
// GET /api/hosts — Lista connessioni SSH
// ------------------------------------------------------------------

static enum MHD_Result handle_get_hosts(struct MHD_Connection *conn,
                                         app_context_t *ctx) {
    json_object *jarr = json_object_new_array();

    for (int i = 0; i < ctx->num_hosts; i++) {
        ssh_host_t *h = &ctx->hosts[i];
        json_object *jh = json_object_new_object();

        json_object_object_add(jh, "name",     json_object_new_string(h->name));
        json_object_object_add(jh, "hostname",  json_object_new_string(h->hostname ?: ""));
        json_object_object_add(jh, "user",      json_object_new_string(h->user ?: ""));
        json_object_object_add(jh, "port",      json_object_new_int(h->port));
        json_object_object_add(jh, "identityFile", json_object_new_string(h->identity_file ?: ""));
        json_object_object_add(jh, "proxyJump",    json_object_new_string(h->proxy_jump ?: ""));

        // Local forwards
        json_object *jlf = json_object_new_array();
        for (int j = 0; j < h->num_local_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr",   json_object_new_string(h->local_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort",   json_object_new_int(h->local_forward[j].bind_port));
            json_object_object_add(jf, "remoteHost", json_object_new_string(h->local_forward[j].remote_host));
            json_object_object_add(jf, "remotePort", json_object_new_int(h->local_forward[j].remote_port));
            json_object_array_add(jlf, jf);
        }
        json_object_object_add(jh, "localForward", jlf);

        // Remote forwards
        json_object *jrf = json_object_new_array();
        for (int j = 0; j < h->num_remote_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr",   json_object_new_string(h->remote_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort",   json_object_new_int(h->remote_forward[j].bind_port));
            json_object_object_add(jf, "remoteHost", json_object_new_string(h->remote_forward[j].remote_host));
            json_object_object_add(jf, "remotePort", json_object_new_int(h->remote_forward[j].remote_port));
            json_object_array_add(jrf, jf);
        }
        json_object_object_add(jh, "remoteForward", jrf);

        // Dynamic forwards
        json_object *jdf = json_object_new_array();
        for (int j = 0; j < h->num_dynamic_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr", json_object_new_string(h->dynamic_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort", json_object_new_int(h->dynamic_forward[j].bind_port));
            json_object_array_add(jdf, jf);
        }
        json_object_object_add(jh, "dynamicForward", jdf);

        // Stato tunnel (dal process manager)
        const char *status = process_manager_get_tunnel_status(ctx->pm, h->name);
        json_object_object_add(jh, "tunnelStatus", json_object_new_string(status));

        json_object_array_add(jarr, jh);
    }

    const char *json_str = json_object_to_json_string(jarr);
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(json_str), (void *)json_str, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    enum MHD_Result ret = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    json_object_put(jarr);  // free
    return ret;
}

// ------------------------------------------------------------------
// GET /api/events — SSE Stream
// ------------------------------------------------------------------

static enum MHD_Result handle_sse(struct MHD_Connection *conn,
                                   app_context_t *ctx) {
    return sse_broadcaster_add_client(ctx->sse, conn);
}

// ------------------------------------------------------------------
// Router principale
// ------------------------------------------------------------------

static enum MHD_Result
request_handler(void *cls,
                struct MHD_Connection *conn,
                const char *url,
                const char *method,
                const char *version,
                const char *upload_data,
                size_t *upload_data_size,
                void **con_cls)
{
    app_context_t *ctx = (app_context_t *)cls;

    // Accumula body POST
    if (strcmp(method, "POST") == 0) {
        if (*con_cls == NULL) {
            post_data_t *pd = calloc(1, sizeof(post_data_t));
            *con_cls = pd;
            return MHD_YES;
        }
        post_data_t *pd = *con_cls;
        if (*upload_data_size > 0) {
            pd->data = realloc(pd->data, pd->size + *upload_data_size + 1);
            memcpy(pd->data + pd->size, upload_data, *upload_data_size);
            pd->size += *upload_data_size;
            pd->data[pd->size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }
        // Body completo, procedi al routing
    }

    // --- Routing ---

    // API GET
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/api/hosts") == 0)
            return handle_get_hosts(conn, ctx);
        if (strcmp(url, "/api/events") == 0)
            return handle_sse(conn, ctx);
        if (strcmp(url, "/api/status") == 0)
            return handle_get_status(conn, ctx);
        if (strncmp(url, "/api/internal/askpass", 20) == 0)
            return handle_askpass(conn, ctx);
    }

    // API POST
    if (strcmp(method, "POST") == 0) {
        post_data_t *pd = *con_cls;
        enum MHD_Result ret = MHD_YES;

        if (strcmp(url, "/api/tunnel/start") == 0)
            ret = handle_tunnel_start(conn, pd->data, ctx);
        else if (strcmp(url, "/api/tunnel/stop") == 0)
            ret = handle_tunnel_stop(conn, pd->data, ctx);
        else if (strcmp(url, "/api/terminal/open") == 0)
            ret = handle_terminal_open(conn, pd->data, ctx);
        else if (strcmp(url, "/api/password") == 0)
            ret = handle_password(conn, pd->data, ctx);

        free(pd->data);
        free(pd);
        *con_cls = NULL;
        return ret;
    }

    // File statici (frontend)
    return serve_static(conn, url);
}

// ------------------------------------------------------------------
// Start/Stop
// ------------------------------------------------------------------

struct MHD_Daemon *http_server_start(int port, app_context_t *ctx) {
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    return MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        port,
        NULL, NULL,
        &request_handler, ctx,
        MHD_OPTION_SOCK_ADDR, &addr,
        MHD_OPTION_END
    );
}
```

### `config_parser.h` — Strutture Dati

```c
#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#define MAX_FORWARDS 16
#define MAX_OPTIONS  32

typedef struct {
    char bind_addr[64];
    int  bind_port;
    char remote_host[256];
    int  remote_port;
} forward_rule_t;

typedef struct {
    char bind_addr[64];
    int  bind_port;
} dynamic_rule_t;

typedef struct {
    char key[64];
    char value[256];
} ssh_option_t;

typedef struct {
    char             name[128];           // Host alias
    char             hostname[256];       // HostName reale
    char             user[64];            // User
    int              port;                // Port (default 22)
    char             identity_file[512];  // IdentityFile
    char             proxy_jump[128];     // ProxyJump

    forward_rule_t   local_forward[MAX_FORWARDS];
    int              num_local_forward;

    forward_rule_t   remote_forward[MAX_FORWARDS];
    int              num_remote_forward;

    dynamic_rule_t   dynamic_forward[MAX_FORWARDS];
    int              num_dynamic_forward;

    ssh_option_t     options[MAX_OPTIONS];
    int              num_options;
} ssh_host_t;

// Parsa ~/.ssh/config (path=NULL → default). Alloca array, ritorna count.
ssh_host_t *parse_ssh_config(const char *path, int *out_count);

// Libera array allocato da parse_ssh_config
void ssh_hosts_free(ssh_host_t *hosts, int count);

#endif
```

### `config_parser.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <unistd.h>
#include "config_parser.h"

static void expand_tilde(char *dest, const char *src, size_t maxlen) {
    if (src[0] == '~' && src[1] == '/') {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(dest, maxlen, "%s%s", home, src + 1);
    } else {
        strncpy(dest, src, maxlen);
    }
}

static int parse_forward(const char *val, forward_rule_t *rule) {
    // Formati supportati:
    //   port:host:hostport
    //   bind_address:port:host:hostport
    char buf[512];
    strncpy(buf, val, sizeof(buf));

    // Conta i ':'
    int colons = 0;
    for (char *p = buf; *p; p++) if (*p == ':') colons++;

    if (colons == 2) {
        // port:host:hostport
        strcpy(rule->bind_addr, "localhost");
        sscanf(buf, "%d:%255[^:]:%d", &rule->bind_port, rule->remote_host, &rule->remote_port);
    } else if (colons == 3) {
        // bind:port:host:hostport
        sscanf(buf, "%63[^:]:%d:%255[^:]:%d",
               rule->bind_addr, &rule->bind_port, rule->remote_host, &rule->remote_port);
    } else {
        return -1;
    }
    return 0;
}

static int parse_dynamic(const char *val, dynamic_rule_t *rule) {
    // Formati: port  oppure  bind_address:port
    if (strchr(val, ':')) {
        sscanf(val, "%63[^:]:%d", rule->bind_addr, &rule->bind_port);
    } else {
        strcpy(rule->bind_addr, "localhost");
        rule->bind_port = atoi(val);
    }
    return 0;
}

static char *trim(char *s) {
    while (isspace(*s)) s++;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace(*end)) *end-- = '\0';
    return s;
}

ssh_host_t *parse_ssh_config(const char *path, int *out_count) {
    char config_path[512];
    if (path) {
        strncpy(config_path, path, sizeof(config_path));
    } else {
        expand_tilde(config_path, "~/.ssh/config", sizeof(config_path));
    }

    FILE *f = fopen(config_path, "r");
    if (!f) { *out_count = 0; return NULL; }

    ssh_host_t *hosts = NULL;
    int count = 0;
    int capacity = 0;
    ssh_host_t *current = NULL;
    ssh_host_t defaults = { .port = 22 };

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;

        // Split "Key Value"
        char *space = trimmed;
        while (*space && !isspace(*space)) space++;
        if (!*space) continue;
        *space = '\0';
        char *key = trimmed;
        char *val = trim(space + 1);

        if (strcasecmp(key, "Host") == 0) {
            // Salta pattern con wildcard
            if (strchr(val, '*') || strchr(val, '?')) {
                current = &defaults;
                continue;
            }

            // Nuovo host
            if (count >= capacity) {
                capacity = capacity ? capacity * 2 : 16;
                hosts = realloc(hosts, capacity * sizeof(ssh_host_t));
            }
            current = &hosts[count++];
            memset(current, 0, sizeof(ssh_host_t));
            current->port = defaults.port;
            if (defaults.user[0]) strcpy(current->user, defaults.user);
            if (defaults.identity_file[0]) strcpy(current->identity_file, defaults.identity_file);
            strncpy(current->name, val, sizeof(current->name));

        } else if (current && current != &defaults) {

            if (strcasecmp(key, "HostName") == 0)
                strncpy(current->hostname, val, sizeof(current->hostname));
            else if (strcasecmp(key, "User") == 0)
                strncpy(current->user, val, sizeof(current->user));
            else if (strcasecmp(key, "Port") == 0)
                current->port = atoi(val);
            else if (strcasecmp(key, "IdentityFile") == 0)
                expand_tilde(current->identity_file, val, sizeof(current->identity_file));
            else if (strcasecmp(key, "ProxyJump") == 0)
                strncpy(current->proxy_jump, val, sizeof(current->proxy_jump));
            else if (strcasecmp(key, "LocalForward") == 0 &&
                     current->num_local_forward < MAX_FORWARDS)
                parse_forward(val, &current->local_forward[current->num_local_forward++]);
            else if (strcasecmp(key, "RemoteForward") == 0 &&
                     current->num_remote_forward < MAX_FORWARDS)
                parse_forward(val, &current->remote_forward[current->num_remote_forward++]);
            else if (strcasecmp(key, "DynamicForward") == 0 &&
                     current->num_dynamic_forward < MAX_FORWARDS)
                parse_dynamic(val, &current->dynamic_forward[current->num_dynamic_forward++]);
            else if (current->num_options < MAX_OPTIONS) {
                strncpy(current->options[current->num_options].key, key, 63);
                strncpy(current->options[current->num_options].value, val, 255);
                current->num_options++;
            }

        } else if (current == &defaults) {
            // Applica ai defaults
            if (strcasecmp(key, "User") == 0) strncpy(defaults.user, val, sizeof(defaults.user));
            else if (strcasecmp(key, "Port") == 0) defaults.port = atoi(val);
            else if (strcasecmp(key, "IdentityFile") == 0)
                expand_tilde(defaults.identity_file, val, sizeof(defaults.identity_file));
        }
    }

    fclose(f);
    *out_count = count;
    return hosts;
}

void ssh_hosts_free(ssh_host_t *hosts, int count) {
    free(hosts);
}
```

### `sse.c` — SSE Broadcaster

```c
#include <microhttpd.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "sse.h"

#define MAX_SSE_CLIENTS 32
#define SSE_BUF_SIZE    4096

typedef struct {
    int         pipe_fd[2];    // pipe: [0]=read (MHD legge), [1]=write (noi scriviamo)
    int         active;
} sse_client_t;

struct sse_broadcaster {
    pthread_mutex_t  mutex;
    sse_client_t     clients[MAX_SSE_CLIENTS];
    int              num_clients;
};

sse_broadcaster_t *sse_broadcaster_create(void) {
    sse_broadcaster_t *b = calloc(1, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    return b;
}

// MHD content reader callback: legge dalla pipe
static ssize_t sse_reader(void *cls, uint64_t pos, char *buf, size_t max) {
    sse_client_t *client = (sse_client_t *)cls;
    if (!client->active) return MHD_CONTENT_READER_END_OF_STREAM;

    ssize_t n = read(client->pipe_fd[0], buf, max);
    if (n <= 0) {
        // Attendi un po' per evitare busy loop, poi riprova
        usleep(50000); // 50ms
        return 0;      // MHD richiamerà di nuovo
    }
    return n;
}

static void sse_free_callback(void *cls) {
    sse_client_t *client = (sse_client_t *)cls;
    client->active = 0;
    close(client->pipe_fd[0]);
    close(client->pipe_fd[1]);
}

enum MHD_Result sse_broadcaster_add_client(sse_broadcaster_t *b,
                                            struct MHD_Connection *conn) {
    pthread_mutex_lock(&b->mutex);

    if (b->num_clients >= MAX_SSE_CLIENTS) {
        pthread_mutex_unlock(&b->mutex);
        return MHD_NO;
    }

    sse_client_t *client = &b->clients[b->num_clients++];
    pipe(client->pipe_fd);
    client->active = 1;

    pthread_mutex_unlock(&b->mutex);

    struct MHD_Response *resp = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN, SSE_BUF_SIZE, &sse_reader, client, &sse_free_callback);

    MHD_add_response_header(resp, "Content-Type", "text/event-stream");
    MHD_add_response_header(resp, "Cache-Control", "no-cache");
    MHD_add_response_header(resp, "Connection", "keep-alive");
    MHD_add_response_header(resp, "X-Accel-Buffering", "no");

    enum MHD_Result ret = MHD_queue_response(conn, 200, resp);
    MHD_destroy_response(resp);
    return ret;
}

void sse_broadcast(sse_broadcaster_t *b, const char *event, const char *json_data) {
    char msg[SSE_BUF_SIZE];
    int len = snprintf(msg, sizeof(msg), "event: %s\ndata: %s\n\n", event, json_data);

    pthread_mutex_lock(&b->mutex);
    for (int i = 0; i < b->num_clients; i++) {
        if (b->clients[i].active) {
            write(b->clients[i].pipe_fd[1], msg, len);
        }
    }
    pthread_mutex_unlock(&b->mutex);
}

void sse_broadcaster_free(sse_broadcaster_t *b) {
    for (int i = 0; i < b->num_clients; i++) {
        if (b->clients[i].active) {
            b->clients[i].active = 0;
            close(b->clients[i].pipe_fd[0]);
            close(b->clients[i].pipe_fd[1]);
        }
    }
    pthread_mutex_destroy(&b->mutex);
    free(b);
}
```

### `process_manager.c` — Tunnel e Terminali

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <json-c/json.h>
#include "process_manager.h"
#include "sse.h"
#include "config_parser.h"

#define MAX_TUNNELS 64

typedef struct {
    char    host[128];
    pid_t   pid;
    int     active;
} tunnel_entry_t;

struct process_manager {
    sse_broadcaster_t *sse;
    pthread_mutex_t    mutex;
    tunnel_entry_t     tunnels[MAX_TUNNELS];
    int                num_tunnels;
};

process_manager_t *process_manager_create(sse_broadcaster_t *sse) {
    process_manager_t *pm = calloc(1, sizeof(*pm));
    pm->sse = sse;
    pthread_mutex_init(&pm->mutex, NULL);
    return pm;
}

// Validazione anti-injection: solo alfanumerici, trattini, underscore, punti
static int is_valid_host_alias(const char *name) {
    for (const char *p = name; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.') return 0;
    }
    return name[0] != '\0';
}

// Thread di monitoraggio processo tunnel
static void *tunnel_monitor_thread(void *arg) {
    tunnel_entry_t *entry = (tunnel_entry_t *)arg;
    process_manager_t *pm = (process_manager_t *)((char *)arg - offsetof(tunnel_entry_t, host)
                            + /* ... trick per accedere al pm — meglio passare una struct dedicata */);

    // Versione semplificata: usa una struct wrapper
    // In pratica passare {pm, entry} come argomento

    int status;
    waitpid(entry->pid, &status, 0);
    entry->active = 0;

    // Notifica via SSE
    char json[256];
    snprintf(json, sizeof(json),
        "{\"host\":\"%s\",\"status\":\"inactive\",\"exitCode\":%d}",
        entry->host, WEXITSTATUS(status));
    // sse_broadcast(pm->sse, "tunnel_status", json);

    return NULL;
}

int process_manager_start_tunnel(process_manager_t *pm, const ssh_host_t *host) {
    if (!is_valid_host_alias(host->name)) return -1;

    // Costruisci argv
    char *argv[128];
    int argc = 0;

    argv[argc++] = "ssh";
    argv[argc++] = "-N";                            // no shell
    argv[argc++] = "-o"; argv[argc++] = "ExitOnForwardFailure=yes";
    argv[argc++] = "-o"; argv[argc++] = "ServerAliveInterval=30";
    argv[argc++] = "-o"; argv[argc++] = "ServerAliveCountMax=3";

    // Buffer statico per le stringhe -L/-R/-D (devono sopravvivere fino a execvp)
    static char fwd_specs[MAX_FORWARDS * 3][256];
    int spec_idx = 0;

    for (int i = 0; i < host->num_local_forward; i++) {
        argv[argc++] = "-L";
        snprintf(fwd_specs[spec_idx], 256, "%s:%d:%s:%d",
            host->local_forward[i].bind_addr, host->local_forward[i].bind_port,
            host->local_forward[i].remote_host, host->local_forward[i].remote_port);
        argv[argc++] = fwd_specs[spec_idx++];
    }
    for (int i = 0; i < host->num_remote_forward; i++) {
        argv[argc++] = "-R";
        snprintf(fwd_specs[spec_idx], 256, "%s:%d:%s:%d",
            host->remote_forward[i].bind_addr, host->remote_forward[i].bind_port,
            host->remote_forward[i].remote_host, host->remote_forward[i].remote_port);
        argv[argc++] = fwd_specs[spec_idx++];
    }
    for (int i = 0; i < host->num_dynamic_forward; i++) {
        argv[argc++] = "-D";
        snprintf(fwd_specs[spec_idx], 256, "%s:%d",
            host->dynamic_forward[i].bind_addr, host->dynamic_forward[i].bind_port);
        argv[argc++] = fwd_specs[spec_idx++];
    }

    argv[argc++] = (char *)host->name;
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Figlio: setup SSH_ASKPASS se necessario
        // setenv("SSH_ASKPASS", askpass_path, 1);
        // setenv("SSH_ASKPASS_REQUIRE", "force", 1);
        // setenv("DISPLAY", ":0", 1);

        execvp("ssh", argv);
        _exit(127);
    }

    // Padre: registra il tunnel
    pthread_mutex_lock(&pm->mutex);
    tunnel_entry_t *entry = &pm->tunnels[pm->num_tunnels++];
    strncpy(entry->host, host->name, sizeof(entry->host));
    entry->pid = pid;
    entry->active = 1;
    pthread_mutex_unlock(&pm->mutex);

    // Notifica SSE
    char json[256];
    snprintf(json, sizeof(json),
        "{\"host\":\"%s\",\"status\":\"starting\",\"pid\":%d}", host->name, pid);
    sse_broadcast(pm->sse, "tunnel_status", json);

    // Monitora in background
    pthread_t tid;
    pthread_create(&tid, NULL, tunnel_monitor_thread, entry);
    pthread_detach(tid);

    return pid;
}

int process_manager_stop_tunnel(process_manager_t *pm, const char *host_name) {
    pthread_mutex_lock(&pm->mutex);
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (pm->tunnels[i].active && strcmp(pm->tunnels[i].host, host_name) == 0) {
            kill(pm->tunnels[i].pid, SIGTERM);
            pthread_mutex_unlock(&pm->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&pm->mutex);
    return -1;
}

const char *process_manager_get_tunnel_status(process_manager_t *pm, const char *host_name) {
    pthread_mutex_lock(&pm->mutex);
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (strcmp(pm->tunnels[i].host, host_name) == 0) {
            const char *s = pm->tunnels[i].active ? "active" : "inactive";
            pthread_mutex_unlock(&pm->mutex);
            return s;
        }
    }
    pthread_mutex_unlock(&pm->mutex);
    return "inactive";
}

void process_manager_kill_all(process_manager_t *pm) {
    pthread_mutex_lock(&pm->mutex);
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (pm->tunnels[i].active) {
            kill(pm->tunnels[i].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&pm->mutex);
    // Breve attesa, poi SIGKILL per i sopravvissuti
    usleep(500000);
    pthread_mutex_lock(&pm->mutex);
    for (int i = 0; i < pm->num_tunnels; i++) {
        if (pm->tunnels[i].active) {
            kill(pm->tunnels[i].pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&pm->mutex);
}

void process_manager_free(process_manager_t *pm) {
    pthread_mutex_destroy(&pm->mutex);
    free(pm);
}
```

### `terminal_launch.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "terminal_launch.h"

typedef struct {
    const char *bin;
    const char **argv_template;  // NULL-terminated, "$CMD" è il placeholder
} terminal_def_t;

// Detect e lancio per Linux
pid_t launch_terminal_with_ssh(const char *host_alias) {
    if (!host_alias || !host_alias[0]) return -1;

    char ssh_cmd[256];
    snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh %s", host_alias);

    // Ordine di preferenza
    typedef struct { const char *check; const char *fmt; } term_t;
    term_t terminals[] = {
        { "gnome-terminal",    "gnome-terminal -- sh -c '%s; exec bash'" },
        { "konsole",           "konsole -e sh -c '%s; exec bash'" },
        { "xfce4-terminal",    "xfce4-terminal -e 'sh -c \"%s; exec bash\"'" },
        { "alacritty",         "alacritty -e sh -c '%s; exec bash'" },
        { "kitty",             "kitty sh -c '%s; exec bash'" },
        { "foot",              "foot sh -c '%s; exec bash'" },
        { "x-terminal-emulator", "x-terminal-emulator -e 'sh -c \"%s\"'" },
        { "xterm",             "xterm -e 'sh -c \"%s\"'" },
        { NULL, NULL },
    };

    for (term_t *t = terminals; t->check; t++) {
        char which[128];
        snprintf(which, sizeof(which), "which %s >/dev/null 2>&1", t->check);
        if (system(which) != 0) continue;

        pid_t pid = fork();
        if (pid == 0) {
            // Figlio
            setsid();  // Nuova sessione, il terminale diventa indipendente
            char full[1024];
            snprintf(full, sizeof(full), t->fmt, ssh_cmd);
            execl("/bin/sh", "sh", "-c", full, NULL);
            _exit(127);
        }
        return pid;
    }

    fprintf(stderr, "Nessun emulatore di terminale trovato\n");
    return -1;
}
```

---

## Frontend — `ui/`

Il frontend è **HTML/CSS/JS puro**, senza framework né bundler. Viene servito come file statici dal server MHD.

### `ui/index.html`

```html
<!DOCTYPE html>
<html lang="it">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>SSHPad</title>
  <link rel="stylesheet" href="/style.css" />
</head>
<body>
  <header>
    <h1>⌁ SSHPad</h1>
    <span id="status-indicator" class="status-dot inactive"></span>
  </header>

  <main id="connections"></main>

  <!-- Password dialog -->
  <dialog id="password-dialog">
    <h3>🔑 Autenticazione SSH</h3>
    <p id="password-prompt"></p>
    <form method="dialog" id="password-form">
      <input type="password" id="password-input" placeholder="Password" autofocus />
      <div class="dialog-actions">
        <button type="button" id="password-cancel">Annulla</button>
        <button type="submit" class="primary">Invia</button>
      </div>
    </form>
  </dialog>

  <script src="/app.js"></script>
</body>
</html>
```

### `ui/style.css`

```css
:root {
  --bg: #0f1117;
  --surface: #181a24;
  --surface-hover: #1e2130;
  --border: #2a2d3e;
  --text: #c9cdd8;
  --text-secondary: #6c7186;
  --accent: #5e8bff;
  --green: #34d399;
  --red: #f87171;
  --amber: #fbbf24;
  --mono: "SF Mono", "Cascadia Code", "JetBrains Mono", "Fira Code", monospace;
}

* { margin: 0; padding: 0; box-sizing: border-box; }

body {
  font-family: var(--mono);
  background: var(--bg);
  color: var(--text);
  padding: 1.5rem;
}

header {
  display: flex;
  align-items: center;
  gap: 0.75rem;
  margin-bottom: 1.5rem;
  border-bottom: 1px solid var(--border);
  padding-bottom: 1rem;
}

header h1 { font-size: 1.3rem; font-weight: 600; letter-spacing: -0.02em; }

/* Status dot */
.status-dot {
  width: 8px; height: 8px;
  border-radius: 50%;
  display: inline-block;
  flex-shrink: 0;
}
.status-dot.active   { background: var(--green); box-shadow: 0 0 6px var(--green); }
.status-dot.inactive { background: var(--text-secondary); }
.status-dot.starting { background: var(--amber); animation: pulse 1.2s ease infinite; }
.status-dot.error    { background: var(--red); }
@keyframes pulse { 0%,100% { opacity: 1; } 50% { opacity: 0.3; } }

/* Connection grid */
#connections {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(360px, 1fr));
  gap: 1rem;
}

/* Card */
.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 1rem 1.2rem;
  transition: border-color 0.2s;
}
.card:hover { border-color: var(--accent); }

.card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 0.6rem;
}

.card-name {
  font-weight: 700;
  font-size: 1rem;
  display: flex;
  align-items: center;
  gap: 0.5rem;
}

.card-meta {
  font-size: 0.8rem;
  color: var(--text-secondary);
  margin-bottom: 0.75rem;
  line-height: 1.5;
}

.card-actions { display: flex; gap: 0.4rem; }

.card-actions button {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--text);
  border-radius: 6px;
  padding: 0.35rem 0.65rem;
  cursor: pointer;
  font-family: var(--mono);
  font-size: 0.85rem;
  transition: all 0.15s;
}
.card-actions button:hover { background: var(--surface-hover); border-color: var(--accent); }
.card-actions .btn-terminal { color: var(--green); }
.card-actions .btn-tunnel   { color: var(--amber); }
.card-actions .btn-stop     { color: var(--red); }

/* Tunnel list */
.tunnels {
  border-top: 1px solid var(--border);
  margin-top: 0.75rem;
  padding-top: 0.6rem;
}

.tunnel-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 0.78rem;
  padding: 0.2rem 0;
  color: var(--text-secondary);
}
.tunnel-type {
  display: inline-block;
  width: 1rem;
  font-weight: 700;
  color: var(--accent);
}

/* Dialog */
dialog {
  border: none;
  border-radius: 12px;
  padding: 1.5rem;
  max-width: 380px;
  width: 90%;
  background: var(--surface);
  color: var(--text);
  box-shadow: 0 20px 60px rgba(0,0,0,0.5);
}
dialog::backdrop { background: rgba(0,0,0,0.6); backdrop-filter: blur(4px); }
dialog h3 { margin-bottom: 0.5rem; }
dialog p { font-size: 0.85rem; color: var(--text-secondary); margin-bottom: 1rem; }
dialog input {
  width: 100%; padding: 0.6rem; font-size: 0.95rem;
  background: var(--bg); color: var(--text);
  border: 1px solid var(--border); border-radius: 6px;
  font-family: var(--mono);
  margin-bottom: 1rem;
}
dialog input:focus { outline: none; border-color: var(--accent); }
.dialog-actions { display: flex; justify-content: flex-end; gap: 0.5rem; }
.dialog-actions button {
  padding: 0.45rem 1rem; border-radius: 6px; border: none;
  cursor: pointer; font-family: var(--mono); font-size: 0.85rem;
}
.dialog-actions .primary { background: var(--accent); color: #fff; }
.dialog-actions button:not(.primary) { background: var(--border); color: var(--text); }
```

### `ui/app.js`

```js
// ─── State ───
let hosts = [];
let tunnelStates = {};  // host → "active" | "inactive" | "starting" | "error"
let pendingPasswordRequest = null;

// ─── API ───
async function fetchHosts() {
  const res = await fetch('/api/hosts');
  hosts = await res.json();
  // Inizializza stato tunnel da risposta
  hosts.forEach(h => {
    if (!tunnelStates[h.name]) tunnelStates[h.name] = h.tunnelStatus || 'inactive';
  });
  render();
}

async function startTunnel(hostName) {
  tunnelStates[hostName] = 'starting';
  render();
  await fetch('/api/tunnel/start', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ host: hostName }),
  });
}

async function stopTunnel(hostName) {
  await fetch('/api/tunnel/stop', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ host: hostName }),
  });
}

async function openTerminal(hostName) {
  await fetch('/api/terminal/open', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ host: hostName }),
  });
}

async function sendPassword(requestId, password) {
  await fetch('/api/password', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ requestId, password }),
  });
}

// ─── SSE ───
function connectSSE() {
  const es = new EventSource('/api/events');
  const indicator = document.getElementById('status-indicator');

  es.onopen = () => { indicator.className = 'status-dot active'; };
  es.onerror = () => { indicator.className = 'status-dot error'; };

  es.addEventListener('tunnel_status', (e) => {
    const data = JSON.parse(e.data);
    tunnelStates[data.host] = data.status;
    render();
  });

  es.addEventListener('password_request', (e) => {
    const data = JSON.parse(e.data);
    showPasswordDialog(data.requestId, data.host, data.prompt);
  });

  es.addEventListener('config_changed', () => {
    fetchHosts();  // Ricarica config
  });
}

// ─── Password Dialog ───
const dialog = document.getElementById('password-dialog');
const form = document.getElementById('password-form');
const promptEl = document.getElementById('password-prompt');
const input = document.getElementById('password-input');
const cancelBtn = document.getElementById('password-cancel');

function showPasswordDialog(requestId, host, prompt) {
  pendingPasswordRequest = requestId;
  promptEl.textContent = prompt || `Password per ${host}:`;
  input.value = '';
  dialog.showModal();
  input.focus();
}

form.addEventListener('submit', (e) => {
  e.preventDefault();
  if (pendingPasswordRequest) {
    sendPassword(pendingPasswordRequest, input.value);
    pendingPasswordRequest = null;
  }
  dialog.close();
});

cancelBtn.addEventListener('click', () => {
  pendingPasswordRequest = null;
  dialog.close();
});

// ─── Render ───
function render() {
  const container = document.getElementById('connections');

  container.innerHTML = hosts.map(h => {
    const status = tunnelStates[h.name] || 'inactive';
    const hasTunnels = h.localForward.length + h.remoteForward.length + h.dynamicForward.length > 0;

    const forwards = [
      ...h.localForward.map(f => ({ type: 'L', label: `${f.bindAddr}:${f.bindPort} → ${f.remoteHost}:${f.remotePort}` })),
      ...h.remoteForward.map(f => ({ type: 'R', label: `${f.bindAddr}:${f.bindPort} → ${f.remoteHost}:${f.remotePort}` })),
      ...h.dynamicForward.map(f => ({ type: 'D', label: `${f.bindAddr}:${f.bindPort} (SOCKS)` })),
    ];

    const tunnelBtnLabel = status === 'active' ? '■ Stop' : '⚡ Tunnel';
    const tunnelBtnClass = status === 'active' ? 'btn-stop' : 'btn-tunnel';

    return `
      <div class="card">
        <div class="card-header">
          <span class="card-name">
            <span class="status-dot ${status}"></span>
            ${esc(h.name)}
          </span>
          <div class="card-actions">
            <button class="btn-terminal" onclick="openTerminal('${esc(h.name)}')" title="Apri terminale">
              &gt;_
            </button>
            ${hasTunnels ? `
              <button class="${tunnelBtnClass}"
                      onclick="${status === 'active' ? `stopTunnel('${esc(h.name)}')` : `startTunnel('${esc(h.name)}')`}"
                      title="${tunnelBtnLabel}">
                ${tunnelBtnLabel}
              </button>
            ` : ''}
          </div>
        </div>
        <div class="card-meta">
          ${esc(h.user || '')}${h.user ? '@' : ''}${esc(h.hostname)}:${h.port || 22}
          ${h.identityFile ? `<br/>Key: ${esc(h.identityFile)}` : ''}
          ${h.proxyJump ? `<br/>Via: ${esc(h.proxyJump)}` : ''}
        </div>
        ${forwards.length > 0 ? `
          <div class="tunnels">
            ${forwards.map(f => `
              <div class="tunnel-row">
                <span><span class="tunnel-type">${f.type}</span> ${esc(f.label)}</span>
                <span class="status-dot ${status}"></span>
              </div>
            `).join('')}
          </div>
        ` : ''}
      </div>
    `;
  }).join('');
}

// HTML escape
function esc(s) {
  if (!s) return '';
  const el = document.createElement('span');
  el.textContent = s;
  return el.innerHTML;
}

// ─── Init ───
fetchHosts();
connectSSE();
```

---

## Build

### Makefile

```makefile
CC      = gcc
PKG     = gtk4 webkitgtk-6.0 libmicrohttpd json-c
CFLAGS  = -Wall -O2 $(shell pkg-config --cflags $(PKG))
LDFLAGS = $(shell pkg-config --libs $(PKG)) -lpthread

SRC     = src/main.c \
          src/http_server.c \
          src/config_parser.c \
          src/process_manager.c \
          src/sse.c \
          src/terminal_launch.c \
          src/port_finder.c \
          src/util.c

TARGET  = sshpad

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -Dm644 ui/index.html $(DESTDIR)/usr/local/share/sshpad/ui/index.html
	install -Dm644 ui/style.css  $(DESTDIR)/usr/local/share/sshpad/ui/style.css
	install -Dm644 ui/app.js     $(DESTDIR)/usr/local/share/sshpad/ui/app.js

.PHONY: clean install
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(sshpad C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(WEBKIT REQUIRED webkitgtk-6.0)
pkg_check_modules(MHD REQUIRED libmicrohttpd)
pkg_check_modules(JSONC REQUIRED json-c)

add_executable(sshpad
    src/main.c
    src/http_server.c
    src/config_parser.c
    src/process_manager.c
    src/sse.c
    src/terminal_launch.c
    src/port_finder.c
    src/util.c
)

target_include_directories(sshpad PRIVATE
    ${GTK4_INCLUDE_DIRS}
    ${WEBKIT_INCLUDE_DIRS}
    ${MHD_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
)

target_link_libraries(sshpad
    ${GTK4_LIBRARIES}
    ${WEBKIT_LIBRARIES}
    ${MHD_LIBRARIES}
    ${JSONC_LIBRARIES}
    pthread
)

install(TARGETS sshpad DESTINATION bin)
install(DIRECTORY ui/ DESTINATION share/sshpad/ui)
```

### Compilazione

```bash
# Con Make
make

# Con CMake
mkdir build && cd build
cmake .. && make

# Run
./sshpad
```

---

## Path dei File UI a Runtime

Il server HTTP deve trovare la directory `ui/`. Ordine di ricerca:

1. `./ui/` (directory corrente — per sviluppo)
2. `<exe_dir>/ui/` (accanto all'eseguibile)
3. `/usr/local/share/sshpad/ui/` (installazione di sistema)
4. `/usr/share/sshpad/ui/` (package manager)

```c
static const char *find_ui_dir(void) {
    static char path[512];
    const char *candidates[] = {
        "./ui",
        NULL,  // placeholder per exe_dir/ui
        "/usr/local/share/sshpad/ui",
        "/usr/share/sshpad/ui",
    };
    // Risolvi exe_dir
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) { *slash = '\0'; snprintf(path, sizeof(path), "%s/ui", exe); }
        candidates[1] = path;
    }
    for (int i = 0; i < 4; i++) {
        if (candidates[i] && access(candidates[i], R_OK) == 0) return candidates[i];
    }
    return "./ui";  // fallback
}
```

Alternativa: **embeddare i file HTML/CSS/JS direttamente nel binario** come stringhe C generate a build time:

```makefile
# Genera embed.h da ui/*
src/embed.h: ui/index.html ui/style.css ui/app.js
	xxd -i ui/index.html > $@
	xxd -i ui/style.css >> $@
	xxd -i ui/app.js >> $@
```

---

## Sicurezza

| Aspetto                 | Implementazione                                                            |
|-------------------------|----------------------------------------------------------------------------|
| **Bind solo localhost** | MHD bound a `127.0.0.1` — nessun accesso da rete                          |
| **Porta random**        | Nessun porta fissa indovinabile; scelta dall'OS                            |
| **Command injection**   | Host alias validato con whitelist alfanumerica prima di passarlo a `exec`  |
| **Password in memoria** | Buffer azzerato con `explicit_bzero()` dopo l'uso                          |
| **Processi figli**      | Tutti terminati con SIGTERM+SIGKILL alla chiusura dell'applicazione        |
| **No logging password** | Le password non vengono mai scritte su stdout/stderr/file                  |

---

## Roadmap

- **Embed risorse**: compilare HTML/CSS/JS nel binario (nessun file esterno)
- **Ricerca/filtro**: search bar per filtrare host per nome
- **Gruppi/tag**: organizzare connessioni con tag custom (commento `# tag:prod`)
- **Editing config**: modificare `~/.ssh/config` direttamente dalla UI
- **Tray icon**: icona nella system tray con stato tunnel (libappindicator)
- **File watcher**: `inotify` su `~/.ssh/config` per refresh automatico
- **macOS/Windows**: porting con WebKitGTK alternativo (WebView2 su Windows, WKWebView su macOS)
- **Notifiche**: notifica desktop quando un tunnel cade (libnotify)
- **Log viewer**: pannello per stdout/stderr dei processi tunnel

---

## Riferimenti

- [GTK 4](https://docs.gtk.org/gtk4/) — Documentazione ufficiale
- [WebKitGTK](https://webkitgtk.org/reference/webkit2gtk/stable/) — API WebKitGTK
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) — HTTP server C embedded
- [json-c](https://github.com/json-c/json-c) — Libreria JSON per C
- [OpenSSH ssh_config](https://man.openbsd.org/ssh_config.5) — Formato file config
- [SSH_ASKPASS](https://man.openbsd.org/ssh.1#SSH_ASKPASS) — Meccanismo askpass
