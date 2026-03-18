#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include "sse.h"
#include "config_parser.h"

typedef struct process_manager process_manager_t;

process_manager_t *process_manager_create(sse_broadcaster_t *sse, const char *askpass_path);
int process_manager_start_tunnel(process_manager_t *pm, const ssh_host_t *host);
int process_manager_stop_tunnel(process_manager_t *pm, const char *host_name);
const char *process_manager_get_tunnel_status(process_manager_t *pm, const char *host_name);
void process_manager_kill_all(process_manager_t *pm);
void process_manager_free(process_manager_t *pm);

#endif /* PROCESS_MANAGER_H */
