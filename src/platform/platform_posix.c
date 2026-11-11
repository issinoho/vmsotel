/*
 * vmsotel: the platform interface on POSIX sockets, for Linux and VMS.
 *
 * Kept to calls already proven on both VMS machines: getaddrinfo,
 * socket, connect, send and recv by probe_otlp; poll, fcntl O_NONBLOCK
 * and socklen_t by vmsguard's platform layer; sys$gettim and sleep()
 * by the probes. The one thing not yet run on VMS is the non-blocking
 * connect (EINPROGRESS, then SO_ERROR), which is plain BSD sockets.
 *
 * VAX (OpenVMS V7.3, TCP/IP Services V5.1; findings, "VAX") lacks
 * three of those: no <poll.h>, so the waits use select(); getaddrinfo
 * links but fails even for a dotted address, so names go through
 * inet_addr and gethostbyname; and there is no socklen_t. Each is its
 * own switch, VO_NET_SELECT and VO_NET_HOSTENT, on by default on VAX,
 * so the Linux build can run those paths too (make check does). And
 * fcntl links there but refuses O_NONBLOCK on a socket with EINVAL (the
 * agent's first VAX run), where ioctl FIONBIO works -- connect then
 * gives EINPROGRESS, select and SO_ERROR behave as elsewhere -- so
 * VO_NET_IOCTL sets non-blocking mode that way.
 *
 * VMS has no gettimeofday, so the clocks differ there: UTC from
 * time(), which docs/findings.md shows follows the system's own
 * differential, and intervals from $GETTIM. $GETTIM follows the wall
 * clock, so an interval stretches or shrinks if the clock is set
 * mid-wait; at one collection a minute that is harmless.
 */

#if !defined(__VMS) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200112L
#endif

/* The build files define _SOCKADDR_LEN on every VMS (the BSD 4.4
   structures getaddrinfo needs). On VAX it turns getaddrinfo into
   __BSD44_GETADDRINFO, which TCP/IP V5.1 lacks, and getaddrinfo is not
   used there, so it is dropped before any header sees it. */
#if defined(__VAX) && defined(_SOCKADDR_LEN)
#  undef _SOCKADDR_LEN
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __VAX
#  ifndef VO_NET_SELECT
#    define VO_NET_SELECT 1
#  endif
#  ifndef VO_NET_HOSTENT
#    define VO_NET_HOSTENT 1
#  endif
#  ifndef VO_NET_IOCTL
#    define VO_NET_IOCTL 1
#  endif
#endif

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#ifdef VO_NET_SELECT
#  include <sys/time.h>
#else
#  include <poll.h>
#endif
#ifdef VO_NET_IOCTL
#  ifdef __VMS
#    include <ioctl.h>
#  else
#    include <sys/ioctl.h>
#  endif
#endif

#ifdef __VAX
typedef unsigned int vo_socklen;        /* VAX has no socklen_t */
#else
typedef socklen_t vo_socklen;
#endif

#ifdef __VMS
#  include <starlet.h>
#endif

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

/* 1 when fd is ready, 0 on timeout, -1 on error. */
static int wait_fd(int fd, int for_write, int timeout_ms)
{
#ifdef VO_NET_SELECT
    fd_set set;
    struct timeval tv;
    int r;

    do {
        FD_ZERO(&set);
        FD_SET(fd, &set);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        r = select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL,
                   NULL, &tv);
    } while (r < 0 && errno == EINTR);
    return r;
#else
    struct pollfd pfd;
    int r;

    pfd.fd = fd;
    pfd.events = for_write ? POLLOUT : POLLIN;
    pfd.revents = 0;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r;
#endif
}

static int set_blocking(int fd, int blocking)
{
#ifdef VO_NET_IOCTL
    int on = !blocking;

    return ioctl(fd, FIONBIO, &on) == 0;
#else
    int fl = fcntl(fd, F_GETFL, 0);

    if (fl < 0)
        return 0;
    fl = blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK);
    return fcntl(fd, F_SETFL, fl) == 0;
#endif
}

/* Connects with a timeout: non-blocking connect, then a wait. */
static int connect_one(int family, int socktype, int protocol,
                       const struct sockaddr *addr, vo_socklen addrlen,
                       int timeout_ms, char *err, size_t errcap)
{
    int fd, r, soerr = 0;
    vo_socklen len = sizeof soerr;

    fd = socket(family, socktype, protocol);
    if (fd < 0) {
        vo_snprintf(err, errcap, "socket: %s", strerror(errno));
        return -1;
    }
    if (!set_blocking(fd, 0)) {
        vo_snprintf(err, errcap, "non-blocking mode: %s", strerror(errno));
        close(fd);
        return -1;
    }
    r = connect(fd, addr, addrlen);
    if (r != 0 && errno != EINPROGRESS) {
        vo_snprintf(err, errcap, "connect: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (r != 0) {
        r = wait_fd(fd, 1, timeout_ms);
        if (r == 0) {
            vo_snprintf(err, errcap, "connect: timed out after %d ms",
                        timeout_ms);
            close(fd);
            return -1;
        }
        if (r < 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *) &soerr, &len) != 0 ||
            soerr != 0) {
            vo_snprintf(err, errcap, "connect: %s",
                        strerror(soerr != 0 ? soerr : errno));
            close(fd);
            return -1;
        }
    }
    if (!set_blocking(fd, 1)) {
        vo_snprintf(err, errcap, "non-blocking mode: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

#ifdef VO_NET_HOSTENT
/* IPv4 only: a dotted quad through inet_addr, else gethostbyname. */
static int resolve_connect(const char *host, const char *port, int timeout_ms,
                           char *err, size_t errcap)
{
    struct sockaddr_in sin;
    char *end;
    unsigned long portnum = strtoul(port, &end, 10);
    vo_u32 a = (vo_u32) inet_addr(host);

    if (*port == '\0' || *end != '\0' || portnum == 0 || portnum > 65535) {
        vo_snprintf(err, errcap, "port %s is not a number", port);
        return -1;
    }
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short) portnum);
    if (a != 0xFFFFFFFFu) {
        memcpy(&sin.sin_addr, &a, 4);           /* network order already */
    } else {
        struct hostent *h = gethostbyname(host);

        if (h == NULL || h->h_addrtype != AF_INET || h->h_length != 4) {
            vo_snprintf(err, errcap, "resolve %s: no IPv4 address", host);
            return -1;
        }
        memcpy(&sin.sin_addr, h->h_addr_list[0], 4);
    }
    return connect_one(AF_INET, SOCK_STREAM, 0, (struct sockaddr *) &sin,
                       (vo_socklen) sizeof sin, timeout_ms, err, errcap);
}
#else
static int resolve_connect(const char *host, const char *port, int timeout_ms,
                           char *err, size_t errcap)
{
    struct addrinfo hints, *res = NULL, *ai;
    int rc, fd = -1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        vo_snprintf(err, errcap, "resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai != NULL && fd < 0; ai = ai->ai_next)
        fd = connect_one(ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                         ai->ai_addr, (vo_socklen) ai->ai_addrlen,
                         timeout_ms, err, errcap);
    freeaddrinfo(res);
    return fd;
}
#endif

vo_conn *vo_net_connect(const char *host, const char *port, int timeout_ms,
                        char *err, size_t errcap)
{
    vo_conn *c;
    int fd = resolve_connect(host, port, timeout_ms, err, errcap);

    if (fd < 0)
        return NULL;
    c = malloc(sizeof *c);
    if (c == NULL) {
        vo_snprintf(err, errcap, "out of memory");
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

#define NS_PER_S 1000000000u

vo_u64 vo_time_utc_ns(void)
{
#ifdef __VMS
    /* No gettimeofday on VMS; time() is UTC per docs/findings.md. */
    return vo_qmul(vo_q32((vo_u32) time(NULL)), NS_PER_S);
#else
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return vo_qadd(vo_qmul(vo_q32((vo_u32) ts.tv_sec), NS_PER_S),
                   vo_q32((vo_u32) ts.tv_nsec));
#endif
}

vo_u64 vo_time_mono_ms(void)
{
#ifdef __VMS
    /*
     * time(), not $GETTIM. VMS system time is local, so $GETTIM jumps
     * back an hour when summer time ends, and every wait computed
     * before the jump would last an hour longer -- an agent silent for
     * an hour each autumn. time() is UTC and does not jump then. Whole
     * seconds are enough: every wait the agent makes is seconds long.
     * A clock set backwards by hand still moves it; the agent's loop
     * watches for that.
     */
    return vo_qmul(vo_q32((vo_u32) time(NULL)), 1000u);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return vo_qadd(vo_qmul(vo_q32((vo_u32) ts.tv_sec), 1000u),
                   vo_q32((vo_u32) (ts.tv_nsec / 1000000L)));
#endif
}

vo_u64 vo_time_precise_ns(void)
{
#ifdef __VMS
    /* $GETTIM's quadword, read as bytes: VAX has no 64-bit type. */
    unsigned char t[8];

    memset(t, 0, sizeof t);
    if (!(sys$gettim((void *) t) & 1))
        return vo_time_utc_ns();
    return vo_qmul(vo_q((vo_u32) t[4] | (vo_u32) t[5] << 8 |
                        (vo_u32) t[6] << 16 | (vo_u32) t[7] << 24,
                        (vo_u32) t[0] | (vo_u32) t[1] << 8 |
                        (vo_u32) t[2] << 16 | (vo_u32) t[3] << 24), 100u);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return vo_qadd(vo_qmul(vo_q32((vo_u32) ts.tv_sec), NS_PER_S),
                   vo_q32((vo_u32) ts.tv_nsec));
#endif
}

void vo_sleep_ms(vo_u32 ms)
{
#ifdef __VMS
    /* Whole seconds, rounded up: sleep() is what the probes proved.
       The agent's waits are seconds long, so nothing finer is needed. */
    if (ms > 0)
        sleep((ms + 999) / 1000);
#else
    struct timespec ts;

    ts.tv_sec = (time_t) (ms / 1000);
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
#endif
}
