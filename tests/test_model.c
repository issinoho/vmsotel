/*
 * vmsotel tests: the model -- counter widening, the restart and reset
 * rules, per-disk state, and CPU modes that go backwards.
 */

#include "check.h"
#include "fixture.h"
#include "procfilter.h"

#define T0 1000000000000ULL
#define T1 (T0 + 60000000000ULL)
#define T2 (T1 + 60000000000ULL)
#define T3 (T2 + 60000000000ULL)
#define TOP 0xFFFFFFFFu
#define TWO32 4294967296ULL

static void test_first_and_increase(void)
{
    vo_wide w;

    memset(&w, 0, sizeof w);
    CHECK_U64(vo_wide_update(&w, 100, T0), 100);
    CHECK_U64(w.start_ns, T0);
    CHECK_U64(vo_wide_update(&w, 250, T1), 250);
    CHECK_U64(w.start_ns, T0);
    CHECK_U64(vo_wide_update(&w, 250, T2), 250);     /* unchanged is fine */
}

static void test_wrap(void)
{
    vo_wide w;

    memset(&w, 0, sizeof w);
    vo_wide_update(&w, TOP - 15, T0);
    /* Past 2^32: the reading restarts low and 2^32 is added. */
    CHECK_U64(vo_wide_update(&w, 16, T1), TWO32 + 16);
    CHECK_U64(w.start_ns, T0);
    CHECK_U64(vo_wide_update(&w, 1000, T2), TWO32 + 1000);
    /* A second wrap adds another 2^32. */
    vo_wide_update(&w, TOP, T2);
    CHECK_U64(vo_wide_update(&w, 0, T3), 2 * TWO32);
    CHECK_U64(w.start_ns, T0);
}

static void test_exact_boundary(void)
{
    vo_wide w;

    /* A drop of exactly 2^31 counts as a wrap; one less is a reset. */
    memset(&w, 0, sizeof w);
    vo_wide_update(&w, 0x80000000u, T0);
    CHECK_U64(vo_wide_update(&w, 0, T1), TWO32);
    CHECK_U64(w.start_ns, T0);

    memset(&w, 0, sizeof w);
    vo_wide_update(&w, 0x7FFFFFFFu, T0);
    CHECK_U64(vo_wide_update(&w, 0, T1), 0);
    CHECK_U64(w.start_ns, T1);
}

static void test_reset(void)
{
    vo_wide w;

    /* A small drop is a reset, not a wrap: no 2^32, and a new start
       time so the backend sees a new series. */
    memset(&w, 0, sizeof w);
    vo_wide_update(&w, 5000, T0);
    CHECK_U64(vo_wide_update(&w, 10, T1), 10);
    CHECK_U64(w.start_ns, T1);
    /* A reset after a wrap drops the 2^32 as well. */
    memset(&w, 0, sizeof w);
    vo_wide_update(&w, TOP, T0);
    vo_wide_update(&w, 5000, T1);                    /* wrapped: 2^32+5000 */
    CHECK_U64(vo_wide_update(&w, 10, T2), 10);
    CHECK_U64(w.start_ns, T2);
}

static void test_restart(void)
{
    vo_state st;
    vo_raw r;
    vo_snapshot s;

    /* After a wrap, a restarted agent cannot know about it. It reports
       the raw reading with its own start time, so the value is lower
       than before but under a new start: a reset, not a fall. */
    fixture_minimal(&r);
    vo_state_init(&st);
    r.now_ns = T0;
    r.buffered_ios = TOP;
    vo_model_update(&st, &r, &s);
    r.now_ns = T1;
    r.buffered_ios = 7;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.buffered_ios.value, TWO32 + 7);
    CHECK_U64(s.buffered_ios.start_ns, T0);

    vo_state_init(&st);                              /* the restart */
    r.now_ns = T2;
    r.buffered_ios = 9;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.buffered_ios.value, 9);
    CHECK_U64(s.buffered_ios.start_ns, T2);
}

static void test_all_counters_widened(void)
{
    vo_state st;
    vo_raw r;
    vo_snapshot s;

    fixture_full(&r);
    vo_state_init(&st);
    r.now_ns = T0;
    r.faults = r.page_reads = r.page_read_ios = r.page_write_ios = TOP;
    r.direct_ios = r.buffered_ios = r.disks[0].ops = TOP;
    vo_model_update(&st, &r, &s);
    r.now_ns = T1;
    r.faults = r.page_reads = r.page_read_ios = r.page_write_ios = 1;
    r.direct_ios = r.buffered_ios = r.disks[0].ops = 1;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.faults.value, TWO32 + 1);
    CHECK_U64(s.page_reads.value, TWO32 + 1);
    CHECK_U64(s.page_read_ios.value, TWO32 + 1);
    CHECK_U64(s.page_write_ios.value, TWO32 + 1);
    CHECK_U64(s.direct_ios.value, TWO32 + 1);
    CHECK_U64(s.buffered_ios.value, TWO32 + 1);
    CHECK_U64(s.disk_ops[0].value, TWO32 + 1);
}

static void test_disks_by_name(void)
{
    vo_state st;
    vo_raw r;
    vo_snapshot s;
    vo_raw_disk tmp;

    /* Widening follows the device name, not the position: a disk that
       moves in the list keeps its own history. */
    fixture_full(&r);
    vo_state_init(&st);
    r.now_ns = T0;
    r.disks[0].ops = TOP;            /* DKA0 */
    r.disks[2].ops = 100;            /* DKA300 */
    vo_model_update(&st, &r, &s);

    tmp = r.disks[0];
    r.disks[0] = r.disks[2];
    r.disks[2] = tmp;
    r.now_ns = T1;
    r.disks[0].ops = 200;            /* DKA300, now first */
    r.disks[2].ops = 3;              /* DKA0, wrapped */
    vo_model_update(&st, &r, &s);
    CHECK(strcmp(r.disks[0].device, "X86VMS$DKA300") == 0);
    CHECK_U64(s.disk_ops[0].value, 200);
    CHECK_U64(s.disk_ops[2].value, TWO32 + 3);
    CHECK(s.disk_ops_present[0] && s.disk_ops_present[1] &&
          s.disk_ops_present[2]);

    /* A disk mounted later starts at the time it was first seen. */
    r.ndisks = 4;
    memset(&r.disks[3], 0, sizeof r.disks[3]);
    strcpy(r.disks[3].device, "X86VMS$DKA400");
    r.disks[3].ops = 5;
    r.now_ns = T2;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.disk_ops[3].start_ns, T2);
    CHECK_U64(s.disk_ops[0].start_ns, T0);
}

static void test_disk_table_full(void)
{
    static vo_state st;
    static vo_raw r;
    static vo_snapshot s;
    int i;

    fixture_minimal(&r);
    vo_state_init(&st);
    r.now_ns = T0;
    r.ndisks = VO_MAX_DISKS;
    for (i = 0; i < VO_MAX_DISKS; i++) {
        sprintf(r.disks[i].device, "NODE$DKA%d", i);
        r.disks[i].ops = (vo_u32) i;
    }
    vo_model_update(&st, &r, &s);
    CHECK(s.disk_ops_present[VO_MAX_DISKS - 1]);

    /* Every slot is taken; a new device gets no ops, the rest keep. */
    strcpy(r.disks[0].device, "NODE$DKA999");
    r.now_ns = T1;
    vo_model_update(&st, &r, &s);
    CHECK(!s.disk_ops_present[0]);
    CHECK(s.disk_ops_present[1]);
}

static void test_cpu_backwards(void)
{
    vo_state st;
    vo_raw r;
    vo_snapshot s;

    /* x86's RMI$_CPUINTSTK reads near 2^64: unusable from the first
       reading, so it is never sent at all. */
    fixture_full(&r);
    vo_state_init(&st);
    r.cpu_ticks[VO_CPU_INTERRUPT] = 0xFFFFFFFFFFD00000ULL;
    CHECK(vo_model_update(&st, &r, &s) == 1);
    CHECK(!s.cpu_present[VO_CPU_INTERRUPT]);
    CHECK(s.cpu_present[VO_CPU_KERNEL]);
    r.cpu_ticks[VO_CPU_INTERRUPT] -= 14;
    CHECK(vo_model_update(&st, &r, &s) == 0);        /* not reported twice */
    CHECK(!s.cpu_present[VO_CPU_INTERRUPT]);

    /* One that is plausible but then falls is dropped when it falls. */
    fixture_full(&r);
    vo_state_init(&st);
    CHECK(vo_model_update(&st, &r, &s) == 0);
    CHECK(s.cpu_present[VO_CPU_INTERRUPT]);
    r.cpu_ticks[VO_CPU_INTERRUPT] -= 14;
    r.cpu_ticks[VO_CPU_KERNEL] += 194;
    CHECK(vo_model_update(&st, &r, &s) == 1);
    CHECK(!s.cpu_present[VO_CPU_INTERRUPT]);
    CHECK(s.cpu_present[VO_CPU_KERNEL]);

    r.cpu_ticks[VO_CPU_INTERRUPT] += 1000;           /* rising again */
    CHECK(vo_model_update(&st, &r, &s) == 0);        /* not reported twice */
    CHECK(!s.cpu_present[VO_CPU_INTERRUPT]);         /* and stays dropped */

    /* 2^63 - 1 is still a count; 2^63 is not. */
    fixture_full(&r);
    vo_state_init(&st);
    r.cpu_ticks[VO_CPU_USER] = 0x7FFFFFFFFFFFFFFFULL;
    r.cpu_ticks[VO_CPU_IDLE] = 0x8000000000000000ULL;
    CHECK(vo_model_update(&st, &r, &s) == 1);
    CHECK(s.cpu_present[VO_CPU_USER]);
    CHECK(!s.cpu_present[VO_CPU_IDLE]);

    /* A mode the source never had is simply absent, not dropped. */
    r.cpu_present[VO_CPU_COMPAT] = 0;
    vo_model_update(&st, &r, &s);
    CHECK(!s.cpu_present[VO_CPU_COMPAT]);
    CHECK(!st.cpu_dropped[VO_CPU_COMPAT]);
}

static void test_wildcards(void)
{
    vo_proc_filter f;

    CHECK(vo_wildcard_match("LTPHP_*", "LTPHP_19000"));
    CHECK(vo_wildcard_match("ltphp_*", "LTPHP_19000"));      /* case-blind */
    CHECK(vo_wildcard_match("*", ""));
    CHECK(vo_wildcard_match("*", "ANYTHING"));
    CHECK(vo_wildcard_match("OPCOM", "OPCOM"));
    CHECK(!vo_wildcard_match("OPCOM", "OPCOM2"));
    CHECK(!vo_wildcard_match("OPCOM", "OPCO"));
    CHECK(vo_wildcard_match("LTPHP_1900%", "LTPHP_19003"));
    CHECK(!vo_wildcard_match("LTPHP_1900%", "LTPHP_1900"));   /* % is one */
    CHECK(vo_wildcard_match("*_19*", "LTPHP_19000"));
    CHECK(vo_wildcard_match("*SERVER", "SECURITY_SERVER"));
    CHECK(!vo_wildcard_match("*SERVER", "SERVER_X"));
    CHECK(vo_wildcard_match("A*B*C", "AxxBxxBxxC"));          /* backtracks */
    CHECK(!vo_wildcard_match("A*B*C", "AxxBxxCxxB"));
    CHECK(vo_wildcard_match("%%%", "abc"));
    CHECK(!vo_wildcard_match("%%%", "ab"));
    CHECK(!vo_wildcard_match("", "x"));

    memset(&f, 0, sizeof f);
    CHECK(!vo_proc_filter_active(&f));
    CHECK(!vo_proc_filter_match(&f, "OPCOM", "SYSTEM"));    /* none: none */
    strcpy(f.name[f.nname++], "MARIADBD*");
    strcpy(f.user[f.nuser++], "LIGHTTPD");
    CHECK(vo_proc_filter_active(&f));
    CHECK(vo_proc_filter_match(&f, "MARIADBD_3306", "MARIADB"));
    CHECK(vo_proc_filter_match(&f, "VMSLIGHTTPD", "LIGHTTPD"));  /* by user */
    CHECK(!vo_proc_filter_match(&f, "OPCOM", "SYSTEM"));
}

static void test_procs(void)
{
    static vo_state st;
    static vo_raw r;
    static vo_snapshot s;
    int i;

    fixture_minimal(&r);
    vo_state_init(&st);
    r.nprocs = 2;
    r.procs[0].pid = 0x438;
    r.procs[0].cpu_ticks = TOP;                     /* about to wrap */
    r.procs[1].pid = 0x435;
    r.procs[1].cpu_ticks = 100;
    r.now_ns = T0;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.proc_cpu[0].value, TOP);
    CHECK_U64(s.proc_cpu[0].start_ns, T0);

    /* Order changes, 0x438 wraps, 0x435 is gone, 0x500 is new. */
    r.procs[0].pid = 0x500;
    r.procs[0].cpu_ticks = 7;
    r.procs[1].pid = 0x438;
    r.procs[1].cpu_ticks = 4;
    r.now_ns = T1;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.proc_cpu[1].value, TWO32 + 4);      /* followed by PID */
    CHECK_U64(s.proc_cpu[1].start_ns, T0);
    CHECK_U64(s.proc_cpu[0].value, 7);
    CHECK_U64(s.proc_cpu[0].start_ns, T1);          /* first seen now */

    /* A PID that comes back after going is a new process: new start. */
    r.procs[0].pid = 0x435;
    r.procs[0].cpu_ticks = 5;
    r.now_ns = T2;
    vo_model_update(&st, &r, &s);
    CHECK_U64(s.proc_cpu[0].start_ns, T2);
    CHECK_U64(s.proc_cpu[0].value, 5);

    /* Processes that come and go never fill the table: 3 x 64 PIDs. */
    for (i = 0; i < 3 * VO_MAX_PROCS; i++) {
        r.nprocs = 1;
        r.procs[0].pid = 0x1000 + (vo_u32) i;
        r.procs[0].cpu_ticks = (vo_u32) i;
        r.now_ns = T3 + (vo_u64) i;
        vo_model_update(&st, &r, &s);
    }
    CHECK_U64(s.proc_cpu[0].value, 3 * VO_MAX_PROCS - 1);
    CHECK_U64(s.proc_cpu[0].start_ns, T3 + 3 * VO_MAX_PROCS - 1);
}

static void test_names(void)
{
    CHECK(strcmp(vo_cpu_mode_name(VO_CPU_EXEC), "executive") == 0);
    CHECK(strcmp(vo_cpu_mode_name(VO_CPU_INTERRUPT), "interrupt") == 0);
    CHECK(strcmp(vo_cpu_mode_name(VO_CPU_MODES), "unknown") == 0);
    CHECK(strcmp(vo_proc_state_name(VO_PS_LEF), "lef") == 0);
    CHECK(strcmp(vo_proc_state_std(VO_PS_CUR), "running") == 0);
    CHECK(strcmp(vo_proc_state_std(VO_PS_COMO), "running") == 0);
    CHECK(strcmp(vo_proc_state_std(VO_PS_HIB), "sleeping") == 0);
    CHECK(strcmp(vo_proc_state_std(VO_PS_SUSPO), "stopped") == 0);
    CHECK(strcmp(vo_proc_state_std(VO_PROC_STATES), "unknown") == 0);
}

int main(void)
{
    test_first_and_increase();
    test_wrap();
    test_exact_boundary();
    test_reset();
    test_restart();
    test_all_counters_widened();
    test_disks_by_name();
    test_disk_table_full();
    test_cpu_backwards();
    test_wildcards();
    test_procs();
    test_names();
    return check_summary("test_model");
}
