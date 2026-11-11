/*
 * Linux stand-in for the system services the probes call, so that each
 * probe's logic -- item tables, wildcard termination, the retry on
 * BADPARAM, the CPU-tick arithmetic -- runs here before it runs on a
 * machine that costs a round trip. Modelled on tools/gateway/pcapstub
 * in vmsguard. The values are invented; only the behaviour matters:
 *
 *   - a declared item the service refuses (SYI$_SWAPFILE_FREE)
 *   - $GETRMI as both machines showed it: the CPU modes, FRLIST and
 *     MODLIST take a quadword, every other item refuses one with
 *     SS$_BADPARAM and takes a longword, and the return length is
 *     written as a longword, not a word. With STUB_RMI_MEM_LONG set,
 *     FRLIST and MODLIST refuse a quadword too, as on Alpha
 *   - CPU-mode counters as 10 ms ticks of two CPUs, from the real clock
 *   - a wildcard scan with a NOPRIV and a SUSPENDED process in it
 *   - disks: a mounted system disk, a shadow set's virtual unit and one
 *     of its members, an unmounted disk, and an x86-style memory disk
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "descrip.h"
#include "ssdef.h"
#include "starlet.h"
#include "syidef.h"
#include "rmidef.h"
#include "jpidef.h"
#include "dvidef.h"
#include "dvsdef.h"
#include "dcdef.h"
#include "lnmdef.h"
#include "rms.h"
#include "rmsdef.h"

typedef struct {
    unsigned short len, code;
    void *buf;
    unsigned short *retlen;
} ile3;

#define VMS_TO_UNIX 35067168000000000ULL

static unsigned long long now_100ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return VMS_TO_UNIX + (unsigned long long) ts.tv_sec * 10000000ULL +
           (unsigned long long) ts.tv_nsec / 100;
}

static void put_num(const ile3 *e, unsigned long long v)
{
    unsigned i, n = e->len < 8 ? e->len : 8;
    unsigned char *b = e->buf;

    for (i = 0; i < n; i++)
        b[i] = (unsigned char) (v >> (8 * i));
    if (e->retlen)
        *e->retlen = (unsigned short) (n > 4 ? 4 : n);
}

static void put_quad(const ile3 *e, unsigned long long v)
{
    put_num(e, v);
    if (e->retlen && e->len >= 8)
        *e->retlen = 8;
}

static void put_text(const ile3 *e, const char *s)
{
    size_t n = strlen(s);

    if (n > e->len)
        n = e->len;
    memcpy(e->buf, s, n);
    if (e->retlen)
        *e->retlen = (unsigned short) n;
}

static void set_iosb(void *iosb, unsigned short st)
{
    if (iosb)
        ((unsigned short *) iosb)[0] = st;
}

int sys$getmsg(unsigned int msgid, unsigned short *msglen, void *bufadr,
               unsigned int flags, unsigned char *outadr)
{
    struct dsc$descriptor_s *d = bufadr;
    char tmp[64];
    size_t n;

    (void) flags;
    (void) outadr;
    sprintf(tmp, "%%STUB-%c-STATUS, status %u",
            (msgid & 1) ? 'S' : 'F', msgid);
    n = strlen(tmp);
    if (n > d->dsc$w_length)
        n = d->dsc$w_length;
    memcpy(d->dsc$a_pointer, tmp, n);
    *msglen = (unsigned short) n;
    return SS$_NORMAL;
}

int sys$gettim(void *timadr)
{
    unsigned long long t = now_100ns();

    memcpy(timadr, &t, sizeof t);
    return SS$_NORMAL;
}

int sys$synch(unsigned int efn, void *iosb)
{
    (void) efn;
    return iosb ? ((unsigned short *) iosb)[0] : SS$_NORMAL;
}

/* ---- $GETSYI ------------------------------------------------------- */

int sys$getsyiw(unsigned int efn, unsigned int *csidadr, void *nodename,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm)
{
    ile3 *e;
    (void) efn; (void) csidadr; (void) nodename; (void) astadr; (void) astprm;

    for (e = itmlst; e->len != 0 || e->code != 0; e++) {
        switch (e->code) {
        case SYI$_NODENAME:     put_text(e, "STUBBY"); break;
        case SYI$_SCSNODE:      put_text(e, "STUBBY  "); break;
        case SYI$_VERSION:      put_text(e, "V9.2-3  "); break;
        case SYI$_ARCH_NAME:    put_text(e, "x86_64"); break;
        case SYI$_HW_NAME:      put_text(e, "Linux stub"); break;
        case SYI$_PAGE_SIZE:    put_num(e, 8192); break;
        case SYI$_MEMSIZE:      put_num(e, 524288); break;
        case SYI$_ACTIVECPU_CNT: put_num(e, 2); break;
        case SYI$_AVAILCPU_CNT: put_num(e, 2); break;
        case SYI$_BOOTTIME:     put_quad(e, now_100ns() - 86400ULL * 3 * 10000000ULL); break;
        case SYI$_PAGEFILE_PAGE: put_num(e, 233216); break;
        case SYI$_PAGEFILE_FREE: put_num(e, 172688); break;
        case SYI$_SWAPFILE_FREE:
            set_iosb(iosb, SS$_BADPARAM);
            return SS$_BADPARAM;
        default:                put_num(e, 7); break;
        }
    }
    set_iosb(iosb, SS$_NORMAL);
    return SS$_NORMAL;
}

/* ---- $GETRMI ------------------------------------------------------- */

int sys$getrmi(unsigned int efn, unsigned int csidadr, unsigned int nodename,
               void *itmlst, void *iosb, void (*astadr)(void), int astprm)
{
    /* Two CPUs' worth of 10 ms ticks since an arbitrary epoch. */
    unsigned long long ticks = (now_100ns() - VMS_TO_UNIX) / 100000ULL * 2;
    ile3 *e;
    (void) efn; (void) csidadr; (void) nodename; (void) astadr; (void) astprm;

    for (e = itmlst; e->len != 0 || e->code != 0; e++) {
        unsigned long long v;
        int quad = 0;
        unsigned i, n;

        /* The documented layout: 4 reserved bytes, then per CPU a
           byte of ID and eight longwords, interrupt counting idle too.
           The values are invented. */
        if (e->code == RMI$_MODES) {
            unsigned char m[4 + 2 * 33];
            unsigned long long c = ticks / 2;
            unsigned int mode[8];
            unsigned cpu, k;

            memset(mode, 0, sizeof mode);
            mode[2] = (unsigned int) (c / 8);
            mode[3] = (unsigned int) (c / 16);
            mode[5] = (unsigned int) (c / 4);
            mode[7] = (unsigned int) (c - c / 4 - c / 8 - c / 16);
            mode[0] = mode[7];          /* interrupt includes idle */
            memset(m, 0, sizeof m);
            m[0] = 2;
            for (cpu = 0; cpu < 2; cpu++) {
                m[4 + cpu * 33] = (unsigned char) cpu;
                for (k = 0; k < 8; k++)
                    for (i = 0; i < 4; i++)
                        m[4 + cpu * 33 + 1 + 4 * k + i] =
                            (unsigned char) (mode[k] >> (8 * i));
            }
            n = e->len < sizeof m ? e->len : (unsigned) sizeof m;
            memcpy(e->buf, m, n);
            if (e->retlen)
                *(unsigned int *) (void *) e->retlen = n;
            continue;
        }
        switch (e->code) {
        case RMI$_CPUUSER:   v = ticks / 4; quad = 1; break;
        case RMI$_CPUKERNEL: v = ticks / 8; quad = 1; break;
        case RMI$_CPUEXEC:   v = ticks / 16; quad = 1; break;
        case RMI$_CPUIDLE:   v = ticks - ticks / 4 - ticks / 8 - ticks / 16; quad = 1; break;
        case RMI$_CPUINTSTK: case RMI$_CPUMPSYNCH: case RMI$_CPUSUPER:
        case RMI$_CPUCOMPAT: v = 0; quad = 1; break;
        case RMI$_FRLIST:    v = 400000; quad = !getenv("STUB_RMI_MEM_LONG"); break;
        case RMI$_MODLIST:   v = 12000; quad = !getenv("STUB_RMI_MEM_LONG"); break;
        case RMI$_FAULTS:    v = 53843179; break;
        case RMI$_PREADS:    v = 12801905; break;
        case RMI$_PREADIO:   v = 4839764; break;
        case RMI$_DIRIO:     v = 19824771; break;
        case RMI$_BUFIO:     v = 645065712; break;
        case RMI$_PROCS:     v = 39; break;
        case RMI$_LEF:       v = 13; break;
        case RMI$_HIB:       v = 26; break;
        case RMI$_CUR:       v = 1; break;
        default:             v = 0; break;
        }
        if (!quad && e->len != 4) {
            set_iosb(iosb, SS$_BADPARAM);
            return SS$_BADPARAM;
        }
        n = e->len < 8 ? e->len : 8;
        for (i = 0; i < n; i++)
            ((unsigned char *) e->buf)[i] = (unsigned char) (v >> (8 * i));
        /* A longword, as on both machines -- wider than item_list_3's
           word, which is what crashed probe_rmi on x86. */
        if (e->retlen)
            *(unsigned int *) (void *) e->retlen = n;
    }
    set_iosb(iosb, SS$_NORMAL);
    return SS$_NORMAL;
}

/* ---- $GETJPI ------------------------------------------------------- */

static const struct {
    unsigned int pid;
    const char *name, *user;
    unsigned int pages, cpu, status;
} procs[] = {
    { 0x00000401, "SWAPPER", "SYSTEM", 0, 10, SS$_NORMAL },
    { 0x00000402, "AUDIT_SERVER", "AUDIT$SERVER", 2400, 900, SS$_NOPRIV },
    { 0x00000403, "OPCOM", "SYSTEM", 1800, 120, SS$_NORMAL },
    { 0x00000404, "BATCH_17", "IAIN", 9000, 50000, SS$_SUSPENDED },
    { 0x00000405, "_FTA3:", "IAIN", 4100, 340, SS$_NORMAL },
};
#define NPROCS (sizeof procs / sizeof procs[0])

/* 0: our own persona; else the one assumed (personas, below). */
static unsigned int stub_persona;

int sys$getjpiw(unsigned int efn, unsigned int *pidadr, void *prcnam,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm)
{
    static size_t cursor = 0;
    size_t k;
    ile3 *e;
    (void) efn; (void) prcnam; (void) astadr; (void) astprm;

    if (pidadr == NULL) {
        k = NPROCS - 1;                 /* "this process" */
    } else {
        if (*pidadr == 0xFFFFFFFFu)
            cursor = 0;
        if (cursor >= NPROCS) {
            set_iosb(iosb, SS$_NOMOREPROC);
            return SS$_NOMOREPROC;
        }
        k = cursor++;
        *pidadr = procs[k].pid;         /* the context a real scan keeps */
        if (procs[k].status != SS$_NORMAL) {
            set_iosb(iosb, (unsigned short) procs[k].status);
            return (int) procs[k].status;
        }
    }
    for (e = itmlst; e->len != 0 || e->code != 0; e++) {
        switch (e->code) {
        case JPI$_PID:      put_num(e, procs[k].pid); break;
        case JPI$_PRCNAM:   put_text(e, procs[k].name); break;
        case JPI$_USERNAME:
            /* This process, under an assumed persona, is its account. */
            put_text(e, pidadr == NULL && stub_persona != 0
                        ? "VMSOTEL" : procs[k].user);
            break;
        case JPI$_CPUTIM:   put_num(e, procs[k].cpu); break;
        case JPI$_PPGCNT:   put_num(e, procs[k].pages); break;
        case JPI$_GPGCNT:   put_num(e, procs[k].pages / 4); break;
        case JPI$_STATE:    put_num(e, 14); break;
        case JPI$_LOGINTIM: put_quad(e, now_100ns() - 36000000000ULL); break;
        case JPI$_IMAGNAME: put_text(e, "STUB$DISK:[SYS0.SYSCOMMON.][SYSEXE]PROBE.EXE"); break;
        default:            put_num(e, 3); break;
        }
    }
    set_iosb(iosb, SS$_NORMAL);
    return SS$_NORMAL;
}

/* ---- $TRNLNM ------------------------------------------------------- */

/*
 * Only SYS$TIMEZONE_DIFFERENTIAL, from the environment variable
 * STUB_TDF (default "0"). The stub's $GETTIM is the real UTC read as
 * local time, so any STUB_TDF but 0 makes time() and $GETTIM minus the
 * differential disagree -- the misconfiguration the agent's startup
 * check refuses. STUB_TDF=none makes the logical undefined.
 */
int sys$trnlnm(unsigned int *attr, void *tabnam, void *lognam,
               unsigned char *acmode, void *itmlst)
{
    struct dsc$descriptor_s *n = lognam;
    const char *v = getenv("STUB_TDF");
    ile3 *e = itmlst;
    (void) attr; (void) tabnam; (void) acmode;

    if (n->dsc$w_length != 25 ||
        memcmp(n->dsc$a_pointer, "SYS$TIMEZONE_DIFFERENTIAL", 25) != 0 ||
        (v != NULL && strcmp(v, "none") == 0))
        return SS$_NOLOGNAM;
    for (; e->len != 0 || e->code != 0; e++)
        if (e->code == LNM$_STRING)
            put_text(e, v != NULL ? v : "0");
    return SS$_NORMAL;
}

/* ---- $DEVICE_SCAN and $GETDVI --------------------------------------- */

static const char *const disk_names[] = {
    "_STUBBY$DKA0:",        /* mounted system disk */
    "_STUBBY$DKA100:",      /* a shadow-set member */
    "_STUBBY$DQA0:",        /* not mounted */
    "_STUBBY$LDM9:",        /* the x86 memory disk, SYS$MD... */
    "_DSA1:"                /* the shadow set's virtual unit */
};
#define NDISKS (sizeof disk_names / sizeof disk_names[0])

int sys$device_scan(void *return_devnam, unsigned short *retlen,
                    void *search_devnam, void *itmlst, void *contxt)
{
    struct dsc$descriptor_s *d = return_devnam;
    unsigned int *ctx = contxt;
    ile3 *e = itmlst;
    size_t n;
    (void) search_devnam;

    if (e == NULL || e->code != DVS$_DEVCLASS ||
        *(unsigned int *) e->buf != DC$_DISK)
        return SS$_BADPARAM;
    if (ctx[0] >= NDISKS)
        return SS$_NOMOREDEV;
    n = strlen(disk_names[ctx[0]]);
    memcpy(d->dsc$a_pointer, disk_names[ctx[0]], n);
    *retlen = (unsigned short) n;
    ctx[0]++;
    return SS$_NORMAL;
}

int sys$getdviw(unsigned int efn, unsigned short chan, void *devnam,
                void *itmlst, void *iosb, void (*astadr)(void), int astprm,
                void *nullarg)
{
    struct dsc$descriptor_s *d = devnam;
    char name[65];
    size_t n = d->dsc$w_length < 64 ? d->dsc$w_length : 64;
    int unmounted, member, memdisk, vu;
    static unsigned int opcnt = 1000;
    ile3 *e;
    (void) efn; (void) chan; (void) astadr; (void) astprm; (void) nullarg;

    memcpy(name, d->dsc$a_pointer, n);
    name[n] = '\0';
    unmounted = strstr(name, "DQA0") != NULL;
    member = strstr(name, "DKA100") != NULL;
    memdisk = strstr(name, "LDM9") != NULL;
    vu = strstr(name, "DSA1") != NULL;

    for (e = itmlst; e->len != 0 || e->code != 0; e++) {
        if (unmounted && (e->code == DVI$_FREEBLOCKS || e->code == DVI$_VOLNAM)) {
            set_iosb(iosb, SS$_DEVNOTMOUNT);
            return SS$_DEVNOTMOUNT;
        }
        switch (e->code) {
        case DVI$_MNT:        put_num(e, !unmounted); break;
        case DVI$_VOLNAM:     put_text(e, memdisk ? "MD2627349A5V" : vu ? "SHADOWED    " : "STUBSYS     "); break;
        case DVI$_LOGVOLNAM:
            if (unmounted)
                put_text(e, "");
            else
                put_text(e, memdisk ? "SYS$MD26273232629V" :
                            vu || member ? "DISK$SHADOWED" : "DISK$STUBSYS");
            break;
        case DVI$_SHDW_MEMBER: put_num(e, member); break;
        case DVI$_SHDW_MASTER: put_num(e, vu); break;
        case DVI$_MAXBLOCK:   put_num(e, memdisk ? 169392 : 41943040); break;
        case DVI$_FREEBLOCKS: put_num(e, memdisk ? 18907 : vu ? 20000000 : 12582912); break;
        case DVI$_OPCNT:      put_num(e, opcnt += 37); break;
        default:              put_num(e, 0); break;
        }
    }
    set_iosb(iosb, SS$_NORMAL);
    return SS$_NORMAL;
}

/* ---- RMS: the operator log and the audit journal -------------------- */

/*
 * Two files under build/, made on first open: an operator log in text
 * records, as OPCOM writes it, and an audit journal of length-prefixed
 * binary records. New lines appended to the log are seen by a stream
 * already at EOF, as a tail would see them -- which VMS does is what
 * probe_logs asks. STUB_NO_TAIL=1 makes a stream that reached EOF stay
 * there until reopened, so probe_logs's other branch runs too.
 * A record's RFA is its byte offset; $GET with RAB$C_RFA seeks there.
 * STUB_BAD_RFA=1 makes that land one record late, so probe_logs's
 * check that it found the same record can be seen to fail.
 */
struct FAB cc$rms_fab;
struct RAB cc$rms_rab;
struct NAM cc$rms_nam;

#define STUB_OPLOG   "build/stub_operator.log"
#define STUB_JOURNAL "build/stub_audit.journal"

static void make_samples(void)
{
    FILE *f;

    if ((f = fopen(STUB_OPLOG, "r")) != NULL)
        fclose(f);
    else if ((f = fopen(STUB_OPLOG, "w")) != NULL) {
        fputs("%%%%%%%%%%%  OPCOM  31-OCT-2026 20:00:00.00  %%%%%%%%%%%\n"
              "Logfile has been initialized by operator _STUBBY$OPA0:\n"
              "Logfile is STUBBY::SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;1\n"
              "%%%%%%%%%%%  OPCOM  31-OCT-2026 20:05:00.00  %%%%%%%%%%%\n"
              "Message from user SYSTEM on STUBBY\n"
              "%SYSTEM-I-STUB, a stub message\n", f);
        fclose(f);
    }
    if ((f = fopen(STUB_JOURNAL, "rb")) != NULL)
        fclose(f);
    else if ((f = fopen(STUB_JOURNAL, "wb")) != NULL) {
        /* NSAMSGDEF as x86's journal showed it: a LOGFAIL (type 5,
           subtype 4, flags 1c), three packets of word size (with its
           own 4 bytes), word type, data; 56 bytes in all. */
        static const unsigned char r1[] = {
            0x05, 0x00, 0x04, 0x00, 0x1c, 0x00, 0x03, 0x00,
            0x38, 0x00, 0x00, 0x03, 0, 0, 0, 0, 0, 0, 0x00, 0x00,
            0x0c, 0x00, 0x06, 0x00, 'S', 'E', 'C', 'U', 'R', 'I', 'T', 'Y',
            0x0c, 0x00, 0x03, 0x00, 'S', 'E', 'C', 'U', 'R', 'I', 'T', 'Y',
            0x0c, 0x00, 0x31, 0x00, 'F', 'T', 'A', '6', '5', '7', '4', ':' };
        int i;

        for (i = 0; i < 3; i++) {
            unsigned char r[sizeof r1];

            memcpy(r, r1, sizeof r);
            /* STUB_BAD_AUDIT: the last record claims a size it lacks. */
            if (i == 2 && getenv("STUB_BAD_AUDIT"))
                r[8] = 0x40;
            fputc((int) sizeof r, f);
            fputc(0, f);
            fwrite(r, 1, sizeof r, f);
        }
        fclose(f);
    }
}

/*
 * Versions: ;1 of each file is its base name under build/, ;N for a
 * higher N the base name with ".N" after it. A name without a version
 * is the highest that exists, as on VMS.
 */
static void version_path(char *out, const char *base, int v)
{
    if (v <= 1)
        strcpy(out, base);
    else
        sprintf(out, "%s.%d", base, v);
}

static int highest_version(const char *base)
{
    char path[300];
    FILE *f;
    int v, hi = 0;

    for (v = 1; v <= 64; v++) {
        version_path(path, base, v);
        if ((f = fopen(path, "r")) != NULL) {
            fclose(f);
            hi = v;
        }
    }
    return hi;
}

int sys$open(void *fabp)
{
    struct FAB *fab = fabp;
    char name[256], path[300];
    const char *base, *shown, *semi;
    FILE *fp;
    int v;

    memcpy(name, fab->fab$l_fna, fab->fab$b_fns);
    name[fab->fab$b_fns] = '\0';
    make_samples();
    if (strstr(name, "OPERATOR.LOG") != NULL) {
        fab->stub_kind = 1;
        base = STUB_OPLOG;
        shown = "STUBBY$DKA0:[SYS0.SYSMGR]OPERATOR.LOG";
    } else if (strstr(name, "AUDIT$JOURNAL") != NULL) {
        fab->stub_kind = 2;
        base = STUB_JOURNAL;
        shown = "STUBBY$DKA0:[SYS0.SYSCOMMON.SYSMGR]SECURITY.AUDIT$JOURNAL";
    } else {
        return RMS$_FNF;
    }
    semi = strchr(name, ';');
    v = semi != NULL ? atoi(semi + 1) : highest_version(base);
    if (v < 1)
        return RMS$_FNF;
    version_path(path, base, v);
    fp = fopen(path, fab->stub_kind == 1 ? "r" : "rb");
    if (fp == NULL)
        return RMS$_FNF;
    fab->fab$b_rfm = FAB$C_VAR;
    fab->fab$w_mrs = 0;
    fab->stub_fp = fp;
    if (fab->fab$l_nam != NULL && fab->fab$l_nam->nam$l_rsa != NULL) {
        char rsa[300];
        size_t n;

        sprintf(rsa, "%s;%d", shown, v);
        n = strlen(rsa);
        if (n > fab->fab$l_nam->nam$b_rss)
            n = fab->fab$l_nam->nam$b_rss;
        memcpy(fab->fab$l_nam->nam$l_rsa, rsa, n);
        fab->fab$l_nam->nam$b_rsl = (unsigned char) n;
    }
    return RMS$_NORMAL;
}

int sys$connect(void *rabp)
{
    (void) rabp;
    return RMS$_NORMAL;
}

int sys$get(void *rabp)
{
    struct RAB *rab = rabp;
    FILE *fp = rab->rab$l_fab->stub_fp;
    char line[1024];
    size_t n;
    int lo, hi;
    long at;

    if (rab->rab$b_rac == RAB$C_RFA) {
        at = (long) rab->rab$w_rfa[0] | (long) rab->rab$w_rfa[1] << 16;
        clearerr(fp);
        if (fseek(fp, at, SEEK_SET) != 0)
            return RMS$_RNF;
        if (getenv("STUB_BAD_RFA")) {
            rab->rab$b_rac = RAB$C_SEQ;
            sys$get(rab);
            rab->rab$b_rac = RAB$C_RFA;
        }
    }
    at = ftell(fp);
    if (rab->rab$l_fab->stub_kind == 1) {
        if (rab->rab$l_fab->stub_kind == 1 && getenv("STUB_NO_TAIL") &&
            feof(fp))
            return RMS$_EOF;
        if (fgets(line, sizeof line, fp) == NULL) {
            if (!getenv("STUB_NO_TAIL"))
                clearerr(fp);
            return RMS$_EOF;
        }
        n = strlen(line);
        if (n > 0 && line[n - 1] == '\n')
            n--;
    } else {
        if ((lo = fgetc(fp)) == EOF || (hi = fgetc(fp)) == EOF) {
            clearerr(fp);
            return RMS$_EOF;
        }
        n = (size_t) (lo | hi << 8);
        if (n > sizeof line || fread(line, 1, n, fp) != n)
            return RMS$_EOF;
    }
    if (n > rab->rab$w_usz)
        n = rab->rab$w_usz;
    memcpy(rab->rab$l_ubf, line, n);
    rab->rab$w_rsz = (unsigned short) n;
    rab->rab$l_rbf = rab->rab$l_ubf;
    rab->rab$w_rfa[0] = (unsigned short) (at & 0xffff);
    rab->rab$w_rfa[1] = (unsigned short) (at >> 16 & 0xffff);
    rab->rab$w_rfa[2] = 0;
    return RMS$_NORMAL;
}

int sys$close(void *fabp)
{
    struct FAB *fab = fabp;

    if (fab->stub_fp)
        fclose(fab->stub_fp);
    fab->stub_fp = NULL;
    return RMS$_NORMAL;
}

void probe_stub_request(const char *text)
{
    char path[300];
    FILE *f;

    version_path(path, STUB_OPLOG, highest_version(STUB_OPLOG));
    f = fopen(path, "a");

    if (f == NULL)
        return;
    fprintf(f, "%%%%%%%%%%%%%%%%%%%%%%  OPCOM  31-OCT-2026 21:00:00.00  "
               "%%%%%%%%%%%%%%%%%%%%%%\nRequest 1, from user IAIN on STUBBY\n"
               "\"%s\"\n", text);
    fclose(f);
}

/* ---- personas and $CREPRC, for the launcher ----
   stub_persona is declared above $GETJPI, which reports the persona's
   username while one is assumed. STUB_PERSONA_IGNORED makes assuming
   take on nothing, as VAX's $PERSONA_ASSUME does with flags 0
   (findings V28). */

static void stub_dsc(char *out, size_t cap, void *d)
{
    struct dsc$descriptor_s *s = d;
    size_t n = s->dsc$w_length < cap - 1 ? s->dsc$w_length : cap - 1;

    memcpy(out, s->dsc$a_pointer, n);
    out[n] = '\0';
}

int sys$persona_create(unsigned int *persona, void *usrnam,
                       unsigned int flags, void *usrpro, void *itmlst)
{
    char u[64];

    (void) flags, (void) usrpro, (void) itmlst;
    stub_dsc(u, sizeof u, usrnam);
    if (strcmp(u, "VMSOTEL") != 0)
        return SS$_NOSUCHUSER;
    *persona = 7;
    return SS$_NORMAL;
}

int sys$persona_assume(unsigned int *persona, unsigned int flags,
                       void *retpersona, void *itmlst)
{
    (void) flags, (void) retpersona, (void) itmlst;
    if (getenv("STUB_PERSONA_IGNORED") != NULL)
        return SS$_NORMAL;
    stub_persona = *persona == 1 ? 0 : *persona;
    return SS$_NORMAL;
}

int sys$persona_delete(unsigned int *persona)
{
    if (*persona == stub_persona)
        return SS$_BADPARAM;        /* VMS refuses to delete the one in use */
    *persona = 0;
    return SS$_NORMAL;
}

int sys$creprc(unsigned int *pidadr, void *image, void *input,
               void *output, void *error, void *prvadr, void *quota,
               void *prcnam, unsigned int baspri, unsigned int uic,
               unsigned short mbxunt, unsigned int stsflg, void *itmlst,
               void *node)
{
    char im[300], in[300], out[300], err[300], name[64];
    const unsigned char *q = quota;
    size_t i;

    (void) prvadr, (void) uic, (void) mbxunt, (void) itmlst, (void) node;
    stub_dsc(im, sizeof im, image);
    stub_dsc(in, sizeof in, input);
    stub_dsc(out, sizeof out, output);
    stub_dsc(err, sizeof err, error);
    stub_dsc(name, sizeof name, prcnam);
    printf("stub: $CREPRC %s as persona %u, input %s, output %s, error %s, "
           "name %s, priority %u, flags %#x, quotas", im, stub_persona, in,
           out, err, name, baspri, stsflg);
    for (i = 0; q != NULL && q[i] != 0; i += 5)
        printf(" %u=%lu", q[i], (unsigned long) q[i + 1] |
               (unsigned long) q[i + 2] << 8 | (unsigned long) q[i + 3] << 16 |
               (unsigned long) q[i + 4] << 24);
    printf("\n");
    if (getenv("STUB_CREPRC_FAIL"))
        return SS$_NOPRIV;
    *pidadr = 0x2001234;
    return SS$_NORMAL;
}
