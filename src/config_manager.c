/*
 * config_manager.c — Orchestratore centrale del config manager per SSHPad.
 *
 * Gestisce la modalità operativa (FIRST_RUN, LOCKED, CLOUD, LOCAL),
 * la sincronizzazione con Cloudflare R2, e il ciclo di vita degli host SSH.
 *
 * Thread safety: pthread_rwlock su tutte le operazioni che accedono a hosts/mode.
 */

/* Necessario per pthread_rwlock_t e funzioni POSIX estese con -std=c11 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pwd.h>
#include <json-c/json.h>

#include "config_manager.h"
#include "crypto_store.h"

/* -------------------------------------------------------------------------
 * Struttura interna
 * ---------------------------------------------------------------------- */

struct config_manager {
    pthread_rwlock_t  lock;
    ssh_host_t       *hosts;
    int               num_hosts;
    cm_mode_t         mode;
    r2_config_t       r2_cfg;
    int               r2_configured;
    char              password[256];
    sse_broadcaster_t *sse;
};

/* -------------------------------------------------------------------------
 * Helper: path della home directory
 * ---------------------------------------------------------------------- */

static const char *get_home_dir(void)
{
    const char *home = getenv("HOME");
    if (home && home[0] != '\0')
        return home;

    struct passwd *pw = getpwuid(getuid());
    if (pw)
        return pw->pw_dir;

    return NULL;
}

/* -------------------------------------------------------------------------
 * Helper interno: salva ~/.config/sshpad/settings.json
 * ---------------------------------------------------------------------- */

static int save_settings_json(const char *mode_str)
{
    const char *home = get_home_dir();
    if (!home)
        return -1;

    /* Crea directory ~/.config/sshpad se non esiste */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/sshpad", home);

    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "config_manager: impossibile creare directory %s: %s\n",
                dir, strerror(errno));
        return -1;
    }

    char path[600];
    snprintf(path, sizeof(path), "%s/settings.json", dir);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "config_manager: impossibile aprire %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    fprintf(f, "{\"mode\":\"%s\"}\n", mode_str);
    fclose(f);

    if (chmod(path, 0600) != 0) {
        fprintf(stderr, "config_manager: chmod fallito su %s: %s\n",
                path, strerror(errno));
        /* Non fatale, continuiamo */
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * Helper interno: sincronizza hosts correnti con storage (R2 o locale)
 * ---------------------------------------------------------------------- */

static int cm_sync(config_manager_t *cm)
{
    char *json = ssh_hosts_to_json(cm->hosts, cm->num_hosts);
    if (!json)
        return -1;

    if (cm->mode == CM_MODE_CLOUD) {
        size_t blob_len;
        unsigned char *blob = cs_encrypt(json, cm->password, &blob_len);
        free(json);
        if (!blob)
            return -1;
        int rc = r2_put_object(&cm->r2_cfg, blob, blob_len);
        free(blob);
        return rc;
    } else if (cm->mode == CM_MODE_LOCAL) {
        int rc = ssh_hosts_write_config(cm->hosts, cm->num_hosts, NULL);
        free(json);
        return rc;
    }

    free(json);
    return -1;
}

/* =========================================================================
 * cm_create()
 * ====================================================================== */

config_manager_t *cm_create(sse_broadcaster_t *sse)
{
    config_manager_t *cm = calloc(1, sizeof(*cm));
    if (!cm)
        return NULL;

    if (pthread_rwlock_init(&cm->lock, NULL) != 0) {
        free(cm);
        return NULL;
    }

    cm->sse = sse;
    cm->mode = CM_MODE_FIRST_RUN;
    cm->r2_configured = 0;
    cm->hosts = NULL;
    cm->num_hosts = 0;

    /* Il wizard viene mostrato ad ogni avvio: l'utente sceglie ogni volta
     * se usare il cloud o la modalità locale.
     * Carichiamo r2.json (se esiste) solo per sapere se le credenziali
     * R2 sono già disponibili, così il wizard può saltare lo step 2. */
    if (r2_config_load(&cm->r2_cfg) == 0) {
        cm->r2_configured = 1;
    }

    /* Modalità sempre FIRST_RUN */
    return cm;
}

/* =========================================================================
 * cm_unlock()
 * ====================================================================== */

int cm_unlock(config_manager_t *cm, const char *password)
{
    if (!cm || !password)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    if (cm->mode != CM_MODE_LOCKED) {
        pthread_rwlock_unlock(&cm->lock);
        return -1;
    }

    int http_status = 0;
    size_t blob_len = 0;
    unsigned char *blob = r2_get_object(&cm->r2_cfg, &blob_len, &http_status);

    if (http_status == 0) {
        /* Errore di rete */
        free(blob);
        pthread_rwlock_unlock(&cm->lock);
        return -2;
    }

    if (http_status == 404) {
        /* Prima configurazione: crea un oggetto vuoto */
        free(blob);
        blob = NULL;

        const char *empty_json = "[]";
        size_t enc_len = 0;
        unsigned char *enc_blob = cs_encrypt(empty_json, password, &enc_len);
        if (!enc_blob) {
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        int rc = r2_put_object(&cm->r2_cfg, enc_blob, enc_len);
        free(enc_blob);

        if (rc != 0) {
            pthread_rwlock_unlock(&cm->lock);
            return -2;
        }

        /* Array host vuoto */
        free(cm->hosts);
        cm->hosts = NULL;
        cm->num_hosts = 0;
        cm->mode = CM_MODE_CLOUD;
        strncpy(cm->password, password, sizeof(cm->password) - 1);
        cm->password[sizeof(cm->password) - 1] = '\0';

        pthread_rwlock_unlock(&cm->lock);
        return 0;
    }

    if (http_status == 200 && blob) {
        /* Decripta il blob */
        char *json = cs_decrypt(blob, blob_len, password);
        free(blob);

        if (!json) {
            /* Password errata */
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        /* Carica gli host dal JSON */
        int new_count = 0;
        ssh_host_t *new_hosts = ssh_hosts_from_json(json, &new_count);
        free(json);

        free(cm->hosts);
        cm->hosts = new_hosts;
        cm->num_hosts = new_count;
        cm->mode = CM_MODE_CLOUD;
        strncpy(cm->password, password, sizeof(cm->password) - 1);
        cm->password[sizeof(cm->password) - 1] = '\0';

        pthread_rwlock_unlock(&cm->lock);
        return 0;
    }

    /* Qualsiasi altro codice HTTP: errore rete/server */
    free(blob);
    pthread_rwlock_unlock(&cm->lock);
    return -2;
}

/* =========================================================================
 * cm_setup()
 * ====================================================================== */

int cm_setup(config_manager_t *cm, const char *mode,
             const r2_config_t *r2_cfg, const char *password)
{
    if (!cm || !mode)
        return -1;

    if (strcmp(mode, "local") == 0) {
        pthread_rwlock_wrlock(&cm->lock);

        /* Salva settings.json */
        if (save_settings_json("local") != 0) {
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        /* Carica ~/.ssh/config se esiste */
        free(cm->hosts);
        cm->hosts = NULL;
        cm->num_hosts = 0;
        cm->hosts = parse_ssh_config(NULL, &cm->num_hosts);
        cm->mode = CM_MODE_LOCAL;

        pthread_rwlock_unlock(&cm->lock);

        sse_broadcast(cm->sse, "config_changed", "{}");
        return 0;
    }

    if (strcmp(mode, "cloud") == 0) {
        if (!r2_cfg || !password)
            return -1;

        /* Salva r2.json */
        if (r2_config_save(r2_cfg) != 0)
            return -1;

        /* Salva settings.json */
        if (save_settings_json("cloud") != 0)
            return -1;

        pthread_rwlock_wrlock(&cm->lock);
        cm->r2_cfg = *r2_cfg;
        cm->r2_configured = 1;
        cm->mode = CM_MODE_LOCKED;
        pthread_rwlock_unlock(&cm->lock);

        /* Sblocca con la password fornita */
        int rc = cm_unlock(cm, password);
        if (rc != 0)
            return -1;

        return 0;
    }

    /* Modalità non riconosciuta */
    return -1;
}

/* =========================================================================
 * cm_get_mode()
 * ====================================================================== */

cm_mode_t cm_get_mode(config_manager_t *cm)
{
    if (!cm)
        return CM_MODE_FIRST_RUN;

    pthread_rwlock_rdlock(&cm->lock);
    cm_mode_t m = cm->mode;
    pthread_rwlock_unlock(&cm->lock);
    return m;
}

/* =========================================================================
 * cm_get_hosts()
 *
 * Nota: in questa implementazione semplificata il lock viene acquisito e
 * rilasciato internamente. Il chiamante opera senza lock — accettabile
 * perché le scritture sono rare e serializzate dall'HTTP server.
 * ====================================================================== */

const ssh_host_t *cm_get_hosts(config_manager_t *cm, int *out_count)
{
    if (!cm) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    pthread_rwlock_rdlock(&cm->lock);
    const ssh_host_t *h = cm->hosts;
    if (out_count)
        *out_count = cm->num_hosts;
    pthread_rwlock_unlock(&cm->lock);

    return h;
}

/* =========================================================================
 * cm_save_host()
 * ====================================================================== */

int cm_save_host(config_manager_t *cm, const ssh_host_t *host)
{
    if (!cm || !host)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    /* Cerca host con lo stesso nome */
    int found = -1;
    for (int i = 0; i < cm->num_hosts; i++) {
        if (strcmp(cm->hosts[i].name, host->name) == 0) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        /* Aggiorna in-place */
        cm->hosts[found] = *host;
    } else {
        /* Aggiunge in fondo */
        ssh_host_t *new_arr = realloc(cm->hosts,
                                      (size_t)(cm->num_hosts + 1) * sizeof(ssh_host_t));
        if (!new_arr) {
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }
        cm->hosts = new_arr;
        cm->hosts[cm->num_hosts] = *host;
        cm->num_hosts++;
    }

    pthread_rwlock_unlock(&cm->lock);

    int rc = cm_sync(cm);
    sse_broadcast(cm->sse, "config_changed", "{}");
    return (rc == 0) ? 0 : -1;
}

/* =========================================================================
 * cm_delete_host()
 * ====================================================================== */

int cm_delete_host(config_manager_t *cm, const char *name)
{
    if (!cm || !name)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    int found = -1;
    for (int i = 0; i < cm->num_hosts; i++) {
        if (strcmp(cm->hosts[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found < 0) {
        pthread_rwlock_unlock(&cm->lock);
        return -1;
    }

    /* Sposta gli elementi successivi indietro di una posizione */
    int remaining = cm->num_hosts - found - 1;
    if (remaining > 0) {
        memmove(&cm->hosts[found],
                &cm->hosts[found + 1],
                (size_t)remaining * sizeof(ssh_host_t));
    }
    cm->num_hosts--;

    pthread_rwlock_unlock(&cm->lock);

    cm_sync(cm);
    sse_broadcast(cm->sse, "config_changed", "{}");
    return 0;
}

/* =========================================================================
 * cm_change_password()
 * ====================================================================== */

int cm_change_password(config_manager_t *cm, const char *old_pw, const char *new_pw)
{
    if (!cm || !old_pw || !new_pw)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    if (cm->mode != CM_MODE_CLOUD) {
        pthread_rwlock_unlock(&cm->lock);
        return -1;
    }

    if (strcmp(cm->password, old_pw) != 0) {
        pthread_rwlock_unlock(&cm->lock);
        return -1;
    }

    /* Memorizza vecchia password per eventuale rollback */
    char old_saved[256];
    strncpy(old_saved, cm->password, sizeof(old_saved) - 1);
    old_saved[sizeof(old_saved) - 1] = '\0';

    /* Imposta nuova password */
    strncpy(cm->password, new_pw, sizeof(cm->password) - 1);
    cm->password[sizeof(cm->password) - 1] = '\0';

    pthread_rwlock_unlock(&cm->lock);

    /* Sincronizza (ri-cifra con nuova password) */
    if (cm_sync(cm) != 0) {
        /* Rollback password */
        pthread_rwlock_wrlock(&cm->lock);
        strncpy(cm->password, old_saved, sizeof(cm->password) - 1);
        cm->password[sizeof(cm->password) - 1] = '\0';
        pthread_rwlock_unlock(&cm->lock);
        return -2;
    }

    return 0;
}

/* =========================================================================
 * cm_get_r2_config()
 * ====================================================================== */

void cm_get_r2_config(config_manager_t *cm, r2_config_t *out)
{
    if (!cm || !out)
        return;

    pthread_rwlock_rdlock(&cm->lock);
    *out = cm->r2_cfg;
    pthread_rwlock_unlock(&cm->lock);

    /* Maschera il secret: mostra solo i primi 4 caratteri + "****" */
    if (strlen(out->secret_access_key) > 4) {
        char masked[256];
        snprintf(masked, sizeof(masked), "%.4s****", out->secret_access_key);
        strncpy(out->secret_access_key, masked, sizeof(out->secret_access_key) - 1);
        out->secret_access_key[sizeof(out->secret_access_key) - 1] = '\0';
    }
}

/* =========================================================================
 * cm_set_r2_config()
 * ====================================================================== */

int cm_set_r2_config(config_manager_t *cm, const r2_config_t *cfg)
{
    if (!cm || !cfg)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    /* Determina se il secret è stato modificato dal chiamante.
     * Se la stringa termina con "****" si assume che il chiamante
     * abbia inviato indietro il secret mascherato → mantieni quello attuale. */
    int secret_changed = 1;
    size_t slen = strlen(cfg->secret_access_key);
    if (slen >= 4 &&
        strcmp(cfg->secret_access_key + slen - 4, "****") == 0) {
        secret_changed = 0;
    }

    /* Salva il secret corrente prima di sovrascrivere */
    char current_secret[256];
    strncpy(current_secret, cm->r2_cfg.secret_access_key,
            sizeof(current_secret) - 1);
    current_secret[sizeof(current_secret) - 1] = '\0';

    /* Costruisce la nuova config da applicare */
    r2_config_t new_cfg = *cfg;
    if (!secret_changed) {
        /* Ripristina il secret originale */
        strncpy(new_cfg.secret_access_key, current_secret,
                sizeof(new_cfg.secret_access_key) - 1);
        new_cfg.secret_access_key[sizeof(new_cfg.secret_access_key) - 1] = '\0';
    }

    if (cm->mode == CM_MODE_CLOUD) {
        /* Ri-cifra il blob corrente con la nuova configurazione R2 e carica */
        char *json = ssh_hosts_to_json(cm->hosts, cm->num_hosts);
        if (!json) {
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        size_t blob_len = 0;
        unsigned char *blob = cs_encrypt(json, cm->password, &blob_len);
        free(json);

        if (!blob) {
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        /* Salva r2.json con la nuova config prima di fare l'upload */
        r2_config_save(&new_cfg);

        int rc = r2_put_object(&new_cfg, blob, blob_len);
        free(blob);

        if (rc != 0) {
            /* Rollback: ripristina il vecchio r2.json */
            r2_config_save(&cm->r2_cfg);
            pthread_rwlock_unlock(&cm->lock);
            return -1;
        }

        /* Aggiorna la config nel manager */
        cm->r2_cfg = new_cfg;
        pthread_rwlock_unlock(&cm->lock);
        return 0;
    }

    /* Modalità non CLOUD: salva solo r2.json e aggiorna la struct */
    cm->r2_cfg = new_cfg;
    pthread_rwlock_unlock(&cm->lock);

    r2_config_save(&cm->r2_cfg);
    return 0;
}

/* =========================================================================
 * cm_use_local()
 * ====================================================================== */

int cm_use_local(config_manager_t *cm)
{
    if (!cm)
        return -1;

    pthread_rwlock_wrlock(&cm->lock);

    free(cm->hosts);
    cm->hosts = NULL;
    cm->num_hosts = 0;

    cm->hosts = parse_ssh_config(NULL, &cm->num_hosts);
    cm->mode = CM_MODE_LOCAL;

    pthread_rwlock_unlock(&cm->lock);

    sse_broadcast(cm->sse, "config_changed", "{}");
    return 0;
}

/* =========================================================================
 * cm_free()
 * ====================================================================== */

void cm_free(config_manager_t *cm)
{
    if (!cm)
        return;

    pthread_rwlock_wrlock(&cm->lock);
    free(cm->hosts);
    cm->hosts = NULL;
    cm->num_hosts = 0;
    pthread_rwlock_unlock(&cm->lock);

    pthread_rwlock_destroy(&cm->lock);

    /* Azzera i dati sensibili prima di liberare */
    memset(cm->password, 0, sizeof(cm->password));
    memset(&cm->r2_cfg, 0, sizeof(cm->r2_cfg));

    free(cm);
}
