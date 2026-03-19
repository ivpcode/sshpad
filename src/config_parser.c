/*
 * config_parser.c — SSH config file parser for SSHPad.
 *
 * Parses ~/.ssh/config (or an arbitrary path) following the OpenSSH config
 * format:
 *   - Lines starting with '#' or empty lines are ignored.
 *   - "Host <pattern>" starts a new block.  Wildcard patterns (containing
 *     '*' or '?') are silently skipped so only concrete named hosts are
 *     returned.
 *   - Directives before the first Host block set global defaults that are
 *     inherited by every host block.
 *   - Supported keywords (case-insensitive):
 *       Host, HostName, User, Port, IdentityFile, ProxyJump,
 *       LocalForward, RemoteForward, DynamicForward,
 *       and any other keyword stored as a generic ssh_option_t.
 *
 * Compiled with -std=c11 -Wall -Wextra -Wpedantic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <unistd.h>

#include "config_parser.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Expand a leading "~/" to the user's home directory. */
static void expand_tilde(char *dest, const char *src, size_t maxlen)
{
    if (src[0] == '~' && src[1] == '/') {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = (pw && pw->pw_dir) ? pw->pw_dir : "/tmp";
        }
        snprintf(dest, maxlen, "%s%s", home, src + 1);
    } else {
        strncpy(dest, src, maxlen - 1);
        dest[maxlen - 1] = '\0';
    }
}

/*
 * Parse a LocalForward / RemoteForward value.
 *
 * Accepted forms (OpenSSH syntax):
 *   <port> <remote_host>:<remote_port>                 (space-separated, bind on localhost)
 *   <bind_addr>:<port> <remote_host>:<remote_port>     (space-separated, explicit bind)
 *   <port>:<remote_host>:<remote_port>                  (colon-only, bind on localhost)
 *   <bind_addr>:<port>:<remote_host>:<remote_port>      (colon-only, explicit bind)
 *
 * Returns 0 on success, -1 on parse error.
 */
static int parse_forward(const char *val, forward_rule_t *rule)
{
    char buf[512];
    strncpy(buf, val, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    memset(rule, 0, sizeof(*rule));

    /* Check for space-separated format first (most common in SSH configs):
     *   <bind_spec> <remote_host>:<remote_port>
     * where bind_spec is either "<port>" or "<bind_addr>:<port>" */
    char *space = strchr(buf, ' ');
    if (!space) space = strchr(buf, '\t');

    if (space) {
        *space = '\0';
        char *bind_part  = buf;
        char *remote_part = space + 1;
        while (*remote_part == ' ' || *remote_part == '\t') remote_part++;

        /* Parse bind part: either "port" or "addr:port" */
        char *bind_colon = strchr(bind_part, ':');
        if (bind_colon) {
            *bind_colon = '\0';
            snprintf(rule->bind_addr, sizeof(rule->bind_addr), "%s", bind_part);
            rule->bind_port = atoi(bind_colon + 1);
        } else {
            strncpy(rule->bind_addr, "localhost", sizeof(rule->bind_addr) - 1);
            rule->bind_port = atoi(bind_part);
        }

        /* Parse remote part: "host:port" */
        char *remote_colon = strrchr(remote_part, ':');
        if (!remote_colon) return -1;
        *remote_colon = '\0';
        snprintf(rule->remote_host, sizeof(rule->remote_host), "%s", remote_part);
        rule->remote_port = atoi(remote_colon + 1);

        return (rule->bind_port > 0 && rule->remote_port > 0) ? 0 : -1;
    }

    /* Colon-only format */
    int colons = 0;
    for (const char *p = buf; *p; p++) {
        if (*p == ':') colons++;
    }

    if (colons == 2) {
        /* port:remote_host:remote_port */
        strncpy(rule->bind_addr, "localhost", sizeof(rule->bind_addr) - 1);
        if (sscanf(buf, "%d:%255[^:]:%d",
                   &rule->bind_port,
                   rule->remote_host,
                   &rule->remote_port) != 3) {
            return -1;
        }
    } else if (colons == 3) {
        /* bind_addr:port:remote_host:remote_port */
        if (sscanf(buf, "%63[^:]:%d:%255[^:]:%d",
                   rule->bind_addr,
                   &rule->bind_port,
                   rule->remote_host,
                   &rule->remote_port) != 4) {
            return -1;
        }
    } else {
        return -1;
    }

    return 0;
}

/*
 * Parse a DynamicForward value.
 *
 * Accepted forms:
 *   <port>
 *   <bind_addr>:<port>
 *
 * Returns 0 always (invalid ports default to 0).
 */
static int parse_dynamic(const char *val, dynamic_rule_t *rule)
{
    memset(rule, 0, sizeof(*rule));

    /* Trim leading whitespace */
    while (*val == ' ' || *val == '\t') val++;

    const char *colon = strchr(val, ':');
    if (colon) {
        size_t alen = (size_t)(colon - val);
        if (alen >= sizeof(rule->bind_addr)) alen = sizeof(rule->bind_addr) - 1;
        strncpy(rule->bind_addr, val, alen);
        rule->bind_addr[alen] = '\0';
        rule->bind_port = atoi(colon + 1);
    } else {
        strncpy(rule->bind_addr, "localhost", sizeof(rule->bind_addr) - 1);
        rule->bind_port = atoi(val);
    }
    return 0;
}

/* In-place trim of leading and trailing whitespace; returns the same pointer. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Case-insensitive string comparison (POSIX strcasecmp). */
static int kw_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Returns 1 if the host pattern contains wildcards that should be skipped. */
static int has_wildcard(const char *pattern)
{
    return (strchr(pattern, '*') != NULL || strchr(pattern, '?') != NULL);
}

/* -------------------------------------------------------------------------
 * apply_defaults: copy inherited global defaults into a new host entry.
 * Only fields that have not been set in the host block are filled.
 * ---------------------------------------------------------------------- */
static void apply_defaults(ssh_host_t *host, const ssh_host_t *defaults)
{
    if (host->hostname[0] == '\0' && defaults->hostname[0] != '\0')
        snprintf(host->hostname, sizeof(host->hostname), "%s", defaults->hostname);

    if (host->user[0] == '\0' && defaults->user[0] != '\0')
        snprintf(host->user, sizeof(host->user), "%s", defaults->user);

    if (host->port == 0 && defaults->port != 0)
        host->port = defaults->port;

    if (host->identity_file[0] == '\0' && defaults->identity_file[0] != '\0')
        snprintf(host->identity_file, sizeof(host->identity_file), "%s", defaults->identity_file);

    if (host->proxy_jump[0] == '\0' && defaults->proxy_jump[0] != '\0')
        snprintf(host->proxy_jump, sizeof(host->proxy_jump), "%s", defaults->proxy_jump);

    /* Merge generic options that are not already present. */
    for (int di = 0; di < defaults->num_options; di++) {
        int found = 0;
        for (int hi = 0; hi < host->num_options; hi++) {
            if (kw_eq(host->options[hi].key, defaults->options[di].key)) {
                found = 1;
                break;
            }
        }
        if (!found && host->num_options < MAX_OPTIONS) {
            host->options[host->num_options] = defaults->options[di];
            host->num_options++;
        }
    }
}

/* -------------------------------------------------------------------------
 * process_kv: apply a keyword/value pair to a host structure.
 * ---------------------------------------------------------------------- */
static void process_kv(ssh_host_t *host, const char *key, const char *val)
{
    if (kw_eq(key, "HostName")) {
        snprintf(host->hostname, sizeof(host->hostname), "%s", val);

    } else if (kw_eq(key, "User")) {
        snprintf(host->user, sizeof(host->user), "%s", val);

    } else if (kw_eq(key, "Port")) {
        int p = atoi(val);
        if (p > 0 && p <= 65535) host->port = p;

    } else if (kw_eq(key, "IdentityFile")) {
        expand_tilde(host->identity_file, val, sizeof(host->identity_file));

    } else if (kw_eq(key, "ProxyJump")) {
        snprintf(host->proxy_jump, sizeof(host->proxy_jump), "%s", val);

    } else if (kw_eq(key, "LocalForward")) {
        if (host->num_local_forward < MAX_FORWARDS) {
            forward_rule_t rule;
            if (parse_forward(val, &rule) == 0) {
                host->local_forward[host->num_local_forward++] = rule;
            }
        }

    } else if (kw_eq(key, "RemoteForward")) {
        if (host->num_remote_forward < MAX_FORWARDS) {
            forward_rule_t rule;
            if (parse_forward(val, &rule) == 0) {
                host->remote_forward[host->num_remote_forward++] = rule;
            }
        }

    } else if (kw_eq(key, "DynamicForward")) {
        if (host->num_dynamic_forward < MAX_FORWARDS) {
            dynamic_rule_t rule;
            parse_dynamic(val, &rule);
            host->dynamic_forward[host->num_dynamic_forward++] = rule;
        }

    } else {
        /* Generic option — store key/value if there is room. */
        if (host->num_options < MAX_OPTIONS) {
            snprintf(host->options[host->num_options].key,
                     sizeof(host->options[0].key), "%s", key);
            snprintf(host->options[host->num_options].value,
                     sizeof(host->options[0].value), "%s", val);
            host->num_options++;
        }
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

/*
 * parse_ssh_config — open and parse an OpenSSH client config file.
 *
 * Parameters:
 *   path      — path to the config file.  If NULL or empty, defaults to
 *               ~/.ssh/config.
 *   out_count — receives the number of parsed hosts on success; set to 0
 *               on failure.
 *
 * Returns a heap-allocated array of ssh_host_t (free with ssh_hosts_free),
 * or NULL if the file cannot be opened or memory allocation fails.
 */
ssh_host_t *parse_ssh_config(const char *path, int *out_count)
{
    *out_count = 0;

    /* ---- Resolve path -------------------------------------------------- */
    char resolved[1024];
    if (!path || path[0] == '\0') {
        expand_tilde(resolved, "~/.ssh/config", sizeof(resolved));
    } else {
        expand_tilde(resolved, path, sizeof(resolved));
    }

    FILE *fp = fopen(resolved, "r");
    if (!fp) return NULL;

    /* ---- Dynamic array bookkeeping ------------------------------------- */
    int         count    = 0;
    int         capacity = 8;
    ssh_host_t *hosts    = calloc((size_t)capacity, sizeof(ssh_host_t));
    if (!hosts) { fclose(fp); return NULL; }

    /*
     * defaults holds directives that appear before any Host block.
     * They act like global options inherited by every host.
     */
    ssh_host_t defaults;
    memset(&defaults, 0, sizeof(defaults));

    /*
     * current points to the ssh_host_t being populated:
     *   - Before the first Host line it points to &defaults.
     *   - After a Host line it points to hosts[count-1].
     *
     * skip_current is set when the current Host pattern has wildcards;
     * all its directives are discarded until the next Host line.
     */
    ssh_host_t *current     = &defaults;
    int         skip_current = 0; /* 1 = wildcard host, ignore directives */

    char line[1024];
    while (fgets(line, (int)sizeof(line), fp)) {

        /* Strip newline / carriage return. */
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        char *s = trim(line);

        /* Skip empty lines and comments. */
        if (*s == '\0' || *s == '#') continue;

        /* Split into keyword and value.
         * OpenSSH accepts "Key Value" (space) or "Key=Value" (equals). */
        char key[64]   = {0};
        char value[960] = {0};

        char *eq = strchr(s, '=');
        char *sp = strchr(s, ' ');
        if (!sp) sp = strchr(s, '\t');

        char *sep = NULL; /* position of delimiter in s */
        if (eq && sp) sep = (eq < sp) ? eq : sp;
        else if (eq)  sep = eq;
        else if (sp)  sep = sp;
        else {
            /* Line with just a keyword and no value — skip. */
            continue;
        }

        size_t klen = (size_t)(sep - s);
        if (klen == 0 || klen >= sizeof(key)) continue;
        strncpy(key, s, klen);
        key[klen] = '\0';

        char *vstart = trim(sep + 1); /* skip delimiter and leading spaces */
        strncpy(value, vstart, sizeof(value) - 1);

        /* ---- Handle the Host keyword specially ------------------------- */
        if (kw_eq(key, "Host")) {
            /*
             * Finalise the previous host: if skip_current is set we have
             * already discarded it, so nothing to do.  Otherwise apply
             * defaults and advance count.
             */
            if (!skip_current && current != &defaults) {
                apply_defaults(current, &defaults);
                /* current is already hosts[count-1]; count was incremented
                 * when the slot was claimed below. */
            }

            /* Wildcard pattern? */
            if (has_wildcard(value)) {
                skip_current = 1;
                current      = &defaults; /* temporary; won't be saved */
                continue;
            }
            skip_current = 0;

            /* Grow array if necessary. */
            if (count >= capacity) {
                int new_cap = capacity * 2;
                ssh_host_t *tmp = realloc(hosts,
                                          (size_t)new_cap * sizeof(ssh_host_t));
                if (!tmp) {
                    fclose(fp);
                    *out_count = count;
                    return hosts;
                }
                /* Zero-initialise the newly allocated slots. */
                memset(tmp + capacity, 0,
                       (size_t)(new_cap - capacity) * sizeof(ssh_host_t));
                hosts    = tmp;
                capacity = new_cap;
            }

            current = &hosts[count];
            memset(current, 0, sizeof(*current));
            snprintf(current->name, sizeof(current->name), "%s", value);
            /* Default: if HostName is never set it mirrors the Host name. */
            snprintf(current->hostname, sizeof(current->hostname), "%s", value);
            current->port = 22; /* SSH default; overridden by Port directive */
            count++;
            continue;
        }

        /* ---- All other keywords ---------------------------------------- */
        if (skip_current) continue; /* wildcard block, discard */

        process_kv(current, key, value);
    }

    /* Finalise the last host block (apply defaults). */
    if (!skip_current && current != &defaults && count > 0) {
        apply_defaults(&hosts[count - 1], &defaults);
    }

    fclose(fp);
    *out_count = count;
    return hosts;
}

/*
 * ssh_hosts_free — release the array returned by parse_ssh_config.
 *
 * The count parameter is accepted for API symmetry but is not used
 * because the array is a single flat allocation.
 */
void ssh_hosts_free(ssh_host_t *hosts, int count)
{
    (void)count; /* suppress unused-parameter warning */
    free(hosts);
}

/* -------------------------------------------------------------------------
 * Serializzazione JSON (richiede json-c)
 * ---------------------------------------------------------------------- */

#include <json-c/json.h>
#include <sys/stat.h>
#include <errno.h>

/*
 * ssh_hosts_to_json — serializza un array di ssh_host_t in una stringa JSON.
 *
 * I nomi dei campi JSON corrispondono a quelli usati in http_server.c.
 * Il chiamante deve liberare la stringa restituita con free().
 * Restituisce NULL in caso di errore di allocazione.
 */
char *ssh_hosts_to_json(const ssh_host_t *hosts, int count)
{
    json_object *jarr = json_object_new_array();
    if (!jarr) return NULL;

    for (int i = 0; i < count; i++) {
        const ssh_host_t *h = &hosts[i];
        json_object *jh = json_object_new_object();

        json_object_object_add(jh, "name",         json_object_new_string(h->name));
        json_object_object_add(jh, "hostname",     json_object_new_string(h->hostname));
        json_object_object_add(jh, "user",         json_object_new_string(h->user));
        json_object_object_add(jh, "port",         json_object_new_int(h->port ? h->port : 22));
        json_object_object_add(jh, "identityFile", json_object_new_string(h->identity_file));
        json_object_object_add(jh, "proxyJump",    json_object_new_string(h->proxy_jump));

        /* LocalForward */
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

        /* RemoteForward */
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

        /* DynamicForward */
        json_object *jdf = json_object_new_array();
        for (int j = 0; j < h->num_dynamic_forward; j++) {
            json_object *jf = json_object_new_object();
            json_object_object_add(jf, "bindAddr", json_object_new_string(h->dynamic_forward[j].bind_addr));
            json_object_object_add(jf, "bindPort", json_object_new_int(h->dynamic_forward[j].bind_port));
            json_object_array_add(jdf, jf);
        }
        json_object_object_add(jh, "dynamicForward", jdf);

        json_object_array_add(jarr, jh);
    }

    /* Duplica la stringa prima di liberare l'oggetto json */
    const char *tmp = json_object_to_json_string_ext(jarr, JSON_C_TO_STRING_PLAIN);
    char *result = tmp ? strdup(tmp) : NULL;
    json_object_put(jarr);
    return result;
}

/*
 * ssh_hosts_from_json — deserializza un array JSON in ssh_host_t[].
 *
 * Il chiamante deve liberare il risultato con ssh_hosts_free().
 * Restituisce NULL in caso di errore (json non valido o OOM).
 */
ssh_host_t *ssh_hosts_from_json(const char *json_str, int *out_count)
{
    *out_count = 0;
    if (!json_str) return NULL;

    json_object *jarr = json_tokener_parse(json_str);
    if (!jarr || !json_object_is_type(jarr, json_type_array)) {
        if (jarr) json_object_put(jarr);
        return NULL;
    }

    int count = (int)json_object_array_length(jarr);
    ssh_host_t *hosts = calloc((size_t)count + 1, sizeof(ssh_host_t));
    if (!hosts) { json_object_put(jarr); return NULL; }

    for (int i = 0; i < count; i++) {
        ssh_host_t  *h  = &hosts[i];
        json_object *jh = json_object_array_get_idx(jarr, i);
        if (!jh) continue;

        json_object *jv;

        /* Campi scalari */
        if (json_object_object_get_ex(jh, "name", &jv))
            snprintf(h->name, sizeof(h->name), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jh, "hostname", &jv))
            snprintf(h->hostname, sizeof(h->hostname), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jh, "user", &jv))
            snprintf(h->user, sizeof(h->user), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jh, "port", &jv))
            h->port = json_object_get_int(jv);
        if (json_object_object_get_ex(jh, "identityFile", &jv))
            snprintf(h->identity_file, sizeof(h->identity_file), "%s", json_object_get_string(jv));
        if (json_object_object_get_ex(jh, "proxyJump", &jv))
            snprintf(h->proxy_jump, sizeof(h->proxy_jump), "%s", json_object_get_string(jv));

        /* LocalForward */
        json_object *jlf;
        if (json_object_object_get_ex(jh, "localForward", &jlf) &&
            json_object_is_type(jlf, json_type_array)) {
            int n = (int)json_object_array_length(jlf);
            if (n > MAX_FORWARDS) n = MAX_FORWARDS;
            for (int j = 0; j < n; j++) {
                json_object    *jf = json_object_array_get_idx(jlf, j);
                forward_rule_t *r  = &h->local_forward[j];
                if (json_object_object_get_ex(jf, "bindAddr", &jv))
                    snprintf(r->bind_addr, sizeof(r->bind_addr), "%s", json_object_get_string(jv));
                if (json_object_object_get_ex(jf, "bindPort", &jv))
                    r->bind_port = json_object_get_int(jv);
                if (json_object_object_get_ex(jf, "remoteHost", &jv))
                    snprintf(r->remote_host, sizeof(r->remote_host), "%s", json_object_get_string(jv));
                if (json_object_object_get_ex(jf, "remotePort", &jv))
                    r->remote_port = json_object_get_int(jv);
                h->num_local_forward++;
            }
        }

        /* RemoteForward */
        json_object *jrf;
        if (json_object_object_get_ex(jh, "remoteForward", &jrf) &&
            json_object_is_type(jrf, json_type_array)) {
            int n = (int)json_object_array_length(jrf);
            if (n > MAX_FORWARDS) n = MAX_FORWARDS;
            for (int j = 0; j < n; j++) {
                json_object    *jf = json_object_array_get_idx(jrf, j);
                forward_rule_t *r  = &h->remote_forward[j];
                if (json_object_object_get_ex(jf, "bindAddr", &jv))
                    snprintf(r->bind_addr, sizeof(r->bind_addr), "%s", json_object_get_string(jv));
                if (json_object_object_get_ex(jf, "bindPort", &jv))
                    r->bind_port = json_object_get_int(jv);
                if (json_object_object_get_ex(jf, "remoteHost", &jv))
                    snprintf(r->remote_host, sizeof(r->remote_host), "%s", json_object_get_string(jv));
                if (json_object_object_get_ex(jf, "remotePort", &jv))
                    r->remote_port = json_object_get_int(jv);
                h->num_remote_forward++;
            }
        }

        /* DynamicForward */
        json_object *jdf;
        if (json_object_object_get_ex(jh, "dynamicForward", &jdf) &&
            json_object_is_type(jdf, json_type_array)) {
            int n = (int)json_object_array_length(jdf);
            if (n > MAX_FORWARDS) n = MAX_FORWARDS;
            for (int j = 0; j < n; j++) {
                json_object    *jf = json_object_array_get_idx(jdf, j);
                dynamic_rule_t *r  = &h->dynamic_forward[j];
                if (json_object_object_get_ex(jf, "bindAddr", &jv))
                    snprintf(r->bind_addr, sizeof(r->bind_addr), "%s", json_object_get_string(jv));
                if (json_object_object_get_ex(jf, "bindPort", &jv))
                    r->bind_port = json_object_get_int(jv);
                h->num_dynamic_forward++;
            }
        }
    }

    json_object_put(jarr);
    *out_count = count;
    return hosts;
}

/* -------------------------------------------------------------------------
 * Scrittura SSH config
 * ---------------------------------------------------------------------- */

/*
 * copy_file — copia src in dst in modalità binaria.
 * Restituisce 0 in caso di successo, -1 in caso di errore.
 */
static int copy_file(const char *src, const char *dst)
{
    FILE *in  = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

/*
 * ssh_hosts_write_config — scrive un array di ssh_host_t in formato
 * OpenSSH client config.
 *
 * Se path è NULL viene usato ~/.ssh/config.
 * Se il file di destinazione esiste viene prima copiato come <path>.bak.
 * La directory ~/.ssh/ viene creata con permessi 0700 se non esiste.
 * Il file scritto riceve permessi 0600.
 *
 * Restituisce 0 in caso di successo, -1 in caso di errore.
 */
int ssh_hosts_write_config(const ssh_host_t *hosts, int count, const char *path)
{
    /* --- Risolvi il path di destinazione --- */
    char resolved[1024];
    if (!path || path[0] == '\0') {
        expand_tilde(resolved, "~/.ssh/config", sizeof(resolved));
    } else {
        expand_tilde(resolved, path, sizeof(resolved));
    }

    /* --- Crea ~/.ssh/ con permessi 0700 se necessario --- */
    char ssh_dir[1024];
    snprintf(ssh_dir, sizeof(ssh_dir), "%s", resolved);
    /* Trova l'ultima '/' per ricavare la directory padre */
    char *last_slash = strrchr(ssh_dir, '/');
    if (last_slash && last_slash != ssh_dir) {
        *last_slash = '\0';
        if (mkdir(ssh_dir, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
    }

    /* --- Backup del file esistente --- */
    char bak_path[1024];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", resolved);
    if (access(resolved, F_OK) == 0) {
        if (copy_file(resolved, bak_path) != 0) {
            return -1;
        }
    }

    /* --- Scrittura del nuovo file --- */
    FILE *fp = fopen(resolved, "w");
    if (!fp) return -1;

    /* Intestazione */
    fprintf(fp, "# Generato da SSHPad — non modificare manualmente\n");

    for (int i = 0; i < count; i++) {
        const ssh_host_t *h = &hosts[i];

        /* Riga vuota di separazione tra host (tranne prima del primo) */
        fprintf(fp, "\n");

        fprintf(fp, "Host %s\n", h->name);

        /* HostName — ometti se uguale all'alias */
        if (h->hostname[0] != '\0' &&
            strcmp(h->hostname, h->name) != 0) {
            fprintf(fp, "    HostName %s\n", h->hostname);
        }

        /* User — ometti se vuoto */
        if (h->user[0] != '\0')
            fprintf(fp, "    User %s\n", h->user);

        /* Port — ometti se 0 o 22 */
        if (h->port != 0 && h->port != 22)
            fprintf(fp, "    Port %d\n", h->port);

        /* IdentityFile — ometti se vuoto */
        if (h->identity_file[0] != '\0')
            fprintf(fp, "    IdentityFile %s\n", h->identity_file);

        /* ProxyJump — ometti se vuoto */
        if (h->proxy_jump[0] != '\0')
            fprintf(fp, "    ProxyJump %s\n", h->proxy_jump);

        /* LocalForward */
        for (int j = 0; j < h->num_local_forward; j++) {
            const forward_rule_t *r = &h->local_forward[j];
            fprintf(fp, "    LocalForward %s:%d %s:%d\n",
                    r->bind_addr, r->bind_port,
                    r->remote_host, r->remote_port);
        }

        /* RemoteForward */
        for (int j = 0; j < h->num_remote_forward; j++) {
            const forward_rule_t *r = &h->remote_forward[j];
            fprintf(fp, "    RemoteForward %s:%d %s:%d\n",
                    r->bind_addr, r->bind_port,
                    r->remote_host, r->remote_port);
        }

        /* DynamicForward */
        for (int j = 0; j < h->num_dynamic_forward; j++) {
            const dynamic_rule_t *r = &h->dynamic_forward[j];
            fprintf(fp, "    DynamicForward %s:%d\n",
                    r->bind_addr, r->bind_port);
        }

        /* Opzioni generiche */
        for (int j = 0; j < h->num_options; j++) {
            fprintf(fp, "    %s %s\n",
                    h->options[j].key,
                    h->options[j].value);
        }
    }

    fclose(fp);

    /* Imposta permessi 0600 sul file scritto */
    chmod(resolved, 0600);

    return 0;
}
