/*
 * vmsotel: a log source that makes records up, so the agent's logs path
 * runs end to end on Linux. One record a second after the first read,
 * alternating between the two files when both are on, each numbered so
 * a test can see that none is lost or sent twice: "fake operator
 * message N", and an audit login failure from user UN.
 *
 * The audit records are built as NSAMSGDEF bytes and go through the
 * real decoder. Two environment variables shape a run:
 *   VMSOTEL_FAKE_LOG_BURST=N   N records waiting at the first read
 *   VMSOTEL_FAKE_LOG_FAIL=1    the audit journal cannot be read: only
 *                              operator messages are made, and every
 *                              read says why
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logsource.h"
#include "platform.h"

static const vo_logs_config *cfg;
static vo_u64 t0_ms, t0_ns;
static unsigned long burst, committed, last_n;
static int started;

void vo_logs_open(const vo_logs_config *c)
{
    const char *b = getenv("VMSOTEL_FAKE_LOG_BURST");

    cfg = c;
    burst = b ? strtoul(b, NULL, 10) : 0;
    committed = last_n = 0;
    started = 0;
}

static size_t put_pkt(unsigned char *b, size_t at, unsigned type,
                      const void *data, size_t n)
{
    b[at] = (unsigned char) ((n + 4) & 0xff);
    b[at + 1] = (unsigned char) ((n + 4) >> 8);
    b[at + 2] = (unsigned char) (type & 0xff);
    b[at + 3] = (unsigned char) (type >> 8);
    memcpy(b + at + 4, data, n);
    return at + 4 + n;
}

/* A LOGFAIL record, as x86 writes them, numbered k. */
static void audit_rec(vo_log_rec *r, unsigned long k, vo_u64 when_ns)
{
    unsigned char b[200], ts[8];
    char user[16], node[40];
    vo_u64 vms = when_ns / 100 + 35067168000000000ULL;
    size_t at = 20;
    int i;

    for (i = 0; i < 8; i++)
        ts[i] = (unsigned char) (vms >> (8 * i));
    sprintf(user, "U%lu", k);
    sprintf(node, "SSH_PASSWORD:192.0.2.%lu", k % 250 + 1);
    at = put_pkt(b, at, 6, "SECURITY", 8);
    at = put_pkt(b, at, 45, user, strlen(user));
    at = put_pkt(b, at, 44, node, strlen(node));
    at = put_pkt(b, at, 50, ts, 8);
    at = put_pkt(b, at, 7, "FAKE", 4);
    memset(b, 0, 20);
    b[0] = 5;                       /* LOGFAIL */
    b[2] = 5;
    b[4] = 0x1c;
    b[6] = 5;
    b[8] = (unsigned char) at;
    b[11] = 3;
    vo_nsa_decode(b, at, 0, r);
}

static void opcom_rec(vo_log_rec *r, unsigned long k, vo_u64 when_ns)
{
    memset(r, 0, sizeof *r);
    r->source = VO_LOG_OPCOM;
    r->time_ns = when_ns;
    r->severity = VO_SEV_INFO;
    r->severity_text = "INFO";
    sprintf(r->body, "Message from user SYSTEM on FAKE\n"
                     "fake operator message %lu", k);
    r->nattrs = 2;
    r->attrs[0].key = "openvms.opcom.user";
    strcpy(r->attrs[0].sval, "SYSTEM");
    r->attrs[1].key = "openvms.opcom.node";
    strcpy(r->attrs[1].sval, "FAKE");
}

int vo_logs_read(vo_log_rec *recs, size_t max, size_t *n, int *more,
                 char *msg, size_t msgcap)
{
    vo_u64 now = vo_time_mono_ms();
    unsigned long avail, i;
    int fail = getenv("VMSOTEL_FAKE_LOG_FAIL") != NULL && cfg->audit;
    int audit_on = cfg->audit && !fail;

    *n = 0;
    *more = 0;
    msg[0] = '\0';
    last_n = 0;
    if (fail)
        snprintf(msg, msgcap, "cannot read %s: fake: no privilege",
                 cfg->audit_file);
    if (!cfg->opcom && !audit_on)
        return !fail;
    if (!started) {
        started = 1;
        t0_ms = now;
        t0_ns = vo_time_utc_ns();
        if (!fail)
            snprintf(msg, msgcap, "fake logs: %lu waiting, then one a "
                     "second", burst);
    }
    avail = burst + (unsigned long) ((now - t0_ms) / 1000);
    for (i = committed; i < avail && *n < max; i++) {
        unsigned long k = i + 1;
        int audit = audit_on && (!cfg->opcom || i % 2 == 1);
        vo_u64 when = t0_ns + (vo_u64) i * 1000000000ULL;

        if (audit) {
            audit_rec(&recs[*n], k, when);
            recs[*n].file = cfg->audit_file;
        } else {
            opcom_rec(&recs[*n], k, when);
            recs[*n].file = cfg->opcom_file;
        }
        (*n)++;
    }
    *more = i < avail;
    last_n = (unsigned long) *n;
    return !fail;
}

void vo_logs_commit(size_t k)
{
    committed += k < (size_t) last_n ? (unsigned long) k : last_n;
    last_n = 0;
}

void vo_logs_close(void)
{
}
