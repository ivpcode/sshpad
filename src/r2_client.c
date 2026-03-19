/*
 * r2_client.c — Client S3/R2 con AWS Signature V4 per SSHPad.
 *
 * Implementa upload, download e test di connessione verso Cloudflare R2
 * tramite l'API compatibile S3. La firma delle richieste segue lo schema
 * AWS Signature Version 4 con region fissa "auto" e service "s3".
 *
 * Dipendenze: libcurl, OpenSSL (EVP + HMAC), json-c.
 * Compilato con -std=c11 -Wall -Wextra.
 */

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#include "r2_client.h"

/* -------------------------------------------------------------------------
 * Costanti
 * ---------------------------------------------------------------------- */

#define R2_REGION        "auto"
#define R2_SERVICE       "s3"
#define R2_ALGORITHM     "AWS4-HMAC-SHA256"
#define R2_MAX_BODY      (10 * 1024 * 1024)   /* 10 MB limite download */

/* SHA-256 della stringa vuota (payload GET/HEAD) */
#define EMPTY_SHA256 \
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

/* -------------------------------------------------------------------------
 * Helper: espansione "~/" con home directory dell'utente corrente
 * ---------------------------------------------------------------------- */

static void expand_tilde_r2(char *dest, const char *src, size_t maxlen)
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

/* -------------------------------------------------------------------------
 * Helper: estrazione host dall'URL endpoint
 * Dato "https://xxx.r2.cloudflarestorage.com" restituisce "xxx.r2.cloudflarestorage.com"
 * ---------------------------------------------------------------------- */

static void extract_host(const char *endpoint, char *host, size_t maxlen)
{
    const char *start = strstr(endpoint, "://");
    if (start) start += 3; else start = endpoint;
    const char *end = strchr(start, '/');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(host, start, len);
    host[len] = '\0';
}

/* -------------------------------------------------------------------------
 * Crittografia: SHA-256 di un buffer, risultato in hex (65 byte con \0)
 * ---------------------------------------------------------------------- */

static void sha256_hex(const unsigned char *data, size_t len, char *out_hex)
{
    unsigned char hash[32];
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, NULL);
    EVP_MD_CTX_free(ctx);
    for (int i = 0; i < 32; i++)
        sprintf(out_hex + i * 2, "%02x", hash[i]);
    out_hex[64] = '\0';
}

/* -------------------------------------------------------------------------
 * Crittografia: HMAC-SHA256
 * ---------------------------------------------------------------------- */

static void hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *data, size_t data_len,
                        unsigned char *out, unsigned int *out_len)
{
    HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, out_len);
}

/* -------------------------------------------------------------------------
 * Struttura per accumulo dati risposta cURL
 * ---------------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t         size;
} curl_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_buf_t *buf   = (curl_buf_t *)userdata;
    size_t      total = size * nmemb;

    /* Limite di sicurezza: non accettare più di 10 MB */
    if (buf->size + total > R2_MAX_BODY)
        return 0;

    unsigned char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp)
        return 0;

    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* -------------------------------------------------------------------------
 * Struttura per fornitura dati in upload a cURL (PUT)
 * ---------------------------------------------------------------------- */

typedef struct {
    const unsigned char *data;
    size_t               size;
    size_t               offset;
} curl_read_buf_t;

static size_t read_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_read_buf_t *rbuf  = (curl_read_buf_t *)userdata;
    size_t           space = size * nmemb;
    size_t           avail = rbuf->size - rbuf->offset;
    size_t           bytes = avail < space ? avail : space;

    if (bytes == 0)
        return 0;

    memcpy(ptr, rbuf->data + rbuf->offset, bytes);
    rbuf->offset += bytes;
    return bytes;
}

/* -------------------------------------------------------------------------
 * Firma AWS Signature V4 — genera l'header Authorization e gli header HTTP
 * necessari (x-amz-date, x-amz-content-sha256).
 *
 * Parametri:
 *   cfg          — configurazione R2
 *   method       — "GET", "PUT", "HEAD"
 *   path         — path URL (es. "/bucket/chiave" o "/bucket")
 *   payload_data — corpo della richiesta (NULL per GET/HEAD)
 *   payload_len  — lunghezza corpo
 *   out_auth     — buffer per l'header Authorization (min 512 byte)
 *   out_date     — buffer per x-amz-date (min 17 byte)
 *   out_sha256   — buffer per x-amz-content-sha256 (min 65 byte)
 * ---------------------------------------------------------------------- */

static void sign_request(const r2_config_t  *cfg,
                         const char         *method,
                         const char         *path,
                         const unsigned char *payload_data,
                         size_t              payload_len,
                         char               *out_auth,
                         char               *out_date,
                         char               *out_sha256)
{
    /* 1. Data e ora correnti in UTC */
    time_t    now = time(NULL);
    struct tm *utc = gmtime(&now);
    char date_str[9];      /* "YYYYMMDD\0" */
    char datetime_str[17]; /* "YYYYMMDDTHHMMSSZ\0" */
    strftime(date_str,     sizeof(date_str),     "%Y%m%d",       utc);
    strftime(datetime_str, sizeof(datetime_str), "%Y%m%dT%H%M%SZ", utc);

    /* Copia le date negli output per uso esterno */
    strncpy(out_date, datetime_str, 17);

    /* 2. SHA-256 del payload */
    char payload_hash[65];
    if (payload_data && payload_len > 0)
        sha256_hex(payload_data, payload_len, payload_hash);
    else
        strncpy(payload_hash, EMPTY_SHA256, sizeof(payload_hash));

    strncpy(out_sha256, payload_hash, 65);

    /* 3. Host dall'endpoint */
    char host[256];
    extract_host(cfg->endpoint, host, sizeof(host));

    /* 4. Canonical Headers (sorted, lowercase) */
    /* Ordine: host, x-amz-content-sha256, x-amz-date */
    char canonical_headers[1024];
    snprintf(canonical_headers, sizeof(canonical_headers),
             "host:%s\n"
             "x-amz-content-sha256:%s\n"
             "x-amz-date:%s\n",
             host, payload_hash, datetime_str);

    const char *signed_headers = "host;x-amz-content-sha256;x-amz-date";

    /* 5. Canonical Request */
    char canonical_request[4096];
    snprintf(canonical_request, sizeof(canonical_request),
             "%s\n"   /* Method */
             "%s\n"   /* Canonical URI (path) */
             "\n"     /* Canonical Query String (vuota) */
             "%s"     /* Canonical Headers (include \n finale) */
             "\n"     /* Riga vuota */
             "%s\n"   /* Signed Headers */
             "%s",    /* Payload Hash */
             method,
             path,
             canonical_headers,
             signed_headers,
             payload_hash);

    /* 6. Hash del Canonical Request */
    char canonical_request_hash[65];
    sha256_hex((const unsigned char *)canonical_request,
               strlen(canonical_request),
               canonical_request_hash);

    /* 7. Credential Scope */
    char credential_scope[128];
    snprintf(credential_scope, sizeof(credential_scope),
             "%s/%s/%s/aws4_request",
             date_str, R2_REGION, R2_SERVICE);

    /* 8. String to Sign */
    char string_to_sign[1024];
    snprintf(string_to_sign, sizeof(string_to_sign),
             "%s\n"   /* Algoritmo */
             "%s\n"   /* DateTime */
             "%s\n"   /* Credential Scope */
             "%s",    /* Hash Canonical Request */
             R2_ALGORITHM,
             datetime_str,
             credential_scope,
             canonical_request_hash);

    /* 9. Signing Key tramite catena HMAC */
    char aws4_secret[300];
    snprintf(aws4_secret, sizeof(aws4_secret), "AWS4%s", cfg->secret_access_key);

    unsigned char k1[32], k2[32], k3[32], k4[32];
    unsigned int  len;

    hmac_sha256((unsigned char *)aws4_secret, strlen(aws4_secret),
                (unsigned char *)date_str,    strlen(date_str),    k1, &len);
    hmac_sha256(k1, 32, (unsigned char *)R2_REGION,        strlen(R2_REGION),        k2, &len);
    hmac_sha256(k2, 32, (unsigned char *)R2_SERVICE,       strlen(R2_SERVICE),       k3, &len);
    hmac_sha256(k3, 32, (unsigned char *)"aws4_request",   12,                       k4, &len);

    /* 10. Firma finale */
    unsigned char sig_bytes[32];
    hmac_sha256(k4, 32,
                (unsigned char *)string_to_sign, strlen(string_to_sign),
                sig_bytes, &len);

    char sig_hex[65];
    for (int i = 0; i < 32; i++)
        sprintf(sig_hex + i * 2, "%02x", sig_bytes[i]);
    sig_hex[64] = '\0';

    /* 11. Authorization header */
    snprintf(out_auth, 512,
             "%s Credential=%s/%s,SignedHeaders=%s,Signature=%s",
             R2_ALGORITHM,
             cfg->access_key_id,
             credential_scope,
             signed_headers,
             sig_hex);
}

/* =========================================================================
 * r2_get_object — scarica un oggetto da R2
 * ======================================================================= */

unsigned char *r2_get_object(const r2_config_t *cfg, size_t *out_len, int *http_status)
{
    if (!cfg || !out_len || !http_status)
        return NULL;

    *out_len     = 0;
    *http_status = 0;

    /* Costruisce URL e path */
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s/%s",
             cfg->endpoint, cfg->bucket, cfg->object_key);

    char path[512];
    snprintf(path, sizeof(path), "/%s/%s", cfg->bucket, cfg->object_key);

    /* Genera firma */
    char auth_header[512];
    char date_header[17];
    char sha256_header[65];
    sign_request(cfg, "GET", path, NULL, 0,
                 auth_header, date_header, sha256_header);

    /* Costruisce header cURL */
    char hdr_auth[600], hdr_date[64], hdr_sha[96];
    snprintf(hdr_auth,  sizeof(hdr_auth),  "Authorization: %s",          auth_header);
    snprintf(hdr_date,  sizeof(hdr_date),  "x-amz-date: %s",             date_header);
    snprintf(hdr_sha,   sizeof(hdr_sha),   "x-amz-content-sha256: %s",   sha256_header);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr_auth);
    headers = curl_slist_append(headers, hdr_date);
    headers = curl_slist_append(headers, hdr_sha);

    /* Buffer di ricezione */
    curl_buf_t buf = { NULL, 0 };

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    *http_status = (int)http_code;

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        /* Errore di rete */
        free(buf.data);
        *http_status = 0;
        return NULL;
    }

    if (http_code != 200) {
        free(buf.data);
        return NULL;
    }

    *out_len = buf.size;
    return buf.data;
}

/* =========================================================================
 * r2_put_object — carica un oggetto su R2
 * ======================================================================= */

int r2_put_object(const r2_config_t *cfg, const unsigned char *data, size_t len)
{
    if (!cfg || !data)
        return -1;

    /* Costruisce URL e path */
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s/%s",
             cfg->endpoint, cfg->bucket, cfg->object_key);

    char path[512];
    snprintf(path, sizeof(path), "/%s/%s", cfg->bucket, cfg->object_key);

    /* Genera firma con il payload effettivo */
    char auth_header[512];
    char date_header[17];
    char sha256_header[65];
    sign_request(cfg, "PUT", path, data, len,
                 auth_header, date_header, sha256_header);

    /* Costruisce header cURL */
    char hdr_auth[600], hdr_date[64], hdr_sha[96], hdr_ct[64];
    snprintf(hdr_auth, sizeof(hdr_auth), "Authorization: %s",          auth_header);
    snprintf(hdr_date, sizeof(hdr_date), "x-amz-date: %s",             date_header);
    snprintf(hdr_sha,  sizeof(hdr_sha),  "x-amz-content-sha256: %s",   sha256_header);
    snprintf(hdr_ct,   sizeof(hdr_ct),   "Content-Type: application/octet-stream");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr_auth);
    headers = curl_slist_append(headers, hdr_date);
    headers = curl_slist_append(headers, hdr_sha);
    headers = curl_slist_append(headers, hdr_ct);

    /* Buffer di lettura per cURL */
    curl_read_buf_t rbuf = { data, len, 0 };

    /* Buffer di ricezione (risposta PUT, solitamente vuota) */
    curl_buf_t resp = { NULL, 0 };

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,       headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,           1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,     read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA,         &rbuf);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,          60L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(resp.data);

    if (res != CURLE_OK)
        return -1;

    /* R2 risponde 200 o 204 per PUT riuscito */
    return (http_code == 200 || http_code == 204) ? 0 : -1;
}

/* =========================================================================
 * r2_test_connection — HEAD sul bucket per verificare la connessione
 * ======================================================================= */

int r2_test_connection(const r2_config_t *cfg)
{
    if (!cfg)
        return -1;

    /* URL e path puntano solo al bucket, senza object_key */
    char url[1024];
    snprintf(url, sizeof(url), "%s/%s", cfg->endpoint, cfg->bucket);

    char path[512];
    snprintf(path, sizeof(path), "/%s", cfg->bucket);

    /* Genera firma per HEAD con payload vuoto */
    char auth_header[512];
    char date_header[17];
    char sha256_header[65];
    sign_request(cfg, "HEAD", path, NULL, 0,
                 auth_header, date_header, sha256_header);

    /* Costruisce header cURL */
    char hdr_auth[600], hdr_date[64], hdr_sha[96];
    snprintf(hdr_auth, sizeof(hdr_auth), "Authorization: %s",          auth_header);
    snprintf(hdr_date, sizeof(hdr_date), "x-amz-date: %s",             date_header);
    snprintf(hdr_sha,  sizeof(hdr_sha),  "x-amz-content-sha256: %s",   sha256_header);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, hdr_auth);
    headers = curl_slist_append(headers, hdr_date);
    headers = curl_slist_append(headers, hdr_sha);

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,        url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOBODY,     1L);   /* HEAD: nessun body */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,    15L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return -1;

    /*
     * Codici attesi:
     *   200 / 204 — bucket accessibile con credenziali valide
     *   403       — credenziali errate ma bucket raggiungibile
     *   405       — Method Not Allowed (bucket esiste ma non supporta HEAD)
     * Qualsiasi altro codice (404, 50x, ecc.) indica un errore reale.
     */
    return (http_code == 200 || http_code == 204) ? 0 : -1;
}

/* =========================================================================
 * r2_config_load — carica la configurazione da ~/.config/sshpad/r2.json
 * ======================================================================= */

int r2_config_load(r2_config_t *cfg)
{
    if (!cfg)
        return -1;

    /* Azzera la struttura */
    memset(cfg, 0, sizeof(*cfg));

    /* Imposta default object_key */
    strncpy(cfg->object_key, "sshpad-config.spd", sizeof(cfg->object_key) - 1);

    /* Espande il path del file di configurazione */
    char path[512];
    expand_tilde_r2(path, "~/.config/sshpad/r2.json", sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    /* Legge l'intero file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 65536) {
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }

    size_t read_bytes = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* Parsa il JSON */
    struct json_object *root = json_tokener_parse(buf);
    free(buf);

    if (!root)
        return -1;

    /* Estrae i campi */
    struct json_object *val;

    if (json_object_object_get_ex(root, "endpoint", &val) && val)
        strncpy(cfg->endpoint, json_object_get_string(val), sizeof(cfg->endpoint) - 1);

    if (json_object_object_get_ex(root, "accessKeyId", &val) && val)
        strncpy(cfg->access_key_id, json_object_get_string(val), sizeof(cfg->access_key_id) - 1);

    if (json_object_object_get_ex(root, "secretAccessKey", &val) && val)
        strncpy(cfg->secret_access_key, json_object_get_string(val), sizeof(cfg->secret_access_key) - 1);

    if (json_object_object_get_ex(root, "bucket", &val) && val)
        strncpy(cfg->bucket, json_object_get_string(val), sizeof(cfg->bucket) - 1);

    if (json_object_object_get_ex(root, "objectKey", &val) && val)
        strncpy(cfg->object_key, json_object_get_string(val), sizeof(cfg->object_key) - 1);

    json_object_put(root);

    /* Verifica che i campi obbligatori siano presenti */
    if (cfg->endpoint[0] == '\0' || cfg->access_key_id[0] == '\0' ||
        cfg->secret_access_key[0] == '\0' || cfg->bucket[0] == '\0')
        return -1;

    return 0;
}

/* =========================================================================
 * r2_config_save — salva la configurazione su ~/.config/sshpad/r2.json
 * ======================================================================= */

int r2_config_save(const r2_config_t *cfg)
{
    if (!cfg)
        return -1;

    /* Assicura che la directory esista */
    char dir_path[512];
    expand_tilde_r2(dir_path, "~/.config/sshpad", sizeof(dir_path));

    /* Crea directory con permessi 0700 se non esiste */
    if (mkdir(dir_path, 0700) != 0) {
        /* Ignora EEXIST: la directory esiste già */
        /* In caso di altro errore, il fopen successivo fallirà */
    }

    /* Path del file */
    char file_path[512];
    expand_tilde_r2(file_path, "~/.config/sshpad/r2.json", sizeof(file_path));

    /* Costruisce il JSON */
    struct json_object *root = json_object_new_object();
    json_object_object_add(root, "endpoint",        json_object_new_string(cfg->endpoint));
    json_object_object_add(root, "accessKeyId",     json_object_new_string(cfg->access_key_id));
    json_object_object_add(root, "secretAccessKey", json_object_new_string(cfg->secret_access_key));
    json_object_object_add(root, "bucket",          json_object_new_string(cfg->bucket));
    json_object_object_add(root, "objectKey",       json_object_new_string(cfg->object_key));

    const char *json_str = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);

    /* Scrive il file */
    FILE *f = fopen(file_path, "w");
    if (!f) {
        json_object_put(root);
        return -1;
    }

    fputs(json_str, f);
    fclose(f);

    json_object_put(root);

    /* Imposta permessi 0600 (solo owner può leggere/scrivere) */
    chmod(file_path, 0600);

    return 0;
}
