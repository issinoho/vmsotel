/*
 * vmsotel: TLS over the platform interface.
 *
 * OpenSSL never sees a socket. It reads and writes two memory BIOs,
 * and this layer moves bytes between them and vo_net_send/vo_net_recv,
 * so TLS needs nothing from the platform but those two calls: a $QIO
 * implementation would carry it unchanged. Only POSIX has run.
 *
 * The certificate is always verified, against cafile if given and the
 * system's default locations otherwise, and the name or address
 * checked. There is no option to skip that (docs/design.md, TLS).
 */

#ifndef VO_TLS_H
#define VO_TLS_H

#include <stddef.h>

#include "platform.h"

typedef struct vo_tls vo_tls;

/* Handshakes over an open connection. NULL on failure, with a reason. */
vo_tls *vo_tls_open(vo_conn *conn, const char *host, const char *cafile,
                    int timeout_ms, char *err, size_t errcap);

/* As vo_net_send and vo_net_recv. */
int vo_tls_send(vo_tls *t, const void *p, size_t n);
long vo_tls_recv(vo_tls *t, void *p, size_t n);

/* "TLSv1.3 TLS_AES_128_GCM_SHA256", for the log. */
const char *vo_tls_describe(vo_tls *t, char *buf, size_t cap);

/* Sends close_notify and frees; the connection stays open. */
void vo_tls_close(vo_tls *t);

/* The OpenSSL version string, for the log. */
const char *vo_tls_library(void);

#endif
