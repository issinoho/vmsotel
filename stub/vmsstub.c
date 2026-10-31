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
 *     written as a longword, not a word
 *   - CPU-mode counters as 10 ms ticks of two CPUs, from the real clock
 *   - a wildcard scan with a NOPRIV and a SUSPENDED process in it
 *   - disks: a mounted system disk, a shadow set's virtual unit and one
 *     of its members, an unmounted disk, and an x86-style memory disk
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
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

        switch (e->code) {
        case RMI$_CPUUSER:   v = ticks / 4; quad = 1; break;
        case RMI$_CPUKERNEL: v = ticks / 8; quad = 1; break;
        case RMI$_CPUEXEC:   v = ticks / 16; quad = 1; break;
        case RMI$_CPUIDLE:   v = ticks - ticks / 4 - ticks / 8 - ticks / 16; quad = 1; break;
        case RMI$_CPUINTSTK: case RMI$_CPUMPSYNCH: case RMI$_CPUSUPER:
        case RMI$_CPUCOMPAT: v = 0; quad = 1; break;
        case RMI$_FRLIST:    v = 400000; quad = 1; break;
        case RMI$_MODLIST:   v = 12000; quad = 1; break;
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
        case JPI$_USERNAME: put_text(e, procs[k].user); break;
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
