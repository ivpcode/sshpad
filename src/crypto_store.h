#ifndef CRYPTO_STORE_H
#define CRYPTO_STORE_H
#include <stddef.h>

/* Cripta plaintext JSON → blob SPD. Il chiamante deve free() il risultato.
 * Ritorna NULL in caso di errore. */
unsigned char *cs_encrypt(const char *plaintext, const char *password, size_t *out_len);

/* Decripta blob SPD → stringa JSON (null-terminated). Il chiamante deve free().
 * Ritorna NULL se la password è errata o il blob è invalido. */
char *cs_decrypt(const unsigned char *blob, size_t blob_len, const char *password);

#endif /* CRYPTO_STORE_H */
