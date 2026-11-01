/*
 * vmsotel probe: what phase 3 (logs) can read -- the operator log and
 * the security audit journal.
 *
 *     probe_logs [--request [--wait SECONDS]] [--oplog FILE] [--journal FILE]
 *
 * Answers, for each file, on each machine, with and without privilege:
 *
 *   OPCOM (SYS$MANAGER:OPERATOR.LOG, or OPC$LOGFILE_NAME)
 *     1. Can another process open it for reading while OPCOM has it
 *        open for writing, through RMS with shared access -- and with
 *        what privilege. If not, the status says why (PRV, FLK, ...).
 *     2. Its record format, and what its records look like: the last
 *        few are printed as they are.
 *     3. With --request: one REQUEST message is sent to the operators
 *        (it appears on operator consoles), and the probe watches for
 *        it. Does a new record appear on the stream already open -- a
 *        tail -- or only after closing and reopening the file -- and
 *        how long after the REQUEST? It looks every 15 s, for up to
 *        --wait seconds (600).
 *     4. On reopening, can it go straight back to the last record read,
 *        by that record's RFA, instead of reading from the start? I64's
 *        log holds 4.5 million records, too many to skip each interval.
 *
 *   Audit (SYS$MANAGER:SECURITY.AUDIT$JOURNAL)
 *     4. Can it be opened for reading, with what privilege.
 *     5. Its record format and record sizes, and the first 64 bytes of
 *        some records in hex, to set beside the NSA$ definitions that
 *        build_probes.com LOGS extracts from the system's libraries.
 *        Nothing here guesses the layout; that is what this is for.
 *
 * Nothing is written and no system setting is changed. The audit
 * listener mailbox needs SET AUDIT/LISTENER, which changes the
 * system's audit configuration, so it is not probed here; see
 * docs/design.md, phase 3.
 */

#include "vmsprobe.h"

#include <rms.h>
#include <rmsdef.h>
#include <unistd.h>

#define REC_MAX   8192          /* audit records can be large */
#define KEEP      6             /* records kept to print */
#define SHOW_TEXT 132
#define POLL_S    15          /* seconds between looks for the marker */
#define WAIT_S    600         /* longest wait for it, by default */

typedef struct {
    struct FAB fab;
    struct RAB rab;
    char name[256];
    int open;
    unsigned short last_rfa[3];     /* of the last record read */
} rfile;

static unsigned char rec[REC_MAX];
static unsigned char kept[KEEP][REC_MAX];
static unsigned short kept_len[KEEP];

static const char *rfm_name(int rfm)
{
    switch (rfm) {
    case FAB$C_FIX:   return "FIX";
    case FAB$C_VAR:   return "VAR";
    case FAB$C_VFC:   return "VFC";
    case FAB$C_STM:   return "STM";
    case FAB$C_STMLF: return "STMLF";
    case FAB$C_STMCR: return "STMCR";
    default:          return "UDF or other";
    }
}

/*
 * Opens for reading with every sharing option, so that a file another
 * process holds open for writing can be opened at all: RMS refuses a
 * reader whose SHR does not allow the writer's PUT. NLK and RRL: take
 * no record locks and read past the writer's.
 */
static unsigned int r_open(rfile *f, const char *name)
{
    unsigned int st;

    memset(f, 0, sizeof *f);
    strncpy(f->name, name, sizeof f->name - 1);
    f->fab = cc$rms_fab;
    f->fab.fab$l_fna = f->name;
    f->fab.fab$b_fns = (unsigned char) strlen(f->name);
    f->fab.fab$b_fac = FAB$M_GET;
    f->fab.fab$b_shr = FAB$M_SHRGET | FAB$M_SHRPUT | FAB$M_SHRUPD;
    st = sys$open((void *) &f->fab);
    if (!(st & 1))
        return st;
    f->rab = cc$rms_rab;
    f->rab.rab$l_fab = &f->fab;
    f->rab.rab$l_ubf = (char *) rec;
    f->rab.rab$w_usz = REC_MAX;
    f->rab.rab$l_rop = RAB$M_NLK | RAB$M_RRL;
    st = sys$connect((void *) &f->rab);
    if (!(st & 1)) {
        sys$close((void *) &f->fab);
        return st;
    }
    f->open = 1;
    return st;
}

static unsigned int r_get(rfile *f)
{
    unsigned int st = sys$get((void *) &f->rab);

    if (st & 1)
        memcpy(f->last_rfa, f->rab.rab$w_rfa, sizeof f->last_rfa);
    return st;
}

/*
 * Goes back to the record whose RFA was saved and reads it again, then
 * returns to sequential access, so the next r_get is the record after
 * it. Returns 1 when the record read equals `want`.
 */
static int r_get_at(rfile *f, const unsigned short rfa[3],
                    const unsigned char *want, unsigned short wantlen,
                    unsigned int *st)
{
    memcpy(f->rab.rab$w_rfa, rfa, sizeof f->rab.rab$w_rfa);
    f->rab.rab$b_rac = RAB$C_RFA;
    *st = sys$get((void *) &f->rab);
    f->rab.rab$b_rac = RAB$C_SEQ;
    if (!(*st & 1))
        return 0;
    memcpy(f->last_rfa, f->rab.rab$w_rfa, sizeof f->last_rfa);
    return f->rab.rab$w_rsz == wantlen && memcmp(rec, want, wantlen) == 0;
}

static void r_close(rfile *f)
{
    if (f->open)
        sys$close((void *) &f->fab);
    f->open = 0;
}

static void keep(unsigned long n, unsigned short len)
{
    int k = (int) (n % KEEP);

    memcpy(kept[k], rec, len);
    kept_len[k] = len;
}

static void print_text(const unsigned char *b, unsigned short len)
{
    unsigned i, n = len < SHOW_TEXT ? len : SHOW_TEXT;

    printf("    [%3u] ", (unsigned) len);
    for (i = 0; i < n; i++)
        putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.');
    printf("%s\n", len > SHOW_TEXT ? " ..." : "");
}

static void print_hex(const unsigned char *b, unsigned short len)
{
    unsigned i, n = len < 64 ? len : 64;

    printf("    [%u bytes]\n", (unsigned) len);
    for (i = 0; i < n; i++)
        printf("%s%02x", i % 16 == 0 ? (i ? "\n      " : "      ") : " ",
               b[i]);
    printf("\n");
    if (len > 0) {
        printf("      ");
        for (i = 0; i < n; i++)
            putchar(b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        printf("\n");
    }
}

static int contains(const unsigned char *b, unsigned short len,
                    const char *s)
{
    size_t n = strlen(s), i;

    for (i = 0; i + n <= len; i++)
        if (memcmp(b + i, s, n) == 0)
            return 1;
    return 0;
}

/* Reads to the end. Returns records read; sizes are counted. */
static unsigned long read_all_sz(rfile *f, unsigned long base,
                                 unsigned short *minlen,
                                 unsigned short *maxlen,
                                 const char *marker, int *hit,
                                 unsigned int *end)
{
    unsigned long n = 0;
    unsigned int st;

    while (((st = r_get(f)) & 1)) {
        unsigned short len = f->rab.rab$w_rsz;

        keep(base + n, len);
        if (minlen && (n == 0 || len < *minlen))
            *minlen = len;
        if (maxlen && len > *maxlen)
            *maxlen = len;
        if (marker && contains(rec, len, marker))
            *hit = 1;
        n++;
    }
    *end = st;
    return n;
}

/* Reads to the end, looking for `marker` when one is given. */
static unsigned long read_all(rfile *f, unsigned long base,
                              const char *marker, int *hit,
                              unsigned int *end)
{
    return read_all_sz(f, base, NULL, NULL, marker, hit, end);
}

static unsigned long reopen_last;   /* records the last reopen read */

/*
 * Opens the log afresh, goes back to the record at `rfa` (checking it
 * is `want`), and reads what follows. If going back by RFA fails it
 * says why, once, and skips `skip` records from the start instead.
 */
static unsigned long reopen_read(const char *name,
                                 const unsigned short rfa[3],
                                 const unsigned char *want,
                                 unsigned short wantlen, unsigned long skip,
                                 const char *marker, int *hit, int report)
{
    rfile g;
    unsigned int st, end;
    unsigned long i, got;
    int ok = 0;

    *hit = 0;
    st = r_open(&g, name);
    if (!(st & 1)) {
        if (report) {
            printf("  reopen: %s\n", probe_msg(st));
            probe_fail("reopen the operator log");
        }
        return 0;
    }
    if (skip > 0) {
        ok = r_get_at(&g, rfa, want, wantlen, &st);
        if (report) {
            if (ok)
                probe_ok("reopened at the last record read, by its RFA");
            else if (!(st & 1)) {
                printf("  $GET by RFA: %s\n", probe_msg(st));
                probe_fail("go back to the last record by its RFA");
            } else {
                printf("  $GET by RFA found a different record:\n");
                print_text(rec, g.rab.rab$w_rsz);
                probe_fail("go back to the last record by its RFA");
            }
        }
        if (!ok) {
            r_close(&g);
            if (!(r_open(&g, name) & 1))
                return 0;
            for (i = 0; i < skip && (r_get(&g) & 1); i++)
                ;
        }
    }
    got = read_all(&g, 0, marker, hit, &end);
    reopen_last = got;
    r_close(&g);
    return got;
}

static void print_kept(unsigned long total, int hex)
{
    unsigned long first = total > KEEP ? total - KEEP : 0, i;

    for (i = first; i < total; i++) {
        if (hex)
            print_hex(kept[i % KEEP], kept_len[i % KEEP]);
        else
            print_text(kept[i % KEEP], kept_len[i % KEEP]);
    }
}


static void send_request(const char *marker)
{
    char cmd[200];

    sprintf(cmd, "REQUEST \"%s\"", marker);
    printf("  sending: %s\n", cmd);
    fflush(stdout);
#ifdef __VMS
    printf("  system() returned %d\n", system(cmd));
#else
    probe_stub_request(marker);
#endif
}

/* ---- OPCOM --------------------------------------------------------- */

static void opcom(const char *name, int request, long wait)
{
    rfile f;
    unsigned int st, end;
    unsigned long n, more, i;
    char marker[80], msg[120];
    long tail_at, reopen_at;
    time_t t0;
    unsigned short rfa[3];
    unsigned char last[REC_MAX];
    unsigned short lastlen;

    printf("\nOPCOM: %s\n", name);
    st = r_open(&f, name);
    if (!(st & 1)) {
        printf("  open: %s\n", probe_msg(st));
        probe_fail("open the operator log for shared reading");
        return;
    }
    probe_ok("open the operator log for shared reading");
    printf("  record format %s (%d), longest record %u, attributes %02x\n",
           rfm_name(f.fab.fab$b_rfm), f.fab.fab$b_rfm,
           (unsigned) f.fab.fab$w_mrs, (unsigned) f.fab.fab$b_rat);

    t0 = time(NULL);
    n = read_all(&f, 0, NULL, NULL, &end);
    printf("  %lu records, then %s\n", n, probe_msg(end));
    printf("  reading them all took %ld s\n", (long) (time(NULL) - t0));
    printf("  the last %d:\n", n < KEEP ? (int) n : KEEP);
    print_kept(n, 0);
    if (end != RMS$_EOF)
        probe_fail("read the operator log to its end");

    if (!request) {
        printf("  (run with --request to test reading new records)\n");
        r_close(&f);
        return;
    }

    /*
     * What the reopening reader goes back to: the last record of the
     * first full read, by its RFA.
     */
    memcpy(rfa, f.last_rfa, sizeof rfa);
    lastlen = 0;
    if (n > 0) {
        lastlen = kept_len[(n - 1) % KEEP];
        memcpy(last, kept[(n - 1) % KEEP], lastlen);
        printf("  last record's RFA: %04x %04x %04x\n",
               (unsigned) rfa[0], (unsigned) rfa[1], (unsigned) rfa[2]);
    }

    sprintf(marker, "vmsotel probe_logs marker %lu", (unsigned long) time(NULL));
    t0 = time(NULL);
    send_request(marker);

    /*
     * Two readers, polled together every POLL_S seconds for up to
     * `wait` seconds: the stream that read to EOF above, reading on (a
     * tail), and a fresh open each time that goes back by RFA. x86 has
     * shown the marker to neither within a minute in one run, and only
     * to a reopen, alongside a message OPCOM wrote 20 s later, in
     * another: OPCOM seems to write when it chooses. So this waits
     * long enough to see when, and stops once a reopen has seen it.
     */
    more = 0;
    tail_at = reopen_at = -1;
    for (i = 0; reopen_at < 0 && (long) (i * POLL_S) < wait; i++) {
        unsigned long got;
        long now;
        int hit = 0;

#ifdef __VMS
        sleep(POLL_S);  /* the stub's log changes only when told to */
#endif
        now = (long) (time(NULL) - t0);
        got = read_all(&f, n + more, marker, &hit, &end);
        more += got;
        if (hit && tail_at < 0)
            tail_at = now;
        got = reopen_read(name, rfa, last, lastlen, n, marker, &hit, i == 0);
        if (hit && reopen_at < 0)
            reopen_at = now;
        printf("  after %4ld s: open stream %lu new, reopened %lu new%s\n",
               now, more, got, hit ? ", with the marker" : "");
        fflush(stdout);
    }
    r_close(&f);

    if (tail_at >= 0) {
        sprintf(msg, "the marker appeared on the open stream after %ld s:"
                " tailing works", tail_at);
        probe_ok(msg);
    } else
        printf("  the open stream never showed the marker\n");
    if (reopen_at >= 0) {
        sprintf(msg, "the marker appeared after reopening, %ld s after "
                "the REQUEST", reopen_at);
        probe_ok(msg);
        if (tail_at < 0)
            printf("  so: tail by reopening\n");
        print_kept(reopen_last, 0);
    } else {
        printf("  not seen after reopening either, in %ld s\n", wait);
        probe_fail("the marker never appeared (is OPCOM logging? "
                   "SHOW LOGICAL OPC$*; REPLY/LOG)");
    }
}

/* ---- audit --------------------------------------------------------- */

/*
 * Reads a record by NSADEF's NSAMSGDEF, as x86's journal showed it: a
 * 20-byte header (word type, word subtype, word flags, word packet
 * count, word record size, byte spare, byte version, three spare
 * words, word facility), then packets of word size (including its own
 * four bytes), word type, data. Checks the packets add up to the
 * size. Returns 1 if they do.
 */
static int decode_nsa(const unsigned char *b, unsigned short len, int print)
{
    unsigned type, subtype, flags, count, size, version, k;
    unsigned off = 20;

    if (len < 20)
        return 0;
    type = b[0] | b[1] << 8;
    subtype = b[2] | b[3] << 8;
    flags = b[4] | b[5] << 8;
    count = b[6] | b[7] << 8;
    size = b[8] | b[9] << 8;
    version = b[11];
    if (print)
        printf("    type %u subtype %u flags %04x packets %u size %u "
               "version %u facility %u\n", type, subtype, flags, count,
               size, version, (unsigned) (b[18] | b[19] << 8));
    for (k = 0; k < count; k++) {
        unsigned psize, ptype, i, text = 1;

        if (off + 4 > len)
            return 0;
        psize = b[off] | b[off + 1] << 8;
        ptype = b[off + 2] | b[off + 3] << 8;
        if (psize < 4 || off + psize > len)
            return 0;
        for (i = off + 4; i < off + psize; i++)
            if (b[i] < 32 || b[i] > 126)
                text = 0;
        if (print) {
            printf("      packet type %3u, %3u bytes: ", ptype, psize - 4);
            for (i = off + 4; i < off + psize && i < off + 4 + 48; i++)
                printf(text ? "%c" : "%02x ", b[i]);
            printf("\n");
        }
        off += psize;
    }
    return size == len && off == len;
}

static void audit(const char *name)
{
    rfile f;
    unsigned int st, end;
    unsigned short minlen = 0, maxlen = 0;
    unsigned long n;

    printf("\nAudit journal: %s\n", name);
    st = r_open(&f, name);
    if (!(st & 1)) {
        printf("  open: %s\n", probe_msg(st));
        probe_fail("open the audit journal for shared reading");
        return;
    }
    probe_ok("open the audit journal for shared reading");
    printf("  record format %s (%d), longest record %u, attributes %02x\n",
           rfm_name(f.fab.fab$b_rfm), f.fab.fab$b_rfm,
           (unsigned) f.fab.fab$w_mrs, (unsigned) f.fab.fab$b_rat);

    /* The first records, as they come, then the last few. */
    st = r_get(&f);
    if (!(st & 1)) {
        printf("  first read: %s\n", probe_msg(st));
        if (st != RMS$_EOF)
            probe_fail("read the audit journal");
        r_close(&f);
        return;
    }
    printf("  the first record:\n");
    print_hex(rec, f.rab.rab$w_rsz);
    minlen = maxlen = f.rab.rab$w_rsz;
    keep(0, f.rab.rab$w_rsz);
    n = 1 + read_all_sz(&f, 1, &minlen, &maxlen, NULL, NULL, &end);
    printf("  %lu records, %u to %u bytes, then %s\n", n, (unsigned) minlen,
           (unsigned) maxlen, probe_msg(end));
    printf("  the last %d:\n", n < KEEP ? (int) n : KEEP);
    print_kept(n, 1);
    if (end != RMS$_EOF)
        probe_fail("read the audit journal to its end");
    r_close(&f);

    /* Every record decoded against NSAMSGDEF; the last few printed. */
    st = r_open(&f, name);
    if (!(st & 1))
        return;
    {
        unsigned long good = 0, bad = 0, i = 0;

        while (r_get(&f) & 1) {
            if (decode_nsa(rec, f.rab.rab$w_rsz, 0))
                good++;
            else if (++bad <= 3)
                print_hex(rec, f.rab.rab$w_rsz);
            keep(i++, f.rab.rab$w_rsz);
        }
        printf("  decoded by NSAMSGDEF: %lu records consistent, %lu not\n",
               good, bad);
        for (i = n > 3 ? n - 3 : 0; i < n; i++)
            decode_nsa(kept[i % KEEP], kept_len[i % KEEP], 1);
        if (bad == 0 && good > 0)
            probe_ok("every record's packets add up to its NSA$W_RECORD_SIZE");
        else
            probe_fail("records that do not fit NSAMSGDEF (dumped above)");
    }
    r_close(&f);
}

int main(int argc, char **argv)
{
    const char *oplog = getenv("OPC$LOGFILE_NAME");
    const char *journal = "SYS$MANAGER:SECURITY.AUDIT$JOURNAL";
    int request = 0, i;
    long wait = WAIT_S;

    if (oplog == NULL || *oplog == '\0')
        oplog = "SYS$MANAGER:OPERATOR.LOG";
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--request") == 0)
            request = 1;
        else if (strcmp(argv[i], "--oplog") == 0 && i + 1 < argc)
            oplog = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc)
            journal = argv[++i];
        else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
            wait = strtol(argv[++i], NULL, 10);
    }

    probe_banner("probe_logs");
    opcom(oplog, request, wait);
    audit(journal);
    return probe_summary();
}
