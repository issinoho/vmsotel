/*
 * vmsotel tests: the pure parts of the transport and the agent --
 * response parsing, HTTP dates, the queue, and the configuration file.
 * The network itself is tested end to end by
 * tools/fake_collector.py --agent-selftest.
 */

#include "check.h"
#include "config.h"
#include "http.h"
#include "queue.h"

static int parse(const char *resp, vo_http_result *r)
{
    memset(r, 0, sizeof *r);
    return vo_http_parse_response(resp, strlen(resp), r);
}

static void test_responses(void)
{
    vo_http_result r;

    CHECK(parse("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Date: Sat, 31 Oct 2026 20:17:56 GMT\r\n\r\n"
                "{\"partialSuccess\":{}}", &r) == VO_HTTP_SENT);
    CHECK(r.status == 200);
    CHECK(r.have_date);
    CHECK_U64(r.date_unix, 1793477876);
    CHECK(r.rejected == -1);
    CHECK(r.retry_after_s == -1);

    /* A 200 that still rejected points, the count as a string or not. */
    CHECK(parse("HTTP/1.1 200 OK\r\n\r\n{\"partialSuccess\":"
                "{\"rejectedDataPoints\":\"5\",\"errorMessage\":"
                "\"bad \\\"unit\\\"\"}}", &r) == VO_HTTP_SENT);
    CHECK(r.rejected == 5);
    CHECK(strcmp(r.message, "bad \"unit\"") == 0);
    CHECK(parse("HTTP/1.1 200 OK\r\n\r\n{\"partialSuccess\":"
                "{\"rejectedDataPoints\": 12}}", &r) == VO_HTTP_SENT);
    CHECK(r.rejected == 12);
    CHECK(parse("HTTP/1.1 200 OK\r\n\r\n{\"partialSuccess\":"
                "{\"rejectedLogRecords\":\"3\"}}", &r) == VO_HTTP_SENT);
    CHECK(r.rejected == 3);

    /* 429 and 503 wait; Retry-After as seconds or as a date. */
    CHECK(parse("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 2\r\n\r\n",
                &r) == VO_HTTP_RETRY);
    CHECK(r.retry_after_s == 2);
    CHECK(parse("HTTP/1.1 503 Service Unavailable\r\n"
                "date: Sat, 31 Oct 2026 20:17:56 GMT\r\n"
                "retry-after: Sat, 31 Oct 2026 20:18:26 GMT\r\n\r\n",
                &r) == VO_HTTP_RETRY);
    CHECK(r.retry_after_s == 30);
    CHECK(parse("HTTP/1.1 500 Internal Server Error\r\n\r\n", &r) ==
          VO_HTTP_RETRY);
    CHECK(parse("HTTP/1.1 502 Bad Gateway\r\n\r\n", &r) == VO_HTTP_RETRY);

    /* Any other status is the request's fault: no retry. */
    CHECK(parse("HTTP/1.1 400 Bad Request\r\n\r\n{\"code\":3,\"message\":"
                "\"proto: unknown field\"}", &r) == VO_HTTP_REJECTED);
    CHECK(r.status == 400);
    CHECK(strcmp(r.message, "proto: unknown field") == 0);
    CHECK(parse("HTTP/1.1 404 Not Found\r\n\r\n", &r) == VO_HTTP_REJECTED);
    CHECK(parse("HTTP/1.0 413 Payload Too Large\r\n\r\n", &r) ==
          VO_HTTP_REJECTED);

    /* Nothing, or not HTTP: try again later. */
    CHECK(parse("", &r) == VO_HTTP_RETRY);
    CHECK(r.status == 0);
    CHECK(parse("\x16\x03\x01\x00\x02\x02\x46 garbage", &r) == VO_HTTP_RETRY);
    CHECK(parse("HTTP/1.1 999 Odd\r\n\r\n", &r) == VO_HTTP_RETRY);
    CHECK(r.status == 0);
    /* Headers cut off before the blank line still yield the status. */
    CHECK(parse("HTTP/1.1 200 OK\r\nDate: Sat, 31 Oct 2026 20:17:56 GMT",
                &r) == VO_HTTP_SENT);
}

static void test_dates(void)
{
    unsigned long t = 0;

    CHECK(vo_http_parse_date("Thu, 01 Jan 1970 00:00:00 GMT", &t));
    CHECK(t == 0);
    CHECK(vo_http_parse_date("Tue, 29 Feb 2028 23:59:59 GMT", &t));
    CHECK(t == 1835481599);
    CHECK(vo_http_parse_date("Sat, 31 Oct 2026 21:40:58 GMT", &t));
    CHECK(t == 1793482858);
    CHECK(!vo_http_parse_date("Sat, 31 Foo 2026 21:40:58 GMT", &t));
    CHECK(!vo_http_parse_date("Sat, 31 Oct 2026 25:40:58 GMT", &t));
    CHECK(!vo_http_parse_date("yesterday", &t));
    /* The range an unsigned long of seconds holds where it is 32 bits:
       1970 to 2105. */
    CHECK(vo_http_parse_date("Mon, 31 Dec 2105 23:59:59 GMT", &t));
    CHECK(t == 4291747199ul);
    CHECK(!vo_http_parse_date("Tue, 01 Jan 2106 00:00:00 GMT", &t));
    CHECK(!vo_http_parse_date("Wed, 31 Dec 1969 23:59:59 GMT", &t));
}

static void test_queue(void)
{
    vo_queue q;
    const char *p;
    size_t n = 0;
    int kind = -1;

    CHECK(!vo_queue_init(&q, 0, 16));
    CHECK(!vo_queue_init(&q, VO_QUEUE_MAX + 1, 16));
    CHECK(vo_queue_init(&q, 3, 8));
    CHECK(vo_queue_peek(&q, &n, &kind) == NULL);

    CHECK(vo_queue_push(&q, "aa", 2, 0) == 0);
    CHECK(vo_queue_push(&q, "bbbbbbbb", 8, 0) == 0);   /* exactly a slot */
    CHECK(vo_queue_push(&q, "ccccccccc", 9, 0) == -1); /* one over */
    CHECK(vo_queue_push(&q, "c", 1, 0) == 0);
    CHECK(q.count == 3);

    /* Full: the oldest goes. */
    CHECK(vo_queue_push(&q, "dd", 2, 0) == 1);
    CHECK(q.count == 3);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && n == 8 && memcmp(p, "bbbbbbbb", 8) == 0);
    vo_queue_pop(&q);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && n == 1 && p[0] == 'c');
    vo_queue_pop(&q);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && n == 2 && memcmp(p, "dd", 2) == 0);
    vo_queue_pop(&q);
    CHECK(vo_queue_peek(&q, &n, &kind) == NULL);
    vo_queue_pop(&q);                               /* empty: harmless */
    CHECK(q.count == 0);

    /* Around the ring more than once. */
    CHECK(vo_queue_push(&q, "1", 1, 0) == 0);
    CHECK(vo_queue_push(&q, "2", 1, 0) == 0);
    vo_queue_pop(&q);
    CHECK(vo_queue_push(&q, "3", 1, 0) == 0);
    CHECK(vo_queue_push(&q, "4", 1, 2) == 0);
    CHECK(vo_queue_push(&q, "5", 1, 1) == 1);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && p[0] == '3' && kind == 0);
    vo_queue_pop(&q);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && p[0] == '4' && kind == 2);
    vo_queue_pop(&q);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && p[0] == '5' && kind == 1);
    vo_queue_free(&q);

    /* Full, with a kind to drop first: the oldest of that kind goes,
       from the middle, and the order of the rest is kept. */
    CHECK(vo_queue_init(&q, 4, 8));
    vo_queue_drop_first(&q, 0);
    CHECK(vo_queue_push(&q, "L1", 2, 1) == 0);
    CHECK(vo_queue_push(&q, "M1", 2, 0) == 0);
    CHECK(vo_queue_push(&q, "L2", 2, 1) == 0);
    CHECK(vo_queue_push(&q, "M2", 2, 0) == 0);
    CHECK(vo_queue_push(&q, "M3", 2, 0) == 1);      /* M1 goes */
    CHECK(q.dropped_kind == 0);
    CHECK(vo_queue_push(&q, "L3", 2, 1) == 1);      /* M2 goes */
    {
        static const char *const want[] = { "L1", "L2", "M3", "L3" };
        int i;

        for (i = 0; i < 4; i++) {
            p = vo_queue_peek(&q, &n, &kind);
            CHECK(p != NULL && n == 2 && memcmp(p, want[i], 2) == 0);
            vo_queue_pop(&q);
        }
    }
    /* None of that kind left: the oldest of any. */
    CHECK(vo_queue_push(&q, "L1", 2, 1) == 0);
    CHECK(vo_queue_push(&q, "L2", 2, 1) == 0);
    CHECK(vo_queue_push(&q, "L3", 2, 1) == 0);
    CHECK(vo_queue_push(&q, "M1", 2, 0) == 0);
    CHECK(vo_queue_push(&q, "M2", 2, 0) == 1);      /* M1 */
    CHECK(vo_queue_push(&q, "L4", 2, 1) == 1);      /* M2 */
    CHECK(vo_queue_push(&q, "L5", 2, 1) == 1);      /* L1: no metrics left */
    CHECK(q.dropped_kind == 1);
    p = vo_queue_peek(&q, &n, &kind);
    CHECK(p != NULL && memcmp(p, "L2", 2) == 0 && kind == 1);
    vo_queue_free(&q);
}

static int config(const char *text, vo_config *c, char *err)
{
    vo_config_defaults(c);
    return vo_config_parse(c, text, err, 160);
}

static void test_config(void)
{
    vo_config c;
    char err[160];

    CHECK(config("# vmsotel\n"
                 "host = 192.168.0.218\n"
                 "   port=4319  \r\n"
                 "! a DCL-style comment\n"
                 "\n"
                 "tls = yes\n"
                 "cafile = SYS$MANAGER:VMSOTEL_CA.PEM\n"
                 "interval = 30\n"
                 "resource.deployment.environment = lab A\n"
                 "resource.site=Edinburgh", &c, err) == 0);
    CHECK(strcmp(c.host, "192.168.0.218") == 0);
    CHECK(strcmp(c.port, "4319") == 0);
    CHECK(c.tls == 1);
    CHECK(strcmp(c.cafile, "SYS$MANAGER:VMSOTEL_CA.PEM") == 0);
    CHECK(c.interval_s == 30);
    CHECK(c.timeout_s == 10);                       /* default kept */
    CHECK(strcmp(c.path, "/v1/metrics") == 0);
    CHECK(c.nextra == 2);
    CHECK(strcmp(c.extra[0].key, "deployment.environment") == 0);
    CHECK(strcmp(c.extra[0].value, "lab A") == 0);
    CHECK(strcmp(c.extra[1].key, "site") == 0);
    CHECK(strcmp(c.extra[1].value, "Edinburgh") == 0);

    CHECK(config("host = h\nprocess.name = MARIADBD*\nprocess.name = OPCOM\n"
                 "process.user = LIGHTTPD\n", &c, err) == 0);
    CHECK(c.procs.nname == 2 && c.procs.nuser == 1);
    CHECK(strcmp(c.procs.name[0], "MARIADBD*") == 0);
    CHECK(strcmp(c.procs.user[0], "LIGHTTPD") == 0);
    CHECK(config("host = h\nprocess.name =\n", &c, err) == 2);
    CHECK(config("host = h\nprocess.name = "
                 "THIRTY_THREE_CHARACTERS_LONG_XXXX\n", &c, err) == 2);
    {
        char text[2048];
        int i;

        strcpy(text, "host = h\n");
        for (i = 0; i < 17; i++)
            strcat(text, "process.user = U\n");
        CHECK(config(text, &c, err) == 18);
        CHECK_CONTAINS(err, "more than 16");
    }

    /* Phase 3: off unless asked for; the files and path have defaults. */
    CHECK(config("host = h\n", &c, err) == 0);
    CHECK(c.logs_opcom == 0 && c.logs_audit == 0);
    CHECK(strcmp(c.logs_path, "/v1/logs") == 0);
    CHECK(strcmp(c.logs_opcom_file, "SYS$MANAGER:OPERATOR.LOG") == 0);
    CHECK(strcmp(c.logs_audit_file, "SYS$MANAGER:SECURITY.AUDIT$JOURNAL") == 0);
    CHECK(config("host = h\nlogs.opcom = yes\nlogs.audit = on\n"
                 "logs.opcom_file = DKA0:[LOGS]OPERATOR.LOG\n"
                 "logs.path = /otlp/v1/logs\n", &c, err) == 0);
    CHECK(c.logs_opcom == 1 && c.logs_audit == 1);
    CHECK(strcmp(c.logs_opcom_file, "DKA0:[LOGS]OPERATOR.LOG") == 0);
    CHECK(strcmp(c.logs_path, "/otlp/v1/logs") == 0);
    CHECK(config("host = h\nlogs.opcom = sometimes\n", &c, err) == 2);
    CHECK(config("host = h\nlogs.path = v1/logs\n", &c, err) == 2);
    CHECK(config("host = h\nlogs.audit_file =\n", &c, err) == 2);

    CHECK(config("port = 4318\n", &c, err) == -1);
    CHECK_CONTAINS(err, "no host");
    CHECK(config("host = h\nbogus = 1\n", &c, err) == 2);
    CHECK_CONTAINS(err, "unknown key \"bogus\"");
    CHECK(config("host = h\nport = 70000\n", &c, err) == 2);
    CHECK(config("host = h\nport = 43a\n", &c, err) == 2);
    CHECK(config("host = h\ninterval = 0\n", &c, err) == 2);
    CHECK(config("host = h\ninterval = -5\n", &c, err) == 2);
    CHECK(config("host = h\nqueue = 65\n", &c, err) == 2);
    CHECK(config("host = h\ntls = maybe\n", &c, err) == 2);
    CHECK(config("host = h\npath = v1/metrics\n", &c, err) == 2);
    CHECK(config("host\n", &c, err) == 1);
    CHECK_CONTAINS(err, "expected key = value");
    CHECK(config("host =\n", &c, err) == 1);
    CHECK(config("host = h\nmax_payload = 4095\n", &c, err) == 2);
    CHECK(config("host = h\ntls = off\nqueue = 64\nmax_payload = 4096",
                 &c, err) == 0);
    CHECK(c.tls == 0 && c.queue_len == 64 && c.max_payload == 4096);
}

/* The shipped example must parse, so it cannot drift from the parser. */
static void test_example(const char *path)
{
    static char text[16384];
    vo_config c;
    char err[160];
    FILE *f = fopen(path, "r");
    size_t n;

    CHECK(f != NULL);
    if (f == NULL)
        return;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    CHECK(config(text, &c, err) == 0);
    if (err[0] != '\0')
        printf("      %s: %s\n", path, err);
    CHECK(strcmp(c.host, "192.168.0.218") == 0);
    CHECK(c.interval_s == 60);
}

int main(int argc, char **argv)
{
    test_responses();
    test_dates();
    test_queue();
    test_config();
    if (argc == 2)
        test_example(argv[1]);
    return check_summary("test_transport");
}
