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
 *   - Memory (SYI$_MEMSIZE, RMI$_FRLIST, RMI$_MODLIST) and the page file
 *     (SYI$_PAGEFILE_PAGE, _FREE) are in pages of SYI$_PAGE_SIZE bytes:
 *     the 2c self-test matched each with SHOW MEMORY/PHYSICAL and
 *     SHOW MEMORY/FILES on both machines (findings 23, 24). PAGEFILE_FREE
 *     is space never written, so "used" is not SHOW MEMORY's "committed".
 *
 * Shadow-set members are skipped by DVI$_SHDW_MEMBER, so a mirrored
 * volume is counted once, as its DSA virtual unit. That item is
 * confirmed accepted on both machines, but neither has a shadow set,
 * so a real member has not been seen. The memory disk, whose logical
 * volume name begins SYS$MD (x86), is skipped too.
 *
 * Built without /POINTER_SIZE: the item lists are item_list_3 only
 * with 32-bit pointers, and vo_source_open() refuses to run otherwise.
 *
 * VAX (OpenVMS V7.3; findings, "VAX"): <rmidef.h> has none of the
 * RMI$_CPU* items, so CPU time by mode comes from RMI$_MODES -- 4
 * reserved bytes, then per CPU a byte of ID and eight longwords:
 * interrupt (which includes idle), MP synch, kernel, exec, super, user,
 * reserved, idle. Those are longwords, so they are widened here, per
 * CPU, idle taken out of interrupt first. VO_RMI_MODES selects this,
 * on by default where RMI$_CPUIDLE is not declared; the Linux build
 * turns it on to run it against the stub. <starlet.h> there has no
 * prototype for sys$getrmi, so one is given. The other RMI$_ items
 * the source reads take a longword there too.
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
#include <jpidef.h>
#include <lnmdef.h>
#include <rmidef.h>
#include <ssdef.h>
#include <starlet.h>
#include <syidef.h>

#include "source.h"

#if !defined(RMI$_CPUIDLE) && !defined(VO_RMI_MODES)
#  define VO_RMI_MODES 1
#endif

#ifdef __VAX
int sys$getrmi(unsigned int efn, unsigned int csidadr, unsigned int nodename,
               void *itmlst, void *iosb, void (*astadr)(void), int astprm);
#endif

/* 100 ns units from 17-NOV-1858 to 1970-01-01: 35067168000000000. */
#define VMS_TO_UNIX vo_q(0x007C9567u, 0x4BEB4000u)
#define NS_PER_S 1000000000u

static vo_u32 le32(const unsigned char *b)
{
    return (vo_u32) b[0] | (vo_u32) b[1] << 8 | (vo_u32) b[2] << 16 |
           (vo_u32) b[3] << 24;
}

static vo_u64 le64(const unsigned char *b)
{
    return vo_q(le32(b + 4), le32(b));
}

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

    memset(b, 0, sizeof b);
    st = syi(code, b, len, &rl);
    *out = le64(b);
    return st;
}

/* The same for an item of up to 4 bytes. */
static unsigned int syi_long(int code, unsigned short len, vo_u32 *out)
{
    vo_u64 v;
    unsigned int st = syi_num(code, len, &v);

    *out = vo_qlo(v);
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
static unsigned int rmi_buf(int code, unsigned char *b, unsigned short len)
{
    unsigned int retlen = 0;
    unsigned short iosb[4] = { 0, 0, 0, 0 };
    ile3 ile[2];
    unsigned int st;

    memset(b, 0, len);
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
    return st;
}

static unsigned int rmi(int code, unsigned short len, vo_u64 *out)
{
    unsigned char b[8];
    unsigned int st = rmi_buf(code, b, len);

    *out = le64(b);
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
    return le32(b);
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
static vo_u32 page_size;
static char host_name[256], host_type[128], os_version[16], scsnode[16];
static const char *host_arch;
static int cluster_member;

/*
 * SYS$TIMEZONE_DIFFERENTIAL from the system table. Not getenv: on x86
 * a process-level definition changed what getenv saw but not what
 * time() returned (docs/findings.md), so getenv would let one stray
 * logical make the clock check below refuse a healthy system.
 */
static int system_tdf(long *tdf, char *err, size_t errcap)
{
    static char table[] = "LNM$SYSTEM_TABLE";
    static char name[] = "SYS$TIMEZONE_DIFFERENTIAL";
    struct dsc$descriptor_s td, nd;
    unsigned int attr = LNM$M_CASE_BLIND;
    char value[64];
    unsigned short len = 0;
    ile3 ile[2];
    unsigned int st;

    make_desc(&td, table, sizeof table - 1);
    make_desc(&nd, name, sizeof name - 1);
    memset(ile, 0, sizeof ile);
    ile[0].len = sizeof value - 1;
    ile[0].code = LNM$_STRING;
    ile[0].buf = value;
    ile[0].retlen = &len;
    st = sys$trnlnm(&attr, (void *) &td, (void *) &nd, 0, (void *) ile);
    if (!(st & 1)) {
        vo_snprintf(err, errcap, "SYS$TIMEZONE_DIFFERENTIAL is not defined "
                 "in LNM$SYSTEM_TABLE (status %%X%08X); without it local "
                 "time cannot be turned into UTC", st);
        return 0;
    }
    value[len] = '\0';
    *tdf = strtol(value, NULL, 10);
    return 1;
}

/* For the log source (vms_logs.c), whose files are in local time. */
int vo_vms_system_tdf(long *tdf, char *err, size_t errcap)
{
    return system_tdf(tdf, err, errcap);
}

/*
 * The startup check from docs/design.md, "Time": time() must agree with
 * $GETTIM less the system's differential. If it does not, the timezone
 * is misconfigured and every point would land hours away from where a
 * collector expects it, silently -- so the agent refuses to start and
 * says why. The two reads are microseconds apart; 2 s allows for
 * time() truncating to the second.
 */
static int clock_check(long tdf, char *err, size_t errcap)
{
    unsigned char t[8];
    vo_u64 now;
    unsigned long from_time, local;
    unsigned int st;
    long d;

    from_time = (unsigned long) time(NULL);
    memset(t, 0, sizeof t);
    st = sys$gettim((void *) t);
    now = le64(t);
    if (!(st & 1) || vo_qcmp(now, VMS_TO_UNIX) <= 0) {
        vo_snprintf(err, errcap, "$GETTIM failed");
        return 0;
    }
    /* Local seconds since 1970, then less the differential. Unsigned
       longs of seconds hold until 2106 where long is 32 bits. */
    local = (unsigned long) vo_qlo(vo_qdiv(vo_qsub(now, VMS_TO_UNIX),
                                           10000000u, NULL));
    local -= (unsigned long) tdf;          /* modulo 2^32: tdf is signed */
    d = from_time >= local ? (long) (from_time - local)
                           : -(long) (local - from_time);
    if (d < -2 || d > 2) {
        vo_snprintf(err, errcap, "clock check failed: time() is %lu, but "
                    "$GETTIM less SYS$TIMEZONE_DIFFERENTIAL (%ld s) is %lu, "
                    "%ld s apart. Every timestamp would be wrong by that "
                    "much; check the differential and the time zone "
                    "(@SYS$MANAGER:UTC$TIME_SETUP)", from_time, tdf,
                    local, d);
        return 0;
    }
    return 1;
}

/*
 * SYI$_BOOTTIME is local time at boot, so it is turned into UTC with
 * the differential in force when the agent starts -- the right one when
 * the agent starts at boot, as SYSTARTUP_VMS.COM will start it. It is
 * read once: a summer-time change while the agent runs must not move
 * the start time, or every counter would look reset. If the agent is
 * started later, across a change since boot, system.uptime is off by
 * the change (docs/design.md, Counters).
 */
static int read_boot(long tdf, char *err, size_t errcap)
{
    vo_u64 bt, unix100, adj;
    unsigned int st = syi_num(SYI$_BOOTTIME, 8, &bt);

    if (!(st & 1) || vo_qcmp(bt, VMS_TO_UNIX) <= 0) {
        vo_snprintf(err, errcap, "SYI$_BOOTTIME: status %%X%08X", st);
        return 0;
    }
    /* Local 100 ns units since 1970, less the differential. */
    unix100 = vo_qsub(bt, VMS_TO_UNIX);
    adj = vo_qmul(vo_q32((vo_u32) (tdf < 0 ? 0ul - (unsigned long) tdf
                                           : (unsigned long) tdf)),
                  10000000u);
    if (tdf < 0)
        unix100 = vo_qadd(unix100, adj);
    else if (vo_qcmp(unix100, adj) > 0)
        unix100 = vo_qsub(unix100, adj);
    else
        unix100 = vo_q32(0);
    boot_ns = vo_qmul(unix100, 100);
    return 1;
}

static const vo_proc_filter *filter;

int vo_source_open(const vo_proc_filter *f, char *err, size_t errcap)
{
    char arch[32];
    vo_u32 v;
    unsigned int st;

    filter = f;
#ifdef __VMS
    if (sizeof(ile3) != 12) {
        vo_snprintf(err, errcap, "item lists need 32-bit pointers; "
                 "build this source without /POINTER_SIZE");
        return 0;
    }
#endif
    st = syi_long(SYI$_PAGE_SIZE, 4, &page_size);
    if (!(st & 1) || page_size == 0) {
        vo_snprintf(err, errcap, "SYI$_PAGE_SIZE: status %%X%08X", st);
        return 0;
    }
    {
        long tdf;

        if (!system_tdf(&tdf, err, errcap) || !clock_check(tdf, err, errcap) ||
            !read_boot(tdf, err, errcap))
            return 0;
    }

    syi_text(SYI$_HW_NAME, host_type, sizeof host_type);
    syi_text(SYI$_VERSION, os_version, sizeof os_version);
    syi_text(SYI$_SCSNODE, scsnode, sizeof scsnode);
    syi_text(SYI$_ARCH_NAME, arch, sizeof arch);
    /* semconv host.arch: x86_64 is amd64; IA64 is ia64. Alpha and VAX
       have no semconv value, so they are named as VMS names them. */
    host_arch = strcmp(arch, "x86_64") == 0 ? "amd64" :
                strcmp(arch, "IA64") == 0 ? "ia64" :
                strcmp(arch, "Alpha") == 0 ? "alpha" :
                strcmp(arch, "VAX") == 0 ? "vax" : NULL;
    cluster_member = (syi_long(SYI$_CLUSTER_MEMBER, 4, &v) & 1) ? v != 0 : -1;

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

#ifdef VO_RMI_MODES
/*
 * CPU time by mode from RMI$_MODES (see the top of the file). Its eight
 * longwords per CPU are in the model's VO_CPU_* order already, with
 * compat in the reserved slot. Each CPU's are widened past 2^32 ticks
 * (497 days) on their own, interrupt less idle, and the totals summed.
 * A CPU that is not active returns zeros; its last totals are kept, so
 * the sums do not fall while it is stopped.
 */
#define MODES_CPUS 32
#define MODES_REC  33

static vo_wide modes_wide[MODES_CPUS][VO_CPU_MODES];
static vo_u64 modes_last[MODES_CPUS][VO_CPU_MODES];

static unsigned int read_modes(vo_raw *r)
{
    static unsigned char b[4 + MODES_CPUS * MODES_REC];
    unsigned int st = rmi_buf(RMI$_MODES, b, (unsigned short) sizeof b);
    int c, m;

    if (!(st & 1))
        return st;
    for (m = 0; m < VO_CPU_MODES; m++) {
        r->cpu_ticks[m] = vo_q32(0);
        r->cpu_present[m] = 1;
    }
    for (c = 0; c < MODES_CPUS; c++) {
        const unsigned char *rec = b + 4 + c * MODES_REC;
        vo_u32 v[VO_CPU_MODES];
        int active = 0;

        for (m = 0; m < VO_CPU_MODES; m++) {
            v[m] = le32(rec + 1 + 4 * m);
            active |= v[m] != 0;
        }
        if (active) {
            v[VO_CPU_INTERRUPT] -= v[VO_CPU_IDLE];
            for (m = 0; m < VO_CPU_MODES; m++)
                modes_last[c][m] = vo_wide_update(&modes_wide[c][m], v[m],
                                                  r->now_ns);
        }
        for (m = 0; m < VO_CPU_MODES; m++)
            r->cpu_ticks[m] = vo_qadd(r->cpu_ticks[m], modes_last[c][m]);
    }
    return st;
}
#else
static const struct { int code; int mode; } cpu_items[VO_CPU_MODES] = {
    { RMI$_CPUINTSTK, VO_CPU_INTERRUPT }, { RMI$_CPUMPSYNCH, VO_CPU_MPSYNCH },
    { RMI$_CPUKERNEL, VO_CPU_KERNEL },    { RMI$_CPUEXEC, VO_CPU_EXEC },
    { RMI$_CPUSUPER, VO_CPU_SUPER },      { RMI$_CPUUSER, VO_CPU_USER },
    { RMI$_CPUCOMPAT, VO_CPU_COMPAT },    { RMI$_CPUIDLE, VO_CPU_IDLE }
};
#endif

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
        (field) = vo_qlo(v_); \
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

static void copy_trim(char *out, size_t cap, const unsigned char *b,
                      unsigned short len)
{
    if (len >= cap)
        len = (unsigned short) (cap - 1);
    while (len > 0 && (b[len - 1] == ' ' || b[len - 1] == '\0'))
        len--;
    memcpy(out, b, len);
    out[len] = '\0';
}

/*
 * The chosen processes (phase 2), by a wildcard $GETJPIW scan with one
 * item list per process, as probe_jpi ran it on both machines. Those
 * items come from the process's control block, not its own context, so
 * no AST is delivered to it; JPI$_IMAGNAME would need one and is left
 * out. Without WORLD only this account's processes are visible: the
 * others come back SS$_NOPRIV and are counted, for the agent to log.
 */
static void read_procs(vo_raw *r)
{
    unsigned char b_pid[4], b_name[16], b_user[12], b_cpu[4], b_pp[4], b_gp[4];
    unsigned short l_pid, l_name, l_user, l_cpu, l_pp, l_gp;
    unsigned int pid = 0xFFFFFFFFu;     /* -1: start a wildcard scan */
    unsigned int errors = 0;
    ile3 ile[7];

    if (filter == NULL || !vo_proc_filter_active(filter))
        return;
    memset(ile, 0, sizeof ile);
    ile[0].len = 4;  ile[0].code = JPI$_PID;      ile[0].buf = b_pid;  ile[0].retlen = &l_pid;
    ile[1].len = 15; ile[1].code = JPI$_PRCNAM;   ile[1].buf = b_name; ile[1].retlen = &l_name;
    ile[2].len = 12; ile[2].code = JPI$_USERNAME; ile[2].buf = b_user; ile[2].retlen = &l_user;
    ile[3].len = 4;  ile[3].code = JPI$_CPUTIM;   ile[3].buf = b_cpu;  ile[3].retlen = &l_cpu;
    ile[4].len = 4;  ile[4].code = JPI$_PPGCNT;   ile[4].buf = b_pp;   ile[4].retlen = &l_pp;
    ile[5].len = 4;  ile[5].code = JPI$_GPGCNT;   ile[5].buf = b_gp;   ile[5].retlen = &l_gp;

    for (;;) {
        unsigned short iosb[4] = { 0, 0, 0, 0 };
        unsigned int st;
        char name[16], user[13];
        vo_raw_proc *p;

        memset(b_name, 0, sizeof b_name);
        memset(b_user, 0, sizeof b_user);
        l_name = l_user = 0;
        st = sys$getjpiw(EFN$C_ENF, &pid, 0, (void *) ile, (void *) iosb, 0, 0);
        if ((st & 1) && !(iosb[0] & 1))
            st = iosb[0];
        if (st == SS$_NOMOREPROC)
            break;
        if (st == SS$_NOPRIV) {
            r->procs_hidden++;
            continue;
        }
        if (!(st & 1)) {                /* gone mid-scan, suspended, ... */
            if (++errors > 100)
                break;
            continue;
        }
        copy_trim(name, sizeof name, b_name, l_name);
        copy_trim(user, sizeof user, b_user, l_user);
        if (!vo_proc_filter_match(filter, name, user))
            continue;
        r->procs_matched++;
        if (r->nprocs == VO_MAX_PROCS)
            continue;
        p = &r->procs[r->nprocs++];
        p->pid = (vo_u32) b_pid[0] | (vo_u32) b_pid[1] << 8 |
                 (vo_u32) b_pid[2] << 16 | (vo_u32) b_pid[3] << 24;
        strcpy(p->name, name);
        strcpy(p->user, user);
        p->cpu_ticks = (vo_u32) b_cpu[0] | (vo_u32) b_cpu[1] << 8 |
                       (vo_u32) b_cpu[2] << 16 | (vo_u32) b_cpu[3] << 24;
        /* Pagelets of 512 bytes, matched with SHOW SYSTEM's pages on
           both machines (findings 11). */
        p->mem_bytes = vo_qmul(vo_qadd(vo_q32(le32(b_pp)), vo_q32(le32(b_gp))),
                               512);
    }
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
        out->total_bytes = vo_qmul(vo_q32(maxblock), 512);
        out->free_bytes = vo_qmul(vo_q32(freeblocks), 512);
        out->ops = ops;
    }
}

int vo_source_read(vo_raw *r, vo_u64 now_ns, char *err, size_t errcap)
{
    const char *what = "";
    unsigned int st;
    vo_u64 v;
    vo_u32 n;
    int i;

    memset(r, 0, sizeof *r);
    r->now_ns = now_ns;
    r->boot_ns = boot_ns;

#ifdef VO_RMI_MODES
    read_modes(r);              /* refused: the modes are left out */
#else
    for (i = 0; i < VO_CPU_MODES; i++) {
        st = rmi(cpu_items[i].code, 8, &r->cpu_ticks[cpu_items[i].mode]);
        r->cpu_present[cpu_items[i].mode] = (st & 1) != 0;
    }
#endif
    st = syi_long(SYI$_ACTIVECPU_CNT, 4, &n);
    if (!(st & 1)) {
        what = "SYI$_ACTIVECPU_CNT";
        goto fail;
    }
    r->cpu_count = n;

    RMI_LONG(RMI$_FAULTS, r->faults);
    RMI_LONG(RMI$_PREADS, r->page_reads);
    RMI_LONG(RMI$_PREADIO, r->page_read_ios);
    RMI_LONG(RMI$_PWRITIO, r->page_write_ios);
    RMI_LONG(RMI$_DIRIO, r->direct_ios);
    RMI_LONG(RMI$_BUFIO, r->buffered_ios);
    for (i = 0; i < VO_PROC_STATES; i++)
        RMI_LONG(state_items[i].code, r->proc_states[state_items[i].state]);

    /* Pages, on both machines (see the top of this file). */
    st = syi_long(SYI$_MEMSIZE, 4, &n);
    if (!(st & 1)) {
        what = "SYI$_MEMSIZE";
        goto fail;
    }
    r->mem_total_bytes = vo_qmul(vo_q32(n), page_size);
    st = rmi(RMI$_FRLIST, 8, &v);
    if (!(st & 1)) {
        what = "RMI$_FRLIST";
        goto fail;
    }
    r->mem_free_bytes = vo_qmul(v, page_size);
    st = rmi(RMI$_MODLIST, 8, &v);
    if (!(st & 1)) {
        what = "RMI$_MODLIST";
        goto fail;
    }
    r->mem_modified_bytes = vo_qmul(v, page_size);
    r->mem_present = 1;

    /* A system with no page file installed is not an error. */
    {
        vo_u32 total = 0, free_pages = 0;

        if ((syi_long(SYI$_PAGEFILE_PAGE, 4, &total) & 1) &&
            (syi_long(SYI$_PAGEFILE_FREE, 4, &free_pages) & 1) && total > 0) {
            r->pagefile_total_bytes = vo_qmul(vo_q32(total), page_size);
            r->pagefile_free_bytes = vo_qmul(vo_q32(free_pages), page_size);
            r->pagefile_present = 1;
        }
    }

    read_disks(r);
    read_procs(r);
    return 1;

fail:
    vo_snprintf(err, errcap, "%s: status %%X%08X", what, st);
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
    char err[400], q1[VO_QSTR_MAX], q2[VO_QSTR_MAX], q3[VO_QSTR_MAX],
         q4[VO_QSTR_MAX], q5[VO_QSTR_MAX];
    vo_u32 memsize = 0, pf_page = 0, pf_free = 0;
    vo_u64 frlist, modlist, total, delta[VO_CPU_MODES];
    size_t i;
    int ok = 1;

    static vo_proc_filter all;

    /* Every process, so the scan can be checked against SHOW SYSTEM. */
    all.nname = 1;
    strcpy(all.name[0], "*");
    if (!vo_source_open(&all, err, sizeof err)) {
        printf("FAIL  open: %s\n", err);
        return 1;
    }
    printf("vmsotel self-test: %s, %s, %s, page size %u\n", scsnode,
           os_version, host_arch ? host_arch : "(unknown arch)", page_size);
    printf("  host.name %s, host.type %s, cluster member %d\n", host_name,
           host_type, cluster_member);
#ifdef VO_RMI_MODES
    printf("  CPU modes from RMI$_MODES\n");
#endif
#ifdef VO_SOFT64
    printf("  64-bit values as two longwords\n");
#endif
    printf("\n");

    if (!vo_source_read(&a, vo_q32(0), err, sizeof err)) {
        printf("FAIL  read: %s\n", err);
        return 1;
    }
    printf("sampling again in 5 s\n\n");
    sleep(5);
    if (!vo_source_read(&b, vo_qmul(vo_q32(5), NS_PER_S), err, sizeof err)) {
        printf("FAIL  read: %s\n", err);
        return 1;
    }

    printf("Boot and uptime                                   compare with\n");
    {
        unsigned long now = (unsigned long) time(NULL);
        unsigned long booted = (unsigned long) vo_qlo(vo_qdiv(boot_ns, NS_PER_S,
                                                              NULL));
        unsigned long up = !vo_qzero(boot_ns) ? now - booted : 0;

        printf("  uptime         %lu s = %lu %02lu:%02lu:%02lu    "
               "SHOW SYSTEM/NOPROCESS \"Uptime\"\n", up, up / 86400,
               (up % 86400) / 3600, (up % 3600) / 60, up % 60);
    }

    printf("\nMemory, in pages of %u bytes                    compare with\n",
           page_size);
    syi_long(SYI$_MEMSIZE, 4, &memsize);
    rmi(RMI$_FRLIST, 8, &frlist);
    rmi(RMI$_MODLIST, 8, &modlist);
    syi_long(SYI$_PAGEFILE_PAGE, 4, &pf_page);
    syi_long(SYI$_PAGEFILE_FREE, 4, &pf_free);
    printf("  SYI$_MEMSIZE        %12u    SHOW MEMORY/PHYSICAL \"Total\"\n",
           memsize);
    printf("  RMI$_FRLIST         %12s    SHOW MEMORY/PHYSICAL \"Free\"\n",
           vo_qstr(frlist, q1));
    printf("  RMI$_MODLIST        %12s    SHOW MEMORY/PHYSICAL \"Modified\"\n",
           vo_qstr(modlist, q1));
    printf("  SYI$_PAGEFILE_PAGE  %12u    SHOW MEMORY/FILES, page files' "
           "\"Total size\" summed\n", pf_page);
    printf("  SYI$_PAGEFILE_FREE  %12u    SHOW MEMORY/FILES, page files' "
           "\"Free\" summed\n", pf_free);
    printf("  reported: total %s, free %s, modified %s bytes; page "
           "file %s of %s free\n", vo_qstr(b.mem_total_bytes, q1),
           vo_qstr(b.mem_free_bytes, q2), vo_qstr(b.mem_modified_bytes, q3),
           vo_qstr(b.pagefile_free_bytes, q4),
           vo_qstr(b.pagefile_total_bytes, q5));

    printf("\nCPU, share of all CPU time in each mode          "
           "MONITOR MODES; with N CPUs\n"
           "                                                  its figures "
           "sum to N x 100%%\n");
    total = vo_q32(0);
    for (i = 0; i < VO_CPU_MODES; i++) {
        delta[i] = b.cpu_present[i] && a.cpu_present[i] &&
                   vo_qcmp(b.cpu_ticks[i], a.cpu_ticks[i]) >= 0
                   ? vo_qsub(b.cpu_ticks[i], a.cpu_ticks[i]) : vo_q32(0);
        total = vo_qadd(total, delta[i]);
    }
    for (i = 0; i < VO_CPU_MODES; i++) {
        if (!b.cpu_present[i])
            printf("  %-12s not read\n", vo_cpu_mode_name((int) i));
        else if (vo_qcmp(b.cpu_ticks[i], a.cpu_ticks[i]) < 0)
            printf("  %-12s WENT BACKWARDS (by %s): will not be reported\n",
                   vo_cpu_mode_name((int) i),
                   vo_qstr(vo_qsub(a.cpu_ticks[i], b.cpu_ticks[i]), q1));
        else
            printf("  %-12s %5.1f%%\n", vo_cpu_mode_name((int) i),
                   !vo_qzero(total) ? 100.0 * vo_qdouble(delta[i]) /
                                      vo_qdouble(total) : 0.0);
    }
    printf("  ticks per CPU-second: %.1f (100 expected), %u CPUs\n",
           vo_qdouble(total) / 5.0 / (b.cpu_count ? b.cpu_count : 1),
           b.cpu_count);
    if (vo_qzero(total))
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
        printf("  %-20s %-16s %12s %12s %12u\n", b.disks[i].device,
               b.disks[i].mountpoint,
               vo_qstr(vo_qdiv(b.disks[i].total_bytes, 512, NULL), q1),
               vo_qstr(vo_qdiv(b.disks[i].free_bytes, 512, NULL), q2),
               b.disks[i].ops);
    if (b.ndisks == 0) {
        printf("  none\n");
        ok = 0;
    }

    printf("\nProcesses: %lu visible, %lu refused (NOPRIV)       "
           "SHOW SYSTEM\n", (unsigned long) b.procs_matched,
           (unsigned long) b.procs_hidden);
    printf("  %-8s %-15s %-12s %14s %10s\n", "PID", "name", "user",
           "CPU (s)", "pages");
    for (i = 0; i < b.nprocs && i < 12; i++)
        printf("  %08X %-15s %-12s %11u.%02u %10s\n", b.procs[i].pid,
               b.procs[i].name, b.procs[i].user, b.procs[i].cpu_ticks / 100,
               b.procs[i].cpu_ticks % 100,
               vo_qstr(vo_qdiv(b.procs[i].mem_bytes, page_size, NULL), q1));
    if (b.nprocs > 12)
        printf("  ... and %lu more\n", (unsigned long) (b.nprocs - 12));
    if (b.nprocs == 0)
        ok = 0;

    printf("\n%s\n", ok ? "self-test read everything" :
                          "SELF-TEST INCOMPLETE: see above");
    return ok ? 0 : 1;
}
