/*
 * vmsotel tests: vo_u64 to and from unsigned long long.
 *
 * vo_u64 is opaque (vo_types.h): two longwords under VO_SOFT64, as on
 * VAX. The tests run only on Linux, where unsigned long long exists,
 * so they build vo_u64 values with Q() and read them back with ULL().
 */

#ifndef VO_TEST_Q_H
#define VO_TEST_Q_H

#include "vo_types.h"

#define Q(v) vo_q((vo_u32) ((unsigned long long) (v) >> 32), (vo_u32) (v))
#define ULL(v) ((unsigned long long) vo_qhi(v) << 32 | vo_qlo(v))

#endif
