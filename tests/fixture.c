/*
 * vmsotel tests: fixed readings. See fixture.h.
 */

#include <string.h>

#include "fixture.h"

static void disk(vo_raw_disk *d, const char *dev, const char *mnt,
                 unsigned long long blocks, unsigned long long free_blocks,
                 vo_u32 ops)
{
    strcpy(d->device, dev);
    strcpy(d->mountpoint, mnt);
    d->total_bytes = Q(blocks * 512);
    d->free_bytes = Q(free_blocks * 512);
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
    r->cpu_ticks[VO_CPU_INTERRUPT] = Q(695781);
    r->cpu_ticks[VO_CPU_MPSYNCH] = Q(87746);
    r->cpu_ticks[VO_CPU_KERNEL] = Q(25332517);
    r->cpu_ticks[VO_CPU_EXEC] = Q(1099358);
    r->cpu_ticks[VO_CPU_SUPER] = Q(124012);
    r->cpu_ticks[VO_CPU_USER] = Q(1871610);
    r->cpu_ticks[VO_CPU_COMPAT] = Q(0);
    r->cpu_ticks[VO_CPU_IDLE] = Q(111328837);
    r->cpu_count = 2;

    r->mem_present = 1;
    r->mem_total_bytes = Q(930264ULL * 8192);
    r->mem_free_bytes = Q(699303ULL * 8192);
    r->mem_modified_bytes = Q(24632ULL * 8192);

    r->faults = 53843179;
    r->page_reads = 12801905;
    r->page_read_ios = 4839764;
    r->page_write_ios = 12;
    r->direct_ios = 19824771;
    r->buffered_ios = 645065712;

    r->pagefile_present = 1;
    r->pagefile_total_bytes = Q(233216ULL * 512);
    r->pagefile_free_bytes = Q(233216ULL * 512);

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

    /* Two chosen processes, from x86's SHOW SYSTEM. */
    r->nprocs = 2;
    r->procs_matched = 2;
    r->procs[0].pid = 0x00000438;
    strcpy(r->procs[0].name, "MARIADBD_3306");
    strcpy(r->procs[0].user, "MARIADB");
    r->procs[0].cpu_ticks = 290331;
    r->procs[0].mem_bytes = Q(209664ULL * 512);
    r->procs[1].pid = 0x00000435;
    strcpy(r->procs[1].name, "LTPHP_19000");
    strcpy(r->procs[1].user, "LIGHTTPD");
    r->procs[1].cpu_ticks = 7805290;
    r->procs[1].mem_bytes = Q(62464ULL * 512);
}

void fixture_minimal(vo_raw *r)
{
    memset(r, 0, sizeof *r);
    r->now_ns = FIX_NOW_NS;
    r->boot_ns = FIX_BOOT_NS;
    r->cpu_count = 1;
    r->mem_present = 1;
    r->mem_total_bytes = Q(524240ULL * 8192);
    r->mem_free_bytes = Q(349972ULL * 8192);
    r->mem_modified_bytes = Q(1849ULL * 8192);
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
    st->start_ns = vo_qsub(FIX_NOW_NS, Q(3600000000000ULL)); /* an hour ago */
    st->exports_sent = 58;
    st->exports_failed = 3;
    st->exports_dropped = 1;
    st->queue_length = 2;
    st->collect_us = 1234;
    st->have_skew = 1;
    st->skew_s = -7;
}

static void log_str(vo_log_rec *r, const char *key, const char *v)
{
    vo_log_attr *a = &r->attrs[r->nattrs++];

    a->key = key;
    a->is_int = 0;
    strcpy(a->sval, v);
}

static void log_int(vo_log_rec *r, const char *key, unsigned long v)
{
    vo_log_attr *a = &r->attrs[r->nattrs++];

    a->key = key;
    a->is_int = 1;
    a->ival = v;
    a->sval[0] = '\0';
}

void fixture_logs(vo_log_rec *recs)
{
    vo_log_rec *r;

    memset(recs, 0, FIX_NLOGS * sizeof *recs);

    r = &recs[0];
    r->source = VO_LOG_OPCOM;
    r->time_ns = Q(1793526797660000000ULL);       /* 1-NOV-2026 09:53:17.66 */
    r->severity = VO_SEV_INFO;
    r->severity_text = "INFO";
    r->file = "SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;249";
    strcpy(r->body, "Message from user IAIN on X86VMS\n"
           "_X86VMS$FTA5864:, vmsotel probe_logs marker 1793526797");
    log_str(r, "openvms.opcom.user", "IAIN");
    log_str(r, "openvms.opcom.node", "X86VMS");

    r = &recs[1];
    r->source = VO_LOG_AUDIT;
    r->time_ns = Q(1793491500783231000ULL);
    r->severity = VO_SEV_WARN;
    r->severity_text = "WARN";
    r->file = "SYS$COMMON:[SYSMGR]SECURITY.AUDIT$JOURNAL;4";
    strcpy(r->body, "Login failure: remote user ROOT from SSH_PASSWORD:192.0.2.10");
    log_str(r, "openvms.audit.type", "LOGFAIL");
    log_int(r, "openvms.audit.subtype", 5);
    log_int(r, "process.pid", 61215);
    log_str(r, "openvms.process.id", "0000EF1F");
    log_str(r, "openvms.audit.remote_username", "ROOT");
    log_str(r, "openvms.audit.remote_nodename", "SSH_PASSWORD:192.0.2.10");

    r = &recs[2];
    r->source = VO_LOG_OPCOM;
    r->time_ns = Q(0);
    r->severity = VO_SEV_ERROR;
    r->severity_text = "ERROR";
    r->file = "SYS$SYSROOT:[SYSMGR]OPERATOR.LOG;249";
    strcpy(r->body, "%LICENSE-E-NOAUTH, \"caf\xe9\" \\ path\ttab");
}
