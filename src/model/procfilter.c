/*
 * vmsotel: process filter. See procfilter.h.
 */

#include <ctype.h>

#include "procfilter.h"

int vo_wildcard_match(const char *p, const char *t)
{
    /* Iterative, with one backtrack point for the last "*": linear in
       practice, and no recursion for a long pattern to exhaust. */
    const char *star = NULL, *mark = NULL;

    while (*t != '\0') {
        if (*p == '*') {
            star = p++;
            mark = t;
        } else if (*p == '%' ||
                   (*p != '\0' &&
                    toupper((unsigned char) *p) == toupper((unsigned char) *t))) {
            p++;
            t++;
        } else if (star != NULL) {
            p = star + 1;
            t = ++mark;
        } else {
            return 0;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

int vo_proc_filter_match(const vo_proc_filter *f, const char *name,
                         const char *user)
{
    size_t i;

    for (i = 0; i < f->nname; i++)
        if (vo_wildcard_match(f->name[i], name))
            return 1;
    for (i = 0; i < f->nuser; i++)
        if (vo_wildcard_match(f->user[i], user))
            return 1;
    return 0;
}

int vo_proc_filter_active(const vo_proc_filter *f)
{
    return f->nname > 0 || f->nuser > 0;
}
