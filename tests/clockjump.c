/*
 * vmsotel tests: makes the monotonic clock jump back an hour, CLOCKJUMP_AFTER
 * seconds into a run, for an agent started with LD_PRELOAD. That is
 * what VMS's local-time clock does when summer time ends, and what a
 * clock set backwards does anywhere; on Linux CLOCK_MONOTONIC never
 * does it by itself, so without this the agent's guard could not be
 * shown to work. Linux and glibc only; not part of the agent.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

static int (*real)(clockid_t, struct timespec *);
static time_t start;

int clock_gettime(clockid_t id, struct timespec *ts)
{
    int r;
    const char *after = getenv("CLOCKJUMP_AFTER");

    if (real == NULL)
        real = (int (*)(clockid_t, struct timespec *)) dlsym(RTLD_NEXT,
                                                             "clock_gettime");
    r = real(id, ts);
    if (r == 0 && id == CLOCK_MONOTONIC) {
        if (start == 0)
            start = ts->tv_sec;
        if (after != NULL && ts->tv_sec - start >= atoi(after))
            ts->tv_sec -= 3600;
    }
    return r;
}
