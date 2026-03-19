#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <microhttpd.h>
#include "sse.h"
#include "process_manager.h"
#include "config_manager.h"
#include "local_proxy.h"

typedef struct app_context {
    int                port;
    struct MHD_Daemon *httpd;
    sse_broadcaster_t *sse;
    process_manager_t *pm;
    config_manager_t  *cm;
    char               askpass_path[512];
    local_proxy_t     *proxy;
} app_context_t;

#endif /* APP_CONTEXT_H */
