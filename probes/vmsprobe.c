/*
 * vmsotel probes: helpers shared by the system-service probes.
 * See vmsprobe.h.
 */

#include "vmsprobe.h"

int probe_failures = 0;

void probe_ok(const char *what)
{
    printf("  ok    %s\n", what);
}

void probe_fail(const char *what)
{
    printf("  FAIL  %s\n", what);
    probe_failures++;
}

/* Status code to message text, e.g. "%SYSTEM-F-NOPRIV, ...". */
const char *probe_msg(unsigned int status)
{
    static char text[257];
    unsigned short len = 0;
    struct dsc$descriptor_s d;

    d.dsc$w_length = (unsigned short) (sizeof text - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = text;

    if (!(sys$getmsg(status, &len, (void *) &d, 15, 0) & 1)) {
        sprintf(text, "%%X%08X", status);
        return text;
    }
    text[len] = '\0';
    return text;
}

/* Little-endian reads; VMS is little-endian on every architecture it
   has run on, but say so rather than cast. */
unsigned int probe_le32(const unsigned char *b, unsigned n)
{
    unsigned int v = 0;
    unsigned i;

    if (n > 4)
        n = 4;
    for (i = n; i > 0; i--)
        v = (v << 8) | b[i - 1];
    return v;
}

probe_q probe_le(const unsigned char *b, unsigned n)
{
    probe_q q;

    if (n > 8)
        n = 8;
    q.lo = probe_le32(b, n < 4 ? n : 4);
    q.hi = n > 4 ? probe_le32(b + 4, n - 4) : 0;
    return q;
}

probe_q probe_q_make(unsigned int hi, unsigned int lo)
{
    probe_q q;

    q.hi = hi;
    q.lo = lo;
    return q;
}

probe_q probe_q_add(probe_q a, probe_q b)
{
    probe_q r;

    r.lo = (a.lo + b.lo) & 0xFFFFFFFFu;
    r.hi = (a.hi + b.hi + (r.lo < a.lo)) & 0xFFFFFFFFu;
    return r;
}

probe_q probe_q_sub(probe_q a, probe_q b)
{
    probe_q r;

    r.lo = (a.lo - b.lo) & 0xFFFFFFFFu;
    r.hi = (a.hi - b.hi - (a.lo < b.lo)) & 0xFFFFFFFFu;
    return r;
}

int probe_q_cmp(probe_q a, probe_q b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

/* Shift-and-subtract, a bit at a time. The remainder stays below d,
   and d below 2^31, so doubling it never overflows a longword. */
unsigned int probe_q_div(probe_q *q, unsigned int d)
{
    unsigned int rem = 0, hi = 0, lo = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        unsigned int bit = i >= 32 ? (q->hi >> (i - 32)) & 1 : (q->lo >> i) & 1;

        rem = (rem << 1) | bit;
        if (rem >= d) {
            rem -= d;
            if (i >= 32)
                hi |= 1u << (i - 32);
            else
                lo |= 1u << i;
        }
    }
    q->hi = hi;
    q->lo = lo;
    return rem;
}

double probe_q_double(probe_q q)
{
    return (double) q.hi * 4294967296.0 + (double) q.lo;
}

char *probe_q_str(probe_q q, char *buf)
{
    char tmp[PROBE_Q_DIGITS];
    int n = 0, i = 0;

    do
        tmp[n++] = (char) ('0' + probe_q_div(&q, 10));
    while (q.hi != 0 || q.lo != 0);
    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

char *probe_q_delta_str(probe_q a, probe_q b, char *buf)
{
    if (probe_q_cmp(b, a) >= 0) {
        buf[0] = '+';
        probe_q_str(probe_q_sub(b, a), buf + 1);
    } else {
        buf[0] = '-';
        probe_q_str(probe_q_sub(a, b), buf + 1);
    }
    return buf;
}

probe_q probe_gettim(void)
{
    unsigned char t[8];

    memset(t, 0, sizeof t);
    sys$gettim((void *) t);
    return probe_le(t, 8);
}

long probe_vms_to_unix_s(probe_q t)
{
    probe_q epoch = probe_q_make(PROBE_VMS_TO_UNIX_HI, PROBE_VMS_TO_UNIX_LO);

    if (probe_q_cmp(t, epoch) <= 0)
        return 0;
    t = probe_q_sub(t, epoch);
    probe_q_div(&t, 10000000u);
    return (long) t.lo;     /* fits until 2038, as time_t does on VAX */
}

void probe_print_value(const probe_item *it,
                              const unsigned char *buf,
                              unsigned short retlen)
{
    unsigned n = retlen ? retlen : it->size;

    /* A text item that returned nothing (an unmounted disk's label) is
       empty, not a buffer's worth of whatever was in it. */
    if (it->kind == PK_TEXT && retlen == 0)
        n = 0;

    switch (it->kind) {
    case PK_TEXT: {
        char s[256];
        unsigned i;

        if (n > sizeof s - 1)
            n = sizeof s - 1;
        for (i = 0; i < n; i++)
            s[i] = (buf[i] >= 32 && buf[i] < 127) ? (char) buf[i] : '.';
        /* Fixed-length VMS strings are blank-padded. */
        while (n > 0 && s[n - 1] == ' ')
            n--;
        s[n] = '\0';
        printf("\"%s\"", s);
        break;
    }
    case PK_TIME: {
        char d[PROBE_Q_DIGITS];
        probe_q t = probe_le(buf, 8);
        long s = probe_vms_to_unix_s(t);

        printf("%s", probe_q_str(t, d));
        if (s > 0) {
            time_t u = (time_t) s;
            struct tm *tm = gmtime(&u);   /* local wall time, shown raw */

            if (tm != NULL)
                printf(" (%04d-%02d-%02d %02d:%02d:%02d as written)",
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
        break;
    }
    default: {
        char d[PROBE_Q_DIGITS];

        printf("%s", probe_q_str(probe_le(buf, n), d));
        break;
    }
    }
    printf("  [%u bytes]", (unsigned) retlen);
}

int probe_check_ile(void)
{
    printf("item list entry: %u bytes, pointers %u bytes\n",
           (unsigned) sizeof(probe_ile3), (unsigned) sizeof(void *));
#ifdef __VMS
    if (sizeof(probe_ile3) != 12) {
        probe_fail("item list layout is not item_list_3; "
                   "build with /POINTER_SIZE=32 or none");
        return 0;
    }
#endif
    return 1;
}

/*
 * Every quadword below is printed through probe_q, so it is checked
 * first, on known values: 2^64 - 1, 12345678901, a borrow across the
 * longwords, and the epoch offset turned into seconds.
 */
static void check_quadwords(void)
{
    char d[PROBE_Q_DIGITS];
    probe_q big = probe_q_make(2u, 0xDFDC1C35u);           /* 12345678901 */
    probe_q max = probe_q_make(0xFFFFFFFFu, 0xFFFFFFFFu);
    probe_q one = probe_q_make(0u, 1u);
    probe_q t;
    int bad = 0;

    if (sizeof(unsigned int) != 4) {
        printf("  unsigned int is %u bytes\n", (unsigned) sizeof(unsigned int));
        probe_fail("probe_q needs a 4-byte unsigned int");
        return;
    }
    printf("quadword check: %s", probe_q_str(big, d));
    bad |= strcmp(d, "12345678901") != 0;
    printf(" %s", probe_q_str(max, d));
    bad |= strcmp(d, "18446744073709551615") != 0;
    t = probe_q_sub(probe_q_make(1u, 0u), one);             /* borrow */
    bad |= t.hi != 0 || t.lo != 0xFFFFFFFFu;
    t = probe_q_add(t, one);                                /* carry */
    bad |= t.hi != 1 || t.lo != 0;
    printf(" %s", probe_q_delta_str(big, one, d));
    bad |= strcmp(d, "-12345678900") != 0;
    /* 2026-11-10 00:00:00 as a VMS time, (40587 + 20767) days x
       864000000000 = 53009856000000000 = 0x00BC542E E4798000. */
    t = probe_q_make(0x00BC542Eu, 0xE4798000u);
    printf(" %ld\n", probe_vms_to_unix_s(t));
    bad |= probe_vms_to_unix_s(t) != 1794268800L;
    if (bad)
        probe_fail("quadword arithmetic; every 64-bit value below is suspect");
}

void probe_banner(const char *name)
{
    printf("vmsotel %s\n", name);
    check_quadwords();
#ifdef __VMS
    printf("built with __VMS");
#  ifdef __ia64
    printf(", __ia64");
#  endif
#  ifdef __x86_64
    printf(", __x86_64");
#  endif
#  ifdef __alpha
    printf(", __alpha");
#  endif
#  ifdef __VAX
    printf(", __VAX");
#  endif
#  ifdef __DECC_VER
    printf(", __DECC_VER %d", (int) __DECC_VER);
#  endif
    printf("\n");
#else
    printf("built WITHOUT __VMS: this is the Linux stub, values are fake\n");
#endif
}

int probe_summary(void)
{
    printf("\n%s: %d failure%s\n",
           probe_failures ? "FAILURES" : "PASS",
           probe_failures, probe_failures == 1 ? "" : "s");
    /* The C RTL maps these to a DCL status of the right severity. */
    return probe_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

