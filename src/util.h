#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/* Genera un UUID v4 in formato stringa (36 caratteri + NUL).
 * Il buffer out deve essere almeno 37 byte. */
void uuid_generate_v4(char *out);

/* Base64 encode. Ritorna stringa allocata (da liberare con free).
 * out_len (opzionale) riceve la lunghezza del risultato. */
char *base64_encode(const unsigned char *data, size_t len, size_t *out_len);

/* Azzera un buffer di memoria in modo sicuro (non ottimizzabile dal compiler). */
void secure_zero(void *buf, size_t len);

#endif /* UTIL_H */
