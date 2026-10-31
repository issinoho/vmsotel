/*
 * vmsotel: the agent.
 *
 *     vmsotel CONFIG [--cycles N] [--drain SECONDS]
 *     vmsotel --selftest
 *
 * Every interval it reads the source, updates the model, encodes a
 * payload and queues it; between collections it sends the queue,
 * oldest first. A failed send keeps the payload and backs off,
 * doubling from one second up to the interval, or longer if the
 * collector's Retry-After asks. A rejected payload is dropped and
 * logged loudly, since sending it again cannot help.
 *
 * --cycles N stops after N collections, once the queue is empty or
 * --drain seconds (default 15) have passed; the exit status says
 * which. That is for tests. Without it the agent runs until stopped.
 *
 * --selftest reads the source twice and prints each value beside the
 * VMS command that shows it, then exits. It needs no configuration.
 *
 * The log goes to standard output, one line per event, UTC.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "http.h"
#include "model.h"
#include "otlp_json.h"
#include "platform.h"
#include "queue.h"
#include "source.h"
#include "version.h"
#ifdef VO_WITH_TLS
#  include "tls.h"
#endif

#define MAX_BACKOFF_MS  3600000ULL      /* Retry-After is honoured up to here */
#define SKEW_WARN_S     2               /* the probes' threshold */
#define CONFIG_MAX      65536

static void vo_log(const char *fmt, ...)
{
    vo_u64 ns = vo_time_utc_ns();
    time_t secs = (time_t) (ns / 1000000000ULL);
    struct tm *tm = gmtime(&secs);
    va_list ap;

    if (tm != NULL)
        printf("%04d-%02d-%02dT%02d:%02d:%02dZ ", tm->tm_year + 1900,
               tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min,
               tm->tm_sec);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static int read_config(const char *path, vo_config *c)
{
    static char text[CONFIG_MAX];
    char err[160];
    FILE *f = fopen(path, "r");
    size_t n;
    int line;

    if (f == NULL) {
        vo_log("cannot open configuration %s", path);
        return 0;
    }
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    if (n == sizeof text - 1) {
        vo_log("configuration %s is larger than %d bytes", path, CONFIG_MAX - 1);
        return 0;
    }
    text[n] = '\0';
    vo_config_defaults(c);
    line = vo_config_parse(c, text, err, sizeof err);
    if (line != 0) {
        vo_log("configuration %s: %s", path, err);
        return 0;
    }
    return 1;
}

typedef struct {
    vo_config cfg;
    vo_http_target target;
    vo_resource res;
    vo_state state;
    vo_raw raw;
    vo_snapshot snap;
    vo_agent_stats stats;
    vo_queue queue;
    char *payload;
    vo_u64 interval_ms;
    vo_u64 backoff_ms;
    int cpu_logged[VO_CPU_MODES];
    int skew_warned;
} agent;

static void collect(agent *a)
{
    char err[160];
    vo_u64 t0 = vo_time_utc_ns(), t1;
    size_t len = 0;
    int dropped, i;

    if (!vo_source_read(&a->raw, t0, err, sizeof err)) {
        vo_log("collection failed: %s", err);
        return;
    }
    if (vo_model_update(&a->state, &a->raw, &a->snap) > 0)
        for (i = 0; i < VO_CPU_MODES; i++)
            if (a->state.cpu_dropped[i] && !a->cpu_logged[i]) {
                a->cpu_logged[i] = 1;
                vo_log("cpu mode %s went backwards; no longer reported",
                       vo_cpu_mode_name(i));
            }
    t1 = vo_time_utc_ns();
    a->stats.collect_us = t1 > t0 ? (t1 - t0) / 1000 : 0;
    a->stats.queue_length = a->queue.count;

    if (vo_otlp_encode(&a->res, &a->snap, &a->stats, a->payload,
                       a->cfg.max_payload, &len) != VO_OK) {
        a->stats.exports_dropped++;
        vo_log("payload needs %lu bytes, more than max_payload %lu; "
               "dropped -- raise max_payload", (unsigned long) len,
               a->cfg.max_payload);
        return;
    }
    dropped = vo_queue_push(&a->queue, a->payload, len);
    if (dropped == 1) {
        a->stats.exports_dropped++;
        vo_log("queue full (%u); oldest payload dropped", a->queue.nslots);
    }
}

/* Sends the oldest payload. Returns 1 if the queue moved on. */
static int send_one(agent *a)
{
    vo_http_result r;
    vo_u64 sent_at;
    size_t len;
    const char *p = vo_queue_peek(&a->queue, &len);
    int outcome;

    if (p == NULL)
        return 0;
    sent_at = vo_time_utc_ns() / 1000000000ULL;
    outcome = vo_http_post(&a->target, p, len, &r);
    if (r.have_date) {
        long long skew = (long long) sent_at - r.date_unix;
        int off = skew > SKEW_WARN_S || skew < -SKEW_WARN_S;

        a->stats.have_skew = 1;
        a->stats.skew_s = skew;
        /* Once on the way out of tolerance and once on the way back, not
           every minute. The timestamps are not corrected: the fix is NTP
           on this node, and a silent correction would hide the need. */
        if (off && !a->skew_warned)
            vo_log("CLOCK: this system is %lld s %s the collector; every "
                   "timestamp is off by that much -- check NTP",
                   skew < 0 ? -skew : skew, skew < 0 ? "behind" : "ahead of");
        else if (!off && a->skew_warned)
            vo_log("clock now within %d s of the collector", SKEW_WARN_S);
        a->skew_warned = off;
    }

    if (outcome == VO_HTTP_SENT) {
        vo_queue_pop(&a->queue);
        a->stats.exports_sent++;
        a->backoff_ms = 0;
        if (r.rejected > 0)
            vo_log("collector accepted the export but rejected %lld data "
                   "points: %s", r.rejected, r.message);
        return 1;
    }
    if (outcome == VO_HTTP_REJECTED) {
        vo_queue_pop(&a->queue);
        a->stats.exports_dropped++;
        vo_log("EXPORT REJECTED, HTTP %d: %s -- payload dropped; this is a "
               "bug in vmsotel or a misconfigured path", r.status,
               r.message[0] ? r.message : "(no message)");
        return 1;
    }

    a->stats.exports_failed++;
    a->backoff_ms = a->backoff_ms == 0 ? 1000 : a->backoff_ms * 2;
    if (a->backoff_ms > a->interval_ms)
        a->backoff_ms = a->interval_ms;
    if (r.retry_after_s >= 0 &&
        (vo_u64) r.retry_after_s * 1000 > a->backoff_ms)
        a->backoff_ms = (vo_u64) r.retry_after_s * 1000 > MAX_BACKOFF_MS
                        ? MAX_BACKOFF_MS : (vo_u64) r.retry_after_s * 1000;
    if (r.status != 0)
        vo_log("export failed, HTTP %d%s%s; retrying in %lu ms", r.status,
               r.message[0] ? ": " : "", r.message,
               (unsigned long) a->backoff_ms);
    else
        vo_log("export failed: %s; retrying in %lu ms", r.message,
               (unsigned long) a->backoff_ms);
    return 0;
}

int main(int argc, char **argv)
{
    static agent a;
    const char *config_path = NULL;
    unsigned long cycles = 0, done = 0, drain_s = 15;
    vo_u64 now, next_collect, next_send, drain_until = 0;
    char err[160];
    int i;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return vo_source_selftest() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc)
            cycles = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--drain") == 0 && i + 1 < argc)
            drain_s = strtoul(argv[++i], NULL, 10);
        else if (config_path == NULL && argv[i][0] != '-')
            config_path = argv[i];
        else
            config_path = NULL, i = argc;
    }
    if (config_path == NULL) {
        printf("usage: vmsotel CONFIG [--cycles N] [--drain SECONDS]\n"
               "       vmsotel --selftest\n");
        return EXIT_FAILURE;
    }
    if (!read_config(config_path, &a.cfg))
        return EXIT_FAILURE;

    a.target.host = a.cfg.host;
    a.target.port = a.cfg.port;
    a.target.path = a.cfg.path;
    a.target.tls = a.cfg.tls;
    a.target.cafile = a.cfg.cafile[0] ? a.cfg.cafile : NULL;
    a.target.timeout_ms = (int) a.cfg.timeout_s * 1000;
    a.interval_ms = (vo_u64) a.cfg.interval_s * 1000;

#ifndef VO_WITH_TLS
    if (a.cfg.tls) {
        vo_log("tls = yes, but this image was built without TLS");
        return EXIT_FAILURE;
    }
#endif
    if (!vo_queue_init(&a.queue, a.cfg.queue_len, a.cfg.max_payload)) {
        vo_log("cannot allocate a queue of %u x %lu bytes", a.cfg.queue_len,
               a.cfg.max_payload);
        return EXIT_FAILURE;
    }
    a.payload = malloc(a.cfg.max_payload);
    if (a.payload == NULL) {
        vo_log("cannot allocate %lu bytes for a payload", a.cfg.max_payload);
        return EXIT_FAILURE;
    }
    if (!vo_source_open(err, sizeof err)) {
        vo_log("source: %s", err);
        return EXIT_FAILURE;
    }
    vo_source_resource(&a.res);
    a.res.service_version = VO_VERSION;
    a.res.extra = a.cfg.extra;
    a.res.nextra = a.cfg.nextra;
    vo_state_init(&a.state);
    a.stats.start_ns = vo_time_utc_ns();

    vo_log("vmsotel %s starting: %s://%s:%s%s, every %u s, queue %u",
           VO_VERSION, a.cfg.tls ? "https" : "http", a.cfg.host, a.cfg.port,
           a.cfg.path, a.cfg.interval_s, a.cfg.queue_len);
#ifdef VO_WITH_TLS
    if (a.cfg.tls)
        vo_log("TLS through %s, CA %s", vo_tls_library(),
               a.target.cafile ? a.target.cafile : "(system default)");
#endif

    next_collect = next_send = vo_time_mono_ms();
    for (;;) {
        vo_u64 wake;

        now = vo_time_mono_ms();
        if ((cycles == 0 || done < cycles) && now >= next_collect) {
            collect(&a);
            done++;
            next_collect += a.interval_ms;
            if (next_collect <= now)                /* fell behind */
                next_collect = now + a.interval_ms;
            if (cycles != 0 && done == cycles)
                drain_until = now + (vo_u64) drain_s * 1000;
        }
        while (a.queue.count > 0 && vo_time_mono_ms() >= next_send) {
            if (!send_one(&a)) {
                next_send = vo_time_mono_ms() + a.backoff_ms;
                break;
            }
        }

        now = vo_time_mono_ms();
        if (cycles != 0 && done >= cycles) {
            if (a.queue.count == 0) {
                vo_log("done: %lu collections, %llu sent, %llu failed "
                       "attempts, %llu dropped", done,
                       a.stats.exports_sent, a.stats.exports_failed,
                       a.stats.exports_dropped);
                return EXIT_SUCCESS;
            }
            if (now >= drain_until) {
                vo_log("stopping with %u payloads unsent", a.queue.count);
                return EXIT_FAILURE;
            }
        }

        wake = (cycles == 0 || done < cycles) ? next_collect : drain_until;
        if (a.queue.count > 0 && next_send < wake)
            wake = next_send;
        if (wake > now)
            vo_sleep_ms((vo_u32) (wake - now > 60000 ? 60000 : wake - now));
    }
}
