# Probe findings

One row per question per machine. Quote the line of output that settles
it. A question is answered only when both machines have a row.

Machines: **x86** = E9.2-4 on QEMU ("QEMU Standard PC (Q35 + ICH9,
2009)"), 2 CPUs, VSI C `__DECC_VER 70790003`. **I64** = rx2660,
V8.4-2L3, VSI C V7.4-001.

Runs so far, all **x86, 2026-10-31**, user IAIN:
1. 20:17, probes from 2be172f, nearly every privilege enabled.
2. 21:28, probes from 7f5ebdb, same privileges (`Process privileges:`
   ACNT … WORLD).
3. 21:28, probes from 7f5ebdb, in a subprocess with
   `Process privileges: NETMBX TMPMBX` only.

Collector output on docker-nuc not yet pasted.

| # | Question | x86 | I64 | Evidence / notes |
| --- | --- | --- | --- | --- |
| 1 | All six probes build; no LINK warnings | yes | | x86: `built:  probe_syi probe_rmi probe_jpi probe_dvi probe_otlp probe_otlp_tls`, no FAILED line |
| 2 | `printf("%llu")` works | yes | | x86: `printf %llu check: 12345678901` |
| 3 | Item-list layout is 12 bytes | yes | | x86: `item list entry: 12 bytes, pointers 4 bytes` |
| 4 | `SYI$_` items: which are declared, which accepted | all but one | | x86: every item accepted except `SYI$_FREE_PAGES  not declared in <syidef.h>`. Not needed: free memory comes from `RMI$_FRLIST`. `HW_MODEL` 2 bytes, `CLUSTER_MEMBER` 1, `CLUSTER_NODES` 2, `MAXPROCESSCNT` 2 — not all longwords |
| 5 | `time()` is UTC | not settled | | x86: `time()` = `$GETTIM` = `$GETTIM minus differential` = `2026-10-31T20:17:43Z`, but the differential is 0 (GMT), so local time and UTC are the same and this cannot tell them apart. Needs a run while a nonzero differential is in force |
| 6 | `SYS$TIMEZONE_DIFFERENTIAL` defined and correct | defined | | x86: `SYS$TIMEZONE_DIFFERENTIAL  0`, `SYS$TIMEZONE_NAME  GMT`. Correct for UK winter; see Surprises on boot time |
| 7 | `RMI$_` items: declared, accepted, byte size | crashed | | x86 runs 1–3: ACCVIO at `virtual address=…0014` straight after the first call, `querying RMI$_CPUINTSTK` (all three runs), with a 4 KB static data buffer too. See Surprises. Probe now keeps IOSB and return length off the stack, guarded; rerun |
| 8 | CPU mode counters are 10 ms ticks summed over CPUs | crashed | | as 7 |
| 9 | `$GETRMI` works unprivileged | not settled | | x86 run 3 (NETMBX, TMPMBX): the call got far enough to crash the same way, so it was not refused outright; values not seen |
| 10 | Which `RMI$_` items are counters and which gauges | crashed | | as 7 |
| 11 | `JPI$_PPGCNT`/`GPGCNT` are pages or pagelets | **pagelets** | | x86: `MARIADBD_3306 … 209664` vs SHOW SYSTEM Pages `13104`; `LTPHP_19000 62464` vs `3904`. Ratio exactly 16 = 8192 / 512. SHOW SYSTEM's column is pages |
| 12 | Processes visible unprivileged vs `SHOW SYSTEM` | **own only** | | x86 run 3: `processes returned  3`, `NOPRIV  38` — IAIN's three processes; 3 + 38 = 41 = SHOW SYSTEM's 41 lines. Privileged run 2: `40`, `NOPRIV 0` = SHOW SYSTEM's 40. The scan does not stop at NOPRIV, it skips |
| 13 | Full `$GETJPI` scan cost | 1–2 ms | | x86: `scan took 2.0 ms` (42 processes), `1.0 ms` (40), `1.0 ms` unprivileged |
| 14 | `$DEVICE_SCAN` finds every disk in `SHOW DEVICE D` | yes, and more | | x86: `7 disks found` — all five of SHOW DEVICE D (DMM0, DKA0, DKA100, DKA200, DKA300) plus `LDM0` and `LDM2585`; see Surprises |
| 15 | `DVI$_FREEBLOCKS` matches `SHOW DEVICE D` | yes | | x86 run 2, exact on all three mounted D disks: `20008384`, `1132080`/`1132064` (DKA200, busy), `7313558`; run 3 `1131408` = `1131408`. Unprivileged too |
| 16 | Shadow-set items available | accepted; untested | | x86: `SHDW_MASTER`, `SHDW_MEMBER` accepted, 1 byte, all 0; `SHDW_MASTER_NAME` 0 bytes. No shadow sets on this system, so behaviour on a member is unseen |
| 17 | `DVI$_OPCNT` is cumulative | yes | | x86: `DKA0: 6460564 -> 6460716 (+152)`, `DKA300: 158705019 -> 158705083 (+64)` over 5 s. Longword |
| 18 | TCP and HTTP exchange to the collector; HTTP 200 | yes | | x86: `ok TCP connect`, `HTTP/1.1 200 OK`, body `{"partialSuccess":{}}` |
| 19 | Collector shows the data points | | | collector output not pasted yet |
| 20 | libssl image name and family found | SSL3 | | x86: `OpenSSL: SYS$LIBRARY:SSL3$LIBSSL_SHR32, SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32, headers SSL3$INCLUDE`; `OpenSSL 3.0.21 9 Jun 2026` |
| 21 | TLS handshake; version and cipher | yes | | x86: `negotiated TLSv1.3, TLS_AES_128_GCM_SHA256`, then HTTP 200 |
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
- **x86: `$GETRMI` returns, then the caller's state is gone.** Mapping
  the listing line numbers in the traceback (`sample 24771`, `main
  24842` = source line 196) puts the fault at probe_rmi.c line 125,
  `rmi_items[i].code`, on the iteration after the first call. With
  4-byte pointers that field of entry 1 is at offset 0x14, so the
  table's address had become 0. The 4 KB static buffer did not help, so
  the data buffer is not what was overrun. Candidates: the IOSB or the
  return length written wider than 8 or 2 bytes onto the stack, or a
  register not preserved across the call. The probe now separates the
  two: guarded static IOSB and return length, raw results printed
  before the loop resumes.
