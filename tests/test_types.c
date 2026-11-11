/*
 * vmsotel tests: vo_u64's arithmetic and the bounded formatter.
 *
 * Built twice by make check: as it is on x86 and I64, and with
 * VO_SOFT64, the two-longword form VAX uses. Either way the answers
 * are checked against the native unsigned long long arithmetic of the
 * Linux compiler, and the formatter against glibc's snprintf.
 */

#include <stdlib.h>

#include "check.h"
#include "vo_types.h"

/* A reproducible 64-bit pseudo-random sequence (xorshift64*). */
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;

static unsigned long long next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}

/* Values near the edges are where carries and borrows go wrong. */
static unsigned long long pick(void)
{
    static const unsigned long long edge[] = {
        0, 1, 0xFFFFFFFFULL, 0x100000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL, 0xFFFFFFFF00000000ULL,
        35067168000000000ULL, 1000000000ULL
    };
    unsigned long long r = next();

    switch (r % 8) {
    case 0: return edge[(r >> 8) % (sizeof edge / sizeof edge[0])];
    case 1: return r >> 32;
    case 2: return r >> (r % 64);
    default: return r;
    }
}

static void test_arithmetic(void)
{
    int i, bad = 0;

    for (i = 0; i < 200000 && bad < 10; i++) {
        unsigned long long a = pick(), b = pick();
        vo_u32 m = (vo_u32) next(), d = (vo_u32) (next() % 0x7FFFFFFFu) + 1;
        vo_u32 rem = 0;
        vo_u64 qa = Q(a), qb = Q(b);
        int want_cmp = a < b ? -1 : a > b;
        int ok = ULL(vo_qadd(qa, qb)) == a + b &&
                 ULL(vo_qsub(qa, qb)) == a - b &&
                 ULL(vo_qmul(qa, m)) == a * m &&
                 ULL(vo_qdiv(qa, d, &rem)) == a / d && rem == a % d &&
                 vo_qcmp(qa, qb) == want_cmp &&
                 vo_qzero(qa) == (a == 0) &&
                 vo_qhi(qa) == (vo_u32) (a >> 32) &&
                 vo_qlo(qa) == (vo_u32) a;

        if (!ok) {
            bad++;
            printf("FAIL  a=%llu b=%llu m=%u d=%u\n", a, b, m, d);
        }
    }
    check_count++;
    if (bad)
        check_failures++;
}

static void test_decimal(void)
{
    char s[VO_QSTR_MAX], want[32];
    int i, bad = 0;

    CHECK(strcmp(vo_qstr(Q(0), s), "0") == 0);
    CHECK(strcmp(vo_qstr(Q(18446744073709551615ULL), s),
                 "18446744073709551615") == 0);
    CHECK(strcmp(vo_qstr(vo_q(2, 0xDFDC1C35u), s), "12345678901") == 0);
    for (i = 0; i < 20000; i++) {
        unsigned long long a = pick();

        sprintf(want, "%llu", a);
        if (strcmp(vo_qstr(Q(a), s), want) != 0 && bad++ < 10)
            printf("FAIL  %llu as %s\n", a, s);
    }
    check_count++;
    if (bad)
        check_failures++;
}

/* The formatter against glibc, one format and argument at a time. */
#define SAME(fmt, arg) do { \
        char g_[64], w_[64]; \
        int gn_ = vo_snprintf(g_, sizeof g_, fmt, arg); \
        int wn_ = snprintf(w_, sizeof w_, fmt, arg); \
        check_count++; \
        if (gn_ != wn_ || strcmp(g_, w_) != 0) { \
            check_failures++; \
            printf("FAIL  %s:%d: \"%s\" gave \"%s\" (%d), want \"%s\" (%d)\n", \
                   __FILE__, __LINE__, fmt, g_, gn_, w_, wn_); \
        } \
    } while (0)

static void test_format(void)
{
    char b[8];
    int n;

    SAME("%d", 0);
    SAME("%d", -42);
    SAME("%d", 2147483647);
    SAME("%d", (-2147483647 - 1));
    SAME("%5d|", -42);
    SAME("%-5d|", -42);
    SAME("%05d|", -42);
    SAME("%02d", 7);
    SAME("%04d", 2026);
    SAME("%3d", 1234);
    SAME("%u", 4294967295u);
    SAME("%12u|", 17u);
    SAME("%ld", -123456789L);
    SAME("%lu", 4000000000ul);
    SAME("%x", 0xbeefu);
    SAME("%X", 0xbeefu);
    SAME("%08X", 0x1Au);
    SAME("%08X", 0xDEADBEEFu);
    SAME("%%X%08X", 0x10u);
    SAME("%s", "text");
    SAME("%-12s|", "left");
    SAME("%12s|", "right");
    SAME("%3s|", "longer");
    SAME("%s", "");
    SAME("%c", 'Q');
    SAME("100%% %d", 1);

    /* Precision and width from arguments, as config.c uses them. */
    {
        char g[32], w[32];

        vo_snprintf(g, sizeof g, "[%.*s]", 3, "abcdef");
        snprintf(w, sizeof w, "[%.*s]", 3, "abcdef");
        CHECK(strcmp(g, w) == 0);
        vo_snprintf(g, sizeof g, "[%*s]", -6, "ab");
        snprintf(w, sizeof w, "[%*s]", -6, "ab");
        CHECK(strcmp(g, w) == 0);
    }

    /* Bounded: never past cap, always terminated, full length returned. */
    memset(b, 'Z', sizeof b);
    n = vo_snprintf(b, 5, "%s-%d", "abcdef", 12);
    CHECK(n == 9);
    CHECK(strcmp(b, "abcd") == 0);
    CHECK(b[5] == 'Z');
    memset(b, 'Z', sizeof b);
    n = vo_snprintf(b, 1, "%s", "abc");
    CHECK(n == 3 && b[0] == '\0' && b[1] == 'Z');
    n = vo_snprintf(NULL, 0, "%u", 12345u);
    CHECK(n == 5);
}

int main(void)
{
#ifdef VO_SOFT64
    printf("vo_u64 as two longwords (VO_SOFT64)\n");
#else
    printf("vo_u64 as unsigned long long\n");
#endif
    test_arithmetic();
    test_decimal();
    test_format();
    return check_summary("test_types");
}
