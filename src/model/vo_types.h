/*
 * vmsotel: fixed-width integer types, the 64-bit one's arithmetic, and
 * a bounded formatter.
 *
 * Not <stdint.h>: VAX has none (Compaq C V6.4), and these are certain.
 * unsigned int is 32 bits on every VMS architecture and on Linux; the
 * typedef below fails to compile if that is ever untrue (C99 has no
 * _Static_assert, so a negative array size does the job).
 *
 * vo_u64 is opaque. On VAX, whose compiler has no 64-bit integer type
 * at all (findings, "VAX"), it is two longwords; everywhere else it is
 * unsigned long long. Code touches it only through the functions
 * below, so both build the same way, and make check builds the
 * two-longword form on Linux too (VO_SOFT64) and runs the golden
 * payloads through it.
 *
 * snprintf and vsnprintf do not exist on VAX either, so the agent uses
 * vo_snprintf and vo_vsnprintf: always NUL-terminated, never past cap,
 * returning the length that full output would have had. They know
 * flags - and 0, a width, a precision (number or *) for %s, the length
 * l, and d u x X s c %. No floating point and no long long: a vo_u64
 * goes through vo_qstr and %s.
 */

#ifndef VO_TYPES_H
#define VO_TYPES_H

#include <stdarg.h>
#include <stddef.h>

#if defined(__VAX) && !defined(VO_SOFT64)
#  define VO_SOFT64 1
#endif

typedef unsigned int vo_u32;

typedef char vo_check_u32_is_4_bytes[sizeof(vo_u32) == 4 ? 1 : -1];

#ifdef VO_SOFT64
typedef struct {
    vo_u32 lo, hi;
} vo_u64;
#else
typedef unsigned long long vo_u64;
typedef char vo_check_u64_is_8_bytes[sizeof(vo_u64) == 8 ? 1 : -1];
#endif

/* Decimal digits of 2^64 - 1, and a NUL. */
#define VO_QSTR_MAX 21

vo_u64 vo_q(vo_u32 hi, vo_u32 lo);          /* hi * 2^32 + lo */
vo_u64 vo_q32(vo_u32 v);
vo_u64 vo_qul(unsigned long v);             /* 32 bits on VMS, 64 on Linux */
vo_u32 vo_qlo(vo_u64 a);
vo_u32 vo_qhi(vo_u64 a);
int vo_qzero(vo_u64 a);
int vo_qcmp(vo_u64 a, vo_u64 b);            /* -1, 0 or 1 */
vo_u64 vo_qadd(vo_u64 a, vo_u64 b);         /* modulo 2^64 */
vo_u64 vo_qsub(vo_u64 a, vo_u64 b);         /* modulo 2^64 */
vo_u64 vo_qmul(vo_u64 a, vo_u32 m);         /* modulo 2^64 */
/* a / d, for 0 < d < 2^31; the remainder in *rem unless it is NULL. */
vo_u64 vo_qdiv(vo_u64 a, vo_u32 d, vo_u32 *rem);
/* Decimal into buf, VO_QSTR_MAX bytes at least; returns buf. */
char *vo_qstr(vo_u64 a, char *buf);
double vo_qdouble(vo_u64 a);

int vo_snprintf(char *buf, size_t cap, const char *fmt, ...);
int vo_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);

#endif
