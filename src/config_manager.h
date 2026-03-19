#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include "config_parser.h"
#include "r2_client.h"
#include "sse.h"

typedef enum {
    CM_MODE_FIRST_RUN,
    CM_MODE_LOCKED,
    CM_MODE_CLOUD,
    CM_MODE_LOCAL
} cm_mode_t;

typedef struct config_manager config_manager_t;

/* Crea il config manager. Determina la modalità iniziale leggendo settings.json. */
config_manager_t *cm_create(sse_broadcaster_t *sse);

/* Sblocca con password. -1 = password errata, -2 = errore rete, 0 = OK. */
int cm_unlock(config_manager_t *cm, const char *password);

/* Completa wizard primo avvio. mode = "cloud" o "local".
 * Se cloud: r2_cfg e password obbligatori. Ritorna 0 OK, -1 errore. */
int cm_setup(config_manager_t *cm, const char *mode,
             const r2_config_t *r2_cfg, const char *password);

/* Ritorna modalità corrente. */
cm_mode_t cm_get_mode(config_manager_t *cm);

/* Ritorna puntatore all'array host. Il chiamante deve tenere il lock (read).
 * Uso: cm_get_hosts(cm, &count) → itera → NON salvare il puntatore oltre il thread. */
const ssh_host_t *cm_get_hosts(config_manager_t *cm, int *out_count);

/* Crea o aggiorna host (matching per name). Ritorna 0 OK, -1 errore. */
int cm_save_host(config_manager_t *cm, const ssh_host_t *host);

/* Elimina host per nome. Ritorna 0 OK, -1 non trovato. */
int cm_delete_host(config_manager_t *cm, const char *name);

/* Cambia password master. Ritorna 0 OK, -1 password vecchia errata, -2 errore sync. */
int cm_change_password(config_manager_t *cm, const char *old_pw, const char *new_pw);

/* Copia config R2 corrente in out (secret mascherata: primi 4 char + "****"). */
void cm_get_r2_config(config_manager_t *cm, r2_config_t *out);

/* Aggiorna config R2. Se in mode=CLOUD, ri-sincronizza blob. Ritorna 0 OK, -1 errore. */
int cm_set_r2_config(config_manager_t *cm, const r2_config_t *cfg);

/* Bypassa R2 per questa sessione, carica ~/.ssh/config → mode=LOCAL. Ritorna 0. */
int cm_use_local(config_manager_t *cm);

/* Libera tutte le risorse. */
void cm_free(config_manager_t *cm);

#endif /* CONFIG_MANAGER_H */
