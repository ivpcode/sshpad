/*
 * crypto_store.c — Crittografia AES-256-GCM per SSHPad
 *
 * Formato file SPD:
 *   Offset  Byte  Campo
 *   0       4     Magic: "SPD\x01"
 *   4       4     Iterazioni PBKDF2 (uint32 big-endian)
 *   8       32    Salt (casuale)
 *   40      12    IV/Nonce (casuale)
 *   52      N     Ciphertext (AES-256-GCM)
 *   52+N    16    GCM Authentication Tag
 *
 * Derivazione chiave: PBKDF2-HMAC-SHA256(password, salt, iter) → 32 byte
 */

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "crypto_store.h"

/* Costanti del formato SPD */
#define SPD_MAGIC        "SPD\x01"
#define SPD_MAGIC_LEN    4
#define SPD_ITER_LEN     4
#define SPD_SALT_LEN     32
#define SPD_IV_LEN       12
#define SPD_TAG_LEN      16
#define SPD_HEADER_LEN   (SPD_MAGIC_LEN + SPD_ITER_LEN + SPD_SALT_LEN + SPD_IV_LEN) /* 52 */
#define SPD_MIN_LEN      (SPD_HEADER_LEN + SPD_TAG_LEN) /* 68 */
#define SPD_DEFAULT_ITER 600000

/*
 * cs_encrypt — Cifra il plaintext UTF-8 e restituisce un blob SPD allocato.
 * Il chiamante è responsabile di free() sul puntatore restituito.
 * Ritorna NULL in caso di errore.
 */
unsigned char *cs_encrypt(const char *plaintext, const char *password, size_t *out_len)
{
    if (!plaintext || !password || !out_len)
        return NULL;

    int plaintext_len = (int)strlen(plaintext);

    unsigned char salt[SPD_SALT_LEN];
    unsigned char iv[SPD_IV_LEN];
    unsigned char key[32];

    /* Genera salt e IV casuali */
    if (RAND_bytes(salt, SPD_SALT_LEN) != 1)
        return NULL;
    if (RAND_bytes(iv, SPD_IV_LEN) != 1)
        return NULL;

    /* Deriva la chiave con PBKDF2-HMAC-SHA256 */
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                           salt, SPD_SALT_LEN,
                           SPD_DEFAULT_ITER,
                           EVP_sha256(),
                           32, key) != 1)
        return NULL;

    /* Alloca buffer per il ciphertext (al massimo uguale al plaintext in GCM) */
    unsigned char *ciphertext = malloc((size_t)plaintext_len);
    if (!ciphertext)
        return NULL;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(ciphertext);
        return NULL;
    }

    int len = 0, final_len = 0;
    int ok = 1;

    /* Inizializzazione cifratura AES-256-GCM */
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, SPD_IV_LEN, NULL);
    ok &= EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv);

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return NULL;
    }

    /* Cifra il plaintext */
    if (EVP_EncryptUpdate(ctx, ciphertext, &len,
                          (const unsigned char *)plaintext, plaintext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return NULL;
    }

    if (EVP_EncryptFinal_ex(ctx, ciphertext + len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return NULL;
    }

    int ciphertext_len = len + final_len;

    /* Recupera il tag GCM */
    unsigned char tag[SPD_TAG_LEN];
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, SPD_TAG_LEN, tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(ciphertext);
        return NULL;
    }

    EVP_CIPHER_CTX_free(ctx);

    /* Calcola dimensione totale del blob SPD */
    size_t total_len = (size_t)(SPD_HEADER_LEN + ciphertext_len + SPD_TAG_LEN);
    unsigned char *blob = malloc(total_len);
    if (!blob) {
        free(ciphertext);
        return NULL;
    }

    /* Costruisce il blob SPD */
    unsigned char *ptr = blob;

    /* Magic "SPD\x01" */
    memcpy(ptr, SPD_MAGIC, SPD_MAGIC_LEN);
    ptr += SPD_MAGIC_LEN;

    /* Iterazioni come uint32 big-endian: 600000 = 0x000927C0 */
    uint32_t iter_be = SPD_DEFAULT_ITER;
    ptr[0] = (unsigned char)((iter_be >> 24) & 0xFF);
    ptr[1] = (unsigned char)((iter_be >> 16) & 0xFF);
    ptr[2] = (unsigned char)((iter_be >>  8) & 0xFF);
    ptr[3] = (unsigned char)( iter_be        & 0xFF);
    ptr += SPD_ITER_LEN;

    /* Salt (32 byte) */
    memcpy(ptr, salt, SPD_SALT_LEN);
    ptr += SPD_SALT_LEN;

    /* IV/Nonce (12 byte) */
    memcpy(ptr, iv, SPD_IV_LEN);
    ptr += SPD_IV_LEN;

    /* Ciphertext */
    memcpy(ptr, ciphertext, (size_t)ciphertext_len);
    ptr += ciphertext_len;

    /* GCM Authentication Tag (16 byte) */
    memcpy(ptr, tag, SPD_TAG_LEN);

    free(ciphertext);

    *out_len = total_len;
    return blob;
}

/*
 * cs_decrypt — Decripta un blob SPD e restituisce una stringa JSON null-terminated.
 * Il chiamante è responsabile di free() sul puntatore restituito.
 * Ritorna NULL se la password è errata o il blob è invalido.
 */
char *cs_decrypt(const unsigned char *blob, size_t blob_len, const char *password)
{
    if (!blob || !password)
        return NULL;

    /* Valida lunghezza minima */
    if (blob_len < SPD_MIN_LEN)
        return NULL;

    /* Valida magic */
    if (memcmp(blob, SPD_MAGIC, SPD_MAGIC_LEN) != 0)
        return NULL;

    /* Leggi iterazioni come uint32 big-endian dall'offset 4 */
    uint32_t iter = ((uint32_t)blob[4] << 24)
                  | ((uint32_t)blob[5] << 16)
                  | ((uint32_t)blob[6] <<  8)
                  |  (uint32_t)blob[7];

    /* Valida le iterazioni */
    if (iter == 0 || iter > 10000000)
        return NULL;

    /* Estrai i campi dal blob */
    const unsigned char *salt       = blob + 8;                  /* 32 byte */
    const unsigned char *iv         = blob + 40;                 /* 12 byte */
    const unsigned char *ciphertext = blob + SPD_HEADER_LEN;    /* N byte  */
    size_t ciphertext_len           = blob_len - SPD_HEADER_LEN - SPD_TAG_LEN;
    const unsigned char *tag        = blob + blob_len - SPD_TAG_LEN; /* 16 byte */

    /* Deriva la chiave con PBKDF2-HMAC-SHA256 */
    unsigned char key[32];
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password),
                           salt, SPD_SALT_LEN,
                           (int)iter,
                           EVP_sha256(),
                           32, key) != 1)
        return NULL;

    /* Alloca buffer per il plaintext (+1 per il null terminator) */
    unsigned char *plaintext = malloc(ciphertext_len + 1);
    if (!plaintext)
        return NULL;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        free(plaintext);
        return NULL;
    }

    int len = 0, final_len = 0;
    int ok = 1;

    /* Inizializzazione decifratura AES-256-GCM */
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, SPD_IV_LEN, NULL);
    ok &= EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv);

    if (!ok) {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return NULL;
    }

    /* Decifra il ciphertext */
    if (EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, (int)ciphertext_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return NULL;
    }

    /* Imposta il tag GCM per la verifica (cast away const richiesto dall'API OpenSSL) */
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, SPD_TAG_LEN, (void *)tag) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        free(plaintext);
        return NULL;
    }

    /* Verifica tag e finalizza: se fallisce la password è errata o i dati sono corrotti */
    int verify_ok = EVP_DecryptFinal_ex(ctx, plaintext + len, &final_len);
    EVP_CIPHER_CTX_free(ctx);

    if (verify_ok <= 0) {
        free(plaintext);
        return NULL;
    }

    /* Aggiunge null terminator */
    plaintext[len + final_len] = '\0';

    return (char *)plaintext;
}
