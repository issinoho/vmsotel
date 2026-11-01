/*
 * vmsotel: the model. See model.h.
 */

#include <string.h>

#include "model.h"

#define VO_2_31 0x80000000u
#define VO_2_32 0x100000000ULL

void vo_state_init(vo_state *st)
{
    memset(st, 0, sizeof *st);
}

vo_u64 vo_wide_update(vo_wide *w, vo_u32 reading, vo_u64 now_ns)
{
    if (!w->seen) {
        w->seen = 1;
        w->base = 0;
        w->start_ns = now_ns;
    } else if (reading < w->last) {
        if (w->last - reading >= VO_2_31) {
            w->base += VO_2_32;
        } else {
            w->base = 0;
            w->start_ns = now_ns;
        }
    }
    w->last = reading;
    return w->base + reading;
}

static void widen(vo_wide *w, vo_u32 reading, vo_u64 now_ns, vo_count *out)
{
    out->value = vo_wide_update(w, reading, now_ns);
    out->start_ns = w->start_ns;
}

/* The state's slot for a device, claiming a free one if it is new.
   NULL when every slot is taken by another device. */
static vo_wide *disk_slot(vo_state *st, const char *device)
{
    size_t i, free_slot = VO_MAX_DISKS;

    for (i = 0; i < VO_MAX_DISKS; i++) {
        if (st->disks[i].used) {
            if (strcmp(st->disks[i].device, device) == 0)
                return &st->disks[i].ops;
        } else if (free_slot == VO_MAX_DISKS) {
            free_slot = i;
        }
    }
    if (free_slot == VO_MAX_DISKS)
        return NULL;
    st->disks[free_slot].used = 1;
    strncpy(st->disks[free_slot].device, device, VO_NAME_MAX);
    st->disks[free_slot].device[VO_NAME_MAX] = '\0';
    memset(&st->disks[free_slot].ops, 0, sizeof st->disks[free_slot].ops);
    return &st->disks[free_slot].ops;
}

int vo_model_update(vo_state *st, const vo_raw *raw, vo_snapshot *snap)
{
    size_t i;
    int newly_dropped = 0;

    memset(snap, 0, sizeof *snap);
    snap->raw = raw;

    /*
     * Quadword CPU counters are never widened, but one that goes
     * backwards is not a counter: x86 E9.2-4's RMI$_CPUINTSTK sits near
     * 2^64 and falls. It is dropped for the life of the agent, since a
     * series that comes and goes is worse than one that is absent.
     *
     * A reading of 2^63 or more is dropped from the first: no count of
     * 10 ms ticks gets there (it would take 10^11 years), and as a
     * signed number it is negative, which is what x86's is. Waiting to
     * see it fall sent one absurd value, 1.8e17 s, with the first
     * export after every start.
     */
    for (i = 0; i < VO_CPU_MODES; i++) {
        if (!raw->cpu_present[i])
            continue;
        if (!st->cpu_dropped[i] &&
            (raw->cpu_ticks[i] >= 0x8000000000000000ULL ||
             (st->cpu_seen && raw->cpu_ticks[i] < st->cpu_last[i]))) {
            st->cpu_dropped[i] = 1;
            newly_dropped++;
        }
        st->cpu_last[i] = raw->cpu_ticks[i];
        snap->cpu_present[i] = !st->cpu_dropped[i];
    }
    st->cpu_seen = 1;

    widen(&st->faults, raw->faults, raw->now_ns, &snap->faults);
    widen(&st->page_reads, raw->page_reads, raw->now_ns, &snap->page_reads);
    widen(&st->page_read_ios, raw->page_read_ios, raw->now_ns,
          &snap->page_read_ios);
    widen(&st->page_write_ios, raw->page_write_ios, raw->now_ns,
          &snap->page_write_ios);
    widen(&st->direct_ios, raw->direct_ios, raw->now_ns, &snap->direct_ios);
    widen(&st->buffered_ios, raw->buffered_ios, raw->now_ns,
          &snap->buffered_ios);

    for (i = 0; i < raw->ndisks && i < VO_MAX_DISKS; i++) {
        vo_wide *w = disk_slot(st, raw->disks[i].device);

        if (w != NULL) {
            widen(w, raw->disks[i].ops, raw->now_ns, &snap->disk_ops[i]);
            snap->disk_ops_present[i] = 1;
        }
    }
    return newly_dropped;
}

const char *vo_cpu_mode_name(int mode)
{
    /* user, kernel, interrupt and idle are semantic-convention values;
       the rest are VMS's own, kept because an administrator reads the
       kernel/executive split in MONITOR MODES. */
    static const char *const names[VO_CPU_MODES] = {
        "interrupt", "mpsynch", "kernel", "executive",
        "supervisor", "user", "compat", "idle"
    };

    return mode >= 0 && mode < VO_CPU_MODES ? names[mode] : "unknown";
}

const char *vo_proc_state_name(int state)
{
    static const char *const names[VO_PROC_STATES] = {
        "com", "como", "cur", "pfw", "lef", "lefo", "hib", "hibo",
        "susp", "suspo", "fpg", "colpg", "mwait", "cef"
    };

    return state >= 0 && state < VO_PROC_STATES ? names[state] : "unknown";
}

const char *vo_proc_state_std(int state)
{
    switch (state) {
    case VO_PS_COM: case VO_PS_COMO: case VO_PS_CUR:
        return "running";
    case VO_PS_SUSP: case VO_PS_SUSPO:
        return "stopped";
    case VO_PS_PFW: case VO_PS_LEF: case VO_PS_LEFO: case VO_PS_HIB:
    case VO_PS_HIBO: case VO_PS_FPG: case VO_PS_COLPG: case VO_PS_MWAIT:
    case VO_PS_CEF:
        return "sleeping";
    default:
        return "unknown";
    }
}
