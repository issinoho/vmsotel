/*
 * vmsotel tests: the operator log and audit journal parsers (phase 3).
 *
 * The operator log lines are as the two machines printed them
 * (findings 34, 35). The I64 audit record is rebuilt byte for byte
 * from what probe_logs decoded of it (findings 37); the login failure
 * and break-in records follow the layout of x86's, with documentation
 * addresses in place of the real ones.
 */

#include "check.h"
#include "logparse.h"

static const char *attr(const vo_log_rec *r, const char *key)
{
    size_t i;

    for (i = 0; i < r->nattrs; i++)
        if (strcmp(r->attrs[i].key, key) == 0)
            return r->attrs[i].is_int ? "(int)" : r->attrs[i].sval;
    return NULL;
}

static long long iattr(const vo_log_rec *r, const char *key)
{
    size_t i;

    for (i = 0; i < r->nattrs; i++)
        if (strcmp(r->attrs[i].key, key) == 0 && r->attrs[i].is_int)
            return r->attrs[i].ival;
    return -1;
}

static int feed(vo_opcom_parser *p, const char *line, vo_log_rec *out)
{
    return vo_opcom_feed(p, line, strlen(line), out);
}

static void test_vms_time(void)
{
    /* I64's journal record, TIME_STAMP packet f6 a0 09 1a 1d 4d bc 00,
       journal ;3 created 1-NOV-2026 00:05:00.75 (local = UTC in
       November). */
    CHECK_U64(vo_vms_time_to_unix_ns(0x00bc4d1d1a09a0f6ULL, 0),
              1793491500783231000ULL);
    CHECK_U64(vo_vms_time_to_unix_ns(0x00bc4d1d1a09a0f6ULL, 3600),
              1793491500783231000ULL - 3600000000000ULL);
    CHECK_U64(vo_vms_time_to_unix_ns(0x00bc4d1d1a09a0f6ULL, -18000),
              1793491500783231000ULL + 18000000000000ULL);
    CHECK_U64(vo_vms_time_to_unix_ns(1000, 0), 0);
}

static void test_opcom(void)
{
    vo_opcom_parser p;
    vo_log_rec r;

    vo_opcom_init(&p, 0);
    /* Lines before the first header belong to no message. */
    CHECK(feed(&p, "stray line", &r) == 0);
    CHECK(feed(&p, "", &r) == 0);
    CHECK(vo_opcom_pending(&p, &r) == 0);

    CHECK(feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:17.66  %%%%%%%%%%%",
               &r) == 0);
    CHECK(feed(&p, "Message from user IAIN on X86VMS", &r) == 0);
    CHECK(feed(&p, "_X86VMS$FTA5864:, vmsotel probe_logs marker 1793526797",
               &r) == 0);
    CHECK_U64(vo_opcom_pending_lines(&p), 3);
    CHECK(vo_opcom_pending(&p, &r) == 1);
    CHECK_U64(r.time_ns, 1793526797660000000ULL);

    /* The empty record ends it. */
    memset(&r, 0, sizeof r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK(r.source == VO_LOG_OPCOM);
    CHECK_U64(r.time_ns, 1793526797660000000ULL);
    CHECK(strcmp(r.body, "Message from user IAIN on X86VMS\n"
                 "_X86VMS$FTA5864:, vmsotel probe_logs marker 1793526797")
          == 0);
    CHECK(r.severity == VO_SEV_INFO);
    CHECK(r.event_name == NULL);
    CHECK(attr(&r, "openvms.opcom.user") != NULL &&
          strcmp(attr(&r, "openvms.opcom.user"), "IAIN") == 0);
    CHECK(attr(&r, "openvms.opcom.node") != NULL &&
          strcmp(attr(&r, "openvms.opcom.node"), "X86VMS") == 0);
    CHECK_U64(vo_opcom_pending_lines(&p), 0);

    /* A header ends the one before too, and starts the next. */
    CHECK(feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:37.32  %%%%%%%%%%%",
               &r) == 0);
    CHECK(feed(&p, "Logfile time stamp", &r) == 0);
    CHECK(feed(&p, "%%%%%%%%%%%  OPCOM  31-OCT-2026 20:00:00.00  %%%%%%%%%%%",
               &r) == 1);
    CHECK(strcmp(r.body, "Logfile time stamp") == 0);
    CHECK(attr(&r, "openvms.opcom.user") == NULL);
    CHECK(feed(&p, "Request 3, from user IAIN on STUBBY", &r) == 0);
    CHECK(feed(&p, "\"mount tape please\"", &r) == 0);
    CHECK(vo_opcom_pending(&p, &r) == 1);
    CHECK_U64(r.time_ns, 1793476800000000000ULL);  /* two-digit day */
    CHECK(iattr(&r, "openvms.opcom.request") == 3);
    CHECK(strcmp(attr(&r, "openvms.opcom.user"), "IAIN") == 0);
    CHECK(strcmp(attr(&r, "openvms.opcom.node"), "STUBBY") == 0);
    /* pending leaves it pending. */
    CHECK_U64(vo_opcom_pending_lines(&p), 3);

    /* I64's five-minutely message: no condition code, so INFO. */
    vo_opcom_init(&p, 0);
    feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 10:16:23.82  %%%%%%%%%%%", &r);
    feed(&p, "Message from user DECNET on MYI64", &r);
    feed(&p, "DECnet event 4.7, circuit down, circuit fault", &r);
    feed(&p, "From node 1.2 (MYI64), 17-FEB-1994 00:00:00.00", &r);
    feed(&p, "Circuit EIA-0, Line synchronization lost", &r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK(r.severity == VO_SEV_INFO);
    CHECK(strcmp(attr(&r, "openvms.opcom.user"), "DECNET") == 0);
    CHECK(strcmp(attr(&r, "openvms.opcom.node"), "MYI64") == 0);

    /* The severity of the first condition message. */
    vo_opcom_init(&p, 0);
    feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 10:16:23.82  %%%%%%%%%%%", &r);
    feed(&p, "Message from user SYSTEM on X86VMS", &r);
    feed(&p, "100% done, %%% not a code, %SYSTEM-W-DISMOUNT, volume gone", &r);
    feed(&p, "%SYSTEM-F-ABORT, later", &r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK(r.severity == VO_SEV_WARN);
    CHECK(strcmp(r.severity_text, "WARN") == 0);
    vo_opcom_init(&p, 0);
    feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 10:16:23.82  %%%%%%%%%%%", &r);
    feed(&p, "%LICENSE-E-NOAUTH, DEC VMS-USER use is not authorized", &r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK(r.severity == VO_SEV_ERROR);

    /* The time differential: local 10:00 at +01:00 is 09:00 UTC. */
    vo_opcom_init(&p, 3600);
    feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:17.66  %%%%%%%%%%%", &r);
    feed(&p, "x", &r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK_U64(r.time_ns, 1793526797660000000ULL - 3600000000000ULL);

    /* A header whose time does not parse: time unknown, not wrong. */
    vo_opcom_init(&p, 0);
    feed(&p, "%%%%%%%%%%%  OPCOM   garbled  %%%%%%%%%%%", &r);
    feed(&p, "x", &r);
    CHECK(feed(&p, "", &r) == 1);
    CHECK_U64(r.time_ns, 0);
    CHECK(vo_opcom_is_header("%%%%%%%%%%%  OPCOM   1-", 22));
    CHECK(!vo_opcom_is_header("%%%%%%%%%%% OPCOM  1-NOV", 24));
}

static void test_opcom_long(void)
{
    vo_opcom_parser p;
    vo_log_rec r;
    char line[60];
    int i;
    size_t n;

    memset(line, 'A', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    vo_opcom_init(&p, 0);
    feed(&p, "%%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:17.66  %%%%%%%%%%%", &r);
    for (i = 0; i < 100; i++)
        feed(&p, line, &r);
    CHECK_U64(vo_opcom_pending_lines(&p), 101);
    CHECK(feed(&p, "", &r) == 1);
    n = strlen(r.body);
    CHECK(n < VO_LOG_BODY);
    CHECK(n >= VO_LOG_BODY - 4);
    CHECK(strcmp(r.body + n - 3, "...") == 0);
}

/* ---- audit ---------------------------------------------------------- */

static size_t put_pkt(unsigned char *b, size_t at, unsigned type,
                      const void *data, unsigned n)
{
    b[at] = (unsigned char) ((n + 4) & 0xff);
    b[at + 1] = (unsigned char) ((n + 4) >> 8);
    b[at + 2] = (unsigned char) (type & 0xff);
    b[at + 3] = (unsigned char) (type >> 8);
    memcpy(b + at + 4, data, n);
    return at + 4 + n;
}

static size_t put_hdr(unsigned char *b, unsigned type, unsigned subtype,
                      unsigned flags, unsigned count, size_t size)
{
    memset(b, 0, 20);
    b[0] = (unsigned char) type;
    b[2] = (unsigned char) subtype;
    b[4] = (unsigned char) flags;
    b[6] = (unsigned char) count;
    b[8] = (unsigned char) (size & 0xff);
    b[9] = (unsigned char) (size >> 8);
    b[11] = 3;
    return 20;
}

static const unsigned char ts_i64[8] =
    { 0xf6, 0xa0, 0x09, 0x1a, 0x1d, 0x4d, 0xbc, 0x00 };

static void test_nsa_i64_record(void)
{
    static const unsigned char sysid[4] = { 0x02, 0x04, 0x00, 0x00 };
    static const char jnl[] = "SYS$COMMON:[SYSMGR]SECURITY.AUDIT$JOURNAL;2";
    unsigned char b[200];
    vo_log_rec r;
    size_t at;

    at = put_hdr(b, 2, 12, 0x04, 6, 120);
    at = put_pkt(b, at, 6, "SECURITY", 8);
    at = put_pkt(b, at, 3, "SECURITY", 8);
    at = put_pkt(b, at, 50, ts_i64, 8);
    at = put_pkt(b, at, 7, "MYI64", 5);
    at = put_pkt(b, at, 8, sysid, 4);
    at = put_pkt(b, at, 254, jnl, 43);
    CHECK_U64(at, 120);
    /* The bytes probe_logs dumped: 02 00 0c 00 04 00 06 00 78 00 00 03 */
    CHECK(b[8] == 0x78 && b[11] == 0x03 && b[24] == 'S');
    CHECK(vo_nsa_decode(b, at, 0, &r) == 1);
    CHECK(r.source == VO_LOG_AUDIT);
    CHECK_U64(r.time_ns, 1793491500783231000ULL);
    CHECK(strcmp(r.event_name, "openvms.audit.audit") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.type"), "AUDIT") == 0);
    CHECK(iattr(&r, "openvms.audit.subtype") == 12);
    CHECK(strcmp(attr(&r, "openvms.audit.system_name"), "MYI64") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.journal"), jnl) == 0);
    CHECK(r.severity == VO_SEV_INFO);
    CHECK(strcmp(r.body, "Change in auditable events: journal "
                 "SYS$COMMON:[SYSMGR]SECURITY.AUDIT$JOURNAL;2") == 0);
}

static size_t logfail(unsigned char *b)
{
    static const unsigned char pid[4] = { 0x1f, 0xef, 0x00, 0x00 };
    static const unsigned char st[4] = { 0xf4, 0x80, 0xd3, 0x00 };
    size_t at = 20;

    at = put_pkt(b, at, 6, "SECURITY", 8);
    at = put_pkt(b, at, 3, "SECURITY", 8);
    at = put_pkt(b, at, 41, pid, 4);
    at = put_pkt(b, at, 42, "SSH$SSH_5627", 12);
    at = put_pkt(b, at, 56, "SSH$SSH", 7);
    at = put_pkt(b, at, 48, st, 4);
    at = put_pkt(b, at, 45, "ROOT", 4);
    at = put_pkt(b, at, 44, "SSH_PASSWORD:192.0.2.10", 23);
    at = put_pkt(b, at, 50, ts_i64, 8);
    at = put_pkt(b, at, 7, "X86VMS", 6);
    put_hdr(b, 5, 5, 0x1c, 10, at);
    return at;
}

static void test_nsa_logfail(void)
{
    unsigned char b[300];
    vo_log_rec r;
    size_t n = logfail(b);

    CHECK(vo_nsa_decode(b, n, 0, &r) == 1);
    CHECK(strcmp(r.event_name, "openvms.audit.logfail") == 0);
    CHECK(r.severity == VO_SEV_WARN);
    CHECK(strcmp(attr(&r, "openvms.audit.remote_username"), "ROOT") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.username"), "SSH$SSH") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.remote_nodename"),
                 "SSH_PASSWORD:192.0.2.10") == 0);
    CHECK(iattr(&r, "process.pid") == 0xef1f);
    CHECK(strcmp(attr(&r, "openvms.process.id"), "0000EF1F") == 0);
    CHECK(strcmp(attr(&r, "openvms.process.name"), "SSH$SSH_5627") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.status"), "%X00D380F4") == 0);
    CHECK(strcmp(r.body, "Login failure: user ROOT from "
                 "SSH_PASSWORD:192.0.2.10") == 0);

    /* Every way the sizes can fail to add up. */
    CHECK(vo_nsa_decode(b, n - 1, 0, &r) == 0);    /* size field != len */
    b[6] = 11;                                      /* one packet too many */
    CHECK(vo_nsa_decode(b, n, 0, &r) == 0);
    b[6] = 9;                                       /* one too few */
    CHECK(vo_nsa_decode(b, n, 0, &r) == 0);
    b[6] = 10;
    b[20] = 2;                                      /* packet size < 4 */
    CHECK(vo_nsa_decode(b, n, 0, &r) == 0);
    b[20] = 0xff;                                   /* past the end */
    CHECK(vo_nsa_decode(b, n, 0, &r) == 0);
    b[20] = 12;
    CHECK(vo_nsa_decode(b, n, 0, &r) == 1);
    CHECK(vo_nsa_decode(b, 10, 0, &r) == 0);
}

static void test_nsa_breakin(void)
{
    unsigned char b[300];
    vo_log_rec r;
    size_t at = 20, i;

    at = put_pkt(b, at, 56, "ROOT", 4);
    at = put_pkt(b, at, 56, "OTHER", 5);           /* repeated: first wins */
    at = put_pkt(b, at, 39, "hunter2", 7);         /* PASSWORD */
    at = put_pkt(b, at, 145, "SET PASSWORD hunter2", 20);
    at = put_pkt(b, at, 49, "FTA6761:", 8);
    at = put_pkt(b, at, 49, "_FTA6761:", 9);
    put_hdr(b, 3, 5, 0x1c, 6, at);
    CHECK(vo_nsa_decode(b, at, 0, &r) == 1);
    CHECK(strcmp(r.event_name, "openvms.audit.breakin") == 0);
    CHECK(r.severity == VO_SEV_WARN);
    CHECK(strcmp(attr(&r, "openvms.audit.username"), "ROOT") == 0);
    CHECK(strcmp(attr(&r, "openvms.audit.terminal"), "FTA6761:") == 0);
    CHECK(r.time_ns == 0);
    /* The password and the command line go nowhere. */
    CHECK(strstr(r.body, "hunter2") == NULL);
    for (i = 0; i < r.nattrs; i++)
        CHECK(strstr(r.attrs[i].sval, "hunter2") == NULL);
    CHECK(strcmp(r.body, "Break-in attempt: user ROOT on FTA6761:") == 0);

    /* An unknown type is kept, by number. */
    put_hdr(b, 77, 1, 0, 6, at);
    CHECK(vo_nsa_decode(b, at, 0, &r) == 1);
    CHECK(r.event_name == NULL);
    CHECK(iattr(&r, "openvms.audit.type_code") == 77);
    CHECK(strncmp(r.body, "Audit event type 77:", 20) == 0);
    CHECK(vo_nsa_type_name(5) != NULL &&
          strcmp(vo_nsa_type_name(5), "LOGFAIL") == 0);
    CHECK(vo_nsa_type_name(0) == NULL && vo_nsa_type_name(34) == NULL);

    /* No packets with anything to say: the subtype. */
    put_hdr(b, 21, 1, 0, 0, 20);
    CHECK(vo_nsa_decode(b, 20, 0, &r) == 1);
    CHECK(strcmp(r.body, "System time change: subtype 1") == 0);
}

int main(void)
{
    test_vms_time();
    test_opcom();
    test_opcom_long();
    test_nsa_i64_record();
    test_nsa_logfail();
    test_nsa_breakin();
    return check_summary("test_logs");
}
