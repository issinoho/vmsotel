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
    unsigned long long value;
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
 * Nothing the service is given lives on the stack. On x86 the first
 * $GETRMI call returned, and then the loop in sample() faulted reading
 * rmi_items[1].code at address 0x14: the table's base address had become
 * 0. The fault was the same with a 4 KB static data buffer, so it is not
 * the data. The likeliest cause is the service writing the IOSB or the
 * return length wider than its 8 or 2 bytes, zeroing a saved pointer
 * beside it on the stack.
 *
 * So the IOSB, the return length and the item list sit in one static
 * block, between bytes filled with a pattern. After each call, guard()
 * reports any pattern byte that changed, which says which one was
 * written too wide and by how much. In the first sample each call's raw
 * results are printed before anything else runs, so even if the image
 * still dies afterwards, what the service returned is in the log.
 */
#define RMI_BUF_SIZE 4096
#define RMI_FILL 0xA5
#define GUARD 32

static unsigned char rmi_buf[RMI_BUF_SIZE];

/* [GUARD] iosb(8) [GUARD] retlen(2) [GUARD-2] item list(2 entries) [GUARD] */
#define OFF_IOSB   GUARD
#define OFF_RETLEN (OFF_IOSB + 8 + GUARD)
#define OFF_ILE    (OFF_RETLEN + GUARD)
#define ILE_BYTES  (2 * sizeof(probe_ile3))
#define BLOCK_SIZE (OFF_ILE + ILE_BYTES + GUARD)

static union {
    unsigned long long align;   /* quadword-aligned, as an IOSB must be */
    unsigned char b[BLOCK_SIZE];
} blk;

static int in_use(unsigned i)
{
    return (i >= OFF_IOSB && i < OFF_IOSB + 8) ||
           (i >= OFF_RETLEN && i < OFF_RETLEN + 2) ||
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
    unsigned short *retlen = (unsigned short *) (blk.b + OFF_RETLEN);
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
        ile[0].retlen = retlen;

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
            r->retlen = *retlen ? *retlen : sizes[attempt];
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

static unsigned long long now_100ns(void)
{
    unsigned long long t = 0;

    sys$gettim((void *) &t);
    return t;
}

int main(void)
{
    size_t i;
    unsigned long long t0, t1, cpu_sum = 0;
    unsigned int ncpu, cpu_items = 0;
    double seconds;

    probe_banner("probe_rmi");
    if (!probe_check_ile())
        return probe_summary();

    ncpu = active_cpus();
    printf("active CPUs (SYI$_ACTIVECPU_CNT): %u\n", ncpu);
    printf("sampling twice, %d s apart\n", INTERVAL_SECONDS);

    t0 = now_100ns();
    sample(first, 1);
    sleep(INTERVAL_SECONDS);
    t1 = now_100ns();
    sample(second, 0);
    seconds = (double) (t1 - t0) / 1e7;

    printf("\n  %-5s %-16s %20s %20s %12s  bytes\n",
           "", "item", "first", "second", "delta");
    for (i = 0; i < RMI_ITEMS_COUNT; i++) {
        const probe_item *it = &rmi_items[i];
        const rmi_result *a = &first[i], *b = &second[i];

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
        printf("  ok    %-16s %20llu %20llu %12lld  %u\n", it->name,
               a->value, b->value,
               (long long) (b->value - a->value), (unsigned) a->retlen);

        if (strncmp(it->name, "RMI$_CPU", 8) == 0) {
            cpu_sum += b->value - a->value;
            cpu_items++;
        }
    }

    if (guard_hits) {
        printf("\n");
        probe_fail("$GETRMI wrote outside its IOSB or return length");
    }

    printf("\nCPU mode counters\n");
    printf("  %u mode items accepted, sum of deltas %llu over %.2f s\n",
           cpu_items, cpu_sum, seconds);
    if (cpu_items == 0) {
        probe_fail("no CPU mode counters available");
    } else {
        double per_cpu_per_sec = (double) cpu_sum / seconds /
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
