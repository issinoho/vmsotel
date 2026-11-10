/*
 * vmsotel probe: $GETRMI performance counters
 *
 * $GETRMI is the source MONITOR itself reads, and the one place CPU
 * time by mode, paging and process-state counts are available
 * system-wide. Whether VSI carried every item to x86-64, and what units
 * they come back in, is not something a header can tell us.
 *
 * Answers:
 *   1. Which RMI$_ items are declared, and which the service accepts.
 *   2. Whether each is a cumulative counter or a gauge: two samples
 *      are taken five seconds apart and the delta printed.
 *   3. The unit of the CPU-mode counters. The sum of all modes over an
 *      interval must equal wall time multiplied by the number of active
 *      CPUs. If the counters are 10 ms ticks, five seconds on one CPU
 *      is 500; the probe works out the ratio and checks it against
 *      SYI$_ACTIVECPU_CNT. That is the test that can fail.
 *
 * Privilege: unknown, which is part of the question. Run it once
 * unprivileged first.
 */

#include "vmsprobe.h"

#include <rmidef.h>
#include <syidef.h>
#include <unistd.h>

#include "items_rmi.h"

#define INTERVAL_SECONDS 5

typedef struct {
    int accepted;
    unsigned short retlen;
    probe_q value;
    unsigned int status;
    unsigned overrun;           /* bytes written, when more than offered */
    unsigned offered;
    unsigned char raw[64];      /* the start of what was written */
} rmi_result;

static rmi_result first[RMI_ITEMS_COUNT], second[RMI_ITEMS_COUNT];

/*
 * One item per call. The buffer is offered as a quadword first; if the
 * service insists on exactly a longword, it is asked again with 4.
 *
 * $GETRMI writes the return length as a LONGWORD, not the word that
 * item_list_3 declares. On x86 E9.2-4 a 2-byte return length on the
 * stack had its neighbour zeroed -- a saved pointer -- and the next loop
 * iteration faulted at 0x14. The guard bytes showed it: retlen+2 and
 * retlen+3 written, nothing else, for every item. So the return length
 * here is 4 bytes.
 *
 * The IOSB, the return length and the item list still sit in one
 * static block between pattern-filled guard bytes, and any guard byte
 * the service changes is reported, so the other machine is checked too.
 * In the first sample each call's raw results are printed before
 * anything else runs.
 */
#define RMI_BUF_SIZE 4096
#define RMI_FILL 0xA5
#define GUARD 32

static unsigned char rmi_buf[RMI_BUF_SIZE];

/* [GUARD] iosb(8) [GUARD] retlen(4) [GUARD-4] item list(2 entries) [GUARD] */
#define OFF_IOSB   GUARD
#define OFF_RETLEN (OFF_IOSB + 8 + GUARD)
#define OFF_ILE    (OFF_RETLEN + GUARD)
#define ILE_BYTES  (2 * sizeof(probe_ile3))
#define BLOCK_SIZE (OFF_ILE + ILE_BYTES + GUARD)

static union {
    double align;               /* quadword-aligned, as an IOSB must be;
                                   a double, as VAX has no 64-bit integer */
    unsigned char b[BLOCK_SIZE];
} blk;

static int in_use(unsigned i)
{
    return (i >= OFF_IOSB && i < OFF_IOSB + 8) ||
           (i >= OFF_RETLEN && i < OFF_RETLEN + 4) ||
           (i >= OFF_ILE && i < OFF_ILE + ILE_BYTES);
}

/* Prints every guard byte the service changed. Returns how many. */
static unsigned guard_report(void)
{
    unsigned i, n = 0;

    for (i = 0; i < BLOCK_SIZE; i++)
        if (!in_use(i) && blk.b[i] != RMI_FILL) {
            if (n == 0)
                printf("    GUARD changed:");
            printf(" %s+%d=%02x",
                   i < OFF_RETLEN ? "iosb" : i < OFF_ILE ? "retlen" : "itmlst",
                   i < OFF_RETLEN ? (int) i - OFF_IOSB :
                   i < OFF_ILE ? (int) i - OFF_RETLEN : (int) i - OFF_ILE,
                   blk.b[i]);
            n++;
        }
    if (n)
        printf("\n");
    return n;
}

/* Index one past the last byte that no longer holds the fill pattern.
   A written byte that happens to equal the pattern makes this an
   underestimate, so the report says "at least". */
static unsigned written_extent(void)
{
    unsigned n = RMI_BUF_SIZE;

    while (n > 0 && rmi_buf[n - 1] == RMI_FILL)
        n--;
    return n;
}

static unsigned guard_hits;

static void query_one(const probe_item *it, rmi_result *r, int trace)
{
    static const unsigned short sizes[2] = { 8, 4 };
    unsigned short *iosb = (unsigned short *) (blk.b + OFF_IOSB);
    unsigned int *retlen = (unsigned int *) (blk.b + OFF_RETLEN);
    probe_ile3 *ile = (probe_ile3 *) (blk.b + OFF_ILE);
    int attempt;

    memset(r, 0, sizeof *r);
    for (attempt = 0; attempt < 2; attempt++) {
        unsigned int st, st_q;
        unsigned extent, hits;

        memset(rmi_buf, RMI_FILL, sizeof rmi_buf);
        memset(blk.b, RMI_FILL, sizeof blk.b);
        memset(iosb, 0, 8);
        *retlen = 0;
        memset(ile, 0, ILE_BYTES);
        ile[0].len = sizes[attempt];
        ile[0].code = (unsigned short) it->code;
        ile[0].buf = rmi_buf;
        ile[0].retlen = (unsigned short *) retlen;   /* see above */

        /* $GETRMI has no W form: queue it, then wait on the IOSB. */
        st_q = st = sys$getrmi(EFN$C_ENF, 0, 0, (void *) ile, (void *) iosb, 0, 0);
        if (st & 1)
            st = sys$synch(EFN$C_ENF, (void *) iosb);

        extent = written_extent();
        if (trace) {
            printf("    len %u: queue %08X synch %08X iosb %04X %04X %04X %04X"
                   " retlen %u wrote %u\n",
                   (unsigned) sizes[attempt], st_q, st,
                   iosb[0], iosb[1], iosb[2], iosb[3],
                   (unsigned) *retlen, extent);
            fflush(stdout);
        }
        hits = guard_report();
        if (hits) {
            if (!trace)
                printf("    (%s, len %u)\n", it->name, (unsigned) sizes[attempt]);
            guard_hits++;
            fflush(stdout);
        }

        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
        r->status = st;
        if (extent > sizes[attempt]) {
            r->overrun = extent;
            r->offered = sizes[attempt];
            memcpy(r->raw, rmi_buf, sizeof r->raw);
        }
        if (st & 1) {
            r->accepted = 1;
            r->retlen = (unsigned short) (*retlen ? *retlen : sizes[attempt]);
            r->value = probe_le(rmi_buf, sizes[attempt]);
            return;
        }
        if (st != SS$_BADPARAM)
            return;
    }
}

/*
 * Names each item before asking for it, so that if the service still
 * brings the image down, the last name printed is the culprit. The
 * index is static and volatile so it is re-read from memory after each
 * call rather than trusted to a register the service might not have
 * preserved; if the crash stops with no guard changed, that is the
 * likelier cause.
 */
static volatile size_t sample_i;

static void sample(rmi_result *out, int trace)
{
    for (sample_i = 0; sample_i < RMI_ITEMS_COUNT; sample_i++) {
        if (rmi_items[sample_i].code == PROBE_UNDECLARED)
            continue;
        if (trace) {
            printf("  querying %s\n", rmi_items[sample_i].name);
            fflush(stdout);
        }
        query_one(&rmi_items[sample_i], &out[sample_i], trace);
    }
}

/* An item that wrote past the length it was offered. The agent must
   size its buffers for this, so it counts as a failure. */
static void report_overrun(const probe_item *it, const rmi_result *r)
{
    unsigned i, show;

    if (r->overrun == 0)
        return;
    printf("  OVERRUN %s: offered %u bytes, service wrote at least %u:\n   ",
           it->name, r->offered, r->overrun);
    show = r->overrun < sizeof r->raw ? r->overrun : (unsigned) sizeof r->raw;
    for (i = 0; i < show; i++)
        printf(" %02x", r->raw[i]);
    printf("%s\n", r->overrun > sizeof r->raw ? " ..." : "");
    probe_fail("$GETRMI wrote past the buffer length it was given");
}

/*
 * RMI$_MODES: VAX's <rmidef.h> declares none of the RMI$_CPU* items
 * (first VAX run), but has RMI$_MODES. Its layout, from the $GETRMI
 * description in the OpenVMS System Services Reference: 4 bytes
 * "reserved for internal use", then one packed 33-byte record per CPU
 * -- a byte of physical CPU ID and eight longwords: interrupt (which
 * "is actually the sum of interrupt and idle"), MP synch, kernel,
 * exec, super, user, reserved (zero), idle -- in 10 ms ticks since
 * boot. The VAX wrote 37 bytes for its one CPU, which fits.
 *
 * The check: with idle taken out of interrupt, the deltas of all
 * modes on all CPUs must add up to 10 ms ticks of wall time per CPU.
 */
#define MODES_CAP 512
#define MODES_HEAD 4
#define MODES_REC 33

static const char *const mode_names[8] = {
    "interrupt+idle", "mpsynch", "kernel", "exec", "super", "user",
    "reserved", "idle"
};

static unsigned modes_len;

/* Returns the status; out gets MODES_CAP bytes, modes_len the extent. */
static unsigned int modes_sample(unsigned char *out, int trace)
{
#ifdef RMI$_MODES
    unsigned short *iosb = (unsigned short *) (blk.b + OFF_IOSB);
    unsigned int *retlen = (unsigned int *) (blk.b + OFF_RETLEN);
    probe_ile3 *ile = (probe_ile3 *) (blk.b + OFF_ILE);
    unsigned int st;

    memset(rmi_buf, RMI_FILL, sizeof rmi_buf);
    memset(blk.b, RMI_FILL, sizeof blk.b);
    memset(iosb, 0, 8);
    *retlen = 0;
    memset(ile, 0, ILE_BYTES);
    ile[0].len = MODES_CAP;
    ile[0].code = RMI$_MODES;
    ile[0].buf = rmi_buf;
    ile[0].retlen = (unsigned short *) retlen;
    st = sys$getrmi(EFN$C_ENF, 0, 0, (void *) ile, (void *) iosb, 0, 0);
    if (st & 1)
        st = sys$synch(EFN$C_ENF, (void *) iosb);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    modes_len = written_extent();
    if (trace)
        printf("  status %08X retlen %u wrote at least %u of %u\n",
               st, (unsigned) *retlen, modes_len, (unsigned) MODES_CAP);
    if (guard_report())
        guard_hits++;
    if (modes_len > MODES_CAP) {
        printf("  OVERRUN RMI$_MODES wrote past %u bytes\n", (unsigned) MODES_CAP);
        probe_fail("$GETRMI wrote past the buffer length it was given");
    }
    memcpy(out, rmi_buf, MODES_CAP);
    return st;
#else
    (void) out;
    (void) trace;
    return 0;
#endif
}

static void hex_dump(const char *label, const unsigned char *b, unsigned n)
{
    unsigned i;

    printf("  %s (%u bytes):", label, n);
    for (i = 0; i < n; i++)
        printf("%s%02x", i % 32 == 0 ? "\n    " : i % 4 == 0 ? "  " : " ", b[i]);
    printf("\n");
}

/* Returns 1 if RMI$_MODES reads as 10 ms ticks of every CPU. */
static int probe_modes(unsigned int ncpu)
{
    static unsigned char a[MODES_CAP], b[MODES_CAP];
    unsigned int st, sum = 0, cpus = ncpu ? ncpu : 1;
    unsigned recs, r, m, n;
    probe_q t0, t1;
    double seconds, rate;

    printf("\nRMI$_MODES\n");
#ifndef RMI$_MODES
    printf("  --    RMI$_MODES not declared in <rmidef.h>\n");
    return 0;
#endif
    t0 = probe_gettim();
    st = modes_sample(a, 1);
    if (!(st & 1)) {
        printf("  no    RMI$_MODES %s\n", probe_msg(st));
        return 0;
    }
    n = modes_len;
    sleep(INTERVAL_SECONDS);
    t1 = probe_gettim();
    st = modes_sample(b, 0);
    if (!(st & 1)) {
        printf("  no    RMI$_MODES, second sample: %s\n", probe_msg(st));
        return 0;
    }
    if (modes_len > n)
        n = modes_len;
    if (n > MODES_CAP)
        n = MODES_CAP;
    seconds = probe_q_double(probe_q_sub(t1, t0)) / 1e7;
    hex_dump("first", a, n);
    hex_dump("second", b, n);

    /* A trailing byte equal to the fill pattern makes n short by up to
       a few bytes, so round up to whole records. */
    recs = n > MODES_HEAD ? (n - MODES_HEAD + MODES_REC - 1) / MODES_REC : 0;
    printf("  %u bytes = 4 + %u x 33%s\n", n, recs,
           n == MODES_HEAD + recs * MODES_REC ? "" : " (not exact)");
    if (recs == 0) {
        probe_fail("RMI$_MODES returned no CPU record");
        return 0;
    }
    for (r = 0; r < recs; r++) {
        const unsigned off = MODES_HEAD + r * MODES_REC;
        unsigned int d[8];

        printf("  CPU %u:\n", (unsigned) a[off]);
        for (m = 0; m < 8; m++) {
            unsigned int v0 = probe_le32(a + off + 1 + 4 * m, 4);
            unsigned int v1 = probe_le32(b + off + 1 + 4 * m, 4);

            d[m] = v1 - v0;
            printf("    %-15s %10u -> %10u  +%u\n", mode_names[m], v0, v1, d[m]);
        }
        /* interrupt includes idle; take it out so idle counts once */
        d[0] -= d[7];
        for (m = 0; m < 8; m++)
            sum += d[m];
    }
    rate = (double) sum / seconds / (double) cpus;
    printf("  sum, idle counted once: %u over %.2f s = %.1f per s per "
           "active CPU\n", sum, seconds, rate);
    if (rate > 90.0 && rate < 110.0) {
        probe_ok("RMI$_MODES is 10 ms ticks by mode, as documented");
        return 1;
    }
    probe_fail("RMI$_MODES does not sum to 10 ms ticks per CPU; "
               "the dump above is the evidence");
    return 0;
}

static unsigned int active_cpus(void)
{
    unsigned int n = 0;
    unsigned short retlen = 0;
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    probe_ile3 ile[2];

#ifdef SYI$_ACTIVECPU_CNT
    memset(ile, 0, sizeof ile);
    ile[0].len = 4;
    ile[0].code = SYI$_ACTIVECPU_CNT;
    ile[0].buf = &n;
    ile[0].retlen = &retlen;
    if (!(sys$getsyiw(EFN$C_ENF, 0, 0, (void *) ile, (void *) iosb, 0, 0) & 1) ||
        !(iosb[0] & 1))
        n = 0;
#endif
    return n;
}

int main(void)
{
    size_t i;
    probe_q t0, t1, cpu_sum;
    unsigned int ncpu, cpu_items = 0;
    int modes_ok;
    double seconds;

    probe_banner("probe_rmi");
    if (!probe_check_ile())
        return probe_summary();

    ncpu = active_cpus();
    printf("active CPUs (SYI$_ACTIVECPU_CNT): %u\n", ncpu);
    printf("sampling twice, %d s apart\n", INTERVAL_SECONDS);

    cpu_sum = probe_q_make(0u, 0u);
    t0 = probe_gettim();
    sample(first, 1);
    sleep(INTERVAL_SECONDS);
    t1 = probe_gettim();
    sample(second, 0);
    seconds = probe_q_double(probe_q_sub(t1, t0)) / 1e7;

    printf("\n  %-5s %-16s %20s %20s %12s  bytes\n",
           "", "item", "first", "second", "delta");
    for (i = 0; i < RMI_ITEMS_COUNT; i++) {
        const probe_item *it = &rmi_items[i];
        const rmi_result *a = &first[i], *b = &second[i];
        char da[PROBE_Q_DIGITS], db[PROBE_Q_DIGITS], dd[PROBE_Q_DIGITS];

        if (it->code == PROBE_UNDECLARED) {
            printf("  --    %-16s not declared in <rmidef.h>\n", it->name);
            continue;
        }
        report_overrun(it, a);
        if (!a->accepted || !b->accepted) {
            printf("  no    %-16s %s\n", it->name,
                   probe_msg(a->accepted ? b->status : a->status));
            continue;
        }
        printf("  ok    %-16s %20s %20s %12s  %u\n", it->name,
               probe_q_str(a->value, da), probe_q_str(b->value, db),
               probe_q_delta_str(a->value, b->value, dd),
               (unsigned) a->retlen);

        /* A mode counter that goes backwards is not a counter; on x86
           RMI$_CPUINTSTK was near 2^64 and fell. Left out of the sum so
           the unit check still says something, and failed loudly. */
        if (strncmp(it->name, "RMI$_CPU", 8) == 0) {
            if (probe_q_cmp(b->value, a->value) < 0) {
                printf("  BACKWARDS %s: %08X%08X -> %08X%08X\n", it->name,
                       a->value.hi, a->value.lo, b->value.hi, b->value.lo);
                probe_fail("a CPU mode counter went backwards; left out of the sum");
            } else {
                cpu_sum = probe_q_add(cpu_sum, probe_q_sub(b->value, a->value));
                cpu_items++;
            }
        }
    }

    if (guard_hits) {
        printf("\n");
        probe_fail("$GETRMI wrote outside its IOSB or return length");
    }

    modes_ok = probe_modes(ncpu);

    printf("\nCPU mode counters\n");
    {
        char d[PROBE_Q_DIGITS];

        printf("  %u mode items accepted, sum of deltas %s over %.2f s\n",
               cpu_items, probe_q_str(cpu_sum, d), seconds);
    }
    if (cpu_items == 0 && modes_ok) {
        printf("  none of RMI$_CPU*; RMI$_MODES has them (above)\n");
    } else if (cpu_items == 0) {
        probe_fail("no CPU mode counters available");
    } else {
        double per_cpu_per_sec = probe_q_double(cpu_sum) / seconds /
                                 (ncpu ? (double) ncpu : 1.0);

        printf("  = %.1f units per second per active CPU\n", per_cpu_per_sec);
        if (per_cpu_per_sec > 90.0 && per_cpu_per_sec < 110.0)
            probe_ok("CPU mode counters are 10 ms ticks, summed over CPUs");
        else if (ncpu > 1 && per_cpu_per_sec * ncpu > 90.0 &&
                 per_cpu_per_sec * ncpu < 110.0)
            probe_fail("CPU counters cover ONE CPU, not all of them");
        else
            probe_fail("CPU mode counters are not 10 ms ticks of all CPUs; "
                       "unit unknown -- paste this whole output");
    }
    return probe_summary();
}
