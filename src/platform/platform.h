/*
 * vmsotel: the platform interface.
 *
 * Everything the agent needs from the operating system beyond plain
 * C: a TCP stream, the time, and a way to wait. Nothing here names a
 * sockaddr, a file descriptor or a timeval, so an implementation on
 * $QIO and $GETTIM is as possible as the POSIX one in
 * platform_posix.c. TLS sits above this interface (transport/tls.c),
 * not inside it, for the same reason.
 */

#ifndef VO_PLATFORM_H
#define VO_PLATFORM_H

#include <stddef.h>

#include "vo_types.h"

typedef struct vo_conn vo_conn;

/* Results of vo_net_recv() other than a byte count. */
#define VO_NET_CLOSED    0
#define VO_NET_ERROR   (-1)
#define VO_NET_TIMEOUT (-2)

/*
 * Opens a TCP connection to host:port, giving up after timeout_ms.
 * Returns NULL on failure with a reason in err.
 */
vo_conn *vo_net_connect(const char *host, const char *port, int timeout_ms,
                        char *err, size_t errcap);

/* Sends all n bytes. 1 on success, 0 on failure. */
int vo_net_send(vo_conn *c, const void *p, size_t n);

/* Receives up to n bytes, waiting at most timeout_ms. Returns the
   count, or VO_NET_CLOSED, VO_NET_ERROR or VO_NET_TIMEOUT. */
long vo_net_recv(vo_conn *c, void *p, size_t n, int timeout_ms);

void vo_net_close(vo_conn *c);

/* Wall-clock time, UTC, in Unix nanoseconds. */
vo_u64 vo_time_utc_ns(void);

/* A clock for intervals, in milliseconds from an arbitrary origin. It
   must not jump when the wall clock is set. */
vo_u64 vo_time_mono_ms(void);

/* An interval clock as fine as the system offers, in nanoseconds, for
   timing work that takes milliseconds. On VMS it is $GETTIM, whose
   unit is 100 ns; on Linux, CLOCK_MONOTONIC. */
vo_u64 vo_time_precise_ns(void);

void vo_sleep_ms(vo_u32 ms);

#endif
