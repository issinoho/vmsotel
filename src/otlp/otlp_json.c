/*
 * vmsotel: OTLP JSON encoder. See otlp_json.h.
 *
 * Follows OpenTelemetry semantic conventions v1.44.0 for names, units,
 * instruments and attribute values, and the protobuf JSON mapping of
 * opentelemetry-proto for the wire form:
 *   - field names in lowerCamelCase
 *   - enums as integers (aggregationTemporality 2 = cumulative)
 *   - 64-bit integers, which covers every timestamp and asInt, as
 *     decimal strings
 * Semconv "counter" is a monotonic cumulative sum, "updowncounter" a
 * non-monotonic one, "gauge" a gauge.
 *
 * Every write goes through put(), which stops writing at the end of the
 * buffer but keeps counting, so a payload that does not fit still
 * reports the size it needs.
 */

#include <string.h>

#include "otlp_json.h"

#define VO_SERVICE_NAME "vmsotel"
#define NS_PER_S        1000000000ULL

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} jw;

static void put(jw *w, const char *s, size_t n)
{
    if (n <= w->cap && w->len <= w->cap - n)
        memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void lit(jw *w, const char *s)
{
    put(w, s, strlen(s));
}

/*
 * A JSON string. VMS text is not UTF-8: names and labels come from DEC
 * MCS, which agrees with Latin-1 for every printable character, so a
 * byte above 127 is written as the Latin-1 code point, \u00XX. Raw
 * bytes above 127 would be invalid UTF-8, which protobuf's JSON parser
 * rejects outright.
 */
static void str(jw *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *) s;

    lit(w, "\"");
    for (; *p; p++) {
        char e[6];

        switch (*p) {
        case '"':  lit(w, "\\\""); break;
        case '\\': lit(w, "\\\\"); break;
        case '\n': lit(w, "\\n");  break;
        case '\r': lit(w, "\\r");  break;
        case '\t': lit(w, "\\t");  break;
        default:
            if (*p < 0x20 || *p >= 0x7f) {
                e[0] = '\\'; e[1] = 'u'; e[2] = '0'; e[3] = '0';
                e[4] = hex[*p >> 4];
                e[5] = hex[*p & 0xf];
                put(w, e, 6);
            } else {
                put(w, (const char *) p, 1);
            }
        }
    }
    lit(w, "\"");
}

/* Unsigned decimal, written here rather than by printf: the encoder
   does no I/O, and the result must not depend on a C RTL. */
static void num(jw *w, vo_u64 v)
{
    char d[20];
    int n = 0;

    do {
        d[sizeof d - 1 - n] = (char) ('0' + (int) (v % 10));
        v /= 10;
        n++;
    } while (v != 0);
    put(w, d + sizeof d - n, (size_t) n);
}

/* A signed number, for the clock skew. */
static void snum(jw *w, long long v)
{
    if (v < 0) {
        lit(w, "-");
        num(w, (vo_u64) -(v + 1) + 1);
    } else {
        num(w, (vo_u64) v);
    }
}

/* Microseconds as seconds, exactly: six decimals from integers. */
static void us_seconds(jw *w, vo_u64 us)
{
    char frac[7];
    vo_u64 f = us % 1000000;
    int i;

    num(w, us / 1000000);
    frac[0] = '.';
    for (i = 6; i >= 1; i--) {
        frac[i] = (char) ('0' + (int) (f % 10));
        f /= 10;
    }
    put(w, frac, 7);
}

/* A 64-bit integer field value: a decimal string. */
static void qnum(jw *w, vo_u64 v)
{
    lit(w, "\"");
    num(w, v);
    lit(w, "\"");
}

/* 10 ms ticks as seconds, exactly: ticks/100 with two decimals. No
   floating point, so no rounding and no dependence on float format. */
static void tick_seconds(jw *w, vo_u64 ticks)
{
    unsigned cents = (unsigned) (ticks % 100);
    char frac[3];

    num(w, ticks / 100);
    frac[0] = '.';
    frac[1] = (char) ('0' + cents / 10);
    frac[2] = (char) ('0' + cents % 10);
    put(w, frac, 3);
}

static void sep(jw *w, int *first)
{
    if (!*first)
        lit(w, ",");
    *first = 0;
}

static void attr_str(jw *w, int *first, const char *key, const char *value)
{
    if (value == NULL)
        return;
    sep(w, first);
    lit(w, "{\"key\":");
    str(w, key);
    lit(w, ",\"value\":{\"stringValue\":");
    str(w, value);
    lit(w, "}}");
}

/* ---- metrics and points -------------------------------------------- */

enum { M_COUNTER, M_UPDOWN, M_GAUGE };
enum { V_INT, V_TICKS, V_SECONDS, V_MICROS };

typedef struct {
    jw *w;
    int first_metric;
    int first_point;
    int kind;
} mw;

static void metric_begin(mw *m, const char *name, const char *unit, int kind)
{
    sep(m->w, &m->first_metric);
    lit(m->w, "{\"name\":");
    str(m->w, name);
    lit(m->w, ",\"unit\":");
    str(m->w, unit);
    lit(m->w, kind == M_GAUGE ? ",\"gauge\":{\"dataPoints\":["
                              : ",\"sum\":{\"dataPoints\":[");
    m->first_point = 1;
    m->kind = kind;
}

static void metric_end(mw *m)
{
    if (m->kind == M_COUNTER)
        lit(m->w, "],\"aggregationTemporality\":2,\"isMonotonic\":true}}");
    else if (m->kind == M_UPDOWN)
        lit(m->w, "],\"aggregationTemporality\":2,\"isMonotonic\":false}}");
    else
        lit(m->w, "]}}");
}

/*
 * One data point, with n string attributes. start_ns 0 leaves
 * startTimeUnixNano out, which OTLP allows; it is always left out for
 * gauges.
 */
static void point_n(mw *m, const vo_attr *attrs, size_t n,
                    vo_u64 start_ns, vo_u64 time_ns, int vkind, vo_u64 value)
{
    jw *w = m->w;
    int first_attr = 1;
    size_t i;

    sep(w, &m->first_point);
    lit(w, "{");
    if (n > 0) {
        lit(w, "\"attributes\":[");
        for (i = 0; i < n; i++)
            attr_str(w, &first_attr, attrs[i].key, attrs[i].value);
        lit(w, "],");
    }
    if (start_ns != 0 && m->kind != M_GAUGE) {
        lit(w, "\"startTimeUnixNano\":");
        qnum(w, start_ns);
        lit(w, ",");
    }
    lit(w, "\"timeUnixNano\":");
    qnum(w, time_ns);
    if (vkind == V_INT) {
        lit(w, ",\"asInt\":");
        qnum(w, value);
    } else if (vkind == V_TICKS) {
        lit(w, ",\"asDouble\":");
        tick_seconds(w, value);
    } else if (vkind == V_MICROS) {
        lit(w, ",\"asDouble\":");
        us_seconds(w, value);
    } else {
        lit(w, ",\"asDouble\":");
        num(w, value);
    }
    lit(w, "}");
}

/* The common case: no attribute (k1 NULL), one, or two (k2 not NULL). */
static void point(mw *m, const char *k1, const char *v1,
                  const char *k2, const char *v2,
                  vo_u64 start_ns, vo_u64 time_ns, int vkind, vo_u64 value)
{
    vo_attr a[2];
    size_t n = 0;

    if (k1 != NULL) {
        a[n].key = k1;
        a[n++].value = v1;
        if (k2 != NULL) {
            a[n].key = k2;
            a[n++].value = v2;
        }
    }
    point_n(m, a, n, start_ns, time_ns, vkind, value);
}

static void counter1(mw *m, const char *name, const char *unit,
                     const vo_count *c, vo_u64 now_ns)
{
    metric_begin(m, name, unit, M_COUNTER);
    point(m, NULL, NULL, NULL, NULL, c->start_ns, now_ns, V_INT, c->value);
    metric_end(m);
}

static vo_u64 minus(vo_u64 a, vo_u64 b)
{
    return a > b ? a - b : 0;
}

static vo_u64 later(vo_u64 a, vo_u64 b)
{
    return a > b ? a : b;
}

static void metrics(mw *m, const vo_snapshot *s)
{
    const vo_raw *r = s->raw;
    const vo_u64 now = r->now_ns, boot = r->boot_ns;
    size_t i;
    int any;

    any = 0;
    for (i = 0; i < VO_CPU_MODES; i++)
        any |= s->cpu_present[i];
    if (any) {
        metric_begin(m, "system.cpu.time", "s", M_COUNTER);
        for (i = 0; i < VO_CPU_MODES; i++)
            if (s->cpu_present[i])
                point(m, "cpu.mode", vo_cpu_mode_name((int) i), NULL, NULL,
                      boot, now, V_TICKS, r->cpu_ticks[i]);
        metric_end(m);
    }

    metric_begin(m, "system.cpu.logical.count", "{cpu}", M_UPDOWN);
    point(m, NULL, NULL, NULL, NULL, boot, now, V_INT, r->cpu_count);
    metric_end(m);

    if (r->mem_present) {
        /* "modified" is VMS's modified page list, kept apart because it is
           neither free nor in use by a process. Not a semconv value. */
        metric_begin(m, "system.memory.usage", "By", M_UPDOWN);
        point(m, "system.memory.state", "used", NULL, NULL, boot, now, V_INT,
              minus(r->mem_total_bytes,
                    r->mem_free_bytes + r->mem_modified_bytes));
        point(m, "system.memory.state", "free", NULL, NULL, boot, now, V_INT,
              r->mem_free_bytes);
        point(m, "system.memory.state", "modified", NULL, NULL, boot, now, V_INT,
              r->mem_modified_bytes);
        metric_end(m);

        metric_begin(m, "system.memory.limit", "By", M_UPDOWN);
        point(m, NULL, NULL, NULL, NULL, boot, now, V_INT, r->mem_total_bytes);
        metric_end(m);
    }

    /* Hard faults are those that read the page in; the rest are soft.
       The derived minor count takes the later of the two start times. */
    metric_begin(m, "system.paging.faults", "{fault}", M_COUNTER);
    point(m, "system.paging.fault.type", "major", NULL, NULL,
          s->page_reads.start_ns, now, V_INT, s->page_reads.value);
    point(m, "system.paging.fault.type", "minor", NULL, NULL,
          later(s->faults.start_ns, s->page_reads.start_ns), now, V_INT,
          minus(s->faults.value, s->page_reads.value));
    metric_end(m);

    metric_begin(m, "system.paging.operations", "{operation}", M_COUNTER);
    point(m, "system.paging.direction", "in", NULL, NULL,
          s->page_read_ios.start_ns, now, V_INT, s->page_read_ios.value);
    point(m, "system.paging.direction", "out", NULL, NULL,
          s->page_write_ios.start_ns, now, V_INT, s->page_write_ios.value);
    metric_end(m);

    if (r->pagefile_present) {
        metric_begin(m, "system.paging.usage", "By", M_UPDOWN);
        point(m, "system.paging.state", "used", NULL, NULL, boot, now, V_INT,
              minus(r->pagefile_total_bytes, r->pagefile_free_bytes));
        point(m, "system.paging.state", "free", NULL, NULL, boot, now, V_INT,
              r->pagefile_free_bytes);
        metric_end(m);
    }

    /* Each VMS state is its own series, labelled with both the standard
       process.state and the VMS name, so a stock dashboard can sum by
       the first and an administrator can read the second. */
    metric_begin(m, "system.process.count", "{process}", M_UPDOWN);
    for (i = 0; i < VO_PROC_STATES; i++)
        point(m, "process.state", vo_proc_state_std((int) i),
              "openvms.process.state", vo_proc_state_name((int) i),
              boot, now, V_INT, r->proc_states[i]);
    metric_end(m);

    if (boot != 0 && now > boot) {
        metric_begin(m, "system.uptime", "s", M_GAUGE);
        point(m, NULL, NULL, NULL, NULL, 0, now, V_SECONDS,
              (now - boot) / NS_PER_S);
        metric_end(m);
    }

    if (r->ndisks > 0) {
        metric_begin(m, "system.filesystem.usage", "By", M_UPDOWN);
        for (i = 0; i < r->ndisks && i < VO_MAX_DISKS; i++) {
            const vo_raw_disk *d = &r->disks[i];
            vo_attr a[3];
            size_t n = 0;

            a[n].key = "system.device";
            a[n++].value = d->device;
            if (d->mountpoint[0] != '\0') {
                a[n].key = "system.filesystem.mountpoint";
                a[n++].value = d->mountpoint;
            }
            a[n].key = "system.filesystem.state";
            a[n++].value = "used";
            point_n(m, a, n, boot, now, V_INT,
                    minus(d->total_bytes, d->free_bytes));
            a[n - 1].value = "free";
            point_n(m, a, n, boot, now, V_INT, d->free_bytes);
        }
        metric_end(m);
    }

    any = 0;
    for (i = 0; i < r->ndisks && i < VO_MAX_DISKS; i++)
        any |= s->disk_ops_present[i];
    if (any) {
        /* DVI$_OPCNT has no read/write split, so no disk.io.direction. */
        metric_begin(m, "system.disk.operations", "{operation}", M_COUNTER);
        for (i = 0; i < r->ndisks && i < VO_MAX_DISKS; i++)
            if (s->disk_ops_present[i])
                point(m, "system.device", r->disks[i].device, NULL, NULL,
                      s->disk_ops[i].start_ns, now, V_INT,
                      s->disk_ops[i].value);
        metric_end(m);
    }

    counter1(m, "openvms.io.direct", "{operation}", &s->direct_ios, now);
    counter1(m, "openvms.io.buffered", "{operation}", &s->buffered_ios, now);
}

/*
 * The agent's own health. vmsotel.exports counts payloads by outcome:
 * sent, failed (an attempt that will be retried) and dropped (lost to a
 * full queue or a rejection). The rest are gauges of the moment.
 */
static void agent_metrics(mw *m, const vo_agent_stats *a, vo_u64 now)
{
    metric_begin(m, "vmsotel.exports", "{request}", M_COUNTER);
    point(m, "vmsotel.outcome", "sent", NULL, NULL, a->start_ns, now, V_INT,
          a->exports_sent);
    point(m, "vmsotel.outcome", "failed", NULL, NULL, a->start_ns, now, V_INT,
          a->exports_failed);
    point(m, "vmsotel.outcome", "dropped", NULL, NULL, a->start_ns, now,
          V_INT, a->exports_dropped);
    metric_end(m);

    metric_begin(m, "vmsotel.queue.length", "{request}", M_GAUGE);
    point(m, NULL, NULL, NULL, NULL, 0, now, V_INT, a->queue_length);
    metric_end(m);

    metric_begin(m, "vmsotel.collection.duration", "s", M_GAUGE);
    point(m, NULL, NULL, NULL, NULL, 0, now, V_MICROS, a->collect_us);
    metric_end(m);

    if (a->have_skew) {
        /* Signed, so written here rather than through point(). */
        metric_begin(m, "vmsotel.clock.skew", "s", M_GAUGE);
        lit(m->w, "{\"timeUnixNano\":");
        qnum(m->w, now);
        lit(m->w, ",\"asDouble\":");
        snum(m->w, a->skew_s);
        lit(m->w, "}");
        metric_end(m);
    }
}

/* ---- the request --------------------------------------------------- */

static void resource(jw *w, const vo_resource *res)
{
    int first = 1;
    size_t i;

    lit(w, "\"resource\":{\"attributes\":[");
    attr_str(w, &first, "service.name", VO_SERVICE_NAME);
    attr_str(w, &first, "service.version", res->service_version);
    attr_str(w, &first, "host.name", res->host_name);
    attr_str(w, &first, "host.arch", res->host_arch);
    attr_str(w, &first, "host.type", res->host_type);
    attr_str(w, &first, "os.type", "openvms");
    attr_str(w, &first, "os.version", res->os_version);
    attr_str(w, &first, "openvms.scsnode", res->scsnode);
    if (res->cluster_member == 0 || res->cluster_member == 1) {
        sep(w, &first);
        lit(w, "{\"key\":\"openvms.cluster.member\",\"value\":{\"boolValue\":");
        lit(w, res->cluster_member ? "true" : "false");
        lit(w, "}}");
    }
    for (i = 0; i < res->nextra; i++)
        attr_str(w, &first, res->extra[i].key, res->extra[i].value);
    lit(w, "]}");
}

int vo_otlp_encode(const vo_resource *res, const vo_snapshot *snap,
                   const vo_agent_stats *stats,
                   char *buf, size_t cap, size_t *len)
{
    jw w;
    mw m;

    w.buf = buf;
    w.cap = cap;
    w.len = 0;
    m.w = &w;
    m.first_metric = 1;
    m.first_point = 1;
    m.kind = M_GAUGE;

    lit(&w, "{\"resourceMetrics\":[{");
    resource(&w, res);
    lit(&w, ",\"scopeMetrics\":[{\"scope\":{\"name\":");
    str(&w, VO_SERVICE_NAME);
    if (res->service_version != NULL) {
        lit(&w, ",\"version\":");
        str(&w, res->service_version);
    }
    lit(&w, "},\"schemaUrl\":");
    str(&w, VO_SCHEMA_URL);
    lit(&w, ",\"metrics\":[");
    metrics(&m, snap);
    if (stats != NULL)
        agent_metrics(&m, stats, snap->raw->now_ns);
    lit(&w, "]}]}]}");

    *len = w.len;
    return w.len <= cap ? VO_OK : VO_ESPACE;
}
