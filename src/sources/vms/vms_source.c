/*
 * vmsotel: the VMS source -- $GETSYI, $GETRMI, $DEVICE_SCAN, $GETDVI.
 *
 * Uses only items and behaviour docs/findings.md confirms on both
 * machines, with the sizes found there:
 *   - $GETRMI: the eight CPU modes, FRLIST and MODLIST are quadwords;
 *     every other item refuses a quadword and takes a longword. The
 *     return length is written as a LONGWORD, not item_list_3's word;
 *     a word on the stack crashed probe_rmi on x86. No privilege.
 *   - CPU modes are 10 ms ticks summed over the active CPUs.
 *   - $DEVICE_SCAN by DC$_DISK finds every disk, plus LD devices on x86.
 *     DVI$_MNT, LOGVOLNAM, MAXBLOCK, FREEBLOCKS (512-byte blocks,
 *     longwords) and OPCNT (a cumulative longword) behave as expected.
 *   - SYI$_BOOTTIME is local time at boot.
 *
 * Held back, because the units are not confirmed: memory (SYI$_MEMSIZE,
 * RMI$_FRLIST, RMI$_MODLIST) and the page file (SYI$_PAGEFILE_PAGE and
 * _FREE) could be pages or pagelets, and nothing has yet compared them
 * with SHOW MEMORY. The self-test prints them beside it; once a run on
 * both machines settles it, set mem_present and pagefile_present
 * with the unit proven, and they are reported.
 *
 * Shadow-set members are skipped by DVI$_SHDW_MEMBER, so a mirrored
 * volume is counted once, as its DSA virtual unit. That item is
 * confirmed accepted on both machines, but neither has a shadow set,
 * so a real member has not been seen. The memory disk, whose logical
 * volume name begins SYS$MD (x86), is skipped too.
 *
 * Built without /POINTER_SIZE: the item lists are item_list_3 only
 * with 32-bit pointers, and vo_source_open() refuses to run otherwise.
 */

/* gethostname, as probe_otlp used it on both machines. */
#if !defined(__VMS) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200112L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <dcdef.h>
#include <descrip.h>
#include <dvidef.h>
#include <dvsdef.h>
#include <efndef.h>
#include <rmidef.h>
#include <ssdef.h>
#include <starlet.h>
#include <syidef.h>

#include "source.h"

/* 100 ns units from 17-NOV-1858 to 1970-01-01. */
#define VMS_TO_UNIX 35067168000000000ULL

typedef struct {
    unsigned short len;
    unsigned short code;
    void *buf;
    void *retlen;       /* a word for most services, a longword for $GETRMI */
} ile3;

/* ---- one item at a time ------------------------------------------- */

static unsigned int syi(int code, void *buf, unsigned short len,
                        unsigned short *retlen)
{
    ile3 ile[2];
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    unsigned int st;

    memset(ile, 0, sizeof ile);
    ile[0].len = len;
    ile[0].code = (unsigned short) code;
    ile[0].buf = buf;
    ile[0].retlen = retlen;
    st = sys$getsyiw(EFN$C_ENF, 0, 0, (void *) ile, (void *) iosb, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    return st;
}

/* A numeric item of up to 8 bytes; shorter ones (1 and 2 bytes, such
   as CLUSTER_MEMBER) land in the low bytes of a zeroed buffer. */
static unsigned int syi_num(int code, unsigned short len, vo_u64 *out)
{
    unsigned char b[8];
    unsigned short rl = 0;
    unsigned int st;
    int i;

    memset(b, 0, sizeof b);
    st = syi(code, b, len, &rl);
    *out = 0;
    for (i = 7; i >= 0; i--)
        *out = (*out << 8) | b[i];
    return st;
}

static void syi_text(int code, char *out, size_t cap)
{
    unsigned short rl = 0;
    char b[256];

    out[0] = '\0';
    if (!(syi(code, b, sizeof b - 1, &rl) & 1))
        return;
    while (rl > 0 && (b[rl - 1] == ' ' || b[rl - 1] == '\0'))
        rl--;
    if (rl >= cap)
        rl = (unsigned short) (cap - 1);
    memcpy(out, b, rl);
    out[rl] = '\0';
}

/* $GETRMI has no W form: queue, then wait on the IOSB. The return
   length is a longword (see the top of the file). */
static unsigned int rmi(int code, unsigned short len, vo_u64 *out)
{
    unsigned char b[8];
    unsigned int retlen = 0;
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    ile3 ile[2];
    unsigned int st;
    int i;

    memset(b, 0, sizeof b);
    memset(ile, 0, sizeof ile);
    ile[0].len = len;
    ile[0].code = (unsigned short) code;
    ile[0].buf = b;
    ile[0].retlen = &retlen;
    st = sys$getrmi(EFN$C_ENF, 0, 0, (void *) ile, (void *) iosb, 0, 0);
    if (st & 1)
        st = sys$synch(EFN$C_ENF, (void *) iosb);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    *out = 0;
    for (i = 7; i >= 0; i--)
        *out = (*out << 8) | b[i];
    return st;
}

static void make_desc(struct dsc$descriptor_s *d, char *s, size_t len)
{
    d->dsc$w_length = (unsigned short) len;
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = s;
}

static unsigned int dvi(const char *dev, int code, void *buf,
                        unsigned short len, unsigned short *retlen)
{
    struct dsc$descriptor_s d;
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    ile3 ile[2];
    unsigned int st;

    make_desc(&d, (char *) dev, strlen(dev));
    memset(ile, 0, sizeof ile);
    ile[0].len = len;
    ile[0].code = (unsigned short) code;
    ile[0].buf = buf;
    ile[0].retlen = retlen;
    st = sys$getdviw(EFN$C_ENF, 0, (void *) &d, (void *) ile, (void *) iosb,
                     0, 0, 0);
    if ((st & 1) && !(iosb[0] & 1))
        st = iosb[0];
    return st;
}

static vo_u32 dvi_long(const char *dev, int code, unsigned int *st)
{
    unsigned char b[4];
    unsigned short rl = 0;

    memset(b, 0, sizeof b);
    *st = dvi(dev, code, b, sizeof b, &rl);
    return (vo_u32) b[0] | (vo_u32) b[1] << 8 | (vo_u32) b[2] << 16 |
           (vo_u32) b[3] << 24;
}

static void dvi_text(const char *dev, int code, char *out, size_t cap)
{
    char b[256];
    unsigned short rl = 0;

    out[0] = '\0';
    if (!(dvi(dev, code, b, sizeof b - 1, &rl) & 1))
        return;
    while (rl > 0 && b[rl - 1] == ' ')
        rl--;
    if (rl >= cap)
        rl = (unsigned short) (cap - 1);
    memcpy(out, b, rl);
    out[rl] = '\0';
}

/* ---- state kept from open ------------------------------------------ */

static vo_u64 boot_ns;
static vo_u64 page_size;
static char host_name[256], host_type[128], os_version[16], scsnode[16];
static const char *host_arch;
static int cluster_member;

/*
 * SYI$_BOOTTIME is local time at boot, so it is turned into UTC with
 * the differential in force when the agent starts -- the right one when
 * the agent starts at boot, as SYSTARTUP_VMS.COM will start it. It is
 * read once: a summer-time change while the agent runs must not move
 * the start time, or every counter would look reset. If the agent is
 * started later, across a change since boot, system.uptime is off by
 * the change (docs/design.md, Counters).
 */
static int read_boot(char *err, size_t errcap)
{
    vo_u64 bt;
    const char *tdf_s = getenv("SYS$TIMEZONE_DIFFERENTIAL");
    long long tdf = tdf_s ? strtol(tdf_s, NULL, 10) : 0;
    long long unix100;
    unsigned int st = syi_num(SYI$_BOOTTIME, 8, &bt);

    if (!(st & 1) || bt <= VMS_TO_UNIX) {
        snprintf(err, errcap, "SYI$_BOOTTIME: status %%X%08X", st);
        return 0;
    }
    unix100 = (long long) (bt - VMS_TO_UNIX) - tdf * 10000000LL;
    boot_ns = unix100 > 0 ? (vo_u64) unix100 * 100ULL : 0;
    return 1;
}

int vo_source_open(char *err, size_t errcap)
{
    char arch[32];
    vo_u64 v;
    unsigned int st;

#ifdef __VMS
    if (sizeof(ile3) != 12) {
        snprintf(err, errcap, "item lists need 32-bit pointers; "
                 "build this source without /POINTER_SIZE");
        return 0;
    }
#endif
    st = syi_num(SYI$_PAGE_SIZE, 4, &page_size);
    if (!(st & 1) || page_size == 0) {
        snprintf(err, errcap, "SYI$_PAGE_SIZE: status %%X%08X", st);
        return 0;
    }
    if (!read_boot(err, errcap))
        return 0;

    syi_text(SYI$_HW_NAME, host_type, sizeof host_type);
    syi_text(SYI$_VERSION, os_version, sizeof os_version);
    syi_text(SYI$_SCSNODE, scsnode, sizeof scsnode);
    syi_text(SYI$_ARCH_NAME, arch, sizeof arch);
    /* semconv host.arch: x86_64 is amd64; IA64 is ia64. */
    host_arch = strcmp(arch, "x86_64") == 0 ? "amd64" :
                strcmp(arch, "IA64") == 0 ? "ia64" : NULL;
    cluster_member = (syi_num(SYI$_CLUSTER_MEMBER, 4, &v) & 1) ? v != 0 : -1;

    /* The TCP/IP name, as the probes sent (x86vms.issinoho.com), or
       the node name when TCP/IP has none. */
    if (gethostname(host_name, sizeof host_name) != 0 || host_name[0] == '\0')
        syi_text(SYI$_NODENAME, host_name, sizeof host_name);
    host_name[sizeof host_name - 1] = '\0';
    return 1;
}

void vo_source_resource(vo_resource *res)
{
    res->host_name = host_name[0] ? host_name : NULL;
    res->host_arch = host_arch;
    res->host_type = host_type[0] ? host_type : NULL;
    res->os_version = os_version[0] ? os_version : NULL;
    res->scsnode = scsnode[0] ? scsnode : NULL;
    res->cluster_member = cluster_member;
}

void vo_source_close(void)
{
}

/* ---- one reading --------------------------------------------------- */

static const struct { int code; int mode; } cpu_items[VO_CPU_MODES] = {
    { RMI$_CPUINTSTK, VO_CPU_INTERRUPT }, { RMI$_CPUMPSYNCH, VO_CPU_MPSYNCH },
    { RMI$_CPUKERNEL, VO_CPU_KERNEL },    { RMI$_CPUEXEC, VO_CPU_EXEC },
    { RMI$_CPUSUPER, VO_CPU_SUPER },      { RMI$_CPUUSER, VO_CPU_USER },
    { RMI$_CPUCOMPAT, VO_CPU_COMPAT },    { RMI$_CPUIDLE, VO_CPU_IDLE }
};

static const struct { int code; int state; } state_items[VO_PROC_STATES] = {
    { RMI$_COM, VO_PS_COM },   { RMI$_COMO, VO_PS_COMO },
    { RMI$_CUR, VO_PS_CUR },   { RMI$_PFW, VO_PS_PFW },
    { RMI$_LEF, VO_PS_LEF },   { RMI$_LEFO, VO_PS_LEFO },
    { RMI$_HIB, VO_PS_HIB },   { RMI$_HIBO, VO_PS_HIBO },
    { RMI$_SUSP, VO_PS_SUSP }, { RMI$_SUSPO, VO_PS_SUSPO },
    { RMI$_FPG, VO_PS_FPG },   { RMI$_COLPG, VO_PS_COLPG },
    { RMI$_MWAIT, VO_PS_MWAIT }, { RMI$_CEF, VO_PS_CEF }
};

#define RMI_LONG(code, field) do { \
        vo_u64 v_; \
        st = rmi((code), 4, &v_); \
        if (!(st & 1)) { what = #code; goto fail; } \
        (field) = (vo_u32) v_; \
    } while (0)

/* "_X86VMS$DKA0:" as $DEVICE_SCAN gives it, to "X86VMS$DKA0". */
static void tidy_device(const char *in, char *out, size_t cap)
{
    size_t n;

    if (*in == '_')
        in++;
    n = strlen(in);
    if (n > 0 && in[n - 1] == ':')
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

/* Mounted disks, less shadow-set members and the memory disk. */
static void read_disks(vo_raw *r)
{
    unsigned int devclass = DC$_DISK;
    unsigned int ctx[2] = { 0, 0 };
    ile3 sel[2];

    memset(sel, 0, sizeof sel);
    sel[0].len = 4;
    sel[0].code = DVS$_DEVCLASS;
    sel[0].buf = &devclass;

    r->ndisks = 0;
    for (;;) {
        char name[65], logvol[VO_NAME_MAX + 1];
        unsigned short len = 0;
        struct dsc$descriptor_s d;
        unsigned int st;
        vo_u32 maxblock, freeblocks, ops;
        vo_raw_disk *out;

        make_desc(&d, name, 64);
        st = sys$device_scan((void *) &d, &len, 0, (void *) sel, (void *) ctx);
        if (!(st & 1))
            break;              /* SS$_NOMOREDEV, or a scan error */
        name[len] = '\0';

        if (!dvi_long(name, DVI$_MNT, &st) || !(st & 1))
            continue;
        if (dvi_long(name, DVI$_SHDW_MEMBER, &st) && (st & 1))
            continue;
        dvi_text(name, DVI$_LOGVOLNAM, logvol, sizeof logvol);
        if (strncmp(logvol, "SYS$MD", 6) == 0)
            continue;
        maxblock = dvi_long(name, DVI$_MAXBLOCK, &st);
        if (!(st & 1))
            continue;
        freeblocks = dvi_long(name, DVI$_FREEBLOCKS, &st);
        if (!(st & 1))
            continue;
        ops = dvi_long(name, DVI$_OPCNT, &st);
        if (!(st & 1))
            continue;
        if (r->ndisks == VO_MAX_DISKS)
            break;

        out = &r->disks[r->ndisks++];
        tidy_device(name, out->device, sizeof out->device);
        strcpy(out->mountpoint, logvol);
        out->total_bytes = (vo_u64) maxblock * 512;
        out->free_bytes = (vo_u64) freeblocks * 512;
        out->ops = ops;
    }
}

int vo_source_read(vo_raw *r, vo_u64 now_ns, char *err, size_t errcap)
{
    const char *what = "";
    unsigned int st;
    vo_u64 v;
    int i;

    memset(r, 0, sizeof *r);
    r->now_ns = now_ns;
    r->boot_ns = boot_ns;

    for (i = 0; i < VO_CPU_MODES; i++) {
        st = rmi(cpu_items[i].code, 8, &r->cpu_ticks[cpu_items[i].mode]);
        r->cpu_present[cpu_items[i].mode] = (st & 1) != 0;
    }
    st = syi_num(SYI$_ACTIVECPU_CNT, 4, &v);
    if (!(st & 1)) {
        what = "SYI$_ACTIVECPU_CNT";
        goto fail;
    }
    r->cpu_count = (vo_u32) v;

    RMI_LONG(RMI$_FAULTS, r->faults);
    RMI_LONG(RMI$_PREADS, r->page_reads);
    RMI_LONG(RMI$_PREADIO, r->page_read_ios);
    RMI_LONG(RMI$_PWRITIO, r->page_write_ios);
    RMI_LONG(RMI$_DIRIO, r->direct_ios);
    RMI_LONG(RMI$_BUFIO, r->buffered_ios);
    for (i = 0; i < VO_PROC_STATES; i++)
        RMI_LONG(state_items[i].code, r->proc_states[state_items[i].state]);

    /* Held back until the self-test settles pages or pagelets: see the
       top of this file. Absent, not zero, so nothing false goes out. */
    r->mem_present = 0;
    r->pagefile_present = 0;

    read_disks(r);
    return 1;

fail:
    snprintf(err, errcap, "%s: status %%X%08X", what, st);
    return 0;
}

/* ---- the self-test ------------------------------------------------- */

/*
 * Prints each value beside the VMS command that shows the same number,
 * so that one pasted log settles whether the source reads what VMS
 * reports. Counters are sampled twice, 5 s apart, and shown as rates,
 * because that is what MONITOR shows.
 */
int vo_source_selftest(void)
{
    static vo_raw a, b;
    char err[160];
    vo_u64 memsize = 0, frlist = 0, modlist = 0, pf_page = 0, pf_free = 0;
    vo_u64 total = 0, delta[VO_CPU_MODES];
    size_t i;
    int ok = 1;

    if (!vo_source_open(err, sizeof err)) {
        printf("FAIL  open: %s\n", err);
        return 1;
    }
    printf("vmsotel self-test: %s, %s, %s, page size %llu\n", scsnode,
           os_version, host_arch ? host_arch : "(unknown arch)", page_size);
    printf("  host.name %s, host.type %s, cluster member %d\n\n",
           host_name, host_type, cluster_member);

    if (!vo_source_read(&a, 0, err, sizeof err)) {
        printf("FAIL  read: %s\n", err);
        return 1;
    }
    printf("sampling again in 5 s\n\n");
    sleep(5);
    if (!vo_source_read(&b, 5000000000ULL, err, sizeof err)) {
        printf("FAIL  read: %s\n", err);
        return 1;
    }

    printf("Boot and uptime                                   compare with\n");
    {
        vo_u64 now = (vo_u64) time(NULL);
        vo_u64 up = boot_ns ? now - boot_ns / 1000000000ULL : 0;

        printf("  uptime         %llu s = %llu %02llu:%02llu:%02llu    "
               "SHOW SYSTEM/NOPROCESS \"Uptime\"\n", up, up / 86400,
               (up % 86400) / 3600, (up % 3600) / 60, up % 60);
    }

    printf("\nMemory, raw -- NOT YET REPORTED, units unconfirmed\n");
    syi_num(SYI$_MEMSIZE, 4, &memsize);
    rmi(RMI$_FRLIST, 8, &frlist);
    rmi(RMI$_MODLIST, 8, &modlist);
    syi_num(SYI$_PAGEFILE_PAGE, 4, &pf_page);
    syi_num(SYI$_PAGEFILE_FREE, 4, &pf_free);
    printf("  SYI$_MEMSIZE        %12llu    SHOW MEMORY/PHYSICAL \"Total\"\n",
           memsize);
    printf("  RMI$_FRLIST         %12llu    SHOW MEMORY/PHYSICAL \"Free\"\n",
           frlist);
    printf("  RMI$_MODLIST        %12llu    SHOW MEMORY/PHYSICAL \"Modified\"\n",
           modlist);
    printf("  SYI$_PAGEFILE_PAGE  %12llu    SHOW MEMORY/FILES, page files' "
           "\"Total size\" summed\n", pf_page);
    printf("  SYI$_PAGEFILE_FREE  %12llu    SHOW MEMORY/FILES, page files' "
           "\"Free\" summed\n", pf_free);
    printf("  (SHOW MEMORY counts in pages, or pagelets with "
           "/UNITS=PAGELETS)\n");

    printf("\nCPU, share of all CPU time in each mode          "
           "MONITOR MODES; with N CPUs\n"
           "                                                  its figures "
           "sum to N x 100%%\n");
    for (i = 0; i < VO_CPU_MODES; i++) {
        delta[i] = b.cpu_present[i] && a.cpu_present[i] &&
                   b.cpu_ticks[i] >= a.cpu_ticks[i]
                   ? b.cpu_ticks[i] - a.cpu_ticks[i] : 0;
        total += delta[i];
    }
    for (i = 0; i < VO_CPU_MODES; i++) {
        if (!b.cpu_present[i])
            printf("  %-12s not read\n", vo_cpu_mode_name((int) i));
        else if (b.cpu_ticks[i] < a.cpu_ticks[i])
            printf("  %-12s WENT BACKWARDS (%lld): will not be reported\n",
                   vo_cpu_mode_name((int) i),
                   (long long) (b.cpu_ticks[i] - a.cpu_ticks[i]));
        else
            printf("  %-12s %5.1f%%\n", vo_cpu_mode_name((int) i),
                   total ? 100.0 * (double) delta[i] / (double) total : 0.0);
    }
    printf("  ticks per CPU-second: %.1f (100 expected), %u CPUs\n",
           (double) total / 5.0 / (b.cpu_count ? b.cpu_count : 1),
           b.cpu_count);
    if (total == 0)
        ok = 0;

    printf("\nRates over the 5 s, per second                    compare with\n");
    printf("  page faults    %8.1f    MONITOR PAGE \"Page Fault Rate\"\n",
           (double) (vo_u32) (b.faults - a.faults) / 5.0);
    printf("  page reads     %8.1f    MONITOR PAGE \"Page Read Rate\"\n",
           (double) (vo_u32) (b.page_reads - a.page_reads) / 5.0);
    printf("  page read I/O  %8.1f    MONITOR PAGE \"Page Read I/O Rate\"\n",
           (double) (vo_u32) (b.page_read_ios - a.page_read_ios) / 5.0);
    printf("  direct I/O     %8.1f    MONITOR IO \"Direct I/O Rate\"\n",
           (double) (vo_u32) (b.direct_ios - a.direct_ios) / 5.0);
    printf("  buffered I/O   %8.1f    MONITOR IO \"Buffered I/O Rate\"\n",
           (double) (vo_u32) (b.buffered_ios - a.buffered_ios) / 5.0);

    printf("\nProcess states                                    "
           "MONITOR STATES, SHOW SYSTEM\n  ");
    for (i = 0; i < VO_PROC_STATES; i++)
        if (b.proc_states[i] != 0)
            printf("%s %u  ", vo_proc_state_name((int) i), b.proc_states[i]);
    printf("\n");

    printf("\nDisks reported (mounted; no shadow members, no memory disk)\n"
           "  %-20s %-16s %12s %12s %12s   SHOW DEVICE D\n",
           "device", "mount point", "blocks", "free", "operations");
    for (i = 0; i < b.ndisks; i++)
        printf("  %-20s %-16s %12llu %12llu %12u\n", b.disks[i].device,
               b.disks[i].mountpoint, b.disks[i].total_bytes / 512,
               b.disks[i].free_bytes / 512, b.disks[i].ops);
    if (b.ndisks == 0) {
        printf("  none\n");
        ok = 0;
    }

    printf("\n%s\n", ok ? "self-test read everything" :
                          "SELF-TEST INCOMPLETE: see above");
    return ok ? 0 : 1;
}
