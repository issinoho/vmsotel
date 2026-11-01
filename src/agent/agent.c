/*
 * vmsotel: the agent.
 *
 *     vmsotel CONFIG [--log FILE] [--stop-file FILE]
 *                    [--cycles N] [--drain SECONDS]
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
 * The log goes to standard output, or with --log to a file, one line
 * per event, UTC.
 *
 * Stopping. With --stop-file, the agent looks for that file once a
 * second; when it appears, the agent deletes it (the acknowledgement
 * vmsotel_stop.com waits for), tries once more to send what is queued,
 * logs what is still unsent, writes a summary and exits. SIGINT and
 * SIGTERM do the same. STOP/IDENTIFICATION cannot: it deletes the
 * process without running anything, so the agent also says, in every
 * failure line, how many exports are waiting -- the last such line is
 * what a STOP/ID lost.
 */

#include <signal.h>
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

static const char *log_path;
static const char *stop_path;
static volatile sig_atomic_t stop_signal;

/*
 * One line, to standard output or to --log. A log file is opened,
 * appended to and closed for every line, as vmsguard's gateway does on
 * both machines: the VSI C RTL opens files for exclusive access, so a
 * log held open could not be read with TYPE while the agent runs, and
 * asking for sharing needs an fopen extension /STANDARD=C99 hides.
 */
static void vo_log(const char *fmt, ...)
{
    vo_u64 ns = vo_time_utc_ns();
    time_t secs = (time_t) (ns / 1000000000ULL);
    struct tm *tm = gmtime(&secs);
    char line[1024];
    size_t n = 0;
    va_list ap;
    FILE *f;

    if (tm != NULL)
        n = (size_t) snprintf(line, sizeof line,
                              "%04d-%02d-%02dT%02d:%02d:%02dZ ",
                              tm->tm_year + 1900, tm->tm_mon + 1,
                              tm->tm_mday, tm->tm_hour, tm->tm_min,
                              tm->tm_sec);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);

    if (log_path != NULL && (f = fopen(log_path, "a")) != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    } else {
        printf("%s\n", line);
        fflush(stdout);
    }
}

static void on_signal(int sig)
{
    (void) sig;
    stop_signal = 1;
}

/* A signal, or the stop file -- deleted once seen, so that it is the
   acknowledgement, and so that the next start does not stop at once. */
static int stop_requested(void)
{
    FILE *f;

    if (stop_signal)
        return 1;
    if (stop_path == NULL || (f = fopen(stop_path, "r")) == NULL)
        return 0;
    fclose(f);
    (void) remove(stop_path);
    return 1;
}

static int read_config(const char *path, vo_config *c)
{
    static char text[CONFIG_MAX];
    char err[400];
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
    char err[400];
    vo_u64 t0 = vo_time_precise_ns(), t1;
    size_t len = 0;
    int dropped, i;

    if (!vo_source_read(&a->raw, vo_time_utc_ns(), err, sizeof err)) {
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
    /* The precise clock: the UTC one is whole seconds on VMS, which
       made this always 0 there. */
    t1 = vo_time_precise_ns();
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
        vo_log("export failed, HTTP %d%s%s; retrying in %lu ms; %u waiting",
               r.status, r.message[0] ? ": " : "", r.message,
               (unsigned long) a->backoff_ms, a->queue.count);
    else
        vo_log("export failed: %s; retrying in %lu ms; %u waiting",
               r.message, (unsigned long) a->backoff_ms, a->queue.count);
    return 0;
}

/* After a stop request: one more try at everything queued, then an
   account of whatever could not go. Nothing is lost silently. */
static void finish(agent *a, unsigned long done)
{
    unsigned lost;

    while (a->queue.count > 0 && send_one(a))
        ;
    lost = a->queue.count;
    if (lost > 0) {
        a->stats.exports_dropped += lost;
        vo_log("stopping with %u export%s unsent; %s lost", lost,
               lost == 1 ? "" : "s", lost == 1 ? "it is" : "they are");
    }
    vo_log("stopped: %lu collections, %llu sent, %llu failed attempts, "
           "%llu dropped", done, a->stats.exports_sent,
           a->stats.exports_failed, a->stats.exports_dropped);
}

int main(int argc, char **argv)
{
    static agent a;
    const char *config_path = NULL;
    unsigned long cycles = 0, done = 0, drain_s = 15;
    vo_u64 now, next_collect, next_send, drain_until = 0, last_now;
    char err[400];
    int i;

    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return vo_source_selftest() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc)
            cycles = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--drain") == 0 && i + 1 < argc)
            drain_s = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log_path = argv[++i];
        else if (strcmp(argv[i], "--stop-file") == 0 && i + 1 < argc)
            stop_path = argv[++i];
        else if (config_path == NULL && argv[i][0] != '-')
            config_path = argv[i];
        else
            config_path = NULL, i = argc;
    }
    if (config_path == NULL) {
        printf("usage: vmsotel CONFIG [--log FILE] [--stop-file FILE]\n"
               "                      [--cycles N] [--drain SECONDS]\n"
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
        vo_log("not starting: %s", err);
        return EXIT_FAILURE;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
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
    if (stop_path != NULL)
        vo_log("to stop: create %s", stop_path);

    next_collect = next_send = last_now = vo_time_mono_ms();
    for (;;) {
        vo_u64 wake;

        /*
         * A clock that steps backwards -- set by hand, or by NTP
         * correcting a fast clock -- would leave every deadline in the
         * future by the size of the step. Collect and send now instead.
         */
        now = vo_time_mono_ms();
        if (now + 1000 < last_now) {
            vo_log("clock went back %llu s; collecting now",
                   (last_now - now) / 1000);
            next_collect = next_send = now;
            if (drain_until != 0)
                drain_until = now + (vo_u64) drain_s * 1000;
        }
        last_now = now;

        if (stop_requested()) {
            vo_log("stop requested");
            finish(&a, done);
            return EXIT_SUCCESS;
        }
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

        /* At most a second at a time, so a stop is seen within one. */
        wake = (cycles == 0 || done < cycles) ? next_collect : drain_until;
        if (a.queue.count > 0 && next_send < wake)
            wake = next_send;
        if (wake > now)
            vo_sleep_ms((vo_u32) (wake - now > 1000 ? 1000 : wake - now));
    }
}
