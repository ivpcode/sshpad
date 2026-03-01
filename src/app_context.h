#ifndef APP_CONTEXT_H
#define APP_CONTEXT_H

#include <microhttpd.h>
#include "sse.h"
#include "process_manager.h"
#include "config_parser.h"

typedef struct app_context {
    int                port;
    struct MHD_Daemon *httpd;
    sse_broadcaster_t *sse;
    process_manager_t *pm;
    ssh_host_t        *hosts;
    int                num_hosts;
} app_context_t;

#endif /* APP_CONTEXT_H */
