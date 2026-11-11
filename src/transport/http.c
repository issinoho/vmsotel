/*
 * vmsotel: one OTLP/HTTP export. See http.h.
 *
 * Response handling follows docs/design.md: a 2xx is delivered, though
 * its body may report rejected points; 429 and 503 -- and any 5xx, or
 * no answer at all -- mean try again later, honouring Retry-After; any
 * other status means the request itself is wrong, so it is not retried.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "platform.h"
#include "version.h"
#ifdef VO_WITH_TLS
#  include "tls.h"
#endif

#define RESP_MAX 16384

/* ---- parsing ------------------------------------------------------- */

/* Days since 1970-01-01 of a proleptic Gregorian date; C99 has no
   timegm, and mktime would apply a local zone. */
static long days_from_civil(int y, int m, int d)
{
    int era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long) era * 146097 + doe - 719468;
}

int vo_http_parse_date(const char *s, unsigned long *out)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    const char *p;
    int d, y, hh, mm, ss;

    if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mm, &ss) != 6)
        return 0;
    p = strstr(months, mon);
    if (p == NULL || (p - months) % 3 != 0 || d < 1 || d > 31 ||
        hh > 23 || mm > 59 || ss > 60 || y < 1970 || y > 2105)
        return 0;
    *out = (unsigned long) days_from_civil(y, (int) ((p - months) / 3) + 1, d)
           * 86400ul + (unsigned long) (hh * 3600L + mm * 60L + ss);
    return 1;
}

/* The value of header `name` within head[0..n), or NULL. Case-blind. */
static const char *header(const char *head, size_t n, const char *name,
                          size_t *vlen)
{
    size_t nl = strlen(name), i = 0;

    while (i < n) {
        const char *line = head + i;
        const char *eol = memchr(line, '\n', n - i);
        size_t ll = eol ? (size_t) (eol - line) : n - i;
        size_t k;

        if (ll > nl && line[nl] == ':') {
            for (k = 0; k < nl; k++)
                if (tolower((unsigned char) line[k]) !=
                    tolower((unsigned char) name[k]))
                    break;
            if (k == nl) {
                const char *v = line + nl + 1;
                size_t len = ll - nl - 1;

                while (len > 0 && (*v == ' ' || *v == '\t')) {
                    v++;
                    len--;
                }
                while (len > 0 && (v[len - 1] == '\r' || v[len - 1] == ' '))
                    len--;
                *vlen = len;
                return v;
            }
        }
        i += ll + 1;
    }
    return NULL;
}

/* A JSON member's value in body[0..n): a number or a string holding
   one, as the int64 mapping allows either. */
static int json_int(const char *body, size_t n, const char *key,
                    long *out)
{
    size_t kl = strlen(key), i;

    for (i = 0; i + kl + 2 < n; i++) {
        size_t j;
        long v = 0;
        int digits = 0;

        if (body[i] != '"' || memcmp(body + i + 1, key, kl) != 0 ||
            body[i + 1 + kl] != '"')
            continue;
        j = i + kl + 2;
        while (j < n && (body[j] == ' ' || body[j] == ':' || body[j] == '"'))
            j++;
        /* Nine digits fit a 32-bit long; no collector rejects more. */
        while (j < n && body[j] >= '0' && body[j] <= '9' && digits < 9) {
            v = v * 10 + (body[j] - '0');
            j++;
            digits++;
        }
        if (digits == 0)
            return 0;
        *out = v;
        return 1;
    }
    return 0;
}

/* A JSON string member's value, unescaped no further than the copy. */
static void json_str(const char *body, size_t n, const char *key,
                     char *out, size_t cap)
{
    size_t kl = strlen(key), i, o = 0;

    for (i = 0; i + kl + 2 < n; i++) {
        size_t j;

        if (body[i] != '"' || memcmp(body + i + 1, key, kl) != 0 ||
            body[i + 1 + kl] != '"')
            continue;
        j = i + kl + 2;
        while (j < n && (body[j] == ' ' || body[j] == ':'))
            j++;
        if (j >= n || body[j] != '"')
            return;
        for (j++; j < n && body[j] != '"' && o + 1 < cap; j++) {
            if (body[j] == '\\' && j + 1 < n)
                j++;
            out[o++] = body[j];
        }
        out[o] = '\0';
        return;
    }
}

int vo_http_parse_response(const char *resp, size_t n, vo_http_result *res)
{
    const char *end_head = NULL, *body, *v;
    size_t head_len, body_len, i, vlen;
    int major, minor, status;
    char tmp[64];

    res->status = 0;
    res->message[0] = '\0';
    res->retry_after_s = -1;
    res->have_date = 0;
    res->rejected = -1;

    for (i = 0; i + 3 < n; i++)
        if (memcmp(resp + i, "\r\n\r\n", 4) == 0) {
            end_head = resp + i;
            break;
        }
    head_len = end_head ? (size_t) (end_head - resp) : n;
    body = end_head ? end_head + 4 : resp + n;
    body_len = (size_t) (resp + n - body);

    if (n < 12 || memcmp(resp, "HTTP/", 5) != 0 ||
        sscanf(resp, "HTTP/%d.%d %3d", &major, &minor, &status) != 3 ||
        status < 100 || status > 599) {
        vo_snprintf(res->message, sizeof res->message,
                 n == 0 ? "no response" : "response is not HTTP");
        return VO_HTTP_RETRY;
    }
    res->status = status;

    v = header(resp, head_len, "Date", &vlen);
    if (v != NULL && vlen < sizeof tmp) {
        memcpy(tmp, v, vlen);
        tmp[vlen] = '\0';
        res->have_date = vo_http_parse_date(tmp, &res->date_unix);
    }

    /* Retry-After is seconds, or an HTTP date to compare with Date. */
    v = header(resp, head_len, "Retry-After", &vlen);
    if (v != NULL && vlen > 0 && vlen < sizeof tmp) {
        unsigned long when;

        memcpy(tmp, v, vlen);
        tmp[vlen] = '\0';
        if (isdigit((unsigned char) tmp[0]))
            res->retry_after_s = strtol(tmp, NULL, 10);
        else if (res->have_date && vo_http_parse_date(tmp, &when))
            res->retry_after_s = when > res->date_unix
                                 ? (long) (when - res->date_unix) : 0;
    }

    json_int(body, body_len, "rejectedDataPoints", &res->rejected);
    if (res->rejected < 0)      /* a logs request's partialSuccess */
        json_int(body, body_len, "rejectedLogRecords", &res->rejected);
    json_str(body, body_len, "errorMessage", res->message,
             sizeof res->message);
    if (res->message[0] == '\0')
        json_str(body, body_len, "message", res->message,
                 sizeof res->message);

    if (status >= 200 && status < 300)
        return VO_HTTP_SENT;
    if (status == 429 || status >= 500)
        return VO_HTTP_RETRY;
    return VO_HTTP_REJECTED;
}

/* ---- the exchange -------------------------------------------------- */

typedef struct {
    vo_conn *conn;
#ifdef VO_WITH_TLS
    vo_tls *tls;
#endif
    int timeout_ms;
} stream;

static int s_send(stream *s, const void *p, size_t n)
{
#ifdef VO_WITH_TLS
    if (s->tls != NULL)
        return vo_tls_send(s->tls, p, n);
#endif
    return vo_net_send(s->conn, p, n);
}

static long s_recv(stream *s, void *p, size_t n)
{
#ifdef VO_WITH_TLS
    if (s->tls != NULL)
        return vo_tls_recv(s->tls, p, n);
#endif
    return vo_net_recv(s->conn, p, n, s->timeout_ms);
}

int vo_http_post(const vo_http_target *t, const char *body, size_t len,
                 vo_http_result *res)
{
    static char resp[RESP_MAX];
    char head[512];
    stream s;
    size_t got = 0;
    int hl, outcome;

    memset(res, 0, sizeof *res);
    res->retry_after_s = -1;
    res->rejected = -1;
    memset(&s, 0, sizeof s);
    s.timeout_ms = t->timeout_ms;

#ifndef VO_WITH_TLS
    if (t->tls) {
        vo_snprintf(res->message, sizeof res->message,
                 "this image was built without TLS");
        return VO_HTTP_REJECTED;
    }
#endif

    s.conn = vo_net_connect(t->host, t->port, t->timeout_ms, res->message,
                            sizeof res->message);
    if (s.conn == NULL)
        return VO_HTTP_RETRY;
#ifdef VO_WITH_TLS
    if (t->tls) {
        s.tls = vo_tls_open(s.conn, t->host, t->cafile, t->timeout_ms,
                            res->message, sizeof res->message);
        if (s.tls == NULL) {
            vo_net_close(s.conn);
            return VO_HTTP_RETRY;
        }
        vo_tls_describe(s.tls, res->tls_info, sizeof res->tls_info);
    }
#endif

    hl = vo_snprintf(head, sizeof head,
                  "POST %s HTTP/1.1\r\n"
                  "Host: %s:%s\r\n"
                  "User-Agent: vmsotel/" VO_VERSION "\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: %lu\r\n"
                  "Connection: close\r\n"
                  "\r\n", t->path, t->host, t->port, (unsigned long) len);
    if (hl < 0 || (size_t) hl >= sizeof head) {
        vo_snprintf(res->message, sizeof res->message, "request line too long");
        outcome = VO_HTTP_REJECTED;
        goto done;
    }
    if (!s_send(&s, head, (size_t) hl) || !s_send(&s, body, len)) {
        vo_snprintf(res->message, sizeof res->message, "send failed");
        outcome = VO_HTTP_RETRY;
        goto done;
    }

    /* Read to the end: the request asked for Connection: close. */
    while (got < sizeof resp) {
        long r = s_recv(&s, resp + got, sizeof resp - got);

        if (r == VO_NET_TIMEOUT) {
            if (got == 0) {
                vo_snprintf(res->message, sizeof res->message,
                         "no response within %d ms", t->timeout_ms);
                outcome = VO_HTTP_RETRY;
                goto done;
            }
            break;
        }
        if (r <= 0)
            break;
        got += (size_t) r;
    }
    outcome = vo_http_parse_response(resp, got, res);

done:
#ifdef VO_WITH_TLS
    vo_tls_close(s.tls);
#endif
    vo_net_close(s.conn);
    return outcome;
}
