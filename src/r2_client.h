#ifndef R2_CLIENT_H
#define R2_CLIENT_H

/*
 * r2_client.h — Interfaccia client S3/R2 con AWS Signature V4 per SSHPad.
 *
 * Supporta upload/download di oggetti su Cloudflare R2 tramite l'API
 * compatibile S3, utilizzando la firma AWS Signature V4.
 */

#include <stddef.h>

typedef struct {
    char endpoint[512];         /* "https://<account>.r2.cloudflarestorage.com" */
    char access_key_id[128];
    char secret_access_key[256];
    char bucket[128];
    char object_key[256];       /* default: "sshpad-config.spd" */
} r2_config_t;

/* Carica config da ~/.config/sshpad/r2.json. Ritorna 0 OK, -1 errore. */
int r2_config_load(r2_config_t *cfg);

/* Salva config su ~/.config/sshpad/r2.json (permessi 0600). Ritorna 0 OK. */
int r2_config_save(const r2_config_t *cfg);

/* Scarica oggetto. Caller deve free() il risultato. http_status riceve il codice HTTP.
 * Ritorna NULL in caso di errore di rete (http_status = 0) o 404. */
unsigned char *r2_get_object(const r2_config_t *cfg, size_t *out_len, int *http_status);

/* Carica oggetto. Ritorna 0 OK, -1 errore. */
int r2_put_object(const r2_config_t *cfg, const unsigned char *data, size_t len);

/* Testa connessione con HEAD sul bucket. Ritorna 0 OK, -1 errore. */
int r2_test_connection(const r2_config_t *cfg);

#endif /* R2_CLIENT_H */
