# Probe findings

One row per question per machine. Quote the line of output that settles
it. A question is answered only when both machines have a row.

Machines: **x86** = E9.2-4 on QEMU ("QEMU Standard PC (Q35 + ICH9,
2009)"), 2 CPUs, VSI C `__DECC_VER 70790003`. **I64** = rx2660,
V8.4-2L3, "HP rx2660 (1.59GHz/6.0MB)", 1 CPU, VSI C `__DECC_VER 70490001`.

Runs so far, all **x86, 2026-10-31**, user IAIN:
1. 20:17, probes from 2be172f, nearly every privilege enabled.
2. 21:28, probes from 7f5ebdb, same privileges (`Process privileges:`
   ACNT … WORLD).
3. 21:28, probes from 7f5ebdb, in a subprocess with
   `Process privileges: NETMBX TMPMBX` only.
4. ~21:34, `probe_rmi` alone from 2eacec7, privileged (4a) and in a
   NETMBX/TMPMBX subprocess (4b).
5. ~21:38, `probe_rmi` alone from 448033c (longword return length),
   privileged; then `MONITOR STATES/SUMMARY` for 5 s.

Runs on **I64 (MYI64), 2026-10-31 21:40**, user IAIN, probes from
890bde9: run 1, full, with the same privileges as x86 (ACNT … WORLD);
the unprivileged run in a NETMBX/TMPMBX subprocess did not start (see
Surprises). Run 2, 21:47, after `SET SECURITY/OWNER=[IAIN]` on the
clone: full, NETMBX/TMPMBX only, log in `SYS$LOGIN`.

Collector output on docker-nuc not yet pasted for either machine.

| # | Question | x86 | I64 | Evidence / notes |
| --- | --- | --- | --- | --- |
| 1 | All six probes build; no LINK warnings | yes | yes | x86: `built:  probe_syi probe_rmi probe_jpi probe_dvi probe_otlp probe_otlp_tls`, no FAILED line. I64: `built:  probe_syi probe_rmi probe_jpi probe_dvi probe_otlp probe_otlp_tls`, no FAILED line |
| 2 | `printf("%llu")` works | yes | yes | x86: `printf %llu check: 12345678901`. I64: `printf %llu check: 12345678901` |
| 3 | Item-list layout is 12 bytes | yes | yes | x86: `item list entry: 12 bytes, pointers 4 bytes`. I64: `item list entry: 12 bytes, pointers 4 bytes` |
| 4 | `SYI$_` items: which are declared, which accepted | all but one | all but one | x86: every item accepted except `SYI$_FREE_PAGES  not declared in <syidef.h>`. Not needed: free memory comes from `RMI$_FRLIST`. `HW_MODEL` 2 bytes, `CLUSTER_MEMBER` 1, `CLUSTER_NODES` 2, `MAXPROCESSCNT` 2 — not all longwords. I64: the same — `SYI$_FREE_PAGES  not declared`, the rest accepted with the same sizes as x86 |
| 5 | `time()` is UTC | narrowed | not settled | x86: `time()` = `$GETTIM` = `$GETTIM minus differential` = `2026-10-31T20:17:43Z`, but the differential is 0 (GMT), so local time and UTC are the same and this cannot tell them apart. Needs a run while a nonzero differential is in force. I64: also `SYS$TIMEZONE_DIFFERENTIAL 0`, all three readings `2026-10-31T21:40:34Z`; cannot tell x86, subprocess with `DEFINE SYS$TIMEZONE_DIFFERENTIAL "3600"`: `time()` stayed at `$GETTIM` (`1793483746` both) and ignored the process-level value — `time() differs from local-minus-differential by 3600 s`. So the C RTL takes the system's differential, not a process logical; whether it subtracts it is still unseen while the system value is 0 |
| 6 | `SYS$TIMEZONE_DIFFERENTIAL` defined and correct | defined | defined | x86: `SYS$TIMEZONE_DIFFERENTIAL  0`, `SYS$TIMEZONE_NAME  GMT`. Correct for UK winter; see Surprises on boot time. I64: `0`, `GMT`, same rule `GMT0BST^1,M3.5.0/01,M10.4.0/02` |
| 7 | `RMI$_` items: declared, accepted, byte size | all 38 | all 38 | x86 run 4: every item declared and accepted. **Quadword** (accepts len 8, `retlen 8`): the eight `CPU*` modes, `FRLIST`, `MODLIST`. **Longword only** (len 8 → `queue 00000014` = SS$_BADPARAM, len 4 → `retlen 4`): everything else, including `PROCS`. Runs 1–3 crashed: see Surprises, return length. Run 5, with a longword return length: no `GUARD changed` line for any item. I64: identical item set and sizes (quadword `CPU*`, `FRLIST`, `MODLIST`; longword-only the rest). No `GUARD changed` line — but this probe already gave a longword return length, so I64 writing 2 or 4 bytes cannot be told apart |
| 8 | CPU mode counters are 10 ms ticks summed over CPUs | yes | 10 ms ticks | x86 run 4a: `sum of deltas 1012 over 5.18 s` = `97.7 units per second per active CPU`, 2 CPUs; 4b `97.3`. That sum includes `CPUINTSTK`'s negative delta (−14, −20); without it 99.0 and 99.2. Run 5, which leaves it out: `7 mode items accepted, sum of deltas 1006 over 5.08 s` = `99.0`. `CPUINTSTK` is not usable: see Surprises. I64: `8 mode items accepted, sum of deltas 504 over 5.05 s` = `99.8`, `CPUINTSTK` normal (`695781`, +0). One CPU (`ACTIVECPU_CNT 1`), so summing over CPUs is shown by x86 only |
| 9 | `$GETRMI` works unprivileged | **yes** | **yes** | x86 run 4b (NETMBX, TMPMBX): every item `ok`, same sizes and plausible values as 4a. I64: the unprivileged run did not start — `%DCL-E-OPENIN … build_probes.COM … -RMS-E-PRV`; see Surprises. I64 run 2 (NETMBX, TMPMBX, after the ownership fix): every item `ok`, `sum of deltas 502 over 5.03 s` = `99.9`. **Answered on both** |
| 10 | Which `RMI$_` items are counters and which gauges | see notes | same as x86 | x86 run 4, by size and 5 s delta. **Counters**: `CPU*` (except `CPUINTSTK`), `FAULTS`, `PREADS`, `PWRITES`, `PREADIO`, `PWRITIO`, `GVALFLTS`, `DZROFLTS`, `TRANSFLTS`, `DIRIO`, `BUFIO` (`645065712 → 645068377`), `MBREADS`, `MBWRITES`; `SYSFAULTS`, `ISWPCNT` were 0. **Gauges**: `PROCS` (39/40), `FRLIST` (`+12`), `MODLIST`, and the state counts `COM` … `CEF` (`LEF 14 → 13`). `BUFIO` at 6.45 × 10⁸ after 8.2 days (≈ 78 M a day) wraps its longword about 7 weeks from now. I64: same split; `BUFIO 1904861111`, `DIRIO 80220279` counters, `FRLIST +1`, `LEF 16 → 15` gauges |
| 11 | `JPI$_PPGCNT`/`GPGCNT` are pages or pagelets | **pagelets** | **pagelets** | x86: `MARIADBD_3306 … 209664` vs SHOW SYSTEM Pages `13104`; `LTPHP_19000 62464` vs `3904`. Ratio exactly 16 = 8192 / 512. SHOW SYSTEM's column is pages. I64: `LTPHP_19000 38688` vs SHOW SYSTEM `2418`; ratio 16. **Answered on both** |
| 12 | Processes visible unprivileged vs `SHOW SYSTEM` | **own only** | **own only** | x86 run 3: `processes returned  3`, `NOPRIV  38` — IAIN's three processes; 3 + 38 = 41 = SHOW SYSTEM's 41 lines. Privileged run 2: `40`, `NOPRIV 0` = SHOW SYSTEM's 40. The scan does not stop at NOPRIV, it skips. I64: privileged only, `processes returned 42`, `NOPRIV 0` = SHOW SYSTEM's 42; unprivileged run did not start. I64 run 2: `processes returned 3`, `NOPRIV 40`; 3 + 40 = 43 = SHOW SYSTEM's 43 lines. **Answered on both** |
| 13 | Full `$GETJPI` scan cost | 1–2 ms | 1 ms | x86: `scan took 2.0 ms` (42 processes), `1.0 ms` (40), `1.0 ms` unprivileged. I64: `scan took 1.0 ms` for 42 processes, privileged. I64 run 2: `0.0 ms` unprivileged (3 returned) |
| 14 | `$DEVICE_SCAN` finds every disk in `SHOW DEVICE D` | yes, and more | yes | x86: `7 disks found` — all five of SHOW DEVICE D (DMM0, DKA0, DKA100, DKA200, DKA300) plus `LDM0` and `LDM2585`; see Surprises. I64: `2 disks found` = SHOW DEVICE D's `DKA800`, `DNA0`. **Answered on both** (x86 adds LD devices) |
| 15 | `DVI$_FREEBLOCKS` matches `SHOW DEVICE D` | yes | yes | x86 run 2, exact on all three mounted D disks: `20008384`, `1132080`/`1132064` (DKA200, busy), `7313558`; run 3 `1131408` = `1131408`. Unprivileged too. I64: `48116480` = `48116480`. **Answered on both** |
| 16 | Shadow-set items available | accepted; untested | accepted; untested | x86: `SHDW_MASTER`, `SHDW_MEMBER` accepted, 1 byte, all 0; `SHDW_MASTER_NAME` 0 bytes. No shadow sets on this system, so behaviour on a member is unseen. I64: same items accepted, no shadow sets either. Still unseen on a shadowed disk |
| 17 | `DVI$_OPCNT` is cumulative | yes | yes | x86: `DKA0: 6460564 -> 6460716 (+152)`, `DKA300: 158705019 -> 158705083 (+64)` over 5 s. Longword. I64: `DKA800: 1868831249 -> 1868831262 (+13)`. Longword, already at 1.87 × 10⁹ after 14 days — wraps in about 2½ weeks. **Answered on both** |
| 18 | TCP and HTTP exchange to the collector; HTTP 200 | yes | yes | x86: `ok TCP connect`, `HTTP/1.1 200 OK`, body `{"partialSuccess":{}}`. I64: `HTTP/1.1 200 OK`, `{"partialSuccess":{}}`. **Answered on both** |
| 19 | Collector shows the data points | yes | yes | docker-nuc, otelcol 0.111.0 debug exporter: every export from both machines decoded — x86 at 20:17:56 and 20:17:57 (http, https), 21:28:28 ×2, 21:28:42–43 ×2; I64 at 21:40:58 ×2 and 21:47:22 ×2. Each: `resource metrics: 1, metrics: 2, data points: 2`, resource `host.name: Str(x86vms.issinoho.com)` / `Str(myi64.issinoho.com)`, `os.type: Str(openvms)`; `vmsotel.probe.heartbeat` as `Gauge`, `vmsotel.probe.exports` as `Sum`, `IsMonotonic: true`, `AggregationTemporality: Cumulative`, attribute `vmsotel.transport: Str(http)` / `Str(https)`. **Answered on both** |
| 20 | libssl image name and family found | SSL3 | SSL3 | x86: `OpenSSL: SYS$LIBRARY:SSL3$LIBSSL_SHR32, SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32, headers SSL3$INCLUDE`; `OpenSSL 3.0.21 9 Jun 2026`. I64: `SYS$LIBRARY:SSL3$LIBSSL_SHR32 …`, `OpenSSL 3.0.22 25 Aug 2026`. **Answered on both** |
| 21 | TLS handshake; version and cipher | yes | yes | x86: `negotiated TLSv1.3, TLS_AES_128_GCM_SHA256`, then HTTP 200. I64: `TLSv1.3, TLS_AES_128_GCM_SHA256`, HTTP 200. **Answered on both** |
| 22 | Certificate verifies with `--cafile` | not run | not run | optional, not run; `build_vms RUN` with `collector.crt` present answers it. x86 2c run: `TLS run skipped: needs collector.crt here`. I64: also skipped |
| 23 | `SYI$_MEMSIZE`, `RMI$_FRLIST`, `RMI$_MODLIST`: pages or pagelets | **pages** | **pages** | `build_vms SELFTEST`: the self-test's raw values beside `SHOW MEMORY/PHYSICAL`. Until answered, the agent sends no memory metrics. x86 2c self-test against SHOW MEMORY/PHYSICAL (pages), 5 s apart: `SYI$_MEMSIZE 930264` = `Total 930264` (`Main Memory (7.09GB)` = 930264 × 8192); `RMI$_FRLIST 696387` vs `Free 696389`; `RMI$_MODLIST 24836` vs `Modified 24830`; MONITOR PAGE `Free List Size 696361`, `Modified List Size 24828`. I64: `SYI$_MEMSIZE 524240` = `Total 524240` (`Main Memory (3.99GB)`); `RMI$_FRLIST 349796` vs `Free 349743`; `RMI$_MODLIST 1859` vs `Modified 1857`; MONITOR PAGE `Free List Size 349748`, `Modified List Size 1853`. **Answered on both**: reported from then on |
| 24 | `SYI$_PAGEFILE_PAGE`, `_FREE`: pages or pagelets | **pages** | **pages** | the same run, beside `SHOW MEMORY/FILES`. Until answered, no `system.paging.usage`. x86: `SYI$_PAGEFILE_PAGE 233216` = SHOW MEMORY/FILES `Total size of all paging files: 233216` (`8KB pages`); `SYI$_PAGEFILE_FREE 233216` = `Free` 172688 + 60528. Note `Total committed paging file usage: 61612`: FREE is space not yet written, not uncommitted space. I64: `524744` = `PAGEFILE.SYS` `Free 524744`, `Size 524744` (`8KB pages`). **Answered on both** |
| 25 | The agent builds, links clean, and its self-test matches SHOW and MONITOR | yes | yes | `build_vms SELFTEST`. x86: `built: vmsotel.exe vmsotel_tls.exe`, no warnings; self-test `self-test read everything`. CPU `99.7` ticks per CPU-second, idle 99.6% vs MONITOR MODES `Idle Time 197.72` of 200; buffered I/O `5.8`/s vs MONITOR IO `5.83`; disks `19981008`, `1127520`, `7331072` = SHOW DEVICE D exactly, memory disk LDM2585 skipped; interrupt `WENT BACKWARDS (-28)`; uptime `8 06:16:08` vs SHOW SYSTEM `8 07:14:58`, the expected hour (agent started after the summer-time change). I64: `built: vmsotel.exe vmsotel_tls.exe`, no warnings; CPU `100.0` ticks per CPU-second, kernel 0.2% vs MONITOR `0.16`; buffered I/O `10.4`/s vs `10.33`; states `cur 1 lef 15 hib 26` = MONITOR STATES exactly; disk `48108624` = SHOW DEVICE D; uptime `13 23:46:58` vs `14 00:47:24`, the expected hour. **Answered on both** |
| 26 | The agent's exports reach the collector from VMS | yes | yes | `build_vms RUN`, with the collector's output on docker-nuc. x86 `RUN`: `done: 3 collections, 3 sent, 0 failed attempts, 0 dropped`, and `CLOCK: this system is 8 s behind the collector`. Collector side not yet pasted. I64 `RUN`: `done: 3 collections, 3 sent, 0 failed attempts, 0 dropped`, and no CLOCK warning: within 2 s now, where the probes measured 13–14 s behind (NTP, `TCPIP$NTP_1`, may have caught up). Second `RUN` with memory reported, from b149bc9: x86 23:09:30–50 (`3 sent`, `CLOCK: … 7 s behind`), I64 23:10:41–23:11:02 (`3 sent`, no warning). Collector side, I64's third export (debug exporter, docker-nuc): `system.paging.usage` free `4298702848` = 524744 × 8192; `system.process.count` cur `1`, lef `15`, hib `26`; `system.filesystem.usage` `11790336000` + `24629739520` = 71132960 × 512; `system.disk.operations` `1868889233`, start `23:10:41` (agent start); boot-based start `2026-10-17 23:18:20`; `vmsotel.exports` sent `2`; `vmsotel.clock.skew` `0`. Defect: `vmsotel.collection.duration` `0.000000`, since the VMS UTC clock is time(), whole seconds -- to be timed with $GETTIM in 2d |
| 27 | Detached start, stop file, logged shutdown (2d) | yes | yes | Both, from 84540ea, started by IAIN with `@[.vmsotel]vmsotel_start`: `%RUN-S-PROC_ID`; VMSOTEL_RUN.LOG `Process privileges: NETMBX TMPMBX` only; clock check passed (the agent started). `vmsotel_stop`: `acknowledged; waiting for it to finish sending`, `stopped`; VMSOTEL.LOG ends `stopped: 3 collections, 3 sent, 0 failed attempts, 0 dropped`, exit `%X00000001`. Not yet seen: a start from SYSTARTUP_VMS.COM, a stop with exports queued, TYPE of the log while running |
| 28 | `vmsotel.collection.duration` from $GETTIM | | yes | I64 collector: `Value: 0.009000`, where it was `0.000000` from time(). x86 not pasted |
| 29 | Collector values against MONITOR SYSTEM | | yes | I64, 23:25–23:27: process count `cur 1` + `lef 16` + `hib 26` = 43 = MONITOR `Process Count 43.00`; page file free `4298702848` = 524744 × 8192; filesystem used + free = 71132960 × 512; `vmsotel.clock.skew -1`. Rates need two exports' counters; only the last export was pasted |
| 30 | Phase 1 review fixes (20a48fd): negative CPU counter dropped at once; TYPE of the live log | yes | | x86, 2026-11-01 01:16:22: `cpu mode interrupt is unusable (it went backwards, or reads as negative); not reported` in the same second as `starting`, where 84540ea logged it a minute later after one export carried it. TYPE of VMSOTEL.LOG during the run worked, no RMS-E-FLK. Stop clean: `stopped: 3 collections, 3 sent, 0 failed attempts, 0 dropped`. The clock-step guard is not observable in a normal run; Linux test only |
| 31 | `JPI$_CPUTIM` is 10 ms ticks (for phase 2) | **yes** | **yes** | From the probe_jpi runs already recorded: x86 `MARIADBD_3306 … 290331` against SHOW SYSTEM `00:48:23.31` (2903.31 s), `LTPHP_19000 … 7805290` against `21:40:52.90`; I64 `BESZEL_AGENT … 131236` against `00:21:52.36`, `LTPHP_19000 … 11` against `00:00:00.11`. Exact on all four. A longword: about 497 days of one process's CPU before it wraps |
| 32 | Phase 2 per-process metrics on VMS | **yes** | **yes** | x86, a64b1b5, 2026-11-01 01:29: the self-test saw `40 visible, 0 refused` = SHOW SYSTEM's 40 lines, and the 12 it printed equal SHOW SYSTEM exactly on CPU and Pages (`SWAPPER 22.18 … 17`, `QUEUE_MANAGER 340.22 … 317`, `ACME_SERVER 76.39 … 425`). VMSOTEL_START with `process.user = LIGHTTPD`, `process.name = BESZEL*`: `privileges NOSAME,NETMBX,TMPMBX,WORLD`; the log has `per-process metrics for 1 name and 1 user patterns` and no could-not-be-read line, so WORLD reached the detached process; `3 sent`. Collector, x86 first export (01:29:26) against SHOW SYSTEM of the same second, exact for all six chosen: `LTPHP_19000 … 80607.040000` = `22:23:27.04`, `29851648` = 3644 × 8192 = Pages `3644`; `LTPHP_19001 14674.44` = `04:04:34.44`, 3837 pages; `LTPHP_19002 4765.36`, 3412; `LTPHP_19003 3243.19`, 3483; `VMSLIGHTTPD 2308.45`, 2549; `BESZEL_AGENT 1472.83` = `00:24:32.83`, `10739712` = 1311. MARIADBD and the rest absent, as chosen. I64 (VMS side not pasted): the same six, memory equal to its earlier SHOW SYSTEM Pages (`LTPHP_19000 19808256` = 2418, `VMSLIGHTTPD` 1284, `BESZEL_AGENT` 1399), CPU consistent (`LTPHP_19000 0.11`). Busy processes' CPU rises export to export (80607.04, 80610.77, 80622.91), idle ones stay flat. **Answered on both** |
| 33 | Phase 3: the operator log opens for shared reading while OPCOM writes it; with what privilege | privileged only | privileged only | `build_probes LOGS`, privileged and in a NETMBX/TMPMBX subprocess: probe_logs `open the operator log`, and DIRECTORY/OWNER/PROTECTION of it. x86, 945bbbd: privileged `ok open the operator log for shared reading`; with NETMBX/TMPMBX only `open: %RMS-E-FNF` -- `OPERATOR.LOG;249 … [SYSTEM] (RWED,RWED,RE,)`, and DIRECTORY of SYS$MANAGER itself refused (`insufficient privilege`), so the file is not even found. The agent would need READALL or SYSPRV. No `OPC$*` logicals: the default SYS$MANAGER:OPERATOR.LOG I64 (2026-11-01 09:57, probes from 945bbbd, as `git pull` refused the rewritten history): privileged `ok open the operator log for shared reading`; NETMBX/TMPMBX `open: %RMS-E-FNF` and DIRECTORY `OPERATOR.LOG;21 insufficient privilege or object protection violation`; `[SYSTEM] (RWED,RWED,RE,)`; no `OPC$*` logicals. **Answered on both**: READALL or SYSPRV |
| 34 | Its record format and what a message looks like | VAR | VAR | probe_logs: record format line and the last records. x86: `record format VAR (2), longest record 0, attributes 02` (carriage return); 817 records; messages are a `%%%%%%%%%%%  OPCOM  31-OCT-2026 23:53:37.18  %%%%%%%%%%%` line, the text lines, and an empty record between messages; hourly `Logfile time stamp` I64: `record format VAR (2), longest record 0, attributes 02`, the same message layout (`%%%%%%%%%%%  OPCOM   1-NOV-2026 09:56:23.82  %%%%%%%%%%%`, `Message from user DECNET on MYI64`, text lines, empty record). But `4506208 records`, `OPERATOR.LOG;21 312425` blocks, one version since 17 October: the agent cannot read it from the start each interval (see 38). **Answered on both** |
| 35 | A new message appears on a stream already open (tail), or only after reopening | **reopen** | | probe_logs `--request`: `tailing works` or `tail by reopening`. x86 run 1 (945bbbd): marker seen neither after 5 s nor after reopening. x86 run 2 (b822d38, 09:53): `system() returned 1`; `on the open stream after 5 s: 0 new records` … `after 60 s: 0 new records`; `after reopening: 7 new records`, among them `%%%%%%%%%%%  OPCOM   1-NOV-2026 09:53:17.66` / `Message from user IAIN on X86VMS` / `_X86VMS$FTA5864:, vmsotel probe_logs marker 1793526797`; `ok the marker appeared after reopening: tail by reopening`. OPCOM wrote it within a second of the REQUEST, yet a stream opened before it never saw it in 60 s, so the agent must reopen the log each interval and skip what it has read (and start again when a new version appears). Run 1's miss is unexplained; it reopened after only 5 s. REPLY/STATUS from the probe's terminal gave `An illegal operator request was made` (the terminal is not enabled as an operator), so which classes the log takes is still unseen; REQUEST's class reaches it I64: the old probe (5 s, then reopen) saw the marker neither way, as x86's run 1 did, three runs out of three; round 2 not yet run there |
| 36 | The audit journal opens for shared reading; with what privilege | privileged only | privileged only | probe_logs `open the audit journal`; SHOW AUDIT/JOURNAL. x86: privileged `ok open the audit journal for shared reading`; unprivileged `open: %RMS-E-PRV` -- `SECURITY.AUDIT$JOURNAL;4 … [SYSTEM] (RWED,RWED,RE,)`. SHOW AUDIT/JOURNAL: `Destination: SYS$COMMON:[SYSMGR]SECURITY.AUDIT$JOURNAL`, `Monitoring: enabled`; unprivileged `%SYSTEM-F-NOSECURITY` I64: privileged `ok open the audit journal for shared reading`; unprivileged `open: %RMS-E-PRV`; `SECURITY.AUDIT$JOURNAL;3 … [SYSTEM] (RWED,RWED,RE,)`; SHOW AUDIT/JOURNAL the same destination, unprivileged `%SYSTEM-F-NOSECURITY`. **Answered on both** |
| 37 | Audit record format, sizes, and layout against NSADEF | **NSAMSGDEF fits** | | probe_logs hex dumps and its NSAMSGDEF decoder, beside the NSADEF lines LOGS prints. x86 run 1: `record format VAR (2), longest record 32767`; 127 records of 121 to 196 bytes. NSADEF is in SYS$STARLET_C.TLB, not SYS$LIB_C.TLB (`%LIBRAR-E-LOOKUPERR … SYS$LIB_C.TLB`): `NSA$C_MSG_HDR_LENGTH 20`, NSAMSGDEF = word type, word subtype, word flags, word packet count, word record size, byte spare, byte version, 3 spare words, word facility; packets are word size (including the 4-byte header, `NSA$C_PKT_HDR_LENGTH 4`), word type, data. x86 run 2 (b822d38): `decoded by NSAMSGDEF: 756 records consistent, 0 not`, `ok every record's packets add up to its NSA$W_RECORD_SIZE`. Packet codes from NSADEF match their contents: 6 `AUDIT_NAME` `SECURITY`, 3 `ALARM_NAME` `SECURITY`, 41 `PROCESS_ID` `1f ef 00 00`, 42 `PROCESS_NAME` `SSH$SSH_5627`, 56 `USERNAME` `SSH$SSH` or `<login>`, 48 `FINAL_STATUS` `f4 80 d3 00`, 45 `REMOTE_USERNAME` `ROOT`, 44 `REMOTE_NODENAME` `SSH_PASSWORD:80.94.95.115`, 49 `TERMINAL` `FTA6741:`, 43 `REMOTE_NODE_ID` (4 bytes), 50 `TIME_STAMP` (8-byte VMS time), 8 `SYSTEM_ID` (6 bytes), 7 `SYSTEM_NAME` `X86VMS`. Packet types are 1 to 282 (`NSA$C_PKT_MAX_CODE 283`) I64 (old probe, no decoder): `record format VAR (2), longest record 32767`; one record, 120 bytes, `02 00 0c 00 04 00 06 00 78 00 00 03 …` = type 2 (MSG_AUDIT), subtype 12, flags 4, 6 packets, size 0x78 = 120 = its length, version 3, then `0c 00 06 00 SECURITY`: the same header as x86. NSADEF again only in SYS$STARLET_C.TLB, with the same `NSA$C_MSG_` codes (`NSA$C_MSG_LOGFAIL 5`, `NSA$C_MSG_LOGIN 6`, `NSA$C_MSG_AUDIT 2`, up to `NSA$C_MSG_MAX_CODE 34`). A one-record journal does not test the decoder; round 2 there will decode it |
| 38 | On reopening the operator log, go straight back to the last record read by its RFA (rather than skip from the start) | | | probe_logs `--request`, after the marker: `ok reopened at the last record read, by its RFA`, or `FAIL go back to the last record by its RFA` and the status or the record found; and `reading them all took N s` for the first full read. Asked because I64's log holds 4.5 million records (34). Not yet run on either machine |

## Surprises

Anything the probes showed that no question anticipated goes here, with
what it changes in the design.

- **I64: the operator log is 160 MB and still growing fast.**
  `OPERATOR.LOG;21 312425` blocks (17 October), 4 506 208 records,
  growing to 312 500 blocks and 4 507 296 records in about four minutes
  (some 1 100 records). The last message before each run, at
  `09:56:23.82` and `10:01:23.82`, is
  `DECnet event 4.7, circuit down, circuit fault` / `Circuit EIA-0, Line
  synchronization lost`. Design: the agent must start at the end of the
  log, not the beginning, and resume by position (38); a filter for
  repeated events would help. Reported to the owner.

- **x86: `SYI$_BOOTTIME` is an hour out after a summer-time change.**
  probe_syi: `BOOTTIME … (2026-10-23 16:48:29 as written)`, `uptime
  703754 s (8 d 03:29)`. SHOW SYSTEM a second later: `Uptime 8
  04:28:06`. The machine booted on 23 October during British Summer
  Time; UK clocks went back on 25 October. BOOTTIME is local time *at
  boot*, so subtracting today's differential (0) makes boot an hour
  late. The remaining ~70 s is probably clock adjustment since boot
  (see the next item), as SHOW SYSTEM's uptime counts ticks. Design:
  boot time cannot be converted with the current differential. Run 2
  repeats it (`8 d 04:39` vs `8 05:38:37`) and shows the rule:
  `SYS$TIMEZONE_RULE  GMT0BST^1,M3.5.0/01,M10.4.0/02` — summer time,
  so the differential was 3600 at boot. Odd detail: `M10.4.0` is the
  *fourth* Sunday of October, not the last (`M10.5.0`); they agree in
  2026, not in years with five October Sundays.
- **x86: the VMS clock is 7–8 s behind the collector.** probe_otlp:
  `clock skew: ours minus collector's = -7 s`; TLS run `-8 s`. Which
  side is wrong is not yet known: check docker-nuc with `timedatectl`.
  If it is VMS, it needs NTP before the agent's timestamps can be
  trusted; the agent should report its skew as a metric.
- **x86: `$DEVICE_SCAN` for `DC$_DISK` returns LD devices.** `LDM0`
  (not mounted) and `LDM2585`, mounted, label `MD2627349A5V`, logical
  name `SYS$MD26273232629V` — the x86 boot memory disk, 89% full.
  SHOW DEVICE D does not list them because their names start with L.
  The agent has to decide whether to report LD volumes; probably yes,
  except this one.
- **Unmounted disks returned empty text items**, which the probe
  printed as a buffer of dots. Fixed in the probe; no design change.
- **The privileges line overflowed DCL**: `%DCL-W-TKNOVF, command
  element is too long`, because `WRITE` of `F$GETJPI(,"CURPRIV")` for a
  fully privileged process is too long. Now `SHOW PROCESS/PRIVILEGES`.
- **x86: `$GETRMI` writes the return length as a longword.** Runs 1–3
  crashed after the first call: the traceback's listing lines (`main
  24842` = source line 196) put the fault at the loop reading
  `rmi_items[1].code`, offset 0x14 from a table address that had become
  0. Run 4 put the return length in a static block between guard bytes:
  `GUARD changed: retlen+2=00 retlen+3=00` on every item, nothing else,
  privileged or not. So `$GETRMI` stores 4 bytes where item_list_3 has
  a word, and a 2-byte return length on the stack loses its neighbour.
  Design: give `$GETRMI` a longword return length. Not yet known
  whether I64 does the same. The other three services wrote words as
  far as their (unguarded) runs show.
- **x86: `RMI$_CPUINTSTK` is negative and falls.** Run 4a:
  `18446744073706407677 → 18446744073706407663`, i.e. −3 143 939 going
  down by 14 in 5 s; 4b by 20. Every other mode rises. It cannot be
  reported as a cumulative sum. Perhaps interrupt time on x86 is
  derived by subtraction and underflows, or it is meant as signed;
  either way interrupt time needs another source, or is dropped.
- **x86: `$GETRMI` state counts are not what MONITOR STATES shows.**
  Run 5: `CUR 2`, `LEF 13 → 12`, `HIB 28 → 29`, `PROCS 39`. MONITOR
  STATES a minute later: `Local Event Flag Wait 13`, `Hibernate 26`,
  `Current Process 1`, total 40, which matches SHOW SYSTEM (26 HIB,
  13 LEF, 1 CUR, 40 lines). So raw `$GETRMI` counts one more CUR and
  two more HIB than MONITOR and SHOW SYSTEM, and `PROCS` is one fewer
  than either. MONITOR evidently corrects the raw counts in a way not
  yet known — perhaps leaving out per-CPU system processes that SHOW
  SYSTEM does not list. Design: `process.state` from raw `$GETRMI`
  would disagree with every VMS tool an administrator checks it
  against. Needs the I64 numbers before choosing.
- **I64: the clone is unreadable without privilege.** In the
  NETMBX/TMPMBX subprocess: `%DCL-E-OPENIN, error opening
  DKA800:[CODE.vmsotel]build_probes.COM; as input`, `-RMS-E-PRV`. The
  same steps worked on x86. Confirmed cause: `CODE.DIR;1  [1,1]` and
  `build_probes.com;1  [1,1]  (RWD,RWD,R,)` — the clone took the owner
  of `[CODE]`, and WORLD has no access. After `SET SECURITY/OWNER=[IAIN]`
  it read `[CYGNET,IAIN]` and the unprivileged run worked. For the
  agent: its install must set owner and protection explicitly.
- **x86: the audit journal's newest records are login failures.** All
  of the last six are type 5 (`NSA$C_MSG_LOGFAIL`), subtypes 4 and 5,
  flags `1c`, naming terminals `FTA6574:`, `FTA6575:`, `FTA6576:` and a
  process `SSH$SSH_…` -- SSH logins failing in a run. Not a finding
  about the agent; reported to the owner (ANALYZE/AUDIT/EVENT=LOGFAIL
  shows who and from where). Run 2, nine hours later, decoded them:
  the journal had grown from 127 to 756 records, and the failures are
  password attempts from public addresses (`SSH_PASSWORD:176.53.159.197`
  as `USER`, `SSH_PASSWORD:80.94.95.115` as `ROOT`). x86's SSH port is
  reachable from the internet and being guessed at continually.
- **x86, 2c self-test: the HIB excess varies.** `cur 1 lef 13 hib 29`
  against MONITOR STATES `Hibernate 26`, `Local Event Flag Wait 13`,
  `Current Process 1` a few seconds later: three extra HIB this time,
  two before. Still x86 only.
- **I64: `$GETRMI` state counts match MONITOR STATES exactly.** Second
  sample `LEF 15`, `HIB 26`, `CUR 1`; MONITOR STATES `15`, `26`, `1`;
  SHOW SYSTEM 15 LEF, 26 HIB, 1 CUR, 42 lines. Run 2 again: `CUR 1`,
  `LEF 16`, `HIB 26` = 43 = SHOW SYSTEM's 43, `PROCS 42`. So the extra CUR and two
  HIB on x86 are x86's (2 CPUs, or the x86 port). On both machines
  `PROCS` is one fewer than the total (I64 41 vs 42, x86 39 vs 40), so
  `PROCS` is not "all processes".
- **I64: `RMI$_CPUINTSTK` is an ordinary counter** (`695781`, steady).
  The negative, falling value is x86-only.
- **The collector's own clock shows the skew too.** Each data point
  is stamped with the VMS time and logged with docker-nuc's arrival
  time: x86 `Timestamp: 2026-10-31 20:17:49` logged at `20:17:56.276`
  (7 s); I64 `21:40:44` logged at `21:40:58.391` (14 s). A dashboard
  would place every VMS point that far in the past.
- **`host.name` came from `gethostname`, not `SYI$_NODENAME`.** The
  probe sent `x86vms.issinoho.com` and `myi64.issinoho.com`, the TCP/IP
  names; the design's `SYI$_NODENAME` would send `X86VMS` and `MYI64`.
  See design.md, Resource attributes.
- **Gauges show `StartTimestamp: 1970-01-01`** in the debug output
  because the probe sends none, which OTLP allows for gauges. Not a
  fault; noted so it is not mistaken for one.
- **Settled: the VMS clocks are wrong, not docker-nuc.** 21:57 UTC,
  from the development laptop (`System clock synchronized: yes`,
  `NTP service: active`): three POSTs to the collector gave `laptop
  minus collector: 0 s` each time. So x86 is 7–8 s slow and I64 13–14 s.
  x86 shows no NTP process in SHOW SYSTEM; I64 runs `TCPIP$NTP_1` but
  is not keeping time.
- **The two VMS clocks disagree with each other.** I64 is 13–14 s
  behind the collector (`-14 s`, `-13 s`), x86 7–8 s, so at least one
  VMS clock is wrong whatever docker-nuc says. I64 runs NTP
  (`TCPIP$NTP_1` in SHOW SYSTEM) and is still the further off.
- **I64: uptime shows the same hour.** `uptime 1203734 s (13 d 22:22)`
  against SHOW SYSTEM `Uptime 13 23:22:57`; booted 17 October, in
  summer time. **Answered on both**: `SYI$_BOOTTIME` is local time at
  boot.
- **I64: the system disk's `DVI$_OPCNT` is at 1 868 831 249** after 14
  days, a longword. It passes 2^31 within days (any signed handling
  breaks there) and wraps 2^32 in about 2½ weeks. Widening is needed
  in practice, not just in theory.
- **x86: `time()` ignores a process-level `SYS$TIMEZONE_DIFFERENTIAL`;
  `getenv` does not.** With the logical defined as 3600 in a
  subprocess, `getenv` returned `3600` and probe_syi failed its UTC
  check, while `time()` did not move. Design: the agent's startup
  clock check must translate the differential from `LNM$SYSTEM_TABLE`
  explicitly, not with `getenv`, or a process logical makes it refuse
  to start for nothing. The same run gives `boot time, UTC
  2026-10-23T15:48:29Z` with the summer differential, within about 70 s
  of what SHOW SYSTEM's uptime implies — consistent with the
  summer-time explanation.
