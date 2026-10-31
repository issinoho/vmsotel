/*
 * vmsotel tests: fixed readings. See fixture.h.
 */

#include <string.h>

#include "fixture.h"

static void disk(vo_raw_disk *d, const char *dev, const char *mnt,
                 vo_u64 blocks, vo_u64 free_blocks, vo_u32 ops)
{
    strcpy(d->device, dev);
    strcpy(d->mountpoint, mnt);
    d->total_bytes = blocks * 512;
    d->free_bytes = free_blocks * 512;
    d->ops = ops;
}

void fixture_full(vo_raw *r)
{
    int i;

    memset(r, 0, sizeof *r);
    r->now_ns = FIX_NOW_NS;
    r->boot_ns = FIX_BOOT_NS;

    for (i = 0; i < VO_CPU_MODES; i++)
        r->cpu_present[i] = 1;
    r->cpu_ticks[VO_CPU_INTERRUPT] = 695781;
    r->cpu_ticks[VO_CPU_MPSYNCH] = 87746;
    r->cpu_ticks[VO_CPU_KERNEL] = 25332517;
    r->cpu_ticks[VO_CPU_EXEC] = 1099358;
    r->cpu_ticks[VO_CPU_SUPER] = 124012;
    r->cpu_ticks[VO_CPU_USER] = 1871610;
    r->cpu_ticks[VO_CPU_COMPAT] = 0;
    r->cpu_ticks[VO_CPU_IDLE] = 111328837;
    r->cpu_count = 2;

    r->mem_present = 1;
    r->mem_total_bytes = 930264ULL * 8192;
    r->mem_free_bytes = 699303ULL * 8192;
    r->mem_modified_bytes = 24632ULL * 8192;

    r->faults = 53843179;
    r->page_reads = 12801905;
    r->page_read_ios = 4839764;
    r->page_write_ios = 12;
    r->direct_ios = 19824771;
    r->buffered_ios = 645065712;

    r->pagefile_present = 1;
    r->pagefile_total_bytes = 233216ULL * 512;
    r->pagefile_free_bytes = 233216ULL * 512;

    r->proc_states[VO_PS_CUR] = 1;
    r->proc_states[VO_PS_LEF] = 13;
    r->proc_states[VO_PS_HIB] = 26;

    r->ndisks = 3;
    disk(&r->disks[0], "X86VMS$DKA0", "DISK$X86SYS", 45000000, 20011552,
         6460564);
    disk(&r->disks[1], "X86VMS$DKA200", "DISK$TOOLS", 1974510, 1133328,
         33713);
    disk(&r->disks[2], "X86VMS$DKA300", "DISK$DUMP", 33554432, 7336282,
         158705019);
}

void fixture_minimal(vo_raw *r)
{
    memset(r, 0, sizeof *r);
    r->now_ns = FIX_NOW_NS;
    r->boot_ns = FIX_BOOT_NS;
    r->cpu_count = 1;
    r->mem_present = 1;
    r->mem_total_bytes = 524240ULL * 8192;
    r->mem_free_bytes = 349972ULL * 8192;
    r->mem_modified_bytes = 1849ULL * 8192;
    r->proc_states[VO_PS_CUR] = 1;
}

void fixture_resource(vo_resource *res)
{
    memset(res, 0, sizeof *res);
    res->service_version = "0.1.0";
    res->host_name = "x86vms.issinoho.com";
    res->host_arch = "amd64";
    res->host_type = "QEMU Standard PC (Q35 + ICH9, 2009)";
    res->os_version = "E9.2-4";
    res->scsnode = "X86VMS";
    res->cluster_member = 0;
}

void fixture_stats(vo_agent_stats *st)
{
    memset(st, 0, sizeof *st);
    st->start_ns = FIX_NOW_NS - 3600000000000ULL;   /* an hour ago */
    st->exports_sent = 58;
    st->exports_failed = 3;
    st->exports_dropped = 1;
    st->queue_length = 2;
    st->collect_us = 1234;
    st->have_skew = 1;
    st->skew_s = -7;
}
