#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <microhttpd.h>

/* Forward declaration - app_context_t è definito in app_context.h */
typedef struct app_context app_context_t;

struct MHD_Daemon *http_server_start(int port, app_context_t *ctx);

#endif /* HTTP_SERVER_H */
