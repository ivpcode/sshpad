#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#define MAX_FORWARDS 16
#define MAX_OPTIONS  32

typedef struct {
    char bind_addr[64];
    int  bind_port;
    char remote_host[256];
    int  remote_port;
} forward_rule_t;

typedef struct {
    char bind_addr[64];
    int  bind_port;
} dynamic_rule_t;

typedef struct {
    char key[64];
    char value[256];
} ssh_option_t;

typedef struct {
    char             name[128];
    char             hostname[256];
    char             user[64];
    int              port;
    char             identity_file[512];
    char             proxy_jump[128];

    forward_rule_t   local_forward[MAX_FORWARDS];
    int              num_local_forward;

    forward_rule_t   remote_forward[MAX_FORWARDS];
    int              num_remote_forward;

    dynamic_rule_t   dynamic_forward[MAX_FORWARDS];
    int              num_dynamic_forward;

    ssh_option_t     options[MAX_OPTIONS];
    int              num_options;
} ssh_host_t;

ssh_host_t *parse_ssh_config(const char *path, int *out_count);
void ssh_hosts_free(ssh_host_t *hosts, int count);

/* --- Serializzazione JSON --- */
ssh_host_t *ssh_hosts_from_json(const char *json_str, int *out_count);
char       *ssh_hosts_to_json(const ssh_host_t *hosts, int count);

/* --- Scrittura SSH config --- */
int ssh_hosts_write_config(const ssh_host_t *hosts, int count, const char *path);

#endif /* CONFIG_PARSER_H */
