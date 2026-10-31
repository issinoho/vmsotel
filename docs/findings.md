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
| 19 | Collector shows the data points | | | collector output not pasted yet. I64: collector output not pasted either |
| 20 | libssl image name and family found | SSL3 | SSL3 | x86: `OpenSSL: SYS$LIBRARY:SSL3$LIBSSL_SHR32, SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32, headers SSL3$INCLUDE`; `OpenSSL 3.0.21 9 Jun 2026`. I64: `SYS$LIBRARY:SSL3$LIBSSL_SHR32 …`, `OpenSSL 3.0.22 25 Aug 2026`. **Answered on both** |
| 21 | TLS handshake; version and cipher | yes | yes | x86: `negotiated TLSv1.3, TLS_AES_128_GCM_SHA256`, then HTTP 200. I64: `TLSv1.3, TLS_AES_128_GCM_SHA256`, HTTP 200. **Answered on both** |
| 22 | Certificate verifies with `--cafile` | | | optional, not run |

## Surprises

Anything the probes showed that no question anticipated goes here, with
what it changes in the design.

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
- **I64: `$GETRMI` state counts match MONITOR STATES exactly.** Second
  sample `LEF 15`, `HIB 26`, `CUR 1`; MONITOR STATES `15`, `26`, `1`;
  SHOW SYSTEM 15 LEF, 26 HIB, 1 CUR, 42 lines. Run 2 again: `CUR 1`,
  `LEF 16`, `HIB 26` = 43 = SHOW SYSTEM's 43, `PROCS 42`. So the extra CUR and two
  HIB on x86 are x86's (2 CPUs, or the x86 port). On both machines
  `PROCS` is one fewer than the total (I64 41 vs 42, x86 39 vs 40), so
  `PROCS` is not "all processes".
- **I64: `RMI$_CPUINTSTK` is an ordinary counter** (`695781`, steady).
  The negative, falling value is x86-only.
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
