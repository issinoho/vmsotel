/*
 * vmsotel: the 64-bit type's arithmetic and the bounded formatter.
 * See vo_types.h.
 */

#include "vo_types.h"

#ifdef VO_SOFT64

vo_u64 vo_q(vo_u32 hi, vo_u32 lo)
{
    vo_u64 r;

    r.hi = hi & 0xFFFFFFFFu;
    r.lo = lo & 0xFFFFFFFFu;
    return r;
}

vo_u32 vo_qlo(vo_u64 a) { return a.lo; }
vo_u32 vo_qhi(vo_u64 a) { return a.hi; }
int vo_qzero(vo_u64 a)  { return a.lo == 0 && a.hi == 0; }

int vo_qcmp(vo_u64 a, vo_u64 b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

vo_u64 vo_qadd(vo_u64 a, vo_u64 b)
{
    vo_u64 r;

    r.lo = (a.lo + b.lo) & 0xFFFFFFFFu;
    r.hi = (a.hi + b.hi + (r.lo < a.lo)) & 0xFFFFFFFFu;
    return r;
}

vo_u64 vo_qsub(vo_u64 a, vo_u64 b)
{
    vo_u64 r;

    r.lo = (a.lo - b.lo) & 0xFFFFFFFFu;
    r.hi = (a.hi - b.hi - (a.lo < b.lo)) & 0xFFFFFFFFu;
    return r;
}

/* Long multiplication in 16-bit halves, so no partial product needs
   more than 32 bits. */
vo_u64 vo_qmul(vo_u64 a, vo_u32 m)
{
    vo_u32 a0 = a.lo & 0xFFFFu, a1 = a.lo >> 16;
    vo_u32 a2 = a.hi & 0xFFFFu, a3 = a.hi >> 16;
    vo_u32 m0 = m & 0xFFFFu, m1 = m >> 16;
    vo_u32 p, carry, w0, w1, w2, w3;

    p = a0 * m0;
    w0 = p & 0xFFFFu;
    carry = p >> 16;

    p = a1 * m0;
    w1 = (p & 0xFFFFu) + carry;
    carry = (p >> 16) + (w1 >> 16);
    w1 &= 0xFFFFu;
    p = a0 * m1;
    w1 += p & 0xFFFFu;
    carry += (p >> 16) + (w1 >> 16);
    w1 &= 0xFFFFu;

    p = a2 * m0;
    w2 = (p & 0xFFFFu) + (carry & 0xFFFFu);
    carry = (carry >> 16) + (p >> 16) + (w2 >> 16);
    w2 &= 0xFFFFu;
    p = a1 * m1;
    w2 += p & 0xFFFFu;
    carry += (p >> 16) + (w2 >> 16);
    w2 &= 0xFFFFu;

    /* The top word: what is left over 2^64 is dropped. */
    w3 = carry + a3 * m0 + a2 * m1;

    return vo_q(((w3 & 0xFFFFu) << 16) | w2, (w1 << 16) | w0);
}

/* Shift and subtract, a bit at a time. The remainder stays below d,
   and d below 2^31, so doubling it never overflows a longword. */
vo_u64 vo_qdiv(vo_u64 a, vo_u32 d, vo_u32 *rem)
{
    vo_u32 r = 0, hi = 0, lo = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        vo_u32 bit = i >= 32 ? (a.hi >> (i - 32)) & 1 : (a.lo >> i) & 1;

        r = (r << 1) | bit;
        if (r >= d) {
            r -= d;
            if (i >= 32)
                hi |= 1u << (i - 32);
            else
                lo |= 1u << i;
        }
    }
    if (rem != NULL)
        *rem = r;
    return vo_q(hi, lo);
}

double vo_qdouble(vo_u64 a)
{
    return (double) a.hi * 4294967296.0 + (double) a.lo;
}

#else   /* unsigned long long */

vo_u64 vo_q(vo_u32 hi, vo_u32 lo)
{
    return (vo_u64) (hi & 0xFFFFFFFFu) << 32 | (lo & 0xFFFFFFFFu);
}

vo_u32 vo_qlo(vo_u64 a) { return (vo_u32) (a & 0xFFFFFFFFu); }
vo_u32 vo_qhi(vo_u64 a) { return (vo_u32) (a >> 32); }
int vo_qzero(vo_u64 a)  { return a == 0; }

int vo_qcmp(vo_u64 a, vo_u64 b)
{
    return a < b ? -1 : a > b;
}

vo_u64 vo_qadd(vo_u64 a, vo_u64 b) { return a + b; }
vo_u64 vo_qsub(vo_u64 a, vo_u64 b) { return a - b; }
vo_u64 vo_qmul(vo_u64 a, vo_u32 m) { return a * m; }

vo_u64 vo_qdiv(vo_u64 a, vo_u32 d, vo_u32 *rem)
{
    if (rem != NULL)
        *rem = (vo_u32) (a % d);
    return a / d;
}

double vo_qdouble(vo_u64 a)
{
    return (double) a;
}

#endif

vo_u64 vo_q32(vo_u32 v)
{
    return vo_q(0, v);
}

/* Shifted in two steps: by 32 at once is undefined where unsigned
   long is 32 bits. */
vo_u64 vo_qul(unsigned long v)
{
    return vo_q((vo_u32) (v >> 16 >> 16), (vo_u32) v);
}

char *vo_qstr(vo_u64 a, char *buf)
{
    char tmp[VO_QSTR_MAX];
    int n = 0, i = 0;

    do {
        vo_u32 digit;

        a = vo_qdiv(a, 10, &digit);
        tmp[n++] = (char) ('0' + digit);
    } while (!vo_qzero(a));
    while (n > 0)
        buf[i++] = tmp[--n];
    buf[i] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* The formatter                                                       */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;         /* what full output would need, without NUL */
} out;

static void emit(out *o, char c)
{
    if (o->cap > 0 && o->len < o->cap - 1)
        o->buf[o->len] = c;
    o->len++;
}

static void field(out *o, const char *s, size_t n, int width, int left,
                  char pad)
{
    size_t i;

    if (!left)
        for (; width > 0 && (size_t) width > n; width--)
            emit(o, pad);
    for (i = 0; i < n; i++)
        emit(o, s[i]);
    if (left)
        for (; width > 0 && (size_t) width > n; width--)
            emit(o, ' ');
}

int vo_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    static const char lower[] = "0123456789abcdef";
    static const char upper[] = "0123456789ABCDEF";
    out o;
    const char *p;

    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    for (p = fmt; *p != '\0'; p++) {
        int left = 0, zero = 0, width = 0, prec = -1, is_long = 0;
        char num[24];
        size_t n;

        if (*p != '%') {
            emit(&o, *p);
            continue;
        }
        p++;
        for (;; p++) {
            if (*p == '-')
                left = 1;
            else if (*p == '0')
                zero = 1;
            else
                break;
        }
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    prec = prec * 10 + (*p++ - '0');
            }
        }
        if (*p == 'l') {
            is_long = 1;
            p++;
        }

        switch (*p) {
        case 'd': case 'i': {
            long v = is_long ? va_arg(ap, long) : (long) va_arg(ap, int);
            unsigned long u = v < 0 ? 0ul - (unsigned long) v : (unsigned long) v;

            n = 0;
            do {
                num[sizeof num - 1 - n++] = (char) ('0' + (int) (u % 10));
                u /= 10;
            } while (u != 0);
            if (v < 0) {
                /* The sign goes before zero padding, not after it. */
                if (zero && !left) {
                    emit(&o, '-');
                    if (width > 0)
                        width--;
                } else {
                    num[sizeof num - 1 - n++] = '-';
                }
            }
            field(&o, num + sizeof num - n, n, width, left,
                  zero && !left ? '0' : ' ');
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long u = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long) va_arg(ap, unsigned);
            unsigned base = *p == 'u' ? 10 : 16;
            const char *digits = *p == 'X' ? upper : lower;

            n = 0;
            do {
                num[sizeof num - 1 - n++] = digits[u % base];
                u /= base;
            } while (u != 0);
            field(&o, num + sizeof num - n, n, width, left,
                  zero && !left ? '0' : ' ');
            break;
        }
        case 'c':
            num[0] = (char) va_arg(ap, int);
            field(&o, num, 1, width, left, ' ');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);

            if (s == NULL)
                s = "(null)";
            for (n = 0; s[n] != '\0' && (prec < 0 || n < (size_t) prec); n++)
                ;
            field(&o, s, n, width, left, ' ');
            break;
        }
        case '%':
            emit(&o, '%');
            break;
        default:
            /* Unknown: shown as written, so the mistake is visible. */
            emit(&o, '%');
            if (*p == '\0')
                p--;
            else
                emit(&o, *p);
            break;
        }
    }
    if (cap > 0)
        buf[o.len < cap ? o.len : cap - 1] = '\0';
    return (int) o.len;
}

int vo_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vo_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
