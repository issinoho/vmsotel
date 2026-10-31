# Probe findings

One row per question per machine. Quote the line of output that settles
it. A question is answered only when both machines have a row.

Machines: **x86** = E9.2-4 on QEMU ("QEMU Standard PC (Q35 + ICH9,
2009)"), 2 CPUs, VSI C `__DECC_VER 70790003`. **I64** = rx2660,
V8.4-2L3, VSI C V7.4-001.

Runs so far: **x86, 2026-10-31**, from user IAIN *with nearly every
privilege enabled* (the privilege line listed CMKRNL … WORLD … READALL
before it overflowed), so it does not answer the unprivileged
questions (9, 12). Probes from commit 2be172f. Collector output on
docker-nuc not yet pasted.

| # | Question | x86 | I64 | Evidence / notes |
| --- | --- | --- | --- | --- |
| 1 | All six probes build; no LINK warnings | yes | | x86: `built:  probe_syi probe_rmi probe_jpi probe_dvi probe_otlp probe_otlp_tls`, no FAILED line |
| 2 | `printf("%llu")` works | yes | | x86: `printf %llu check: 12345678901` |
| 3 | Item-list layout is 12 bytes | yes | | x86: `item list entry: 12 bytes, pointers 4 bytes` |
| 4 | `SYI$_` items: which are declared, which accepted | all but one | | x86: every item accepted except `SYI$_FREE_PAGES  not declared in <syidef.h>`. Not needed: free memory comes from `RMI$_FRLIST`. `HW_MODEL` 2 bytes, `CLUSTER_MEMBER` 1, `CLUSTER_NODES` 2, `MAXPROCESSCNT` 2 — not all longwords |
| 5 | `time()` is UTC | not settled | | x86: `time()` = `$GETTIM` = `$GETTIM minus differential` = `2026-10-31T20:17:43Z`, but the differential is 0 (GMT), so local time and UTC are the same and this cannot tell them apart. Needs a run while a nonzero differential is in force |
| 6 | `SYS$TIMEZONE_DIFFERENTIAL` defined and correct | defined | | x86: `SYS$TIMEZONE_DIFFERENTIAL  0`, `SYS$TIMEZONE_NAME  GMT`. Correct for UK winter; see Surprises on boot time |
| 7 | `RMI$_` items: declared, accepted, byte size | crashed | | x86: `probe_rmi` ACCVIO in `sample`, writing to `virtual address=…0014`, before printing anything. Probe now traces each item and catches overruns; rerun |
| 8 | CPU mode counters are 10 ms ticks summed over CPUs | crashed | | as 7 |
| 9 | `$GETRMI` works unprivileged | not run | | run was privileged, and crashed |
| 10 | Which `RMI$_` items are counters and which gauges | crashed | | as 7 |
| 11 | `JPI$_PPGCNT`/`GPGCNT` are pages or pagelets | **pagelets** | | x86: `MARIADBD_3306 … 209664` vs SHOW SYSTEM Pages `13104`; `LTPHP_19000 62464` vs `3904`. Ratio exactly 16 = 8192 / 512. SHOW SYSTEM's column is pages |
| 12 | Processes visible unprivileged vs `SHOW SYSTEM` | not run | | privileged run only: `processes returned  42`, `NOPRIV 0`; SHOW SYSTEM a moment later listed 41 |
| 13 | Full `$GETJPI` scan cost | 2.0 ms | | x86: `scan took  2.0 ms` for 42 processes, privileged |
| 14 | `$DEVICE_SCAN` finds every disk in `SHOW DEVICE D` | yes, and more | | x86: `7 disks found` — all five of SHOW DEVICE D (DMM0, DKA0, DKA100, DKA200, DKA300) plus `LDM0` and `LDM2585`; see Surprises |
| 15 | `DVI$_FREEBLOCKS` matches `SHOW DEVICE D` | yes | | x86: DKA200 `1133328` = `1133328`. DKA0 `20011552` vs `20011520`, DKA300 `7336282` vs `7336175`: busy volumes, read seconds apart |
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
  boot time cannot be converted with the current differential. Not yet
  shown: what `SYS$TIMEZONE_RULE` says (probe_syi now prints it).
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
