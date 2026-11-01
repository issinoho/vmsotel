/*
 * vmsotel: the VMS log source -- the operator log and the audit journal,
 * read with RMS (phase 3). See logsource.h.
 *
 * What the probes found on both machines (docs/findings.md, 33-38), and
 * what this does about it:
 *   - Both files open for shared reading while OPCOM and the audit
 *     server write them, with READALL or SYSPRV, through RMS with every
 *     sharing option and no record locks -- as probe_logs opened them.
 *   - A stream left open never sees new records; a reopen does. So
 *     every read opens each file afresh and closes it again.
 *   - Going back to a record by its RFA works and is immediate, where
 *     reading I64's 4.5-million-record operator log takes 41 s. So the
 *     place kept between reads is the RFA of the last record consumed,
 *     with its length and first bytes to check it is still that record.
 *     The whole file is read once, at the first read, to find its end:
 *     the agent starts with what is new, not with years of history.
 *   - OPCOM writes no record after a message, so the last message in
 *     the log has nothing to end it until the next one comes. It is
 *     sent once it is unchanged over a whole interval.
 *   - Each file gets new versions (I64's journal did at 00:05 one day).
 *     Once the version being read has nothing new, the name without a
 *     version is looked up; a higher version is read from its start.
 *   - Times in both files are local: the system's differential is read
 *     on every read, so the hour after a summer-time change is right.
 *
 * Built without /POINTER_SIZE, like the rest of the VMS code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <descrip.h>
#include <rms.h>
#include <rmsdef.h>
#include <starlet.h>

#include "logsource.h"

#define REC_MAX   32768         /* the journal's longest: 32767 (finding 37) */
#define HEAD_MAX  64            /* bytes kept to know a record again */
#define AFTER_MAX 256           /* records one read can return */
#define RSS_MAX   255           /* NAM$C_MAXRSS */

/* in vms_source.c */
int vo_vms_system_tdf(long long *tdf, char *err, size_t errcap);

/* A record: where it is, and enough of it to know it again. */
typedef struct {
    unsigned short rfa[3];
    unsigned short len;
    unsigned char head[HEAD_MAX];
} vpos;

typedef struct {
    int kind;                   /* VO_LOG_OPCOM or VO_LOG_AUDIT */
    int on;
    const char *name;           /* as configured, no version */
    int primed;                 /* its end has been found */
    char spec[RSS_MAX + 1];     /* the version being read */
    int at_start;               /* committed: before its first record */
    vpos pos;                   /* committed: the last record consumed */
    /* OPCOM: the unfinished message at the end, as the last read saw it */
    int have_pend;
    vpos pend_hdr;
    size_t pend_lines;
} lfile;

typedef struct {
    struct FAB fab;
    struct RAB rab;
    struct NAM nam;
    char name[RSS_MAX + 1];
    char rsa[RSS_MAX + 1];
    int open;
} rfile;

static lfile files[2];
static unsigned char rec[REC_MAX];

/* Where each record of the last read leaves its file. */
static struct {
    int file;
    vpos pos;
} after[AFTER_MAX];
static size_t last_n;

static char *rms_msg(unsigned int st)
{
    static char text[256];
    unsigned short len = 0;
    struct dsc$descriptor_s d;

    d.dsc$w_length = (unsigned short) (sizeof text - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = text;
    if (!(sys$getmsg(st, &len, (void *) &d, 15, 0) & 1)) {
        sprintf(text, "%%X%08X", st);
        return text;
    }
    text[len] = '\0';
    return text;
}

/*
 * Opens for reading with every sharing option, so that a file another
 * process holds open for writing can be opened at all, and takes no
 * record locks. The NAM block gives the full name, version included.
 */
static unsigned int r_open(rfile *f, const char *name)
{
    unsigned int st;

    memset(f, 0, sizeof *f);
    strncpy(f->name, name, sizeof f->name - 1);
    f->fab = cc$rms_fab;
    f->nam = cc$rms_nam;
    f->fab.fab$l_fna = f->name;
    f->fab.fab$b_fns = (unsigned char) strlen(f->name);
    f->fab.fab$b_fac = FAB$M_GET;
    f->fab.fab$b_shr = FAB$M_SHRGET | FAB$M_SHRPUT | FAB$M_SHRUPD;
    f->fab.fab$l_nam = &f->nam;
    f->nam.nam$l_rsa = f->rsa;
    f->nam.nam$b_rss = RSS_MAX;
    st = sys$open((void *) &f->fab);
    if (!(st & 1))
        return st;
    f->rsa[f->nam.nam$b_rsl] = '\0';
    f->rab = cc$rms_rab;
    f->rab.rab$l_fab = &f->fab;
    f->rab.rab$l_ubf = (char *) rec;
    f->rab.rab$w_usz = REC_MAX - 1;
    f->rab.rab$l_rop = RAB$M_NLK | RAB$M_RRL;
    f->rab.rab$b_rac = RAB$C_SEQ;
    st = sys$connect((void *) &f->rab);
    if (!(st & 1)) {
        sys$close((void *) &f->fab);
        return st;
    }
    f->open = 1;
    return st;
}

static void r_close(rfile *f)
{
    if (f->open)
        sys$close((void *) &f->fab);
    f->open = 0;
}

static unsigned int r_get(rfile *f, vpos *p)
{
    unsigned int st = sys$get((void *) &f->rab);
    unsigned short n;

    if (!(st & 1))
        return st;
    n = f->rab.rab$w_rsz;
    memcpy(p->rfa, f->rab.rab$w_rfa, sizeof p->rfa);
    p->len = n;
    memcpy(p->head, rec, n < HEAD_MAX ? n : HEAD_MAX);
    return st;
}

/* Back to the record at p, by its RFA: 1 if it is still that record,
   and the next r_get is the one after it. */
static int r_get_at(rfile *f, const vpos *p)
{
    unsigned int st;
    unsigned short n;

    memcpy(f->rab.rab$w_rfa, p->rfa, sizeof p->rfa);
    f->rab.rab$b_rac = RAB$C_RFA;
    st = sys$get((void *) &f->rab);
    f->rab.rab$b_rac = RAB$C_SEQ;
    if (!(st & 1))
        return 0;
    n = f->rab.rab$w_rsz;
    return n == p->len &&
           memcmp(rec, p->head, n < HEAD_MAX ? n : HEAD_MAX) == 0;
}

static int same_pos(const vpos *a, const vpos *b)
{
    return memcmp(a->rfa, b->rfa, sizeof a->rfa) == 0 && a->len == b->len;
}

static void add_msg(char *msg, size_t cap, const char *fmt, const char *a,
                    const char *b)
{
    size_t have = strlen(msg);

    if (have > 0 && have + 2 < cap) {
        strcpy(msg + have, "; ");
        have += 2;
    }
    if (have < cap)
        snprintf(msg + have, cap - have, fmt, a, b);
}

void vo_logs_open(const vo_logs_config *c)
{
    memset(files, 0, sizeof files);
    files[0].kind = VO_LOG_OPCOM;
    files[0].on = c->opcom;
    files[0].name = c->opcom_file;
    files[1].kind = VO_LOG_AUDIT;
    files[1].on = c->audit;
    files[1].name = c->audit_file;
    last_n = 0;
}

/*
 * The first read of a file: to its end, keeping only the last record's
 * place. What was there before the agent started is not sent.
 */
static int prime(lfile *lf, char *msg, size_t cap)
{
    rfile f;
    vpos p, last;
    unsigned long count = 0;
    time_t t0 = time(NULL);
    unsigned int st;
    char note[300];

    memset(&last, 0, sizeof last);
    st = r_open(&f, lf->name);
    if (!(st & 1)) {
        add_msg(msg, cap, "cannot read %s: %s", lf->name, rms_msg(st));
        return 0;
    }
    while ((st = r_get(&f, &p)) & 1) {
        last = p;
        count++;
    }
    strcpy(lf->spec, f.rsa);
    r_close(&f);
    lf->primed = 1;
    lf->have_pend = 0;
    lf->at_start = count == 0;
    if (count > 0)
        lf->pos = last;
    sprintf(note, "%lu records, %ld s", count, (long) (time(NULL) - t0));
    add_msg(msg, cap, "starting at the end of %s (%s)", lf->spec, note);
    return 1;
}

static void emit(vo_log_rec *recs, size_t *n, int file, const vpos *p,
                 const lfile *lf)
{
    recs[*n].file = lf->spec;
    after[*n].file = file;
    after[*n].pos = *p;
    (*n)++;
}

/*
 * Reads one file from its committed place: new records into recs from
 * *n on, up to max. Returns 0 if it could not be read.
 */
static int read_file(int file, vo_log_rec *recs, size_t max, size_t *n,
                     int *more, long tdf, char *msg, size_t cap)
{
    lfile *lf = &files[file];
    rfile f;
    vpos p, prev, hdr;
    vo_opcom_parser parser;
    unsigned int st;
    size_t first = *n;
    int have_prev = 0, eof = 0;

    if (!lf->primed)
        return prime(lf, msg, cap);

    st = r_open(&f, lf->spec);
    if (!(st & 1)) {
        /* The version being read is gone (purged): carry on with the
           current one, from its start. */
        char now[RSS_MAX + 1];

        st = r_open(&f, lf->name);
        if (!(st & 1)) {
            add_msg(msg, cap, "cannot read %s: %s", lf->name, rms_msg(st));
            return 0;
        }
        strcpy(now, f.rsa);
        add_msg(msg, cap, "%s is gone; reading %s from its start",
                lf->spec, now);
        strcpy(lf->spec, now);
        lf->at_start = 1;
        lf->have_pend = 0;
    }
    if (!lf->at_start && !r_get_at(&f, &lf->pos)) {
        /* The record at the saved place is not the one saved. Starting
           over from the top could resend millions of records, so it
           starts again from the end and says so. */
        r_close(&f);
        lf->primed = 0;
        add_msg(msg, cap, "lost its place in %s; starting again from "
                "the end%s", lf->spec, "");
        return prime(lf, msg, cap) ? 1 : 0;
    }

    vo_opcom_init(&parser, tdf);
    memset(&hdr, 0, sizeof hdr);
    while (*n < max) {
        st = r_get(&f, &p);
        if (!(st & 1)) {
            eof = 1;
            break;
        }
        if (lf->kind == VO_LOG_AUDIT) {
            vo_log_rec *r = &recs[*n];

            if (!vo_nsa_decode(rec, p.len, tdf, r)) {
                memset(r, 0, sizeof *r);
                r->source = VO_LOG_AUDIT;
                r->severity = VO_SEV_WARN;
                r->severity_text = "WARN";
                sprintf(r->body, "Audit journal record of %u bytes that "
                        "does not decode as NSAMSGDEF", (unsigned) p.len);
            }
            emit(recs, n, file, &p, lf);
            continue;
        }
        /* OPCOM: a message ends at the record before this one. */
        if (vo_opcom_feed(&parser, (const char *) rec, p.len, &recs[*n]) &&
            have_prev)
            emit(recs, n, file, &prev, lf);
        if (vo_opcom_is_header((const char *) rec, p.len))
            hdr = p;
        prev = p;
        have_prev = 1;
    }
    if (!eof) {
        *more = 1;
        r_close(&f);
        return 1;
    }
    if (st != RMS$_EOF)
        add_msg(msg, cap, "reading %s: %s", lf->spec, rms_msg(st));

    /* The message at the end, once it has not grown for one read. */
    if (lf->kind == VO_LOG_OPCOM) {
        size_t lines = vo_opcom_pending_lines(&parser);

        if (lines == 0) {
            lf->have_pend = 0;
        } else if (lf->have_pend && same_pos(&lf->pend_hdr, &hdr) &&
                   lf->pend_lines == lines && *n < max) {
            vo_opcom_pending(&parser, &recs[*n]);
            emit(recs, n, file, &prev, lf);
            lf->have_pend = 0;
        } else {
            lf->have_pend = 1;
            lf->pend_hdr = hdr;
            lf->pend_lines = lines;
        }
    }
    r_close(&f);

    /* Nothing new and nothing waiting: has a new version begun? */
    if (*n == first && !lf->have_pend) {
        rfile g;

        if ((r_open(&g, lf->name) & 1)) {
            if (strcmp(g.rsa, lf->spec) != 0) {
                add_msg(msg, cap, "%s is finished; now reading %s",
                        lf->spec, g.rsa);
                strcpy(lf->spec, g.rsa);
                lf->at_start = 1;
            }
            r_close(&g);
        }
    }
    return 1;
}

int vo_logs_read(vo_log_rec *recs, size_t max, size_t *n, int *more,
                 char *msg, size_t msgcap)
{
    long long tdf = 0;
    char err[200];
    int ok = 1, i;

    *n = 0;
    *more = 0;
    msg[0] = '\0';
    last_n = 0;
    if (max > AFTER_MAX)
        max = AFTER_MAX;
    if (!vo_vms_system_tdf(&tdf, err, sizeof err))
        tdf = 0;            /* the source refuses to start without one */
    for (i = 0; i < 2; i++)
        if (files[i].on && !*more)
            ok &= read_file(i, recs, max, n, more, (long) tdf, msg, msgcap);
    last_n = *n;
    return ok;
}

void vo_logs_commit(size_t k)
{
    size_t i;

    if (k > last_n)
        k = last_n;
    for (i = 0; i < k; i++) {
        lfile *lf = &files[after[i].file];

        lf->pos = after[i].pos;
        lf->at_start = 0;
    }
    last_n = 0;
}

void vo_logs_close(void)
{
}
