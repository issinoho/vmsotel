/*
 * vmsotel: one OTLP/HTTP export -- an HTTP/1.1 POST, over TCP or TLS.
 *
 * Each export is its own connection with Connection: close. At one
 * request a minute that costs nothing worth saving, and it means no
 * idle connection for a firewall or a collector restart to break.
 */

#ifndef VO_HTTP_H
#define VO_HTTP_H

#include <stddef.h>

typedef struct {
    const char *host;
    const char *port;
    const char *path;
    int tls;
    const char *cafile;         /* NULL: the system's default CAs */
    int timeout_ms;             /* connect, and each read */
} vo_http_target;

typedef struct {
    int status;                 /* HTTP status, 0 if none was read */
    long retry_after_s;         /* -1 if the response gave none */
    int have_date;
    long long date_unix;        /* the collector's Date header */
    long long rejected;         /* partialSuccess.rejectedDataPoints or
                                   rejectedLogRecords, -1 if absent */
    char message[160];          /* errorMessage, or the transport error */
    char tls_info[96];          /* protocol and cipher, when TLS */
} vo_http_result;

/* What the agent should do with the payload. */
enum {
    VO_HTTP_SENT,               /* 2xx: delivered (check rejected) */
    VO_HTTP_RETRY,              /* no answer, 429, 5xx: keep it, try later */
    VO_HTTP_REJECTED            /* any other answer: retrying cannot help */
};

int vo_http_post(const vo_http_target *t, const char *body, size_t len,
                 vo_http_result *res);

/*
 * Parses a whole response (status line, headers, body) into res.
 * Returns the VO_HTTP_* outcome. Pure, so the tests can feed it
 * anything a collector might say.
 */
int vo_http_parse_response(const char *resp, size_t n, vo_http_result *res);

/* "Sat, 31 Oct 2026 12:34:56 GMT" to Unix seconds. 1 on success. */
int vo_http_parse_date(const char *s, long long *out);

#endif
