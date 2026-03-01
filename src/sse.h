#ifndef SSE_H
#define SSE_H

#include <microhttpd.h>

typedef struct sse_broadcaster sse_broadcaster_t;

sse_broadcaster_t *sse_broadcaster_create(void);
enum MHD_Result sse_broadcaster_add_client(sse_broadcaster_t *b, struct MHD_Connection *conn);
void sse_broadcast(sse_broadcaster_t *b, const char *event, const char *json_data);
void sse_broadcaster_free(sse_broadcaster_t *b);

#endif /* SSE_H */
