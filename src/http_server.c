#include <microhttpd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <json-c/json.h>
#include <arpa/inet.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>   /* _NSGetExecutablePath */
#include <libgen.h>         /* dirname */
#endif

#include "http_server.h"
#include "app_context.h"
#include "sse.h"
#include "config_parser.h"
#include "config_manager.h"
#include "r2_client.h"
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
    if (strstr(path, ".woff2")) return "font/woff2";
    if (strstr(path, ".woff")) return "font/woff";
    if (strstr(path, ".json")) return "application/json";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* Helper: cerca la directory ui/ in più posizioni                     */
/* ------------------------------------------------------------------ */

static const char *find_ui_dir(void)
{
    static char exe_ui_path[512];
    static char bundle_ui_path[512];
    static int  resolved = 0;

    if (!resolved) {
        resolved = 1;

#ifdef __APPLE__
        /* macOS: _NSGetExecutablePath() */
        char exe[508];
        uint32_t sz = sizeof(exe);
        if (_NSGetExecutablePath(exe, &sz) == 0) {
            /* Resolve symlinks */
            char *real = realpath(exe, NULL);
            if (real) {
                char *dir = dirname(real);
                snprintf(exe_ui_path, sizeof(exe_ui_path), "%s/ui", dir);
                /* .app bundle: Contents/MacOS/sshpad → Contents/Resources/ui */
                char *contents = strstr(dir, "/Contents/MacOS");
                if (contents) {
                    *contents = '\0';
                    snprintf(bundle_ui_path, sizeof(bundle_ui_path),
                             "%s/Contents/Resources/ui", dir);
                }
                free(real);
            }
        }
#else
        /* Linux: /proc/self/exe */
        char exe[508];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = '\0';
                snprintf(exe_ui_path, sizeof(exe_ui_path), "%s/ui", exe);
            }
        }
#endif
    }

    /* Preferisce ui/dist/ (output Vite build) se esiste, altrimenti ui/ */
    const char *candidates[] = {
        "./ui/dist",
        "./ui",
        exe_ui_path[0]    ? exe_ui_path    : "",
        bundle_ui_path[0] ? bundle_ui_path : "",
#ifdef __APPLE__
        "/usr/local/share/sshpad/ui",
#else
        "/usr/local/share/sshpad/ui",
        "/usr/share/sshpad/ui",
#endif
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
    MHD_add_response_header(resp, "Cache-Control", "no-cache, no-store, must-revalidate");
    MHD_add_response_header(resp, "Pragma", "no-cache");
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
    int count = 0;
    const ssh_host_t *hosts = cm_get_hosts(ctx->cm, &count);

    json_object *jarr = json_object_new_array();

    for (int i = 0; i < count; i++) {
        const ssh_host_t *h = &hosts[i];
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
    int count = 0;
    cm_get_hosts(ctx->cm, &count);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"numHosts\":%d,\"port\":%d}",
             count, ctx->port);
    return json_response(conn, MHD_HTTP_OK, buf);
}

/* ------------------------------------------------------------------ */
/* GET /api/internal/askpass — Richiesta password dallo script askpass  */
/*                                                                      */
/* Lo script sshpad-askpass (invocato da SSH via SSH_ASKPASS) chiama    */
/* questo endpoint con ?id=UUID&prompt=... . Il server:                 */
/*   1. Invia un evento SSE "password_request" alla UI                  */
/*   2. Blocca in attesa che l'utente inserisca la password nella UI    */
/*   3. Risponde con la password in plain text (letta dallo script)     */
/* ------------------------------------------------------------------ */

static enum MHD_Result
handle_internal_askpass(struct MHD_Connection *conn, app_context_t *ctx)
{
    const char *id     = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "id");
    const char *prompt = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "prompt");

    if (!id || id[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST,
                             "{\"error\":\"missing id parameter\"}");

    /* Cerca il nome alias dell'host a partire dal prompt SSH.
     * Il prompt ha la forma "user@hostname's password:" oppure
     * "Password for user@hostname:" — estraiamo hostname e cerchiamo
     * tra gli host del config quale ha quel HostName. */
    int count = 0;
    const ssh_host_t *hosts = cm_get_hosts(ctx->cm, &count);
    const char *host_alias = "";
    if (prompt) {
        for (int i = 0; i < count; i++) {
            const ssh_host_t *h = &hosts[i];
            /* Cerca hostname o nome alias nel prompt */
            if ((h->hostname[0] && strstr(prompt, h->hostname)) ||
                strstr(prompt, h->name)) {
                host_alias = h->name;
                break;
            }
        }
    }

    /* Broadcast SSE: chiedi la password nella UI */
    {
        json_object *jobj = json_object_new_object();
        json_object_object_add(jobj, "requestId",
                               json_object_new_string(id));
        json_object_object_add(jobj, "host",
                               json_object_new_string(host_alias));
        json_object_object_add(jobj, "prompt",
                               json_object_new_string(prompt ? prompt : "Password:"));
        sse_broadcast(ctx->sse, "password_request",
                      json_object_to_json_string(jobj));
        json_object_put(jobj);
    }

    /* Blocca finché la UI non consegna la password (timeout ~120s) */
    char *password = askpass_wait_for_password(id);

    if (!password) {
        /* Timeout o errore: rispondi stringa vuota (SSH fallirà) */
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            0, "", MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(resp, "Content-Type", "text/plain");
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* Rispondi con la password (lo script la stampa su stdout per SSH) */
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        strlen(password), password, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "text/plain");
    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return ret;
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
    int count = 0;
    const ssh_host_t *hosts = cm_get_hosts(ctx->cm, &count);
    const ssh_host_t *found = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(hosts[i].name, host_name) == 0) {
            found = &hosts[i];
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
/* GET /api/config/status                                              */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_status(struct MHD_Connection *conn, app_context_t *ctx)
{
    cm_mode_t mode = cm_get_mode(ctx->cm);
    const char *mode_str;
    switch (mode) {
        case CM_MODE_FIRST_RUN: mode_str = "first_run"; break;
        case CM_MODE_LOCKED:    mode_str = "locked";    break;
        case CM_MODE_CLOUD:     mode_str = "cloud";     break;
        case CM_MODE_LOCAL:     mode_str = "local";     break;
        default:                mode_str = "local";     break;
    }
    r2_config_t r2 = {0};
    cm_get_r2_config(ctx->cm, &r2);
    int r2_configured = (r2.endpoint[0] != '\0') ? 1 : 0;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"r2Configured\":%s}",
             mode_str, r2_configured ? "true" : "false");
    return json_response(conn, MHD_HTTP_OK, buf);
}

/* ------------------------------------------------------------------ */
/* POST /api/config/setup — completa wizard primo avvio                */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_setup(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

    json_object *jmode = NULL;
    if (!json_object_object_get_ex(jreq, "mode", &jmode)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing field: mode\"}");
    }
    const char *mode = json_object_get_string(jmode);

    if (strcmp(mode, "local") == 0) {
        int rc = cm_setup(ctx->cm, "local", NULL, NULL);
        json_object_put(jreq);
        return rc == 0 ? json_response(conn, MHD_HTTP_OK, "{\"ok\":true}")
                       : json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"setup failed\"}");
    }

    if (strcmp(mode, "cloud") == 0) {
        json_object *jr2 = NULL, *jpwd = NULL;
        if (!json_object_object_get_ex(jreq, "r2", &jr2) ||
            !json_object_object_get_ex(jreq, "password", &jpwd)) {
            json_object_put(jreq);
            return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing r2 or password\"}");
        }

        r2_config_t r2 = {0};
        json_object *jv;
        if (json_object_object_get_ex(jr2, "endpoint", &jv))
            snprintf(r2.endpoint, sizeof(r2.endpoint), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jr2, "accessKeyId", &jv))
            snprintf(r2.access_key_id, sizeof(r2.access_key_id), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jr2, "secretAccessKey", &jv))
            snprintf(r2.secret_access_key, sizeof(r2.secret_access_key), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jr2, "bucket", &jv))
            snprintf(r2.bucket, sizeof(r2.bucket), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jr2, "objectKey", &jv))
            snprintf(r2.object_key, sizeof(r2.object_key), "%s", json_object_get_string(jv));
        else
            strncpy(r2.object_key, "sshpad-config.spd", sizeof(r2.object_key) - 1);

        const char *password = json_object_get_string(jpwd);
        int rc = cm_setup(ctx->cm, "cloud", &r2, password);
        json_object_put(jreq);

        if (rc == -1) return json_response(conn, MHD_HTTP_UNAUTHORIZED, "{\"error\":\"bad_password\"}");
        if (rc < 0)   return json_response(conn, MHD_HTTP_BAD_GATEWAY, "{\"error\":\"network_error\"}");
        return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
    }

    json_object_put(jreq);
    return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid mode\"}");
}

/* ------------------------------------------------------------------ */
/* POST /api/config/unlock                                             */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_unlock(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

    json_object *jpwd = NULL;
    if (!json_object_object_get_ex(jreq, "password", &jpwd)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing field: password\"}");
    }

    const char *password = json_object_get_string(jpwd);
    int rc = cm_unlock(ctx->cm, password);
    json_object_put(jreq);

    if (rc == -1) return json_response(conn, MHD_HTTP_UNAUTHORIZED, "{\"error\":\"bad_password\"}");
    if (rc < 0)   return json_response(conn, MHD_HTTP_BAD_GATEWAY,  "{\"error\":\"network_error\"}");
    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/host/save                                                 */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_host_save(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    cm_mode_t mode = cm_get_mode(ctx->cm);
    if (mode == CM_MODE_LOCKED)
        return json_response(conn, MHD_HTTP_FORBIDDEN, "{\"error\":\"config not unlocked\"}");
    if (mode == CM_MODE_FIRST_RUN)
        return json_response(conn, MHD_HTTP_FORBIDDEN, "{\"error\":\"setup not completed\"}");

    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    /* Wrap in array for ssh_hosts_from_json */
    size_t body_len = strlen(body);
    char *arr_json = malloc(body_len + 3);
    if (!arr_json)
        return json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"out of memory\"}");
    arr_json[0] = '[';
    memcpy(arr_json + 1, body, body_len);
    arr_json[body_len + 1] = ']';
    arr_json[body_len + 2] = '\0';

    int count = 0;
    ssh_host_t *hosts = ssh_hosts_from_json(arr_json, &count);
    free(arr_json);

    if (!hosts || count == 0) {
        free(hosts);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid host JSON\"}");
    }
    if (hosts[0].name[0] == '\0') {
        free(hosts);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing field: name\"}");
    }

    int rc = cm_save_host(ctx->cm, &hosts[0]);
    free(hosts);

    return rc == 0 ? json_response(conn, MHD_HTTP_OK, "{\"ok\":true}")
                   : json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"save failed\"}");
}

/* ------------------------------------------------------------------ */
/* POST /api/host/delete                                               */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_host_delete(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    cm_mode_t mode = cm_get_mode(ctx->cm);
    if (mode == CM_MODE_LOCKED)
        return json_response(conn, MHD_HTTP_FORBIDDEN, "{\"error\":\"config not unlocked\"}");
    if (mode == CM_MODE_FIRST_RUN)
        return json_response(conn, MHD_HTTP_FORBIDDEN, "{\"error\":\"setup not completed\"}");

    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

    json_object *jname = NULL;
    if (!json_object_object_get_ex(jreq, "name", &jname)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing field: name\"}");
    }

    const char *name = json_object_get_string(jname);
    int rc = cm_delete_host(ctx->cm, name);
    json_object_put(jreq);

    if (rc == -1) return json_response(conn, MHD_HTTP_NOT_FOUND, "{\"error\":\"host not found\"}");
    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/config/use-local                                          */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_use_local(struct MHD_Connection *conn, app_context_t *ctx)
{
    cm_use_local(ctx->cm);
    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* POST /api/config/change-password                                    */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_change_password(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

    json_object *jold = NULL, *jnew = NULL;
    if (!json_object_object_get_ex(jreq, "oldPassword", &jold) ||
        !json_object_object_get_ex(jreq, "newPassword", &jnew)) {
        json_object_put(jreq);
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing oldPassword or newPassword\"}");
    }

    const char *old_pw = json_object_get_string(jold);
    const char *new_pw = json_object_get_string(jnew);
    int rc = cm_change_password(ctx->cm, old_pw, new_pw);
    json_object_put(jreq);

    if (rc == -1) return json_response(conn, MHD_HTTP_UNAUTHORIZED, "{\"error\":\"bad_password\"}");
    if (rc < 0)   return json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"sync failed\"}");
    return json_response(conn, MHD_HTTP_OK, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ */
/* GET /api/config/r2-settings                                         */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_r2_settings_get(struct MHD_Connection *conn, app_context_t *ctx)
{
    r2_config_t r2 = {0};
    cm_get_r2_config(ctx->cm, &r2);

    json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "endpoint",        json_object_new_string(r2.endpoint));
    json_object_object_add(jobj, "accessKeyId",     json_object_new_string(r2.access_key_id));
    json_object_object_add(jobj, "secretAccessKey", json_object_new_string(r2.secret_access_key));
    json_object_object_add(jobj, "bucket",          json_object_new_string(r2.bucket));
    json_object_object_add(jobj, "objectKey",       json_object_new_string(r2.object_key));

    const char *json_str = json_object_to_json_string(jobj);
    enum MHD_Result ret = json_response(conn, MHD_HTTP_OK, json_str);
    json_object_put(jobj);
    return ret;
}

/* ------------------------------------------------------------------ */
/* POST /api/config/r2-settings                                        */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_r2_settings_post(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    if (!body || body[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"empty body\"}");

    json_object *jreq = json_tokener_parse(body);
    if (!jreq)
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

    r2_config_t new_cfg = {0};
    json_object *jv;
    if (json_object_object_get_ex(jreq, "endpoint", &jv))
        snprintf(new_cfg.endpoint, sizeof(new_cfg.endpoint), "%s", json_object_get_string(jv));
    if (json_object_object_get_ex(jreq, "accessKeyId", &jv))
        snprintf(new_cfg.access_key_id, sizeof(new_cfg.access_key_id), "%s", json_object_get_string(jv));
    if (json_object_object_get_ex(jreq, "secretAccessKey", &jv))
        snprintf(new_cfg.secret_access_key, sizeof(new_cfg.secret_access_key), "%s", json_object_get_string(jv));
    if (json_object_object_get_ex(jreq, "bucket", &jv))
        snprintf(new_cfg.bucket, sizeof(new_cfg.bucket), "%s", json_object_get_string(jv));
    if (json_object_object_get_ex(jreq, "objectKey", &jv))
        snprintf(new_cfg.object_key, sizeof(new_cfg.object_key), "%s", json_object_get_string(jv));

    int rc = cm_set_r2_config(ctx->cm, &new_cfg);
    json_object_put(jreq);

    return rc == 0 ? json_response(conn, MHD_HTTP_OK, "{\"ok\":true}")
                   : json_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"sync failed with new credentials\"}");
}

/* ------------------------------------------------------------------ */
/* POST /api/config/r2-test                                            */
/* ------------------------------------------------------------------ */
static enum MHD_Result
handle_config_r2_test(struct MHD_Connection *conn, const char *body, app_context_t *ctx)
{
    r2_config_t test_cfg = {0};
    int use_body = (body && body[0] != '\0' && strcmp(body, "{}") != 0);

    if (use_body) {
        json_object *jreq = json_tokener_parse(body);
        if (!jreq)
            return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");

        json_object *jv;
        if (json_object_object_get_ex(jreq, "endpoint", &jv))
            snprintf(test_cfg.endpoint, sizeof(test_cfg.endpoint), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jreq, "accessKeyId", &jv))
            snprintf(test_cfg.access_key_id, sizeof(test_cfg.access_key_id), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jreq, "secretAccessKey", &jv))
            snprintf(test_cfg.secret_access_key, sizeof(test_cfg.secret_access_key), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jreq, "bucket", &jv))
            snprintf(test_cfg.bucket, sizeof(test_cfg.bucket), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jreq, "objectKey", &jv))
            snprintf(test_cfg.object_key, sizeof(test_cfg.object_key), "%s", json_object_get_string(jv));
        json_object_put(jreq);
    } else {
        /* Usa credenziali salvate */
        cm_get_r2_config(ctx->cm, &test_cfg);
        /* Se la secret è mascherata, rileggi dal file direttamente */
        if (test_cfg.endpoint[0] == '\0')
            return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"no R2 configuration\"}");
        /* La cm_get_r2_config restituisce secret mascherata. Per il test con credenziali salvate
         * usiamo r2_config_load direttamente per avere secret reale */
        r2_config_t real_cfg = {0};
        if (r2_config_load(&real_cfg) == 0) {
            test_cfg = real_cfg;
        }
    }

    if (test_cfg.endpoint[0] == '\0')
        return json_response(conn, MHD_HTTP_BAD_REQUEST, "{\"error\":\"no R2 configuration\"}");

    int rc = r2_test_connection(&test_cfg);
    return rc == 0 ? json_response(conn, MHD_HTTP_OK, "{\"ok\":true}")
                   : json_response(conn, MHD_HTTP_BAD_GATEWAY, "{\"error\":\"connection failed\"}");
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
        else if (strcmp(url, "/api/config/setup") == 0)
            ret = handle_config_setup(conn, body, ctx);
        else if (strcmp(url, "/api/config/unlock") == 0)
            ret = handle_config_unlock(conn, body, ctx);
        else if (strcmp(url, "/api/host/save") == 0)
            ret = handle_host_save(conn, body, ctx);
        else if (strcmp(url, "/api/host/delete") == 0)
            ret = handle_host_delete(conn, body, ctx);
        else if (strcmp(url, "/api/config/use-local") == 0)
            ret = handle_config_use_local(conn, ctx);
        else if (strcmp(url, "/api/config/change-password") == 0)
            ret = handle_config_change_password(conn, body, ctx);
        else if (strcmp(url, "/api/config/r2-settings") == 0)
            ret = handle_config_r2_settings_post(conn, body, ctx);
        else if (strcmp(url, "/api/config/r2-test") == 0)
            ret = handle_config_r2_test(conn, body, ctx);
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
        if (strcmp(url, "/api/internal/askpass") == 0)
            return handle_internal_askpass(conn, ctx);
        if (strcmp(url, "/api/config/status") == 0)
            return handle_config_status(conn, ctx);
        if (strcmp(url, "/api/config/r2-settings") == 0)
            return handle_config_r2_settings_get(conn, ctx);
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
