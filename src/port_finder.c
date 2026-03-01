#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "port_finder.h"

int find_free_port(const char *bind_addr) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = 0; /* OS assegna una porta libera */
    addr.sin_addr.s_addr = inet_addr(bind_addr);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }

    int port = (int)ntohs(addr.sin_port);
    close(fd);
    return port;
}
