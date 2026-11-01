/*
 * vmsotel: the model.
 *
 * A source fills a vo_raw with one reading of the system, in plain C
 * types and final units (bytes, 10 ms ticks, counts). vo_model_update()
 * turns it into a vo_snapshot, which is what the encoder reads. Between
 * the two sits vo_state, the only thing kept from one reading to the
 * next: the widening of 32-bit counters to 64 bits, and which CPU mode
 * counters have shown themselves unusable.
 *
 * No I/O and no VMS dependency, so all of it is tested on Linux. See
 * docs/design.md, "Counters" and "Metrics, version 1".
 */

#ifndef VO_MODEL_H
#define VO_MODEL_H

#include <stddef.h>

#include "vo_types.h"

/* CPU modes, in the order $GETRMI's RMI$_CPU* items are listed. */
enum {
    VO_CPU_INTERRUPT,
    VO_CPU_MPSYNCH,
    VO_CPU_KERNEL,
    VO_CPU_EXEC,
    VO_CPU_SUPER,
    VO_CPU_USER,
    VO_CPU_COMPAT,
    VO_CPU_IDLE,
    VO_CPU_MODES
};

/* VMS scheduling states, as $GETRMI counts them (RMI$_COM ... CEF). */
enum {
    VO_PS_COM, VO_PS_COMO, VO_PS_CUR, VO_PS_PFW, VO_PS_LEF, VO_PS_LEFO,
    VO_PS_HIB, VO_PS_HIBO, VO_PS_SUSP, VO_PS_SUSPO, VO_PS_FPG,
    VO_PS_COLPG, VO_PS_MWAIT, VO_PS_CEF,
    VO_PROC_STATES
};

#define VO_MAX_DISKS   64
#define VO_NAME_MAX    64      /* device and mount names, without NUL */
#define VO_MAX_PROCS   64      /* chosen processes reported per export */

typedef struct {
    char device[VO_NAME_MAX + 1];       /* e.g. "X86VMS$DKA0" */
    char mountpoint[VO_NAME_MAX + 1];   /* e.g. "DISK$X86SYS" */
    vo_u64 total_bytes;
    vo_u64 free_bytes;
    vo_u32 ops;                         /* DVI$_OPCNT, a longword */
} vo_raw_disk;

/* One chosen process (phase 2). */
typedef struct {
    vo_u32 pid;
    char name[16];                      /* JPI$_PRCNAM, at most 15 */
    char user[13];                      /* JPI$_USERNAME, blanks trimmed */
    vo_u32 cpu_ticks;                   /* JPI$_CPUTIM: 10 ms, a longword */
    vo_u64 mem_bytes;                   /* PPGCNT + GPGCNT pagelets x 512 */
} vo_raw_proc;

/*
 * One reading. A source fills every field it can; *_present flags say
 * which optional parts it could not. Times are Unix nanoseconds, UTC.
 */
typedef struct {
    vo_u64 now_ns;
    vo_u64 boot_ns;

    int cpu_present[VO_CPU_MODES];
    vo_u64 cpu_ticks[VO_CPU_MODES];     /* 10 ms ticks, summed over CPUs */
    vo_u32 cpu_count;

    int mem_present;
    vo_u64 mem_total_bytes;
    vo_u64 mem_free_bytes;
    vo_u64 mem_modified_bytes;

    vo_u32 faults;                      /* longword counters */
    vo_u32 page_reads;
    vo_u32 page_read_ios;
    vo_u32 page_write_ios;
    vo_u32 direct_ios;
    vo_u32 buffered_ios;

    int pagefile_present;
    vo_u64 pagefile_total_bytes;
    vo_u64 pagefile_free_bytes;

    vo_u32 proc_states[VO_PROC_STATES];

    size_t ndisks;                      /* mounted disks to report */
    vo_raw_disk disks[VO_MAX_DISKS];

    /* Phase 2: processes the filter chose, the first VO_MAX_PROCS. */
    size_t nprocs;
    vo_raw_proc procs[VO_MAX_PROCS];
    size_t procs_matched;               /* chosen, before the cap */
    size_t procs_hidden;                /* the scan was refused (NOPRIV) */
} vo_raw;

/* A 32-bit counter widened to 64 bits. */
typedef struct {
    int seen;
    vo_u32 last;
    vo_u64 base;        /* multiples of 2^32 added so far */
    vo_u64 start_ns;    /* startTimeUnixNano to report */
} vo_wide;

/* A counter as the encoder sees it. */
typedef struct {
    vo_u64 value;
    vo_u64 start_ns;
} vo_count;

typedef struct {
    vo_wide faults, page_reads, page_read_ios, page_write_ios;
    vo_wide direct_ios, buffered_ios;

    struct {
        int used;
        char device[VO_NAME_MAX + 1];
        vo_wide ops;
    } disks[VO_MAX_DISKS];

    int cpu_seen;
    vo_u64 cpu_last[VO_CPU_MODES];
    int cpu_dropped[VO_CPU_MODES];      /* went backwards once: never again */

    /* Per-process CPU, widened by PID; a slot whose process was not in
       the latest reading is freed, so processes that come and go do
       not fill the table. */
    struct {
        int used;
        vo_u32 pid;
        vo_wide cpu;
    } procs[VO_MAX_PROCS];
} vo_state;

typedef struct {
    const vo_raw *raw;                  /* everything not widened */
    int cpu_present[VO_CPU_MODES];      /* raw's, less any dropped */
    vo_count faults, page_reads, page_read_ios, page_write_ios;
    vo_count direct_ios, buffered_ios;
    int disk_ops_present[VO_MAX_DISKS]; /* 0 when the disk table is full */
    vo_count disk_ops[VO_MAX_DISKS];
    vo_count proc_cpu[VO_MAX_PROCS];    /* ticks, widened, per raw->procs */
} vo_snapshot;

void vo_state_init(vo_state *st);

/*
 * Widens one longword reading. The first reading starts the counter,
 * with now_ns as its start time. A reading lower than the last by 2^31
 * or more is a wrap: 2^32 is added from then on. A smaller drop is a
 * reset (a disk remounted, say): the base returns to zero and the
 * start time becomes now_ns, so a backend sees a new series rather
 * than a counter going backwards. A restart of the agent is the same
 * as a first reading, for the same reason.
 */
vo_u64 vo_wide_update(vo_wide *w, vo_u32 reading, vo_u64 now_ns);

/*
 * Builds a snapshot from a reading. snap->raw points at raw, which must
 * outlive the snapshot. Returns the number of CPU modes newly dropped
 * in this update -- one that went backwards, or read 2^63 or more --
 * so the caller can log them; vo_cpu_mode_name() names them.
 */
int vo_model_update(vo_state *st, const vo_raw *raw, vo_snapshot *snap);

const char *vo_cpu_mode_name(int mode);         /* cpu.mode value */
const char *vo_proc_state_name(int state);      /* VMS name, lower-case */
const char *vo_proc_state_std(int state);       /* process.state value */

#endif
