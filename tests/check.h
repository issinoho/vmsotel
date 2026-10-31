/*
 * vmsotel tests: the smallest useful check macros. Each failure prints
 * where and what, and the test binary exits non-zero if any failed.
 */

#ifndef VO_CHECK_H
#define VO_CHECK_H

#include <stdio.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond) do { \
        check_count++; \
        if (!(cond)) { \
            check_failures++; \
            printf("FAIL  %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_U64(got, want) do { \
        unsigned long long g_ = (got), w_ = (want); \
        check_count++; \
        if (g_ != w_) { \
            check_failures++; \
            printf("FAIL  %s:%d: %s = %llu, want %llu\n", \
                   __FILE__, __LINE__, #got, g_, w_); \
        } \
    } while (0)

#define CHECK_CONTAINS(hay, needle) do { \
        check_count++; \
        if (strstr((hay), (needle)) == NULL) { \
            check_failures++; \
            printf("FAIL  %s:%d: output lacks %s\n", \
                   __FILE__, __LINE__, (needle)); \
        } \
    } while (0)

static int check_summary(const char *name)
{
    printf("%s: %d checks, %d failed\n", name, check_count, check_failures);
    return check_failures ? 1 : 0;
}

#endif
