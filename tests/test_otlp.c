/*
 * vmsotel tests: the OTLP JSON encoder.
 *
 *     test_otlp GOLDEN_DIR           compare with the golden payloads
 *     test_otlp --write GOLDEN_DIR   rewrite them (make golden)
 *
 * The golden payloads are also parsed against the official OTLP schema
 * by tools/fake_collector.py --validate, from make check. Rewriting
 * them is a deliberate act: read the diff before committing it.
 */

#include <stdlib.h>

#include "check.h"
#include "fixture.h"

#define BIG 65536

static char out[BIG];

static size_t encode_with(const vo_raw *r, const vo_resource *res,
                          const vo_agent_stats *stats)
{
    vo_state st;
    vo_snapshot s;
    size_t len = 0;

    vo_state_init(&st);
    vo_model_update(&st, r, &s);
    CHECK(vo_otlp_encode(res, &s, stats, out, sizeof out, &len) == VO_OK);
    if (len < sizeof out)
        out[len] = '\0';
    return len;
}

static size_t encode(const vo_raw *r, const vo_resource *res)
{
    return encode_with(r, res, NULL);
}

static int golden(const char *dir, const char *name, int write,
                  const char *got, size_t len)
{
    char path[512];
    static char want[BIG];
    FILE *f;
    size_t n;

    sprintf(path, "%s/%s", dir, name);
    if (write) {
        f = fopen(path, "wb");
        if (f == NULL || fwrite(got, 1, len, f) != len || fputc('\n', f) == EOF) {
            printf("FAIL  cannot write %s\n", path);
            return 1;
        }
        fclose(f);
        printf("wrote %s (%lu bytes)\n", path, (unsigned long) len);
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL  cannot read %s; make golden writes it\n", path);
        check_failures++;
        return 1;
    }
    n = fread(want, 1, sizeof want, f);
    fclose(f);
    check_count++;
    /* The file ends with one newline the encoder does not write. */
    if (n != len + 1 || memcmp(want, got, len) != 0 || want[len] != '\n') {
        size_t i = 0;

        while (i < len && i < n && want[i] == got[i])
            i++;
        printf("FAIL  %s differs from the encoder at byte %lu:\n"
               "  want ...%.60s\n  got  ...%.60s\n", path,
               (unsigned long) i, want + i, got + i);
        check_failures++;
        return 1;
    }
    return 0;
}

static void test_buffer_sizes(void)
{
    vo_raw r;
    vo_resource res;
    vo_state st;
    vo_snapshot s;
    size_t need = 0, len = 0;
    char *buf;

    fixture_full(&r);
    fixture_resource(&res);
    vo_state_init(&st);
    vo_model_update(&st, &r, &s);

    CHECK(vo_otlp_encode(&res, &s, NULL, out, sizeof out, &need) == VO_OK);

    /* Exactly the size needed, with a guard byte after it. */
    buf = malloc(need + 1);
    CHECK(buf != NULL);
    if (buf == NULL)
        return;
    buf[need] = 'Z';
    CHECK(vo_otlp_encode(&res, &s, NULL, buf, need, &len) == VO_OK);
    CHECK_U64(len, need);
    CHECK(memcmp(buf, out, need) == 0);
    CHECK(buf[need] == 'Z');

    /* One byte short: an error, the size needed, and nothing written
       past the end. */
    buf[need - 1] = 'Z';
    CHECK(vo_otlp_encode(&res, &s, NULL, buf, need - 1, &len) == VO_ESPACE);
    CHECK_U64(len, need);
    CHECK(buf[need - 1] == 'Z');

    /* No room at all. */
    buf[0] = 'Z';
    CHECK(vo_otlp_encode(&res, &s, NULL, buf, 0, &len) == VO_ESPACE);
    CHECK_U64(len, need);
    CHECK(buf[0] == 'Z');
    free(buf);
}

static void test_escaping(void)
{
    vo_raw r;
    vo_resource res;
    vo_attr extra[3];

    fixture_minimal(&r);
    fixture_resource(&res);
    extra[0].key = "deployment.environment";
    extra[0].value = "lab \"A\" \\ B";
    extra[1].key = "note\twith\ncontrols\001";
    extra[1].value = "caf\351";                     /* Latin-1 e-acute */
    extra[2].key = "del";
    extra[2].value = "x\177y";
    res.extra = extra;
    res.nextra = 3;
    encode(&r, &res);
    CHECK_CONTAINS(out, "\"stringValue\":\"lab \\\"A\\\" \\\\ B\"");
    CHECK_CONTAINS(out, "\"key\":\"note\\twith\\ncontrols\\u0001\"");
    CHECK_CONTAINS(out, "\"stringValue\":\"caf\\u00e9\"");
    CHECK_CONTAINS(out, "\"stringValue\":\"x\\u007fy\"");

    /* A device name with a quote, in a data-point attribute. */
    fixture_full(&r);
    strcpy(r.disks[1].device, "ODD\"NAME");
    encode(&r, &res);
    CHECK_CONTAINS(out, "\"stringValue\":\"ODD\\\"NAME\"");
}

static void test_numbers(void)
{
    vo_raw r;
    vo_resource res;

    fixture_minimal(&r);
    fixture_resource(&res);
    r.cpu_present[VO_CPU_USER] = 1;
    r.cpu_ticks[VO_CPU_USER] = 12345;               /* 123.45 s */
    r.cpu_present[VO_CPU_IDLE] = 1;
    r.cpu_ticks[VO_CPU_IDLE] = 5;                   /* 0.05 s */
    r.cpu_present[VO_CPU_KERNEL] = 1;
    r.cpu_ticks[VO_CPU_KERNEL] = 100;               /* 1.00 s */
    r.mem_total_bytes = 18446744073709551615ULL;    /* 2^64 - 1 */
    encode(&r, &res);
    CHECK_CONTAINS(out, "\"value\":{\"stringValue\":\"user\"}}],"
                        "\"startTimeUnixNano\":\"1792770509000000000\","
                        "\"timeUnixNano\":\"1793477869000000000\","
                        "\"asDouble\":123.45}");
    CHECK_CONTAINS(out, "\"asDouble\":0.05}");
    CHECK_CONTAINS(out, "\"asDouble\":1.00}");
    CHECK_CONTAINS(out, "\"asInt\":\"18446744073709551615\"");
    /* Uptime: 707360 s between the fixture's boot and now. */
    CHECK_CONTAINS(out, "\"name\":\"system.uptime\",\"unit\":\"s\","
                        "\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asDouble\":707360}]}}");
}

static void test_optional_parts(void)
{
    vo_raw r;
    vo_resource res;

    fixture_minimal(&r);
    fixture_resource(&res);
    res.cluster_member = -1;
    res.host_type = NULL;
    encode(&r, &res);
    CHECK(strstr(out, "system.cpu.time") == NULL);
    CHECK(strstr(out, "system.paging.usage") == NULL);
    CHECK(strstr(out, "system.filesystem.usage") == NULL);
    CHECK(strstr(out, "system.disk.operations") == NULL);
    CHECK(strstr(out, "openvms.cluster.member") == NULL);
    CHECK(strstr(out, "host.type") == NULL);
    CHECK_CONTAINS(out, "system.process.count");
    CHECK_CONTAINS(out, "system.memory.usage");

    /* Memory a source cannot vouch for is absent, not zero. */
    r.mem_present = 0;
    encode(&r, &res);
    CHECK(strstr(out, "system.memory.usage") == NULL);
    CHECK(strstr(out, "system.memory.limit") == NULL);
    r.mem_present = 1;

    /* No boot time: no uptime, and no start time on boot-based sums. */
    r.boot_ns = 0;
    encode(&r, &res);
    CHECK(strstr(out, "system.uptime") == NULL);
    CHECK_CONTAINS(out, "\"name\":\"system.memory.limit\",\"unit\":\"By\","
                        "\"sum\":{\"dataPoints\":[{\"timeUnixNano\"");

    /* Memory figures that do not add up never go negative. */
    r.mem_total_bytes = 10;
    r.mem_free_bytes = 8;
    r.mem_modified_bytes = 8;
    encode(&r, &res);
    CHECK_CONTAINS(out, "\"stringValue\":\"used\"}}],\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asInt\":\"0\"}");
}

static void test_shape(void)
{
    vo_raw r;
    vo_resource res;

    fixture_full(&r);
    fixture_resource(&res);
    encode(&r, &res);
    CHECK_CONTAINS(out, "{\"resourceMetrics\":[{\"resource\":{\"attributes\":"
                        "[{\"key\":\"service.name\",\"value\":{\"stringValue\":"
                        "\"vmsotel\"}}");
    CHECK_CONTAINS(out, "\"schemaUrl\":\"https://opentelemetry.io/schemas/1.44.0\"");
    CHECK_CONTAINS(out, "{\"key\":\"openvms.cluster.member\",\"value\":"
                        "{\"boolValue\":false}}");
    /* Counters monotonic, up-down counters not. */
    CHECK_CONTAINS(out, "\"aggregationTemporality\":2,\"isMonotonic\":true");
    CHECK_CONTAINS(out, "\"aggregationTemporality\":2,\"isMonotonic\":false");
    /* Both the standard and the VMS process state on one point. */
    CHECK_CONTAINS(out, "[{\"key\":\"process.state\",\"value\":{\"stringValue\":"
                        "\"sleeping\"}},{\"key\":\"openvms.process.state\","
                        "\"value\":{\"stringValue\":\"lef\"}}]");
    /* Filesystems carry device, mount point and state. */
    CHECK_CONTAINS(out, "[{\"key\":\"system.device\",\"value\":{\"stringValue\":"
                        "\"X86VMS$DKA200\"}},{\"key\":\"system.filesystem."
                        "mountpoint\",\"value\":{\"stringValue\":\"DISK$TOOLS\"}},"
                        "{\"key\":\"system.filesystem.state\",\"value\":"
                        "{\"stringValue\":\"free\"}}]");
    /* Minor faults are all faults less the major ones. */
    CHECK_CONTAINS(out, "\"major\"}}],\"startTimeUnixNano\":"
                        "\"1793477869000000000\",\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asInt\":\"12801905\"}");
    CHECK_CONTAINS(out, "\"asInt\":\"41041274\"}");
}

static void test_agent_stats(void)
{
    vo_raw r;
    vo_resource res;
    vo_agent_stats a;

    fixture_minimal(&r);
    fixture_resource(&res);
    encode(&r, &res);
    CHECK(strstr(out, "vmsotel.") == NULL);         /* NULL: none */

    fixture_stats(&a);
    encode_with(&r, &res, &a);
    CHECK_CONTAINS(out, "\"name\":\"vmsotel.exports\"");
    CHECK_CONTAINS(out, "\"failed\"}}],\"startTimeUnixNano\":"
                        "\"1793474269000000000\",\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asInt\":\"3\"}");
    CHECK_CONTAINS(out, "\"name\":\"vmsotel.collection.duration\",\"unit\":"
                        "\"s\",\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asDouble\":0.001234}]}}");
    CHECK_CONTAINS(out, "\"name\":\"vmsotel.clock.skew\",\"unit\":\"s\","
                        "\"gauge\":{\"dataPoints\":[{\"timeUnixNano\":"
                        "\"1793477869000000000\",\"asDouble\":-7}]}}");

    /* Extremes of the signed and microsecond writers. */
    a.skew_s = -9223372036854775807LL - 1;
    a.collect_us = 12000001;
    encode_with(&r, &res, &a);
    CHECK_CONTAINS(out, "\"asDouble\":-9223372036854775808}");
    CHECK_CONTAINS(out, "\"asDouble\":12.000001}");
    a.skew_s = 0;
    a.have_skew = 0;
    encode_with(&r, &res, &a);
    CHECK(strstr(out, "vmsotel.clock.skew") == NULL);
}

int main(int argc, char **argv)
{
    int write = argc == 3 && strcmp(argv[1], "--write") == 0;
    const char *dir = write ? argv[2] : (argc == 2 ? argv[1] : NULL);
    vo_raw r;
    vo_resource res;
    vo_agent_stats a;
    size_t len;

    if (dir == NULL) {
        printf("usage: test_otlp [--write] GOLDEN_DIR\n");
        return 2;
    }

    fixture_resource(&res);
    fixture_stats(&a);
    fixture_full(&r);
    len = encode_with(&r, &res, &a);
    golden(dir, "full.json", write, out, len);
    fixture_minimal(&r);
    len = encode(&r, &res);
    golden(dir, "minimal.json", write, out, len);
    if (write)
        return 0;

    test_buffer_sizes();
    test_escaping();
    test_numbers();
    test_optional_parts();
    test_shape();
    test_agent_stats();
    return check_summary("test_otlp");
}
