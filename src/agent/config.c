/*
 * vmsotel: the configuration file. See config.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

void vo_config_defaults(vo_config *c)
{
    memset(c, 0, sizeof *c);
    strcpy(c->port, "4318");
    strcpy(c->path, "/v1/metrics");
    c->interval_s = 60;
    c->timeout_s = 10;
    c->queue_len = 10;
    c->max_payload = 131072;
}

static int copy(char *dst, size_t cap, const char *v, size_t n)
{
    if (n >= cap)
        return 0;
    memcpy(dst, v, n);
    dst[n] = '\0';
    return 1;
}

static int number(const char *v, size_t n, unsigned long lo, unsigned long hi,
                  unsigned long *out)
{
    char buf[24], *end;
    unsigned long x;

    if (n == 0 || n >= sizeof buf || v[0] == '-')
        return 0;
    memcpy(buf, v, n);
    buf[n] = '\0';
    x = strtoul(buf, &end, 10);
    if (*end != '\0' || x < lo || x > hi)
        return 0;
    *out = x;
    return 1;
}

static int boolean(const char *v, size_t n, int *out)
{
    static const char *const yes[] = { "yes", "true", "on", "1" };
    static const char *const no[] = { "no", "false", "off", "0" };
    size_t i;

    for (i = 0; i < 4; i++) {
        if (strlen(yes[i]) == n && memcmp(v, yes[i], n) == 0) {
            *out = 1;
            return 1;
        }
        if (strlen(no[i]) == n && memcmp(v, no[i], n) == 0) {
            *out = 0;
            return 1;
        }
    }
    return 0;
}

static int is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r';
}

#define KEY(k) (kl == sizeof(k) - 1 && memcmp(key, k, kl) == 0)

int vo_config_parse(vo_config *c, const char *text, char *err, size_t errcap)
{
    const char *p = text;
    int line = 0;

    err[0] = '\0';
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        const char *end = eol ? eol : p + strlen(p);
        const char *key, *eq, *val;
        size_t kl, vl;
        unsigned long n;
        int ok = 1;

        line++;
        while (p < end && is_space(*p))
            p++;
        if (p == end || *p == '#' || *p == '!')
            goto next;

        eq = memchr(p, '=', (size_t) (end - p));
        if (eq == NULL) {
            snprintf(err, errcap, "line %d: expected key = value", line);
            return line;
        }
        key = p;
        kl = (size_t) (eq - p);
        while (kl > 0 && is_space(key[kl - 1]))
            kl--;
        val = eq + 1;
        while (val < end && is_space(*val))
            val++;
        vl = (size_t) (end - val);
        while (vl > 0 && is_space(val[vl - 1]))
            vl--;

        if (KEY("host"))
            ok = vl > 0 && copy(c->host, sizeof c->host, val, vl);
        else if (KEY("port"))
            ok = number(val, vl, 1, 65535, &n) &&
                 copy(c->port, sizeof c->port, val, vl);
        else if (KEY("path"))
            ok = vl > 0 && val[0] == '/' && copy(c->path, sizeof c->path, val, vl);
        else if (KEY("tls"))
            ok = boolean(val, vl, &c->tls);
        else if (KEY("cafile"))
            ok = copy(c->cafile, sizeof c->cafile, val, vl);
        else if (KEY("interval"))
            ok = number(val, vl, 1, 86400, &n) && (c->interval_s = (unsigned) n, 1);
        else if (KEY("timeout"))
            ok = number(val, vl, 1, 300, &n) && (c->timeout_s = (unsigned) n, 1);
        else if (KEY("queue"))
            ok = number(val, vl, 1, 64, &n) && (c->queue_len = (unsigned) n, 1);
        else if (KEY("max_payload"))
            ok = number(val, vl, 4096, 4194304, &n) && (c->max_payload = n, 1);
        else if (kl > 9 && memcmp(key, "resource.", 9) == 0) {
            size_t i = c->nextra;

            if (i == VO_CONFIG_MAX_EXTRA) {
                snprintf(err, errcap, "line %d: more than %d resource "
                         "attributes", line, VO_CONFIG_MAX_EXTRA);
                return line;
            }
            ok = copy(c->extra_key[i], VO_CONFIG_STR, key + 9, kl - 9) &&
                 copy(c->extra_value[i], VO_CONFIG_STR, val, vl);
            if (ok) {
                c->extra[i].key = c->extra_key[i];
                c->extra[i].value = c->extra_value[i];
                c->nextra++;
            }
        } else {
            snprintf(err, errcap, "line %d: unknown key \"%.*s\"", line,
                     (int) (kl > 40 ? 40 : kl), key);
            return line;
        }
        if (!ok) {
            snprintf(err, errcap, "line %d: bad value for \"%.*s\"", line,
                     (int) (kl > 40 ? 40 : kl), key);
            return line;
        }
next:
        p = eol ? eol + 1 : end;
    }
    if (c->host[0] == '\0') {
        snprintf(err, errcap, "no host given");
        return -1;
    }
    return 0;
}
