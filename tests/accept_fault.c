#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/socket.h>

// 前 ACCEPT_FAULT_COUNT 次 accept 返回 ACCEPT_FAULT_ERRNO，用于验证监听错误分类。
static atomic_int calls;

static int inject_fault(void)
{
    const char* count = getenv("ACCEPT_FAULT_COUNT");
    if (count == NULL || atomic_fetch_add(&calls, 1) >= atoi(count))
    {
        return 0;
    }
    const char* value = getenv("ACCEPT_FAULT_ERRNO");
    errno = value != NULL ? atoi(value) : EINVAL;
    return -1;
}

int accept(int fd, struct sockaddr* address, socklen_t* length)
{
    if (inject_fault() != 0)
    {
        return -1;
    }
    int (*next_accept)(int, struct sockaddr*, socklen_t*) = dlsym(RTLD_NEXT, "accept");
    return next_accept(fd, address, length);
}

int accept4(int fd, struct sockaddr* address, socklen_t* length, int flags)
{
    if (inject_fault() != 0)
    {
        return -1;
    }
    int (*next_accept4)(int, struct sockaddr*, socklen_t*, int) = dlsym(RTLD_NEXT, "accept4");
    return next_accept4(fd, address, length, flags);
}
