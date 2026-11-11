/*
 * vmsotel: a source with scripted values, so the agent runs end to end
 * on Linux. The numbers move the way the x86 machine's did, with three
 * things arranged to happen early, so a short run exercises them:
 *   - buffered I/O starts just under 2^32 and wraps on the third reading
 *   - the interrupt-mode CPU counter falls, as x86's RMI$_CPUINTSTK does,
 *     and the model drops it on the second reading
 *   - a disk is mounted from the fourth reading on
 */

#include <stdio.h>
#include <string.h>

#include "source.h"

static vo_u64 opened_ns;
static unsigned reads;
static const vo_proc_filter *filter;

int vo_source_open(const vo_proc_filter *f, char *err, size_t errcap)
{
    (void) err;
    (void) errcap;
    opened_ns = vo_q32(0);
    reads = 0;
    filter = f;
    return 1;
}

/*
 * Processes, shaped like x86's SHOW SYSTEM. LTPHP_19001 ends after the
 * second reading and LTPHP_19004 starts with the third, so a short run
 * sees one process go and another come.
 */
static void processes(vo_raw *r, vo_u32 k)
{
    static const struct {
        vo_u32 pid;
        const char *name, *user;
        vo_u32 cpu, pagelets;
        int from, until;            /* readings it exists for */
    } t[] = {
        { 0x0000040B, "OPCOM", "SYSTEM", 1479, 3040, 0, 1000 },
        { 0x00000438, "MARIADBD_3306", "MARIADB", 290331, 209664, 0, 1000 },
        { 0x00000435, "LTPHP_19000", "LIGHTTPD", 7805290, 62464, 0, 1000 },
        { 0x00000436, "LTPHP_19001", "LIGHTTPD", 1408017, 62336, 0, 2 },
        { 0x00000500, "LTPHP_19004", "LIGHTTPD", 10, 30000, 2, 1000 },
        { 0x0000068D, "BESZEL_AGENT", "BESZEL", 142210, 20976, 0, 1000 }
    };
    size_t i;

    if (filter == NULL || !vo_proc_filter_active(filter))
        return;
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        vo_raw_proc *p;

        if ((int) k < t[i].from || (int) k >= t[i].until ||
            !vo_proc_filter_match(filter, t[i].name, t[i].user))
            continue;
        r->procs_matched++;
        if (r->nprocs == VO_MAX_PROCS)
            continue;
        p = &r->procs[r->nprocs++];
        p->pid = t[i].pid;
        strcpy(p->name, t[i].name);
        strcpy(p->user, t[i].user);
        p->cpu_ticks = t[i].cpu + k * 37;
        p->mem_bytes = vo_qmul(vo_q32(t[i].pagelets), 512);
    }
}

static void disk(vo_raw_disk *d, const char *dev, const char *mnt,
                 vo_u32 blocks, vo_u32 free_blocks, vo_u32 ops)
{
    strncpy(d->device, dev, VO_NAME_MAX);
    d->device[VO_NAME_MAX] = '\0';
    strncpy(d->mountpoint, mnt, VO_NAME_MAX);
    d->mountpoint[VO_NAME_MAX] = '\0';
    d->total_bytes = vo_qmul(vo_q32(blocks), 512);
    d->free_bytes = vo_qmul(vo_q32(free_blocks), 512);
    d->ops = ops;
}

int vo_source_read(vo_raw *r, vo_u64 now_ns, char *err, size_t errcap)
{
    vo_u64 e;           /* ticks since the source opened */
    vo_u32 k;
    int i;

    (void) err;
    (void) errcap;
    if (vo_qzero(opened_ns))
        opened_ns = now_ns;
    k = reads++;
    /* Two CPUs' worth of 10 ms ticks since the source opened. */
    e = vo_qmul(vo_qdiv(vo_qsub(now_ns, opened_ns), 10000000u, NULL), 2);

    memset(r, 0, sizeof *r);
    r->now_ns = now_ns;
    /* A day ago: 86400 x 10^9 ns. */
    r->boot_ns = vo_qsub(opened_ns, vo_qmul(vo_q32(86400u), 1000000000u));

    for (i = 0; i < VO_CPU_MODES; i++)
        r->cpu_present[i] = 1;
    r->cpu_ticks[VO_CPU_INTERRUPT] = vo_qsub(vo_q(0xFFFFFFFFu, 0xFFD00000u),
                                             vo_q32(k * 14u));
    r->cpu_ticks[VO_CPU_MPSYNCH] = vo_q32(87746);
    r->cpu_ticks[VO_CPU_KERNEL] = vo_qadd(vo_q32(25332517),
                                          vo_qdiv(e, 5, NULL));
    r->cpu_ticks[VO_CPU_EXEC] = vo_qadd(vo_q32(1099358), vo_qdiv(e, 50, NULL));
    r->cpu_ticks[VO_CPU_SUPER] = vo_q32(124012);
    r->cpu_ticks[VO_CPU_USER] = vo_qadd(vo_q32(1871610), vo_qdiv(e, 100, NULL));
    r->cpu_ticks[VO_CPU_COMPAT] = vo_q32(0);
    r->cpu_ticks[VO_CPU_IDLE] =
        vo_qsub(vo_qsub(vo_qsub(vo_qadd(vo_q32(111328837), e),
                                vo_qdiv(e, 5, NULL)),
                        vo_qdiv(e, 50, NULL)),
                vo_qdiv(e, 100, NULL));
    r->cpu_count = 2;

    r->mem_present = 1;
    r->mem_total_bytes = vo_qmul(vo_q32(930264u), 8192);
    r->mem_free_bytes = vo_qmul(vo_q32(699303u - (k % 7) * 3), 8192);
    r->mem_modified_bytes = vo_qmul(vo_q32(24632u), 8192);

    r->faults = 53843179u + k * 13u;
    r->page_reads = 12801905u + k;
    r->page_read_ios = 4839764u + k;
    r->page_write_ios = 12;
    r->direct_ios = 19824771u + k * 38u;
    r->buffered_ios = 0xFFFFF000u + k * 2048u;      /* wraps at k = 2 */

    r->pagefile_present = 1;
    r->pagefile_total_bytes = vo_qmul(vo_q32(233216u), 512);
    r->pagefile_free_bytes = vo_qmul(vo_q32(233216u), 512);

    r->proc_states[VO_PS_CUR] = 1;
    r->proc_states[VO_PS_LEF] = 13 - (k % 2);
    r->proc_states[VO_PS_HIB] = 26 + (k % 2);

    r->ndisks = 2;
    disk(&r->disks[0], "FAKE$DKA0", "DISK$FAKESYS", 45000000,
         20011552 - k * 64, 6460564 + k * 152);
    disk(&r->disks[1], "FAKE$DKA200", "DISK$TOOLS", 1974510, 1133328,
         33713 + k);
    if (k >= 3) {
        r->ndisks = 3;
        disk(&r->disks[2], "FAKE$DKA300", "DISK$DUMP", 33554432, 7336282,
             158705019 + k * 64);
    }
    processes(r, k);
    return 1;
}

void vo_source_resource(vo_resource *res)
{
    res->host_name = "fake-vms.example";
    res->host_arch = "amd64";
    res->host_type = "vmsotel fake source";
    res->os_version = "V0.0-FAKE";
    res->scsnode = "FAKEVMS";
    res->cluster_member = 0;
}

void vo_source_close(void)
{
}

int vo_source_selftest(void)
{
    printf("the fake source has nothing to compare: it makes its values "
           "up\n");
    return 0;
}
