/*
 * vmsotel probe: what phase 3 (logs) can read -- the operator log and
 * the security audit journal.
 *
 *     probe_logs [--request] [--oplog FILE] [--journal FILE]
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
 *        tail -- or only after closing and reopening the file?
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

typedef struct {
    struct FAB fab;
    struct RAB rab;
    char name[256];
    int open;
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
    return sys$get((void *) &f->rab);
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

/* Reads to the end. Returns records read; sizes are counted. */
static unsigned long read_all(rfile *f, unsigned long base,
                              unsigned short *minlen, unsigned short *maxlen,
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
        n++;
    }
    *end = st;
    return n;
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

static int contains(const unsigned char *b, unsigned short len,
                    const char *s)
{
    size_t n = strlen(s), i;

    for (i = 0; i + n <= len; i++)
        if (memcmp(b + i, s, n) == 0)
            return 1;
    return 0;
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

static void opcom(const char *name, int request)
{
    rfile f;
    unsigned int st, end;
    unsigned long n, more, i;
    char marker[80];
    int seen = 0;

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

    n = read_all(&f, 0, NULL, NULL, &end);
    printf("  %lu records, then %s\n", n, probe_msg(end));
    printf("  the last %d:\n", n < KEEP ? (int) n : KEEP);
    print_kept(n, 0);
    if (end != RMS$_EOF)
        probe_fail("read the operator log to its end");

    if (!request) {
        printf("  (run with --request to test reading new records)\n");
        r_close(&f);
        return;
    }

    sprintf(marker, "vmsotel probe_logs marker %lu", (unsigned long) time(NULL));
    send_request(marker);
    sleep(5);

    /* The same stream, after EOF: does RMS hand over what was added? */
    more = read_all(&f, n, NULL, NULL, &end);
    printf("  on the open stream after 5 s: %lu new records, then %s\n",
           more, probe_msg(end));
    for (i = n; i < n + more && i < n + KEEP; i++) {
        print_text(kept[i % KEEP], kept_len[i % KEEP]);
        if (contains(kept[i % KEEP], kept_len[i % KEEP], marker))
            seen = 1;
    }
    if (seen) {
        probe_ok("the marker appeared on the open stream: tailing works");
        r_close(&f);
        return;
    }
    printf("  marker not seen on the open stream\n");
    r_close(&f);

    /* Reopen and skip what was read before. */
    st = r_open(&f, name);
    if (!(st & 1)) {
        printf("  reopen: %s\n", probe_msg(st));
        probe_fail("reopen the operator log");
        return;
    }
    for (i = 0; i < n + more && (r_get(&f) & 1); i++)
        ;
    more = read_all(&f, 0, NULL, NULL, &end);
    printf("  after reopening: %lu new records\n", more);
    for (i = 0; i < more && i < KEEP; i++)
        if (contains(kept[i % KEEP], kept_len[i % KEEP], marker))
            seen = 1;
    print_kept(more, 0);
    r_close(&f);
    if (seen)
        probe_ok("the marker appeared after reopening: tail by reopening");
    else
        probe_fail("the marker never appeared (is OPCOM logging? "
                   "SHOW LOGICAL OPC$*; REPLY/LOG)");
}

/* ---- audit --------------------------------------------------------- */

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
    n = 1 + read_all(&f, 1, &minlen, &maxlen, &end);
    printf("  %lu records, %u to %u bytes, then %s\n", n, (unsigned) minlen,
           (unsigned) maxlen, probe_msg(end));
    printf("  the last %d:\n", n < KEEP ? (int) n : KEEP);
    print_kept(n, 1);
    if (end != RMS$_EOF)
        probe_fail("read the audit journal to its end");
    r_close(&f);
}

int main(int argc, char **argv)
{
    const char *oplog = getenv("OPC$LOGFILE_NAME");
    const char *journal = "SYS$MANAGER:SECURITY.AUDIT$JOURNAL";
    int request = 0, i;

    if (oplog == NULL || *oplog == '\0')
        oplog = "SYS$MANAGER:OPERATOR.LOG";
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--request") == 0)
            request = 1;
        else if (strcmp(argv[i], "--oplog") == 0 && i + 1 < argc)
            oplog = argv[++i];
        else if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc)
            journal = argv[++i];
    }

    probe_banner("probe_logs");
    opcom(oplog, request);
    audit(journal);
    return probe_summary();
}
