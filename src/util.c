#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

void uuid_generate_v4(char *out) {
    unsigned char b[16];

    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t nr = fread(b, 1, sizeof(b), f);
        if (nr != sizeof(b)) memset(b, 0, sizeof(b));
        fclose(f);
    } else {
        /* Fallback: zero-fill (non sicuro, ma evita UB) */
        memset(b, 0, sizeof(b));
    }

    /* Imposta version 4 */
    b[6] = (b[6] & 0x0f) | 0x40;
    /* Imposta variant RFC 4122 */
    b[8] = (b[8] & 0x3f) | 0x80;

    snprintf(out, 37,
             "%02x%02x%02x%02x-"
             "%02x%02x-"
             "%02x%02x-"
             "%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             b[0],  b[1],  b[2],  b[3],
             b[4],  b[5],
             b[6],  b[7],
             b[8],  b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
}

char *base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /* Output length: ceil(len / 3) * 4, plus NUL */
    size_t encoded_len = ((len + 2) / 3) * 4;
    char *out = malloc(encoded_len + 1);
    if (!out)
        return NULL;

    size_t i = 0, j = 0;
    while (i + 2 < len) {
        out[j++] = table[(data[i] >> 2) & 0x3f];
        out[j++] = table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0f)];
        out[j++] = table[((data[i + 1] & 0x0f) << 2) | ((data[i + 2] >> 6) & 0x03)];
        out[j++] = table[data[i + 2] & 0x3f];
        i += 3;
    }

    /* Gestione padding */
    if (i < len) {
        out[j++] = table[(data[i] >> 2) & 0x3f];
        if (i + 1 < len) {
            out[j++] = table[((data[i] & 0x03) << 4) | ((data[i + 1] >> 4) & 0x0f)];
            out[j++] = table[(data[i + 1] & 0x0f) << 2];
        } else {
            out[j++] = table[(data[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }

    out[j] = '\0';
    if (out_len)
        *out_len = encoded_len;

    return out;
}

void secure_zero(void *buf, size_t len) {
    volatile unsigned char *p = buf;
    while (len--)
        *p++ = 0;
}
