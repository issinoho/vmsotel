/*
 * vmsotel: TLS over memory BIOs. See tls.h.
 *
 * Every call used here exists in OpenSSL 1.1.1 and 3.x alike, so there
 * is no version branch. What vmsguard learned does apply: the headers
 * are picked by the OPENSSL logical, the image by the link, and nothing
 * else makes them agree. The build says which family it links with
 * VO_EXPECT_OPENSSL_1 or VO_EXPECT_OPENSSL_3, and a mismatch stops the
 * compile here instead of failing inside OpenSSL at run time.
 */

#include <stdio.h>
#include <string.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "tls.h"

#if OPENSSL_VERSION_NUMBER < 0x10101000L
#error "OpenSSL 1.1.1 or later is needed"
#endif
#if defined(VO_EXPECT_OPENSSL_1) && OPENSSL_VERSION_NUMBER >= 0x30000000L
#error "OpenSSL 3.x headers with a 1.1.1 image: the OPENSSL logical does not match the image the link names"
#endif
#if defined(VO_EXPECT_OPENSSL_3) && OPENSSL_VERSION_NUMBER < 0x30000000L
#error "OpenSSL 1.1.1 headers with a 3.x image: the OPENSSL logical does not match the image the link names"
#endif

struct vo_tls {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *rbio;          /* bytes from the network, for OpenSSL */
    BIO *wbio;          /* bytes from OpenSSL, for the network */
    vo_conn *conn;
    int timeout_ms;
};

static void ssl_error(const char *what, char *err, size_t errcap)
{
    unsigned long e = ERR_get_error();
    char buf[200];

    if (e != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        snprintf(err, errcap, "%s: %s", what, buf);
    } else {
        snprintf(err, errcap, "%s failed", what);
    }
    ERR_clear_error();
}

/* Sends whatever OpenSSL has written. */
static int flush_out(vo_tls *t)
{
    char buf[4096];
    int n;

    while (BIO_ctrl_pending(t->wbio) > 0) {
        n = BIO_read(t->wbio, buf, (int) sizeof buf);
        if (n <= 0)
            return 0;
        if (!vo_net_send(t->conn, buf, (size_t) n))
            return 0;
    }
    return 1;
}

/* Feeds OpenSSL one read from the network. Returns as vo_net_recv. */
static long fill_in(vo_tls *t)
{
    char buf[4096];
    long n = vo_net_recv(t->conn, buf, sizeof buf, t->timeout_ms);

    if (n > 0 && BIO_write(t->rbio, buf, (int) n) != (int) n)
        return VO_NET_ERROR;
    return n;
}

/* A dotted quad or anything with a colon is an address, not a name:
   it is matched against an IP SAN and never sent as SNI. */
static int is_address(const char *host)
{
    const char *p;

    if (strchr(host, ':') != NULL)
        return 1;
    for (p = host; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.'))
            return 0;
    return *host != '\0';
}

vo_tls *vo_tls_open(vo_conn *conn, const char *host, const char *cafile,
                    int timeout_ms, char *err, size_t errcap)
{
    vo_tls *t;
    int r;

    t = OPENSSL_zalloc(sizeof *t);
    if (t == NULL) {
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    t->conn = conn;
    t->timeout_ms = timeout_ms;

    t->ctx = SSL_CTX_new(TLS_client_method());
    if (t->ctx == NULL) {
        ssl_error("SSL_CTX_new", err, errcap);
        goto fail;
    }
    SSL_CTX_set_min_proto_version(t->ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(t->ctx, SSL_VERIFY_PEER, NULL);
    if (cafile != NULL && *cafile != '\0') {
        if (SSL_CTX_load_verify_locations(t->ctx, cafile, NULL) != 1) {
            ssl_error("load cafile", err, errcap);
            goto fail;
        }
    } else if (SSL_CTX_set_default_verify_paths(t->ctx) != 1) {
        ssl_error("default CA locations", err, errcap);
        goto fail;
    }

    t->ssl = SSL_new(t->ctx);
    t->rbio = BIO_new(BIO_s_mem());
    t->wbio = BIO_new(BIO_s_mem());
    if (t->ssl == NULL || t->rbio == NULL || t->wbio == NULL) {
        ssl_error("SSL_new", err, errcap);
        BIO_free(t->rbio);
        BIO_free(t->wbio);
        t->rbio = t->wbio = NULL;
        goto fail;
    }
    /* An empty read BIO means "wait for more", not end of stream. */
    BIO_set_mem_eof_return(t->rbio, -1);
    SSL_set_bio(t->ssl, t->rbio, t->wbio);      /* the SSL owns them now */
    /* SSL_connect would say this; SSL_do_handshake needs telling. */
    SSL_set_connect_state(t->ssl);

    if (is_address(host)) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(t->ssl), host);
    } else {
        SSL_set_tlsext_host_name(t->ssl, host);
        SSL_set1_host(t->ssl, host);
    }

    for (;;) {
        r = SSL_do_handshake(t->ssl);
        if (!flush_out(t)) {
            snprintf(err, errcap, "TLS handshake: send failed");
            goto fail;
        }
        if (r == 1)
            break;
        if (SSL_get_error(t->ssl, r) == SSL_ERROR_WANT_READ) {
            long n = fill_in(t);

            if (n > 0)
                continue;
            snprintf(err, errcap, "TLS handshake: %s",
                     n == VO_NET_TIMEOUT ? "timed out" :
                     n == VO_NET_CLOSED ? "connection closed by peer" :
                     "receive failed");
            goto fail;
        }
        if (SSL_get_verify_result(t->ssl) != X509_V_OK) {
            snprintf(err, errcap, "TLS certificate: %s",
                     X509_verify_cert_error_string(
                         SSL_get_verify_result(t->ssl)));
            ERR_clear_error();
        } else {
            ssl_error("TLS handshake", err, errcap);
        }
        goto fail;
    }
    return t;

fail:
    if (t->ssl != NULL)
        SSL_free(t->ssl);
    if (t->ctx != NULL)
        SSL_CTX_free(t->ctx);
    OPENSSL_free(t);
    return NULL;
}

int vo_tls_send(vo_tls *t, const void *p, size_t n)
{
    const char *q = p;

    while (n > 0) {
        int chunk = n > 16384 ? 16384 : (int) n;
        int w = SSL_write(t->ssl, q, chunk);

        if (w <= 0) {
            if (SSL_get_error(t->ssl, w) == SSL_ERROR_WANT_READ &&
                flush_out(t) && fill_in(t) > 0)
                continue;
            ERR_clear_error();
            return 0;
        }
        if (!flush_out(t))
            return 0;
        q += w;
        n -= (size_t) w;
    }
    return 1;
}

long vo_tls_recv(vo_tls *t, void *p, size_t n)
{
    int chunk = n > 16384 ? 16384 : (int) n;

    for (;;) {
        int r = SSL_read(t->ssl, p, chunk);
        long got;

        if (r > 0)
            return r;
        switch (SSL_get_error(t->ssl, r)) {
        case SSL_ERROR_ZERO_RETURN:
            return VO_NET_CLOSED;
        case SSL_ERROR_WANT_READ:
            if (!flush_out(t))
                return VO_NET_ERROR;
            got = fill_in(t);
            if (got > 0)
                continue;
            /* A peer that closes without close_notify, as plenty of
               HTTP servers do after Connection: close, is an end of
               stream here: the response length is the HTTP layer's
               concern. */
            return got;
        default:
            ERR_clear_error();
            return VO_NET_ERROR;
        }
    }
}

const char *vo_tls_describe(vo_tls *t, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s %s", SSL_get_version(t->ssl),
             SSL_CIPHER_get_name(SSL_get_current_cipher(t->ssl)));
    return buf;
}

void vo_tls_close(vo_tls *t)
{
    if (t == NULL)
        return;
    if (SSL_shutdown(t->ssl) >= 0)
        flush_out(t);
    ERR_clear_error();
    SSL_free(t->ssl);
    SSL_CTX_free(t->ctx);
    OPENSSL_free(t);
}

const char *vo_tls_library(void)
{
    return OpenSSL_version(OPENSSL_VERSION);
}
