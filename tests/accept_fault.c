#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

// 放行前 ACCEPT_FAULT_SKIP 个真实连接，随后 ACCEPT_FAULT_COUNT 个真实连接返回 ACCEPT_FAULT_ERRNO。
// 只对已建立的连接注入，Asio 的探测性 accept（EAGAIN）不计数。
static atomic_int accepted;

static int setting(const char* name, int fallback)
{
    const char* value = getenv(name);
    return value != NULL ? atoi(value) : fallback;
}

static int inject_fault(int fd)
{
    if (fd < 0)
    {
        return fd;
    }
    const int index = atomic_fetch_add(&accepted, 1);
    const int skip = setting("ACCEPT_FAULT_SKIP", 0);
    if (index < skip || index >= skip + setting("ACCEPT_FAULT_COUNT", 0))
    {
        return fd;
    }
    close(fd);
    errno = setting("ACCEPT_FAULT_ERRNO", EINVAL);
    return -1;
}

int accept(int fd, struct sockaddr* address, socklen_t* length)
{
    int (*next_accept)(int, struct sockaddr*, socklen_t*) = dlsym(RTLD_NEXT, "accept");
    return inject_fault(next_accept(fd, address, length));
}

int accept4(int fd, struct sockaddr* address, socklen_t* length, int flags)
{
    int (*next_accept4)(int, struct sockaddr*, socklen_t*, int) = dlsym(RTLD_NEXT, "accept4");
    return inject_fault(next_accept4(fd, address, length, flags));
}
