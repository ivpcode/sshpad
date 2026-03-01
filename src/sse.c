#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#include "sse.h"

#define MAX_SSE_CLIENTS 32
#define SSE_BUF_SIZE    4096

typedef struct {
    int  pipe_fd[2]; /* pipe: [0]=read (MHD legge), [1]=write (noi scriviamo) */
    int  active;
} sse_client_t;

struct sse_broadcaster {
    pthread_mutex_t  mutex;
    sse_client_t     clients[MAX_SSE_CLIENTS];
    int              num_clients;
};

/* -------------------------------------------------------------------------
 * sse_broadcaster_create
 * ---------------------------------------------------------------------- */
sse_broadcaster_t *sse_broadcaster_create(void)
{
    sse_broadcaster_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    if (pthread_mutex_init(&b->mutex, NULL) != 0) {
        free(b);
        return NULL;
    }

    return b;
}

/* -------------------------------------------------------------------------
 * Callback reader chiamata da MHD per leggere dati da inviare al client SSE.
 * cls  -> sse_client_t *
 * pos  -> offset (ignorato per SSE)
 * buf  -> buffer di output fornito da MHD
 * max  -> dimensione massima del buffer
 * ---------------------------------------------------------------------- */
static ssize_t sse_reader_callback(void *cls, uint64_t pos,
                                   char *buf, size_t max)
{
    sse_client_t *client = (sse_client_t *)cls;

    (void)pos; /* non usato */

    if (!client->active)
        return MHD_CONTENT_READER_END_OF_STREAM;

    ssize_t n = read(client->pipe_fd[0], buf, max);

    if (n > 0)
        return n;

    if (n == 0) {
        /* Pipe chiusa: il broadcaster ha terminato questo client */
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    /* n < 0 */
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Nessun dato disponibile: dormi per evitare busy loop */
        usleep(50000); /* 50 ms */
        return 0;      /* Segnala a MHD di riprovare */
    }

    /* Errore reale */
    return MHD_CONTENT_READER_END_WITH_ERROR;
}

/* -------------------------------------------------------------------------
 * Callback free chiamata da MHD quando la connessione viene chiusa.
 * ---------------------------------------------------------------------- */
static void sse_free_callback(void *cls)
{
    sse_client_t *client = (sse_client_t *)cls;
    client->active = 0;

    if (client->pipe_fd[0] >= 0) {
        close(client->pipe_fd[0]);
        client->pipe_fd[0] = -1;
    }
    if (client->pipe_fd[1] >= 0) {
        close(client->pipe_fd[1]);
        client->pipe_fd[1] = -1;
    }
}

/* -------------------------------------------------------------------------
 * sse_broadcaster_add_client
 * ---------------------------------------------------------------------- */
enum MHD_Result sse_broadcaster_add_client(sse_broadcaster_t *b,
                                            struct MHD_Connection *conn)
{
    pthread_mutex_lock(&b->mutex);

    if (b->num_clients >= MAX_SSE_CLIENTS) {
        pthread_mutex_unlock(&b->mutex);
        return MHD_NO;
    }

    sse_client_t *client = &b->clients[b->num_clients];
    memset(client, 0, sizeof(*client));
    client->pipe_fd[0] = -1;
    client->pipe_fd[1] = -1;

    if (pipe(client->pipe_fd) != 0) {
        pthread_mutex_unlock(&b->mutex);
        return MHD_NO;
    }

    /* Rendi il lato lettura non-bloccante per evitare che MHD si blocchi */
    {
        int flags = fcntl(client->pipe_fd[0], F_GETFL, 0);
        if (flags == -1)
            flags = 0;
        fcntl(client->pipe_fd[0], F_SETFL, flags | O_NONBLOCK);
    }

    client->active = 1;
    b->num_clients++;

    pthread_mutex_unlock(&b->mutex);

    /* Crea la risposta MHD con il reader callback */
    struct MHD_Response *resp =
        MHD_create_response_from_callback(MHD_SIZE_UNKNOWN,
                                          SSE_BUF_SIZE,
                                          sse_reader_callback,
                                          client,
                                          sse_free_callback);
    if (!resp) {
        pthread_mutex_lock(&b->mutex);
        client->active = 0;
        close(client->pipe_fd[0]);
        close(client->pipe_fd[1]);
        client->pipe_fd[0] = -1;
        client->pipe_fd[1] = -1;
        b->num_clients--;
        pthread_mutex_unlock(&b->mutex);
        return MHD_NO;
    }

    MHD_add_response_header(resp, "Content-Type",      "text/event-stream");
    MHD_add_response_header(resp, "Cache-Control",     "no-cache");
    MHD_add_response_header(resp, "Connection",        "keep-alive");
    MHD_add_response_header(resp, "X-Accel-Buffering", "no");

    enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);

    return ret;
}

/* -------------------------------------------------------------------------
 * sse_broadcast
 * Formatta "event: <event>\ndata: <json_data>\n\n" e scrive su tutti i
 * pipe dei client attivi.
 * ---------------------------------------------------------------------- */
void sse_broadcast(sse_broadcaster_t *b, const char *event,
                   const char *json_data)
{
    char msg[SSE_BUF_SIZE];
    int  msg_len = snprintf(msg, sizeof(msg),
                            "event: %s\ndata: %s\n\n",
                            event, json_data);

    if (msg_len <= 0 || (size_t)msg_len >= sizeof(msg))
        return;

    pthread_mutex_lock(&b->mutex);

    for (int i = 0; i < b->num_clients; i++) {
        sse_client_t *c = &b->clients[i];
        if (!c->active || c->pipe_fd[1] < 0)
            continue;

        ssize_t written = 0;
        ssize_t remaining = msg_len;
        const char *ptr = msg;

        while (remaining > 0) {
            written = write(c->pipe_fd[1], ptr, (size_t)remaining);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                /* Pipe rotta o errore: disattiva il client */
                c->active = 0;
                break;
            }
            ptr += written;
            remaining -= written;
        }
    }

    pthread_mutex_unlock(&b->mutex);
}

/* -------------------------------------------------------------------------
 * sse_broadcaster_free
 * ---------------------------------------------------------------------- */
void sse_broadcaster_free(sse_broadcaster_t *b)
{
    if (!b)
        return;

    pthread_mutex_lock(&b->mutex);

    for (int i = 0; i < b->num_clients; i++) {
        sse_client_t *c = &b->clients[i];
        c->active = 0;

        if (c->pipe_fd[0] >= 0) {
            close(c->pipe_fd[0]);
            c->pipe_fd[0] = -1;
        }
        if (c->pipe_fd[1] >= 0) {
            close(c->pipe_fd[1]);
            c->pipe_fd[1] = -1;
        }
    }

    pthread_mutex_unlock(&b->mutex);
    pthread_mutex_destroy(&b->mutex);
    free(b);
}
