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

int vo_source_open(char *err, size_t errcap)
{
    (void) err;
    (void) errcap;
    opened_ns = 0;
    reads = 0;
    return 1;
}

static void disk(vo_raw_disk *d, const char *dev, const char *mnt,
                 vo_u64 blocks, vo_u64 free_blocks, vo_u32 ops)
{
    strncpy(d->device, dev, VO_NAME_MAX);
    d->device[VO_NAME_MAX] = '\0';
    strncpy(d->mountpoint, mnt, VO_NAME_MAX);
    d->mountpoint[VO_NAME_MAX] = '\0';
    d->total_bytes = blocks * 512;
    d->free_bytes = free_blocks * 512;
    d->ops = ops;
}

int vo_source_read(vo_raw *r, vo_u64 now_ns, char *err, size_t errcap)
{
    vo_u64 elapsed_ticks;
    vo_u32 k;
    int i;

    (void) err;
    (void) errcap;
    if (opened_ns == 0)
        opened_ns = now_ns;
    k = reads++;
    /* Two CPUs' worth of 10 ms ticks since the source opened. */
    elapsed_ticks = (now_ns - opened_ns) / 10000000ULL * 2;

    memset(r, 0, sizeof *r);
    r->now_ns = now_ns;
    r->boot_ns = opened_ns - 86400ULL * 1000000000ULL;     /* a day ago */

    for (i = 0; i < VO_CPU_MODES; i++)
        r->cpu_present[i] = 1;
    r->cpu_ticks[VO_CPU_INTERRUPT] = 0xFFFFFFFFFFD00000ULL - k * 14ULL;
    r->cpu_ticks[VO_CPU_MPSYNCH] = 87746;
    r->cpu_ticks[VO_CPU_KERNEL] = 25332517 + elapsed_ticks / 5;
    r->cpu_ticks[VO_CPU_EXEC] = 1099358 + elapsed_ticks / 50;
    r->cpu_ticks[VO_CPU_SUPER] = 124012;
    r->cpu_ticks[VO_CPU_USER] = 1871610 + elapsed_ticks / 100;
    r->cpu_ticks[VO_CPU_COMPAT] = 0;
    r->cpu_ticks[VO_CPU_IDLE] = 111328837 + elapsed_ticks
                                - elapsed_ticks / 5 - elapsed_ticks / 50
                                - elapsed_ticks / 100;
    r->cpu_count = 2;

    r->mem_present = 1;
    r->mem_total_bytes = 930264ULL * 8192;
    r->mem_free_bytes = (699303ULL - (k % 7) * 3) * 8192;
    r->mem_modified_bytes = 24632ULL * 8192;

    r->faults = 53843179u + k * 13u;
    r->page_reads = 12801905u + k;
    r->page_read_ios = 4839764u + k;
    r->page_write_ios = 12;
    r->direct_ios = 19824771u + k * 38u;
    r->buffered_ios = 0xFFFFF000u + k * 2048u;      /* wraps at k = 2 */

    r->pagefile_present = 1;
    r->pagefile_total_bytes = 233216ULL * 512;
    r->pagefile_free_bytes = 233216ULL * 512;

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
