#include <microhttpd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <json-c/json.h>
#include <arpa/inet.h>

#include "http_server.h"
#include "app_context.h"
#include "sse.h"
#include "config_parser.h"
#include "process_manager.h"
#include "terminal_launch.h"
#include "askpass.h"

/* ------------------------------------------------------------------ */
/* Struttura per accumulare il body delle richieste POST               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t size;
} post_data_t;

/* ------------------------------------------------------------------ */
/* Helper: invia una risposta JSON con header CORS                     */
/* ------------------------------------------------------------------ */

static enum MHD_Result
json_response(struct MHD_Connection *conn, int status_code, const char *json_str)
{
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(json_str), (void *)json_str, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Helper: content-type per file statici                               */
/* ------------------------------------------------------------------ */

static const char *get_content_type(const char *path)
{
    if (strstr(path, ".html")) return "text/html; charset=utf-8";
    if (strstr(path, ".css"))  return "text/css; charset=utf-8";
    if (strstr(path, ".js"))   return "application/javascript; charset=utf-8";
    if (strstr(path, ".svg"))  return "image/svg+xml";
    if (strstr(path, ".png"))  return "image/png";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* Helper: cerca la directory ui/ in più posizioni                     */
/* ------------------------------------------------------------------ */

static const char *find_ui_dir(void)
{
    static char exe_ui_path[512];
    static int  resolved = 0;

    if (!resolved) {
        resolved = 1;
        char exe[508];  /* 508 + len("/ui") + NUL fits in exe_ui_path[512] */
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                snprintf(exe_ui_path, sizeof(exe_ui_path), "%s/ui", exe);
            }
        }
    }

    const char *candidates[] = {
        "./ui",
        exe_ui_path[0] ? exe_ui_path : "",
        "/usr/local/share/sshpad/ui",
        "/usr/share/sshpad/ui",
    };

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (candidates[i][0] != '\0' && access(candidates[i], R_OK) == 0)
            return candidates[i];
    }
    return "./ui"; /* fallback */
}

/* ------------------------------------------------------------------ */
/* serve_static: serve file dalla directory ui/                        */
/* ------------------------------------------------------------------ */

static enum MHD_Result
serve_static(struct MHD_Connection *conn, const char *url)
{
    const char *ui_dir = find_ui_dir();
    char filepath[1024];

    if (strcmp(url, "/") == 0)
        snprintf(filepath, sizeof(filepath), "%s/index.html", ui_dir);
    else
        snprintf(filepath, sizeof(filepath), "%s%s", ui_dir, url);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *not_found = "404 Not Found";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(not_found), (void *)not_found, MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(resp, "Content-Type", "text/plain");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = malloc((size_t)fsize);
    if (!content) {
        fclose(f);
        const char *err = "500 Internal Server Error";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    if (fread(content, 1, (size_t)fsize, f) != (size_t)fsize) {
        fclose(f);
        free(content);
        const char *err = "500 Read Error";
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(err), (void *)err, MHD_RESPMEM_PERSISTENT);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, resp);
        MHD_destroy_response(resp);
        return ret;
    }
    fclose(f);

    struct MHD_Response *resp = MHD_create_response_from_buffer(
        (size_t)fsize, content, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", get_content_type(filepath));
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* GET /api/hosts — Lista host SSH con stato tunnel                    */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_get_hosts(struct MHD_Connection *conn, app_context_t *ctx)
{
    json_object *jarr = json_object_new_array();

    for (int i = 0; i < ctx->num_hosts; i++) {
        ssh_host_t  *h  = &ctx->hosts[i];
        json_object *jh = json_object_new_object();

        json_object_object_add(jh, "name",
            json_object_new_string(h->name));
        json_object_object_add(jh, "hostname",
            json_object_new_string(h->hostname[0] ? h->hostname : ""));
        json_object_object_add(jh, "user",
            json_object_new_string(h->user[0] ? h->user : ""));
        json_object_object_add(jh, "port",
            json_object_new_int(h->port ? h->port : 22));
        json_object_object_add(jh, "identityFile",
            json_object_new_string(h->identity_file[0] ? h->identity_file : ""));
        json_object_object_add(jh, "proxyJump",
            json_object_new_string(h->proxy_jump[0] ? h->proxy_jump : ""));

        /* Local forwards */
        json_object *jlf = json_object_new_array();
        for (int j = 0; j < h->num_local_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr",
                json_object_new_string(h->local_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort",
                json_object_new_int(h->local_forward[j].bind_port));
            json_object_object_add(jf, "remoteHost",
                json_object_new_string(h->local_forward[j].remote_host));
            json_object_object_add(jf, "remotePort",
                json_object_new_int(h->local_forward[j].remote_port));
            json_object_array_add(jlf, jf);
        }
        json_object_object_add(jh, "localForward", jlf);

        /* Remote forwards */
        json_object *jrf = json_object_new_array();
        for (int j = 0; j < h->num_remote_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr",
                json_object_new_string(h->remote_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort",
                json_object_new_int(h->remote_forward[j].bind_port));
            json_object_object_add(jf, "remoteHost",
                json_object_new_string(h->remote_forward[j].remote_host));
            json_object_object_add(jf, "remotePort",
                json_object_new_int(h->remote_forward[j].remote_port));
            json_object_array_add(jrf, jf);
        }
        json_object_object_add(jh, "remoteForward", jrf);

        /* Dynamic forwards */
        json_object *jdf = json_object_new_array();
        for (int j = 0; j < h->num_dynamic_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr",
                json_object_new_string(h->dynamic_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort",
                json_object_new_int(h->dynamic_forward[j].bind_port));
            json_object_array_add(jdf, jf);
        }
        json_object_object_add(jh, "dynamicForward", jdf);

        /* Stato tunnel dal process manager */
        const char *tunnel_status =
            process_manager_get_tunnel_status(ctx->pm, h->name);
        json_object_object_add(jh, "tunnelStatus",
            json_object_new_string(tunnel_status ? tunnel_status : "inactive"));

        json_object_array_add(jarr, jh);
    }

    const char *json_str = json_object_to_json_string(jarr);
    enum MHD_Result ret  = json_response(conn, MHD_HTTP_OK, json_str);
    json_object_put(jarr);
    return ret;
}

/* ------------------------------------------------------------------ */
/* GET /api/events — SSE stream                                        */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_sse(struct MHD_Connection *conn, app_context_t *ctx)
{
    return sse_broadcaster_add_client(ctx->sse, conn);
}

/* ------------------------------------------------------------------ */
/* GET /api/status — Stato generale applicazione                       */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_get_status(struct MHD_Connection *conn, app_context_t *ctx)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"numHosts\":%d,\"port\":%d}",
             ctx->num_hosts, ctx->port);
    return json_response(conn, MHD_HTTP_OK, buf);
}

/* ------------------------------------------------------------------ */
/* POST /api/tunnel/start — Avvia tunnel SSH                           */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_tunnel_start(struct MHD_Connection *conn, const char *body,
                    app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"invalid JSON\"}");

    json_object *jhost = NULL;
    if (!json_object_object_get_ex(jreq, "host", &jhost)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"missing field: host\"}");
    }

    const char *host_name = json_object_get_string(jhost);

    /* Cerca l'host per nome */
    ssh_host_t *found = NULL;
    for (int i = 0; i < ctx->num_hosts; i++) {
        if (strcmp(ctx->hosts[i].name, host_name) == 0) {
            found = &ctx->hosts[i];
            break;
        }
    }

    if (!found) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_NOT_FOUND,
                             "{\"error\":\"host not found\"}");
    }

    int rc = process_manager_start_tunnel(ctx->pm, found);
    json_object_put(jreq);

    if (rc < 0)
        return json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "{\"error\":\"failed to start tunnel\"}");

    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/tunnel/stop — Ferma tunnel SSH                            */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_tunnel_stop(struct MHD_Connection *conn, const char *body,
                   app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"invalid JSON\"}");

    json_object *jhost = NULL;
    if (!json_object_object_get_ex(jreq, "host", &jhost)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"missing field: host\"}");
    }

    const char *host_name = json_object_get_string(jhost);
    int rc = process_manager_stop_tunnel(ctx->pm, host_name);
    json_object_put(jreq);

    if (rc < 0)
        return json_response(conn, MHD_HTTP_NOT_FOUND,
                             "{\"error\":\"tunnel not running\"}");

    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/terminal/open — Apre terminale con SSH                    */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_terminal_open(struct MHD_Connection *conn, const char *body,
                     app_context_t *ctx)
{
    (void)ctx; /* ctx non usato direttamente qui */

    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"invalid JSON\"}");

    json_object *jhost = NULL;
    if (!json_object_object_get_ex(jreq, "host", &jhost)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"missing field: host\"}");
    }

    const char *host_name = json_object_get_string(jhost);
    pid_t pid = launch_terminal_with_ssh(host_name);
    json_object_put(jreq);

    if (pid < 0)
        return json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                             "{\"error\":\"failed to launch terminal\"}");

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"pid\":%d}", (int)pid);
    return json_response(conn, MHD_HTTP_OK, buf);
}

/* ------------------------------------------------------------------ */
/* POST /api/password — Consegna password all'helper askpass           */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_password(struct MHD_Connection *conn, const char *body,
                app_context_t *ctx)
{
    (void)ctx;

    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"invalid JSON\"}");

    json_object *jrid = NULL;
    json_object *jpwd = NULL;

    if (!json_object_object_get_ex(jreq, "requestId", &jrid) ||
        !json_object_object_get_ex(jreq, "password",  &jpwd)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"missing field: requestId or password\"}");
    }

    const char *request_id = json_object_get_string(jrid);
    const char *password   = json_object_get_string(jpwd);

    int rc = askpass_deliver_password(request_id, password);
    json_object_put(jreq);

    if (rc < 0)
        return json_response(conn, MHD_HTTP_NOT_FOUND,
                             "{\"error\":\"no pending request with that id\"}");

    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* Callback principale MHD — router                                    */
/* ------------------------------------------------------------------ */

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
    (void)version;

    app_context_t *ctx = (app_context_t *)cls;

    /* Prima chiamata per questa connessione: inizializza lo stato */
    if (*con_cls == NULL) {
        if (strcmp(method, "POST") == 0) {
            post_data_t *pd = calloc(1, sizeof(post_data_t));
            if (!pd)
                return MHD_NO;
            *con_cls = pd;
            return MHD_YES;
        }
        /* GET e altri metodi: usa un marker non-NULL per non rientrare */
        *con_cls = (void *)1;
        return MHD_YES;
    }

    /* Accumula il body POST (MHD può chiamare più volte con chunk parziali) */
    if (strcmp(method, "POST") == 0) {
        post_data_t *pd = (post_data_t *)*con_cls;

        if (*upload_data_size > 0) {
            char *new_data = realloc(pd->data, pd->size + *upload_data_size + 1);
            if (!new_data)
                return MHD_NO;
            pd->data = new_data;
            memcpy(pd->data + pd->size, upload_data, *upload_data_size);
            pd->size += *upload_data_size;
            pd->data[pd->size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }

        /* Body completo: esegui routing POST */
        enum MHD_Result ret;
        const char *body = pd->data ? pd->data : "";

        if (strcmp(url, "/api/tunnel/start") == 0)
            ret = handle_tunnel_start(conn, body, ctx);
        else if (strcmp(url, "/api/tunnel/stop") == 0)
            ret = handle_tunnel_stop(conn, body, ctx);
        else if (strcmp(url, "/api/terminal/open") == 0)
            ret = handle_terminal_open(conn, body, ctx);
        else if (strcmp(url, "/api/password") == 0)
            ret = handle_password(conn, body, ctx);
        else
            ret = json_response(conn, MHD_HTTP_NOT_FOUND,
                                "{\"error\":\"unknown endpoint\"}");

        free(pd->data);
        free(pd);
        *con_cls = NULL;
        return ret;
    }

    /* Routing GET */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, "/api/hosts") == 0)
            return handle_get_hosts(conn, ctx);
        if (strcmp(url, "/api/events") == 0)
            return handle_sse(conn, ctx);
        if (strcmp(url, "/api/status") == 0)
            return handle_get_status(conn, ctx);
        /* Tutto il resto: file statici */
        return serve_static(conn, url);
    }

    /* Metodo non supportato */
    return json_response(conn, MHD_HTTP_METHOD_NOT_ALLOWED,
                         "{\"error\":\"method not allowed\"}");
}

/* ------------------------------------------------------------------ */
/* http_server_start — avvia MHD bound su 127.0.0.1                   */
/* ------------------------------------------------------------------ */

struct MHD_Daemon *
http_server_start(int port, app_context_t *ctx)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_THREAD_PER_CONNECTION,
        (uint16_t)port,
        NULL, NULL,                     /* accept policy callback */
        &request_handler, ctx,          /* handler + userdata */
        MHD_OPTION_SOCK_ADDR, &addr,    /* bind solo a 127.0.0.1 */
        MHD_OPTION_END
    );

    return daemon;
}
