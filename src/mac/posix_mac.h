/* macOS: the few Linux calls the POSIX layer (src/linux) uses that Darwin spells differently. Force-included
 * (-include) into every C file of the macOS build, so the Linux files build unchanged where they can; the rest
 * have `#ifdef __APPLE__` at the spot. Linux builds never see this file. */
#pragma once
#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Darwin has no SOCK_CLOEXEC / accept4 / pipe2: the flag is taken off and set with fcntl. Nothing forks between
 * the two calls on another thread that matters here (children are curl, ffprobe, the engine: none reads them). */
#define SOCK_CLOEXEC  0x10000000
#define SOCK_NONBLOCK 0x20000000
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0             /* the service ignores SIGPIPE */
#endif

static inline int sp_mac_flags(int fd, int flags)
{
    if (fd < 0) return fd;
    if (flags & (SOCK_CLOEXEC | O_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags & (SOCK_NONBLOCK | O_NONBLOCK)) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static inline int sp_mac_socket(int domain, int type, int proto)
{
    return sp_mac_flags(socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), proto), type);
}
#define socket(d, t, p) sp_mac_socket((d), (t), (p))

static inline int sp_mac_accept4(int fd, struct sockaddr *a, socklen_t *l, int flags)
{
    return sp_mac_flags(accept(fd, a, l), flags);
}
#define accept4 sp_mac_accept4

/* Newer SDKs declare pipe2, but only for newer systems than the one we build for. */
static inline int sp_mac_pipe2(int fd[2], int flags)
{
    if (pipe(fd)) return -1;
    sp_mac_flags(fd[0], flags);
    sp_mac_flags(fd[1], flags);
    return 0;
}
#define pipe2 sp_mac_pipe2
#endif
