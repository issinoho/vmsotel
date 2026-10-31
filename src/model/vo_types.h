/*
 * vmsotel: fixed-width integer types.
 *
 * Not <stdint.h>: whether VSI C on both machines ships it has not been
 * probed, and these two are certain. unsigned int is 32 bits and
 * unsigned long long 64 on every VMS architecture and on Linux; the
 * typedefs below fail to compile if that is ever untrue (C99 has no
 * _Static_assert, so a negative array size does the job).
 */

#ifndef VO_TYPES_H
#define VO_TYPES_H

typedef unsigned int vo_u32;
typedef unsigned long long vo_u64;

typedef char vo_check_u32_is_4_bytes[sizeof(vo_u32) == 4 ? 1 : -1];
typedef char vo_check_u64_is_8_bytes[sizeof(vo_u64) == 8 ? 1 : -1];

#endif
