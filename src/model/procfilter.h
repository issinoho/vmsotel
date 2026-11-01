/*
 * vmsotel: which processes get per-process metrics.
 *
 * Patterns for the process name and for the username, VMS style: "*"
 * matches any run of characters, "%" exactly one, and case does not
 * matter. A process is chosen when any name pattern or any user
 * pattern matches it. With no patterns at all nothing is chosen, and
 * the agent does not scan processes -- the default, which needs no
 * WORLD privilege.
 *
 * Pure: no I/O and no VMS dependency.
 */

#ifndef VO_PROCFILTER_H
#define VO_PROCFILTER_H

#include <stddef.h>

#define VO_FILTER_MAX      16
#define VO_PATTERN_MAX     32

typedef struct {
    size_t nname;
    size_t nuser;
    char name[VO_FILTER_MAX][VO_PATTERN_MAX + 1];
    char user[VO_FILTER_MAX][VO_PATTERN_MAX + 1];
} vo_proc_filter;

/* 1 if text matches pattern ("*", "%", case-blind). */
int vo_wildcard_match(const char *pattern, const char *text);

/* 1 if the filter chooses a process with this name and username. */
int vo_proc_filter_match(const vo_proc_filter *f, const char *name,
                         const char *user);

/* 1 if the filter has any pattern at all. */
int vo_proc_filter_active(const vo_proc_filter *f);

#endif
