#ifndef LOCAL_PROXY_H
#define LOCAL_PROXY_H

#include "config_parser.h"
#include "sse.h"

typedef struct local_proxy local_proxy_t;

/* Verifica che mkcert sia installato. Ritorna 0 se OK, -1 se assente. */
int lp_check_mkcert(void);

/* Crea il proxy: genera certificati, aggiorna /etc/hosts.
 * Ritorna NULL se fallisce o se nessun host ha LocalForward. */
local_proxy_t *lp_create(const ssh_host_t *hosts, int num_hosts,
                          sse_broadcaster_t *sse);

/* Avvia il listener sulla porta 443 (fallback 8443). */
int  lp_start(local_proxy_t *lp);

/* Ferma il proxy e chiude tutte le connessioni. */
void lp_stop(local_proxy_t *lp);

/* Libera tutte le risorse. */
void lp_free(local_proxy_t *lp);

#endif /* LOCAL_PROXY_H */
