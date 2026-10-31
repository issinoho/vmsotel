/*
 * vmsotel: the platform interface on POSIX sockets and clocks.
 *
 * Written to the subset that VMS TCP/IP Services also offers --
 * getaddrinfo, socket, connect, select, send, recv, as probe_otlp used
 * on both machines -- so that it may serve on VMS too. The clock and
 * sleep calls are the parts that will not: VMS has no gettimeofday,
 * and clock_gettime is unprobed there. Step 2c settles those.
 */

#if !defined(__VMS) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200112L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "platform.h"

struct vo_conn {
    int fd;
};

/* MSG_NOSIGNAL keeps a peer's reset from raising SIGPIPE on Linux. */
#ifdef MSG_NOSIGNAL
#  define SEND_FLAGS MSG_NOSIGNAL
#else
#  define SEND_FLAGS 0
#endif

static int wait_fd(int fd, int for_write, int timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int r;

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    do {
        r = select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL,
                   NULL, &tv);
    } while (r < 0 && errno == EINTR);
    return r;
}

static int set_blocking(int fd, int blocking)
{
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return 0;
    fl = blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl) == 0;
}

/* Connects with a timeout: non-blocking connect, then select. */
static int connect_one(const struct addrinfo *ai, int timeout_ms,
                       char *err, size_t errcap)
{
    int fd, r, soerr = 0;
    socklen_t len = sizeof soerr;

    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        snprintf(err, errcap, "socket: %s", strerror(errno));
        return -1;
    }
    if (!set_blocking(fd, 0)) {
        snprintf(err, errcap, "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }
    r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r != 0 && errno != EINPROGRESS) {
        snprintf(err, errcap, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (r != 0) {
        r = wait_fd(fd, 1, timeout_ms);
        if (r == 0) {
            snprintf(err, errcap, "connect: timed out after %d ms", timeout_ms);
            close(fd);
            return -1;
        }
        if (r < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 ||
            soerr != 0) {
            snprintf(err, errcap, "connect: %s",
                     strerror(soerr != 0 ? soerr : errno));
            close(fd);
            return -1;
        }
    }
    if (!set_blocking(fd, 1)) {
        snprintf(err, errcap, "fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

vo_conn *vo_net_connect(const char *host, const char *port, int timeout_ms,
                        char *err, size_t errcap)
{
    struct addrinfo hints, *res = NULL, *ai;
    vo_conn *c;
    int rc, fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        snprintf(err, errcap, "resolve %s: %s", host, gai_strerror(rc));
        return NULL;
    }
    for (ai = res; ai != NULL && fd < 0; ai = ai->ai_next)
        fd = connect_one(ai, timeout_ms, err, errcap);
    freeaddrinfo(res);
    if (fd < 0)
        return NULL;

    c = malloc(sizeof *c);
    if (c == NULL) {
        snprintf(err, errcap, "out of memory");
        close(fd);
        return NULL;
    }
    c->fd = fd;
    return c;
}

int vo_net_send(vo_conn *c, const void *p, size_t n)
{
    const char *q = p;

    while (n > 0) {
        ssize_t w = send(c->fd, q, n, SEND_FLAGS);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return 0;
        q += w;
        n -= (size_t) w;
    }
    return 1;
}

long vo_net_recv(vo_conn *c, void *p, size_t n, int timeout_ms)
{
    ssize_t r;
    int w = wait_fd(c->fd, 0, timeout_ms);

    if (w == 0)
        return VO_NET_TIMEOUT;
    if (w < 0)
        return VO_NET_ERROR;
    do {
        r = recv(c->fd, p, n, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return VO_NET_ERROR;
    return (long) r;
}

void vo_net_close(vo_conn *c)
{
    if (c == NULL)
        return;
    close(c->fd);
    free(c);
}

vo_u64 vo_time_utc_ns(void)
{
#ifdef __VMS
    /* No gettimeofday on VMS; time() is UTC per docs/findings.md. */
    return (vo_u64) time(NULL) * 1000000000ULL;
#else
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (vo_u64) ts.tv_sec * 1000000000ULL + (vo_u64) ts.tv_nsec;
#endif
}

vo_u64 vo_time_mono_ms(void)
{
#ifdef __VMS
    /* Placeholder until step 2c: $GETTIM-based, so it follows the
       wall clock. */
    return (vo_u64) time(NULL) * 1000ULL;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (vo_u64) ts.tv_sec * 1000ULL + (vo_u64) ts.tv_nsec / 1000000ULL;
#endif
}

void vo_sleep_ms(vo_u32 ms)
{
#ifdef __VMS
    sleep((ms + 999) / 1000);
#else
    struct timespec ts;

    ts.tv_sec = (time_t) (ms / 1000);
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
#endif
}
