# vmsotel — design draft

An agent that reports OpenVMS host metrics to an OpenTelemetry
Collector. It began inside vmsguard and shares that project's approach,
but none of its code.

Status: **draft, nothing built.** Every claim below about what VMS
returns is a question for `probes/` until the probes have been run on
both machines. They are marked *(probe)*.

## Goal

A VMS system shows up in Grafana, Prometheus, Datadog or Splunk the
same way a Linux host running the collector's `hostmetrics` receiver
does: CPU by mode, memory, paging, processes, disks, uptime. The same
dashboards should mostly just work.

### Not in the first version

- Tracing or an instrumentation library for applications (later, see
  Phases)
- Logs (OPCOM, audit) — phase 3
- Pulling metrics from other cluster members. Each node runs its own
  agent and reports itself.
- Receiving anything. The agent only pushes.
- Prometheus scrape endpoint. Pushing OTLP needs no listener on VMS and
  no firewall change, and the collector can expose Prometheus itself.

## Shape

```
 +------------------------ VMS node --------------------------+
 |                                                            |
 |  sources/        $GETRMI  $GETSYI  $GETJPI  $GETDVI         |
 |   (VMS only)        \        |        |        /           |
 |                      v       v        v       v            |
 |  model/          snapshot: plain structs, no VMS types     |
 |   (pure)                    |                              |
 |                             v                              |
 |  otlp/           JSON encoder -> ExportMetricsServiceRequest|
 |   (pure)                    |                              |
 |                             v                              |
 |  transport/      HTTP/1.1 POST over TCP, optional TLS       |
 |   (platform)                |                              |
 +-----------------------------|------------------------------+
                               v
                OpenTelemetry Collector (Linux), port 4318
                               |
               Prometheus / Grafana / Splunk / ...
```

It follows vmsguard's rules, for vmsguard's reasons:

- **The encoder and the model do no I/O** and have no VMS dependency, so
  they are tested exhaustively on Linux. That covers most of the code.
- **Sources are the only VMS-specific code**, behind an interface that
  fills a snapshot struct. On Linux, a fake source fills it with
  scripted values, so the whole agent runs end to end against a real
  collector without a VMS machine.
- **The transport goes through a small platform interface**: connect,
  send, receive, close, time, sleep. No `sockaddr` in the interface, so
  a `$QIO` version stays possible.
- **C99, VSI C**, the same compiler settings as vmsguard
  (`/STANDARD=C99 /PREFIX_LIBRARY_ENTRIES=ALL_ENTRIES`, and
  `_SOCKADDR_LEN`).
- **Three build files** from the start: `Makefile`, `build_vms.com`,
  `descrip.mms`.

### Why not opentelemetry-cpp

It needs C++14 (very likely out of reach for Itanium's compiler), and its OTLP
exporters pull in gRPC, protobuf, abseil and libcurl. The OTLP JSON
encoding over HTTP needs none of that: the payload is text, and the
collector does all the fan-out. The JSON form is a stable part of the
OTLP specification, not a debugging aid.

### Licensing

OpenTelemetry's specification and `.proto` files are Apache-2.0, so
unlike vmsguard there is no clean-room constraint: the message
definitions can be read and followed directly. The agent itself is
**MIT**, like vmsguard (`LICENSE`), which sits alongside Apache-2.0
without difficulty.

## The wire format

OTLP/HTTP, `POST /v1/metrics`, `Content-Type: application/json`, body
an `ExportMetricsServiceRequest` in the protobuf JSON mapping. The rules
that bite:

- field names are lowerCamelCase (`timeUnixNano`, `dataPoints`)
- enums are integers (`"aggregationTemporality": 2`)
- 64-bit integers, which covers every timestamp and every `asInt`, are
  **decimal strings**
- unknown fields are an error to a strict parser

The encoder (`src/otlp/`) writes into a caller-supplied buffer with
explicit length checks and no allocation. When the payload does not
fit it returns `VO_ESPACE` with the size it needs. Its tests compare
against golden payloads (`tests/golden/`) that `make check` parses with
protobuf's own JSON parser against the official schema
(`tools/fake_collector.py --validate`, with the `opentelemetry-proto`
package). That check is a development-time tool; nothing on VMS needs
Python.

Two choices the encoder makes:

- **Text is escaped as Latin-1.** VMS names and labels are DEC MCS,
  not UTF-8, and protobuf's parser rejects invalid UTF-8. Every byte
  above 127 is written as `\u00XX`, which is right for every printable
  DEC MCS character.
- **CPU seconds are written exactly.** 10 ms ticks become `ticks/100`
  with two decimals, built from integers, so no floating-point
  formatting is involved on either architecture.

Responses (`src/transport/http.c`): a 2xx is success, but the body may
carry `partialSuccess` with `rejectedDataPoints`, which is logged. 429,
any 5xx, and no answer at all are retried, keeping the payload; any
other status means the request itself is wrong, so the payload is
dropped and logged loudly, not retried.

Retries back off from 1 s, doubling, but never past the collection
interval, so a long outage costs one attempt per interval. A
`Retry-After` (seconds, or an HTTP date) longer than that wins, up to an
hour.

Each export is its own connection with `Connection: close`. At one
request a minute that costs nothing worth saving, and it leaves no idle
connection for a firewall or a collector restart to break.

No compression in version 1. A full host export is a few kilobytes
every 30 to 60 seconds; gzip can come later if zlib proves to be
available on both machines.

## Time

OTLP timestamps are Unix nanoseconds, UTC. VMS system time is **local**
time. If the C RTL's `time()` follows the timezone differential, it is
UTC and usable directly; if a system's timezone is misconfigured, every
point lands hours out and is silently mis-graphed, which is worse than
an error. *(probe: `probe_syi` checks `time()` against
`$GETTIM − SYS$TIMEZONE_DIFFERENTIAL`; `probe_otlp` measures skew
against the collector's `Date` header.)*

The agent will compare the two at startup and refuse to run if they
disagree by more than a few seconds, saying why.

That check cannot see a VMS clock that is simply wrong, and both test
machines' were: 7 and 13 s slow against an NTP-synced collector
(`docs/findings.md`). So the agent also reads the `Date` header of every
collector response, as `probe_otlp` does, reports the difference as its
own metric (`vmsotel.clock.skew`, seconds), and logs a warning once
when it passes 2 s and once when it comes back. It does not correct its timestamps: the fix is NTP
on the VMS node, and a silent correction would hide that.

The differential for that check is translated from `LNM$SYSTEM_TABLE`,
not read with `getenv`: on x86 a process-level
`SYS$TIMEZONE_DIFFERENTIAL` changed what `getenv` saw but not what
`time()` returned, so `getenv` would make the check fail spuriously.

## Counters

Every VMS counter of interest is cumulative since boot (or process
start), so the metrics are OTLP **cumulative sums**. The agent stores no
history and computes no rates; the backend does that. `startTimeUnixNano`
is the boot time from `SYI$_BOOTTIME` for system counters, and
`JPI$_LOGINTIM` for per-process ones.

**Boot time is local time at boot, not now.** On both machines it came
out an hour away from SHOW SYSTEM's uptime, because summer time ended
after they booted (`docs/findings.md`, Surprises). Converting it with
today's `SYS$TIMEZONE_DIFFERENTIAL` is wrong across a change. Both hold
the POSIX-style rule `GMT0BST^1,M3.5.0/01,M10.4.0/02` in
`SYS$TIMEZONE_RULE`, so one option is to apply that rule to the boot
date; the other is an uptime that counts ticks, as SHOW SYSTEM's does.
*(probe: an unprivileged tick-based uptime)*

As built, the VMS source converts it once, at startup, with the
differential in force then -- the right one when the agent starts at
boot, as it will from `SYSTARTUP_VMS.COM`. It is never re-read, so a
summer-time change while the agent runs cannot move the start time and
make every counter look reset. The same applies to `JPI$_LOGINTIM`.
A start time an hour out does not corrupt rates, but `system.uptime`
would be wrong.

**Longword counters wrap.** A 32-bit count of 10 ms ticks wraps after
497 days, and if the system-wide CPU totals sum over all CPUs, a
four-CPU system wraps after about 124 days of uptime. VMS systems run that long
routinely. The agent widens each 32-bit counter to 64 bits by adding
2^32 whenever a reading is lower than the last. A restart of the agent
loses the widening, so after a restart it reports a new start time
instead of a value that has gone backwards.

As built (`src/model/`, `vo_wide_update`): a widened counter's start
time is when the agent first saw it. A reading lower than the last by
2^31 or more is a wrap; a smaller drop is a reset (a disk remounted,
say), which zeroes the widening and takes a new start time. Disk
counters are widened per device name, in a table of 64; a disk beyond
that is reported without `system.disk.operations`. A quadword CPU mode
counter that goes backwards (x86's interrupt counter) is dropped for
the life of the agent, and reported once so the agent can log it.

This is not hypothetical. On both machines only the `RMI$_CPU*` modes,
`FRLIST` and `MODLIST` are quadwords; every other `RMI$_` counter, and
`DVI$_OPCNT`, is a longword. The Itanium's system disk had counted
1 868 831 249 operations in 14 days, so it passes 2^31 within days and
wraps 2^32 in about two and a half weeks. Every longword is read as
unsigned.

## Metrics, version 1

Names follow the OpenTelemetry semantic conventions for `system.*`
where one fits, so that existing host dashboards work. The conventions
have been renaming attributes (`state` became `cpu.mode`, and so on), so
the set is pinned: **semantic conventions v1.44.0** (released
2026-08-04), schema URL `https://opentelemetry.io/schemas/1.44.0`,
`VO_SEMCONV_VERSION` in `src/otlp/otlp_json.h`.

Types follow the convention's instrument: a *counter* is a monotonic
cumulative sum, an *updowncounter* a non-monotonic cumulative sum, and
only `system.uptime` is a gauge. That is how the collector's own
`hostmetrics` receiver sends them, so dashboards built on it read these
the same way. Up-down counters carry the boot time as their start
time; widened counters carry their own (see Counters).

| Metric | Type | Unit | Source | Notes |
| --- | --- | --- | --- | --- |
| `system.cpu.time` | counter | s | `RMI$_CPU*` | attribute `cpu.mode`; see mapping below. Quadword 10 ms ticks on both machines, summed over all CPUs (shown on x86; I64 has one CPU) |
| `system.cpu.logical.count` | updowncounter | {cpu} | `SYI$_ACTIVECPU_CNT` | |
| `system.memory.usage` | updowncounter | By | `RMI$_FRLIST`, `RMI$_MODLIST`, `SYI$_MEMSIZE` | `system.memory.state` = free, used, `modified` (VMS-specific: the modified page list); used = total − free − modified, never below 0. All three in pages of `SYI$_PAGE_SIZE`, matched with SHOW MEMORY/PHYSICAL on both machines |
| `system.memory.limit` | updowncounter | By | `SYI$_MEMSIZE` × `SYI$_PAGE_SIZE` | |
| `system.paging.faults` | counter | {fault} | `RMI$_FAULTS`, `RMI$_PREADS` | `system.paging.fault.type` = major (page reads), minor (the rest, with the later of the two start times) |
| `system.paging.operations` | counter | {operation} | `RMI$_PREADIO`, `RMI$_PWRITIO` | `system.paging.direction` = in, out |
| `system.paging.usage` | updowncounter | By | `SYI$_PAGEFILE_PAGE`, `SYI$_PAGEFILE_FREE` | `system.paging.state` = used, free. Both longwords, in pages, on both machines. FREE is space never written: on x86 SHOW MEMORY/FILES showed all 233216 pages free with 61612 committed, so `used` is not "committed". Left out when no page file is installed |
| `system.process.count` | updowncounter | {process} | `RMI$_COM`, `LEF`, `HIB`, … | one series per VMS state, each with `process.state` (semconv: COM, COMO, CUR → running; SUSP, SUSPO → stopped; the rest → sleeping) and `openvms.process.state` (the VMS name, lower-cased), so a stock dashboard sums by the first and an administrator reads the second. On I64 the raw counts equal MONITOR STATES; on x86 they show one more CUR and two more HIB. `PROCS` is one short of the total on both, so it is not used. *(open: what x86's extras are)* |
| `system.uptime` | gauge | s | `SYI$_BOOTTIME` | whole seconds, as a double |
| `system.filesystem.usage` | updowncounter | By | `DVI$_FREEBLOCKS`, `DVI$_MAXBLOCK` | per mounted disk; 512-byte blocks; `system.device`, `system.filesystem.mountpoint` = the logical volume name (`DISK$X86SYS`), `system.filesystem.state` = used, free |
| `system.disk.operations` | counter | {operation} | `DVI$_OPCNT` | per disk, widened per device name; no read/write split exists, so no `disk.io.direction` |
| `openvms.io.direct` | counter | {operation} | `RMI$_DIRIO` | no semconv equivalent |
| `openvms.io.buffered` | counter | {operation} | `RMI$_BUFIO` | |

Per-process metrics (`process.cpu.time`, `process.memory.usage`) come
in phase 2, filtered by name. A wildcard scan of every process every
interval, with a series per PID, is the cardinality mistake that gets
an agent switched off.

### Phase 2: chosen processes

As built (`src/model/procfilter.c`, `read_procs` in the VMS source):

- **Choosing.** `process.name` and `process.user` patterns in the
  configuration, VMS wildcards (`*`, `%`), case ignored, up to 16 of
  each; a process is chosen when any matches. None, the default, means
  no process scan at all. At most 64 are reported; more is logged once.
- **Reading.** One wildcard `$GETJPIW` scan per collection with one
  item list -- `PID`, `PRCNAM`, `USERNAME`, `CPUTIM`, `PPGCNT`,
  `GPGCNT` -- exactly as probe_jpi ran it on both machines. All come
  from the process's control block, so no AST goes to the process.
  `JPI$_IMAGNAME` would need one, has not been probed for other
  processes, and is left out, so there is no
  `process.executable.name`.
- **Privilege.** Other accounts' processes need WORLD (findings 12);
  without it they come back `SS$_NOPRIV`, are counted, and the agent
  logs the count once. `VMSOTEL_START.COM` grants WORLD only when a
  configuration line begins `process.`.
- **Metrics.** Each chosen process is its own resource, as the
  collector's hostmetrics receiver sends them: the host's attributes
  plus `process.pid` (an int), `openvms.process.id` (the hexadecimal PID
  SHOW SYSTEM prints), `openvms.process.name` and `process.owner`.
  `process.cpu.time` is `JPI$_CPUTIM`, 10 ms ticks (findings 31), total
  only: VMS keeps no user/system split per process, so there is no
  `cpu.mode`. It is a longword, widened by PID like the other counters,
  starting when the agent first saw the process. `process.memory.usage`
  is `PPGCNT + GPGCNT` pagelets × 512 (findings 11).

### CPU modes

| VMS mode | `cpu.mode` |
| --- | --- |
| user | `user` |
| kernel | `kernel` |
| executive | `executive` |
| supervisor | `supervisor` |
| interrupt stack | `interrupt` — a normal counter on I64; negative and falling on x86, so dropped there |
| MP synchronisation | `mpsynch` |
| compatibility | `compat` (always zero after VAX; dropped if so) |
| idle | `idle` |

**`$GETRMI` returns lengths as longwords.** Its item list has the
item_list_3 shape, but the return-length field it writes is 4 bytes
(x86, `docs/findings.md`). The agent gives it a longword. The `CPU*`
modes, `FRLIST` and `MODLIST` are quadwords; every other item used here
is a longword and refuses an 8-byte buffer with `SS$_BADPARAM`, so the
longword counters need the 2^32 widening above.

`user`, `kernel`, `interrupt` and `idle` are standard values. The others
are VMS's own and are kept rather than folded into `system`, because the
split between kernel and executive time is what a VMS administrator
actually reads in MONITOR.

### Resource attributes

| Attribute | Source |
| --- | --- |
| `service.name` | `vmsotel` |
| `host.name` | `gethostname()`, the TCP/IP name (`x86vms.issinoho.com` in the probes), falling back to `SYI$_NODENAME`. Matches what DNS and other hosts' agents report; the SCS name stays in `openvms.scsnode` |
| `host.arch` | `SYI$_ARCH_NAME` → `amd64` / `ia64` |
| `host.type` | `SYI$_HW_NAME` |
| `os.type` | `openvms` (not in the semconv enum; custom values are allowed) |
| `os.version` | `SYI$_VERSION`, trimmed |
| `openvms.cluster.member` | `SYI$_CLUSTER_MEMBER` |
| `openvms.scsnode` | `SYI$_SCSNODE` |

### Disks and shadow sets

Shadow-set members are skipped and the virtual unit (`DSAn:`) is
reported, or a mirrored volume counts twice. Unmounted disks are
skipped. *(probe: `probe_dvi` prints the shadow items per disk.)*

`$DEVICE_SCAN` by `DC$_DISK` also returns LD devices (x86: `LDM0`, and
`LDM2585`, the boot memory disk `SYS$MD…`), which SHOW DEVICE D does
not list. LD volumes a user mounted are real filesystems and are
reported; the memory disk is not.

As built (`src/sources/vms/vms_source.c`): a disk is reported when
`DVI$_MNT` is set, `DVI$_SHDW_MEMBER` is not, and its logical volume
name does not begin `SYS$MD`. The device is the scanned name without
its leading underscore and trailing colon (`X86VMS$DKA0`); the mount
point is the logical volume name. Neither machine has a shadow set, so
the member test has only been seen to pass disks through.

`DVI$_FREEBLOCKS` and `DVI$_MAXBLOCK` are longwords: correct up to 2
TiB per volume. `DVI$_VOLSIZE` is a longword too on both machines, so it
does not help; volumes over 2 TiB are out of scope until a 64-bit item
is found.

## Running it

- A detached process, started from `SYSTARTUP_VMS.COM` the same way
  vmsguard's gateway is (`RUN/DETACHED`, output to a log file).
- Configuration in a small `key = value` file (`src/agent/config.c`,
  `vmsotel.conf.example`): host, port, path, tls, cafile, interval
  (default 60 s), timeout, queue, max_payload, and `resource.*` extra
  attributes. The process filter comes with phase 2.
- **Privilege**: none. `$GETRMI` needs none on either machine.
  Per-process metrics need WORLD: an unprivileged scan saw only its own
  user's processes (x86 3 of 41, I64 3 of 43). That is phase 2 and
  opt-in.
- **Install**: owner and protection are set explicitly. On I64 a git
  clone made with privileges took the owner of its parent directory,
  `[1,1]`, and could not be read once they were dropped.
- Collection takes a snapshot, encodes it, sends it. If the collector is
  unreachable, a bounded queue keeps the last N exports (default 10) and
  drops the oldest. A VMS production box must never fill its disk or
  its memory because a monitoring server is down.
- The agent's own health goes in its own metrics, beside the system's:
  `vmsotel.exports` by `vmsotel.outcome` (sent; failed, an attempt that
  will be retried; dropped, a payload lost to a full queue, a rejection
  or `max_payload`), `vmsotel.queue.length`,
  `vmsotel.collection.duration` and `vmsotel.clock.skew`.
- The log is standard output, one line per event, UTC: start, failures
  and their reasons, drops, a CPU mode given up, clock warnings.

## TLS

Through the installed OpenSSL, 3.x or 1.1.1, with the same care over
the `OPENSSL` logical and pointer size as vmsguard. Every call
`src/transport/tls.c` makes exists in both, so there is no version
branch; what it keeps from vmsguard is the check that the headers match
the image linked (`VO_EXPECT_OPENSSL_1` or `_3`, set by the build),
which turns a mismatch into a compile error.

OpenSSL never sees a socket. It reads and writes memory BIOs, and
`tls.c` moves the bytes through the platform interface, so a `$QIO`
implementation of that interface would carry TLS unchanged. None
exists yet; POSIX sockets are what has run.

The certificate is always verified, against `cafile` or the system's
default CAs, and the host name or address checked (an address against
an IP SAN, and never sent as SNI). There is no option to skip that.
TLS 1.2 is the minimum. *(probe: `--cafile` verification on VMS,
findings question 22)*

## Testing

| Layer | Where | How |
| --- | --- | --- |
| encoder | Linux | unit tests; output checked against the official schema |
| model, counter widening | Linux | unit tests, including a wrap at 2^32 |
| agent end to end | Linux | fake source → real collector |
| sources | VMS | self-test that prints each value beside its MONITOR / SHOW equivalent |
| whole thing | VMS | `MONITOR SYSTEM` beside the Grafana panel |

The rule from vmsguard holds: watch both ends. A 200 from the
collector proves only that the JSON parsed. The collector's debug output
and the numbers in MONITOR are the other end.

## Phases

0. **Probes** — `probes/`, this draft. Answer the *(probe)* questions on
   both machines.
1. **System metrics** — the table above, plain HTTP and TLS, running
   detached.
2. **Per-process metrics** — filtered by process name or username;
   needs WORLD.
3. **Logs** — OPCOM messages and the audit journal as OTLP logs
   (`/v1/logs`). Needs its own probes, now written (`probe_logs`,
   `build_probes LOGS`, findings 33–37). They try the least invasive
   route first: reading the operator log and the audit journal as
   files, shared, while OPCOM and the audit server write them. The
   alternatives the first draft named -- a mailbox enabled as an
   operator for OPCOM, and the audit server's listener mailbox -- need
   OPER, SECURITY and a change to the system's audit configuration
   (`SET AUDIT/LISTENER`), so they are held back unless the files
   cannot do. *(probe: findings 33–37)*
4. **Application instrumentation** — a small C library VMS applications
   link to emit their own metrics and spans through the same transport.

## Open questions

| Question | Answered by |
| --- | --- |
| ~~Which `RMI$_` items exist, and their sizes~~ — all 38, same sizes on both | `probe_rmi` |
| ~~Are the CPU mode counters 10 ms ticks summed over all CPUs~~ — yes | `probe_rmi` |
| ~~Does `$GETRMI` need privilege~~ — no, on both | `probe_rmi` run unprivileged |
| Is `time()` UTC — needs a nonzero differential to tell | `probe_syi`, `probe_otlp` |
| ~~`JPI$_PPGCNT`: pages or pagelets~~ — pagelets on both | `probe_jpi` beside `SHOW SYSTEM` |
| ~~How many processes an unprivileged scan sees~~ — the user's own, on both | `probe_jpi` |
| ~~Does `$DEVICE_SCAN` by class find every disk~~ — yes, plus LD devices on x86; shadow items accepted but untested | `probe_dvi` beside `SHOW DEVICE D` |
| ~~Does `printf` support `%llu`~~ — yes | every probe's first line |
| ~~TCP and an HTTP exchange to the collector~~ — yes, and every point decoded on the collector | `probe_otlp` |
| ~~libssl image names; TLS handshake~~ — SSL3 (3.0.21 / 3.0.22), TLS 1.3 | `probe_otlp_tls`, `build_probes.com` |
| ~~Which clock is wrong~~ — the VMS ones: x86 7 s, I64 13 s slow | laptop vs collector `Date` |
| Is zlib available (gzip) | later |
