#define _GNU_SOURCE
#include <dlfcn.h>
#include <sys/socket.h>

int connect(int fd, const struct sockaddr* address, socklen_t length)
{
    int (*next_connect)(int, const struct sockaddr*, socklen_t) = dlsym(RTLD_NEXT, "connect");
    const struct linger reset = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
    return next_connect(fd, address, length);
}
