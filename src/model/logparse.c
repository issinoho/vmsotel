/*
 * vmsotel: the operator log and audit journal parsers. See logparse.h.
 */

#include <string.h>

#include "logparse.h"

#define NS_PER_S 1000000000ULL

/* 17-Nov-1858 to 1-Jan-1970 in 100 ns units. */
#define VMS_UNIX_OFFSET 35067168000000000ULL

vo_u64 vo_vms_time_to_unix_ns(vo_u64 vms, long tdf_s)
{
    vo_u64 ns;
    long long adj;

    if (vms < VMS_UNIX_OFFSET)
        return 0;
    ns = (vms - VMS_UNIX_OFFSET) * 100;
    adj = (long long) tdf_s * (long long) NS_PER_S;
    if (adj > 0 && (vo_u64) adj > ns)
        return 0;
    return adj >= 0 ? ns - (vo_u64) adj : ns + (vo_u64) -adj;
}

/* Days since 1970-01-01 of a civil date (proleptic Gregorian). */
static long long days_from_civil(int y, int m, int d)
{
    long long era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void rec_clear(vo_log_rec *r, int source)
{
    r->source = source;
    r->time_ns = 0;
    r->severity = VO_SEV_INFO;
    r->severity_text = "INFO";
    r->file = NULL;
    r->body[0] = '\0';
    r->nattrs = 0;
}

static int has_attr(const vo_log_rec *r, const char *key)
{
    size_t i;

    for (i = 0; i < r->nattrs; i++)
        if (strcmp(r->attrs[i].key, key) == 0)
            return 1;
    return 0;
}

/* A string attribute from n bytes, trailing NULs and spaces trimmed.
   The first of a key wins: a journal record can repeat a packet type
   (findings, x86 run 4: USERNAME twice in a BREAKIN record). */
static void add_str(vo_log_rec *r, const char *key, const char *s, size_t n)
{
    vo_log_attr *a;

    while (n > 0 && (s[n - 1] == '\0' || s[n - 1] == ' '))
        n--;
    if (n == 0 || r->nattrs >= VO_LOG_ATTRS || has_attr(r, key))
        return;
    a = &r->attrs[r->nattrs++];
    a->key = key;
    a->is_int = 0;
    if (n > VO_LOG_VAL - 1)
        n = VO_LOG_VAL - 1;
    memcpy(a->sval, s, n);
    a->sval[n] = '\0';
}

static void add_int(vo_log_rec *r, const char *key, long long v)
{
    vo_log_attr *a;

    if (r->nattrs >= VO_LOG_ATTRS || has_attr(r, key))
        return;
    a = &r->attrs[r->nattrs++];
    a->key = key;
    a->is_int = 1;
    a->ival = v;
    a->sval[0] = '\0';
}

static const char *attr_val(const vo_log_rec *r, const char *key)
{
    size_t i;

    for (i = 0; i < r->nattrs; i++)
        if (!r->attrs[i].is_int && strcmp(r->attrs[i].key, key) == 0)
            return r->attrs[i].sval;
    return NULL;
}

static void set_severity(vo_log_rec *r, char letter)
{
    switch (letter) {
    case 'W': r->severity = VO_SEV_WARN;  r->severity_text = "WARN";  break;
    case 'E': r->severity = VO_SEV_ERROR; r->severity_text = "ERROR"; break;
    case 'F': r->severity = VO_SEV_FATAL; r->severity_text = "FATAL"; break;
    default:  r->severity = VO_SEV_INFO;  r->severity_text = "INFO";  break;
    }
}

static int is_name_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '$' ||
           c == '_';
}

/*
 * The severity of the first VMS condition message in the text,
 * %FACILITY-L-IDENT, where L is S, I, W, E or F. None: INFO.
 */
static void severity_from_text(vo_log_rec *r, const char *s)
{
    for (; (s = strchr(s, '%')) != NULL; s++) {
        const char *p = s + 1;

        while (is_name_char(*p))
            p++;
        if (p == s + 1 || p[0] != '-' || strchr("SIWEF", p[1]) == NULL ||
            p[1] == '\0' || p[2] != '-' || !is_name_char(p[3]))
            continue;
        set_severity(r, p[1]);
        return;
    }
}

/* ---- the operator log ---------------------------------------------- */

static const char *const months[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

static int digits(const char *s, size_t n, int *v)
{
    size_t i;

    *v = 0;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        *v = *v * 10 + (s[i] - '0');
    }
    return 1;
}

/*
 * The header's time, "D-MMM-YYYY HH:MM:SS.CC" after "OPCOM" and
 * spaces, local, as Unix ns. 0 if it does not parse.
 */
static vo_u64 header_time(const char *line, size_t len, long tdf_s)
{
    char t[26];
    const char *p = line + 11 + 2 + 5;          /* past %x11, 2 sp, OPCOM */
    const char *end = line + len;
    size_t n;
    int day, mon, year, hh, mm, ss, cc, i;
    long long secs;

    while (p < end && *p == ' ')
        p++;
    n = (size_t) (end - p) < sizeof t - 2 ? (size_t) (end - p) : sizeof t - 2;
    memcpy(t, p, n);
    t[n] = '\0';
    /* One- or two-digit day. */
    if (t[1] == '-') {
        memmove(t + 1, t, n + 1);
        t[0] = '0';
    }
    if (strlen(t) < 22 || t[2] != '-' || t[6] != '-' || t[11] != ' ' ||
        t[14] != ':' || t[17] != ':' || t[20] != '.')
        return 0;
    if (!digits(t, 2, &day) || !digits(t + 7, 4, &year) ||
        !digits(t + 12, 2, &hh) || !digits(t + 15, 2, &mm) ||
        !digits(t + 18, 2, &ss) || !digits(t + 21, 2, &cc))
        return 0;
    for (mon = -1, i = 0; i < 12; i++)
        if (memcmp(t + 3, months[i], 3) == 0)
            mon = i;
    if (mon < 0 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60 ||
        year < 1970)
        return 0;
    secs = days_from_civil(year, mon + 1, day) * 86400LL + hh * 3600LL +
           mm * 60LL + ss - tdf_s;
    if (secs < 0)
        return 0;
    return (vo_u64) secs * NS_PER_S + (vo_u64) cc * 10000000ULL;
}

int vo_opcom_is_header(const char *line, size_t len)
{
    return len >= 22 && memcmp(line, "%%%%%%%%%%%  OPCOM ", 19) == 0;
}

void vo_opcom_init(vo_opcom_parser *p, long tdf_s)
{
    memset(p, 0, sizeof *p);
    p->tdf_s = tdf_s;
}

static void opcom_begin(vo_opcom_parser *p, const char *line, size_t len)
{
    rec_clear(&p->rec, VO_LOG_OPCOM);
    p->rec.time_ns = header_time(line, len, p->tdf_s);
    p->in_msg = 1;
    p->body_len = 0;
    p->lines = 0;
    p->truncated = 0;
}

/* Lines joined by newlines; a body that would not fit ends in "...". */
static void opcom_line(vo_opcom_parser *p, const char *line, size_t len)
{
    const size_t cap = VO_LOG_BODY - 4;     /* room for "..." and NUL */
    char *b = p->rec.body;

    p->lines++;
    if (p->truncated)
        return;
    if (p->lines > 1 && p->body_len < cap)
        b[p->body_len++] = '\n';
    if (p->body_len + len > cap) {
        len = cap - p->body_len;
        p->truncated = 1;
    }
    memcpy(b + p->body_len, line, len);
    p->body_len += len;
    if (p->truncated) {
        memcpy(b + p->body_len, "...", 3);
        p->body_len += 3;
    }
    b[p->body_len] = '\0';
}

/* "Message from user IAIN on X86VMS", "Request 3, from user IAIN on
   STUBBY": the sender, and a request's number. */
static void opcom_attrs(vo_log_rec *r)
{
    const char *b = r->body, *u, *e;
    int n;

    if (memcmp(b, "Request ", 8) == 0 && digits(b + 8, 1, &n)) {
        long long req = 0;

        for (e = b + 8; *e >= '0' && *e <= '9'; e++)
            req = req * 10 + (*e - '0');
        add_int(r, "openvms.opcom.request", req);
    }
    e = strchr(b, '\n');
    if ((u = strstr(b, "from user ")) != NULL && (e == NULL || u < e)) {
        const char *w = u + 10, *we = w;

        while (*we && *we != ' ' && *we != '\n')
            we++;
        add_str(r, "openvms.opcom.user", w, (size_t) (we - w));
        if (memcmp(we, " on ", 4) == 0) {
            const char *nd = we + 4, *ne = nd;

            while (*ne && *ne != ' ' && *ne != '\n' && *ne != ',')
                ne++;
            add_str(r, "openvms.opcom.node", nd, (size_t) (ne - nd));
        }
    }
}

static void opcom_finish(const vo_opcom_parser *p, vo_log_rec *out)
{
    memcpy(out, &p->rec, sizeof *out);
    severity_from_text(out, out->body);
    opcom_attrs(out);
}

int vo_opcom_feed(vo_opcom_parser *p, const char *line, size_t len,
                  vo_log_rec *out)
{
    int done = 0;

    if (vo_opcom_is_header(line, len)) {
        if (p->in_msg) {
            opcom_finish(p, out);
            done = 1;
        }
        opcom_begin(p, line, len);
        return done;
    }
    if (len == 0) {
        if (p->in_msg) {
            opcom_finish(p, out);
            done = 1;
        }
        p->in_msg = 0;
        return done;
    }
    if (p->in_msg)
        opcom_line(p, line, len);
    return 0;
}

int vo_opcom_pending(const vo_opcom_parser *p, vo_log_rec *out)
{
    if (!p->in_msg)
        return 0;
    opcom_finish(p, out);
    return 1;
}

size_t vo_opcom_pending_lines(const vo_opcom_parser *p)
{
    return p->in_msg ? p->lines + 1 : 0;
}

/* ---- the audit journal --------------------------------------------- */

/*
 * NSADEF's message types (NSA$C_MSG_*), as build_probes LOGS printed
 * them on both machines, with what each is.
 */
static const struct {
    const char *name;
    const char *what;
} nsa_types[] = {
    { NULL, NULL },
    { "SERVER",     "Audit server control" },
    { "AUDIT",      "Change in auditable events" },
    { "BREAKIN",    "Break-in attempt" },
    { "INSTALL",    "Change to INSTALLed images" },
    { "LOGFAIL",    "Login failure" },
    { "LOGIN",      "Login" },
    { "LOGOUT",     "Logout" },
    { "MOUNT",      "Volume mount or dismount" },
    { "NETPROXY",   "Network proxy change" },
    { "ACCESS",     "Object access" },
    { "SYSUAF",     "SYSUAF change" },
    { "RIGHTSDB",   "Rights database change" },
    { "CUSTOMER",   "Application event" },
    { "CSS",        "CSS application event" },
    { "LP",         "Layered product event" },
    { "PRVAUD",     "Privilege use" },
    { "RESERVED_1", "Reserved" },
    { "RESERVED_2", "Reserved" },
    { "RESERVED_3", "Reserved" },
    { "RESERVED_4", "Reserved" },
    { "SYSTIME",    "System time change" },
    { "SYSGEN",     "SYSGEN parameter change" },
    { "OBJ_CREATE", "Object creation" },
    { "OBJ_DELETE", "Object deletion" },
    { "OBJ_DEACCESS", "Object deaccess" },
    { "OBJ_ACCESS", "Object access" },
    { "CONNECTION", "Connection" },
    { "RESERVED_5", "Reserved" },
    { "NCP",        "NCP command" },
    { "PROCESS",    "Process control" },
    { "OBJECT_SERVICE", "Cluster object service" },
    { "AUTHENTICATE", "Distributed authentication" },
    { "PERSONA",    "Persona event" }
};

#define NSA_NTYPES (sizeof nsa_types / sizeof nsa_types[0])

const char *vo_nsa_type_name(unsigned type)
{
    return type > 0 && type < NSA_NTYPES ? nsa_types[type].name : NULL;
}

/* NSA$C_PKT_* codes used here, from NSADEF. */
enum {
    PKT_ACCOUNT = 2,
    PKT_SYSTEM_NAME = 7,
    PKT_IMAGE_NAME = 19,
    PKT_OBJECT_NAME = 29,
    PKT_PASSWORD = 39,
    PKT_PROCESS_ID = 41,
    PKT_PROCESS_NAME = 42,
    PKT_REMOTE_NODENAME = 44,
    PKT_REMOTE_USERNAME = 45,
    PKT_FINAL_STATUS = 48,
    PKT_TERMINAL = 49,
    PKT_TIME_STAMP = 50,
    PKT_USERNAME = 56,
    PKT_COMMAND_LINE = 145,
    PKT_REMOTE_NODE_FULLNAME = 175,
    PKT_LOG_CLOSED = 254,
    PKT_LOG_OPENED = 255
};

static unsigned le16(const unsigned char *b)
{
    return (unsigned) b[0] | (unsigned) b[1] << 8;
}

static vo_u64 le(const unsigned char *b, unsigned n)
{
    vo_u64 v = 0;

    while (n-- > 0)
        v = v << 8 | b[n];
    return v;
}

static void hex32(char *out, vo_u64 v)
{
    static const char hex[] = "0123456789ABCDEF";
    int k;

    for (k = 7; k >= 0; k--)
        out[7 - k] = hex[(v >> (4 * k)) & 0xf];
    out[8] = '\0';
}

static void packet(vo_log_rec *r, unsigned type, const unsigned char *d,
                   unsigned n, long tdf_s)
{
    const char *s = (const char *) d;
    char h[12];

    switch (type) {
    case PKT_PASSWORD:
    case PKT_COMMAND_LINE:
        break;                          /* never: see logparse.h */
    case PKT_TIME_STAMP:
        if (n == 8 && r->time_ns == 0)
            r->time_ns = vo_vms_time_to_unix_ns(le(d, 8), tdf_s);
        break;
    case PKT_PROCESS_ID:
        if (n == 4 && !has_attr(r, "process.pid")) {
            add_int(r, "process.pid", (long long) le(d, 4));
            hex32(h, le(d, 4));
            add_str(r, "openvms.process.id", h, 8);
        }
        break;
    case PKT_FINAL_STATUS:
        if (n == 4) {
            h[0] = '%';
            h[1] = 'X';
            hex32(h + 2, le(d, 4));
            add_str(r, "openvms.audit.status", h, 10);
        }
        break;
    case PKT_USERNAME:
        add_str(r, "openvms.audit.username", s, n);
        break;
    case PKT_REMOTE_USERNAME:
        add_str(r, "openvms.audit.remote_username", s, n);
        break;
    case PKT_REMOTE_NODENAME:
        add_str(r, "openvms.audit.remote_nodename", s, n);
        break;
    case PKT_REMOTE_NODE_FULLNAME:
        add_str(r, "openvms.audit.remote_node_fullname", s, n);
        break;
    case PKT_TERMINAL:
        add_str(r, "openvms.audit.terminal", s, n);
        break;
    case PKT_PROCESS_NAME:
        add_str(r, "openvms.process.name", s, n);
        break;
    case PKT_ACCOUNT:
        add_str(r, "openvms.audit.account", s, n);
        break;
    case PKT_IMAGE_NAME:
        add_str(r, "openvms.audit.image", s, n);
        break;
    case PKT_OBJECT_NAME:
        add_str(r, "openvms.audit.object_name", s, n);
        break;
    case PKT_SYSTEM_NAME:
        add_str(r, "openvms.audit.system_name", s, n);
        break;
    case PKT_LOG_CLOSED:
    case PKT_LOG_OPENED:
        add_str(r, "openvms.audit.journal", s, n);
        break;
    default:
        break;
    }
}

static void dec(char *out, unsigned v)
{
    char d[12];
    int n = 0;

    do {
        d[n++] = (char) ('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
        *out++ = d[--n];
    *out = '\0';
}

static void body_add(vo_log_rec *r, const char *s)
{
    size_t have = strlen(r->body), n = strlen(s);

    if (have + n > VO_LOG_BODY - 1)
        n = VO_LOG_BODY - 1 - have;
    memcpy(r->body + have, s, n);
    r->body[have + n] = '\0';
}

/*
 * "Login failure: remote user ROOT from SSH_PASSWORD:192.0.2.1 on
 * FTA6741:". The remote user is who asked, as the far end gave it: the
 * name an SSH client sent, or the user who ran SET HOST (findings 41).
 * It is named as such so that a failure from IAIN@MYI64 does not read as
 * IAIN's own login failing. The name tried is not in the journal when it
 * matched no account: USERNAME is then "<login>".
 */
static void audit_body(vo_log_rec *r, unsigned type, unsigned subtype)
{
    const char *user = attr_val(r, "openvms.audit.remote_username");
    const char *label = " remote user ";
    const char *from = attr_val(r, "openvms.audit.remote_nodename");
    const char *term = attr_val(r, "openvms.audit.terminal");
    const char *file = attr_val(r, "openvms.audit.journal");
    char num[24];

    if (user == NULL) {
        user = attr_val(r, "openvms.audit.username");
        label = " user ";
    }
    if (type < NSA_NTYPES && nsa_types[type].what != NULL) {
        body_add(r, nsa_types[type].what);
    } else {
        body_add(r, "Audit event type ");
        dec(num, type);
        body_add(r, num);
    }
    body_add(r, ":");
    if (user) { body_add(r, label);       body_add(r, user); }
    if (from) { body_add(r, " from ");    body_add(r, from); }
    if (term) { body_add(r, " on ");      body_add(r, term); }
    if (file) { body_add(r, " journal "); body_add(r, file); }
    if (!user && !from && !term && !file) {
        body_add(r, " subtype ");
        dec(num, subtype);
        body_add(r, num);
    }
}

int vo_nsa_decode(const unsigned char *b, size_t len, long tdf_s,
                  vo_log_rec *out)
{
    unsigned type, subtype, count, size, version, hdr, i;
    size_t at;

    if (len < 12)
        return 0;
    type = le16(b);
    subtype = le16(b + 2);
    count = le16(b + 6);
    size = le16(b + 8);
    version = b[11];
    hdr = version == 1 ? 12 : 20;       /* NSA$C_V1_MSG_HDR_LENGTH, _HDR_ */
    if (len < hdr || size != len)
        return 0;

    /* Check the packets before using any. */
    for (at = hdr, i = 0; i < count; i++) {
        unsigned psize;

        if (at + 4 > len)
            return 0;
        psize = le16(b + at);
        if (psize < 4 || at + psize > len)
            return 0;
        at += psize;
    }
    if (at != len)
        return 0;

    rec_clear(out, VO_LOG_AUDIT);
    if (type < NSA_NTYPES && nsa_types[type].name != NULL) {
        add_str(out, "openvms.audit.type", nsa_types[type].name,
                strlen(nsa_types[type].name));
    } else {
        add_int(out, "openvms.audit.type_code", (long long) type);
    }
    add_int(out, "openvms.audit.subtype", (long long) subtype);
    for (at = hdr, i = 0; i < count; i++) {
        unsigned psize = le16(b + at);

        packet(out, le16(b + at + 2), b + at + 4, psize - 4, tdf_s);
        at += psize;
    }
    if (type == 3 || type == 5)         /* BREAKIN, LOGFAIL */
        set_severity(out, 'W');
    audit_body(out, type, subtype);
    return 1;
}
