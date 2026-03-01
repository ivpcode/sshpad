#ifndef ASKPASS_H
#define ASKPASS_H

/* Inizializza il meccanismo SSH_ASKPASS.
 * Crea un script helper temporaneo che comunica con sshpad via HTTP.
 * askpass_path_out: buffer dove scrivere il path dello script (deve essere >= 512 byte).
 * server_port: porta del server HTTP locale. */
int askpass_init(char *askpass_path_out, int server_port);

/* Consegna la password per una richiesta askpass in corso.
 * request_id: UUID della richiesta.
 * password: la password da consegnare. */
int askpass_deliver_password(const char *request_id, const char *password);

/* Pulizia: rimuove lo script askpass temporaneo. */
void askpass_cleanup(const char *askpass_path);

/* Gestisce la richiesta HTTP interna GET /api/internal/askpass?id=UUID
 * Blocca finché non arriva la password via askpass_deliver_password.
 * Ritorna la password (allocata con malloc, da liberare con free) o NULL. */
char *askpass_wait_for_password(const char *request_id);

#endif /* ASKPASS_H */
