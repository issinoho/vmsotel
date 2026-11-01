# Running vmsotel

The operator's page: install, configure, start, stop, and what the log
says. vmsotel sends this system's CPU, memory, paging, process, disk and
I/O figures to an OpenTelemetry collector over OTLP/HTTP, every minute
by default. It needs no privilege beyond NETMBX and TMPMBX.

## Install

Two ways. **From the PCSI kit** (`ISSINOHO-<base>-VMSOTEL-...PCSI`),
the agent runs as an account of its own, never as SYSTEM:

```dcl
$ product install vmsotel /source=dev:[dir]
$ @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE
```

then set `host` in `VMSOTEL$ROOT:[CONF]VMSOTEL.CONF`, and start it with
`@VMSOTEL$ROOT:[COM]VMSOTEL_START`. `VMSOTEL$ROOT:[DOC]README.VMS` has
the rest, and `docs/design.md`, "The installation kit", the reasons.
The kit is built from a source tree by `@[.vmsotel.kit]MAKE_KIT` after
`build_vms`, into `[.vmsotel.KIT_<arch>]`. Everything else on this page
applies to both; in the kit, `VMSOTEL.CONF` is in `[CONF]`, the logs and
the stop file in `[LOG]`, and the procedures in `[COM]`.

**From a source tree**, which runs as whoever starts it. The tree can
live anywhere; everything below is found relative to it. From the
directory that is to hold it:

```dcl
$ git clone https://github.com/issinoho/vmsotel.git
$ @[.vmsotel]build_vms
```

`build_vms` ends with `built: vmsotel.exe vmsotel_tls.exe`. Anything
under `FAILED:` or `compiled with warnings:` should be looked at before
going on. To check the agent reads this system correctly, run
`@[.vmsotel]build_vms SELFTEST`: it prints every value beside the SHOW
or MONITOR command that shows the same number.

If the tree was cloned with privileges, check that the account that
will run the agent can read it: a clone takes the owner of the
directory it is made in. `SET SECURITY/OWNER=` fixes that.

## Configure

Create `VMSOTEL.CONF` in the vmsotel directory. One `key = value` per
line; `!` or `#` starts a comment. Only `host` is required:

```text
host = 192.168.0.218
port = 4318
interval = 60
```

`vmsotel.conf.example` lists every key: `port`, `path`, `tls`,
`cafile`, `interval`, `timeout`, `queue`, `max_payload`,
`resource.NAME = value` for extra attributes on every export, and
`process.name` and `process.user` for per-process metrics, and
`logs.opcom` and `logs.audit` for the operator log and the audit
journal (both below). With
`tls = yes` the collector's certificate is always verified, against
`cafile` if given; there is no setting to skip that.

### Per-process metrics

Name the processes to report one by one, by process name or by
username, with VMS wildcards (`*` any run, `%` one character, case
ignored). Each may be given up to 16 times; a process is chosen if any
pattern matches:

```text
process.name = MARIADBD*
process.name = OPCOM
process.user = LIGHTTPD
```

Each chosen process goes out as its own resource -- the host's
attributes plus `process.pid`, `openvms.process.id` (the hexadecimal
PID SHOW SYSTEM prints), `openvms.process.name` and `process.owner` --
with `process.cpu.time` (seconds; VMS keeps no user/system split per
process) and `process.memory.usage` (bytes of physical memory, SHOW
SYSTEM's Pages × 8192). At most 64 processes are reported; if more
match, the log says so once. Choose servers, not every process: a
series per PID for everything is what makes a monitoring system drop
an agent.

Other accounts' processes need WORLD. `VMSOTEL_START.COM` grants it
when a line of the configuration begins `process.`, and not otherwise;
commented-out lines do not count. Without
it the log says how many processes could not be read.

### Logs: the operator log and the audit journal

```text
logs.opcom = yes
logs.audit = yes
```

Each new OPCOM message and each new audit journal record goes to the
collector as an OTLP log record, at `logs.path` (default `/v1/logs`;
the collector needs a `logs` pipeline, as `collector/collector.yaml`
has). The files are `SYS$MANAGER:OPERATOR.LOG` and
`SYS$MANAGER:SECURITY.AUDIT$JOURNAL` unless `logs.opcom_file` or
`logs.audit_file` names others.

- **Privilege.** Both files are SYSTEM's and closed to the world; the
  agent needs READALL. `VMSOTEL_START.COM` grants it when `logs.opcom`
  or `logs.audit` is yes on a line that is not a comment.
- **Where it starts.** At the end of each file: what was there before
  the agent started is not sent. The first read finds the end by
  reading the whole file once -- 41 s for I64's 4.5 million records,
  logged as `starting at the end of …` -- and from then on the agent
  goes straight back to where it was, by RFA.
- **Delay.** OPCOM writes its log in batches minutes apart (findings,
  35), so an operator message reaches the collector up to about a
  quarter of an hour after it happened. Its `timeUnixNano` is OPCOM's
  own time for it, from its header line; `observedTimeUnixNano` is
  when the agent read it. The last message in the log is sent once
  it has been unchanged for one interval, since OPCOM writes nothing
  after a message to say it is complete.
- **Audit records** carry `openvms.audit.type` (`LOGFAIL`, `BREAKIN`,
  `LOGIN`, …), `openvms.audit.subtype`, and what the record holds of
  username, remote user and node, terminal, process and final status.
  Login failures and break-in attempts are WARN. The password packet
  and the command line are never sent. In a body, "remote user" is who
  asked from the far end -- the name an SSH client sent, or whoever ran
  SET HOST -- not the account tried. A name that matched no account is
  not journalled: `openvms.audit.username` is then `<login>`.
- **New versions** of either file are followed: once the version being
  read has nothing new, the agent moves to the highest, from its start.
- **Nothing is lost while the collector is away**: log records stay
  in the file until the agent has queued them, and two queue slots are
  kept for metrics, so a burst of audit events cannot push those out.
  When the queue does fill, metrics exports are dropped before logs.

## Start

```dcl
$ @dev:[dir.VMSOTEL]VMSOTEL_START
```

It starts a detached process named `VMSOTEL` with NETMBX and TMPMBX
only, running as the UIC that started it, and refuses if one is already
running. To start it at boot, add that line to
`SYS$MANAGER:SYSTARTUP_VMS.COM` after TCP/IP has started (not yet
tried on the test machines). Boot time is converted to UTC with the
time zone in force when the agent starts, so `system.uptime` should be
right when it starts at boot; started later, across a summer-time
change since boot, it is an hour out -- which is what the test runs,
started by hand after 25 October, showed.

The agent will not start if its clock check fails: `time()` must agree
with the system time less `SYS$TIMEZONE_DIFFERENTIAL` (from the system
logical name table) to within 2 s. If it does not, every timestamp would
be off by the difference, so the log says so and the process exits.

## Stop

```dcl
$ @dev:[dir.VMSOTEL]VMSOTEL_STOP
```

It creates `VMSOTEL.STOP`. Within a second the agent deletes the file,
tries once more to send anything queued, logs whatever it could not,
writes a summary, and exits. The procedure waits for each step.

Avoid `STOP/IDENTIFICATION`: it deletes the process without letting it
run anything, so queued exports are lost and the log never says so.
If you must use it, the last line ending `N waiting` in the log is how
many were lost.

## The log

`VMSOTEL.LOG`, in the vmsotel directory, one line per event, times in
UTC. It is opened and closed for each line -- vmsguard's way round the
C RTL's exclusive opens -- so `TYPE` works while the agent runs (seen
on x86). Each start appends to the same file. `VMSOTEL_RUN.LOG` holds DCL's output: look there if the
process ends without writing to `VMSOTEL.LOG`.

| Line | Meaning |
| --- | --- |
| `vmsotel 0.1.0 starting: http://…, every 60 s, queue 10` | started |
| `to stop: create …VMSOTEL.STOP` | where the stop file goes |
| `not starting: clock check failed: …` | time zone misconfigured; nothing sent |
| `CLOCK: this system is N s behind the collector …` | timestamps are off by N s (more than 2); fix NTP. Logged once; `clock now within 1 s of the collector` when it recovers |
| `cpu mode interrupt is unusable (…); not reported` | x86: that counter reads as negative there; expected, at start |
| `export failed: …; retrying in N ms; M waiting` | the collector did not take it; it is kept and retried |
| `export sent again after N failed attempts over S s` | the outage is over; what waited is being sent |
| `queue full (10); oldest metrics payload dropped` | the collector has been away a while; the oldest metrics export is lost (logs go only when no metrics are left to drop) |
| `EXPORT REJECTED, HTTP 400 on /v1/metrics: …` | the collector refused the export outright; a bug, or a wrong `path` |
| `EXPORT REJECTED, HTTP 404 on /v1/logs: … no logs pipeline …` | the collector takes no logs: add a `logs` pipeline (I64's first run, before docker-nuc's was restarted) |
| `collector accepted the export but rejected N data points` | partly refused; the message says why |
| `payload needs N bytes, more than max_payload …` | raise `max_payload` |
| `stopping with N exports unsent; they are lost` | at stop, or at the end of a `--cycles` run, the collector could not be reached |
| `stopped: …` | the summary: collections, sent, failed attempts, dropped |
| `per-process metrics for N name and M user patterns …` | process.* is set |
| `N processes could not be read (no WORLD privilege?) …` | the agent lacks WORLD; only its own account's processes are chosen from |
| `N processes match the process.* patterns; reporting the first 64` | narrow the patterns |
| `logs to /v1/logs: …; they need READALL` | logs.* is set |
| `logs: starting at the end of … (N records, S s)` | the first read of a file; nothing before this is sent |
| `logs: cannot read …: %RMS-E-PRV, …` | no READALL; logged once, retried every interval, and `logs: readable again` when it works |
| `logs: … is finished; now reading …;N` | a new version of the file (REPLY/LOG, SET AUDIT/SERVER=NEW_LOG): the rest of the old one was sent first |
| `logs: lost its place in …; starting again from the end` | the record at the saved RFA has changed; what came between is not sent |
| `log records: N queued, M too large` | at stop: log records sent, and dropped for exceeding `max_payload` |

The same counts go to the collector as `vmsotel.exports`,
`vmsotel.queue.length`, `vmsotel.collection.duration` and
`vmsotel.clock.skew`.

## Checking the numbers against MONITOR

While the agent runs, take a two-minute MONITOR summary:

```dcl
$ monitor system/nodisplay/summary=sys$login:system.sum/ending="+0:02:00"
$ type sys$login:system.sum
```

On the collector's host, this prints one line per data point --
host, process (for phase 2's per-process resources), metric,
attributes, value -- for the last five minutes (the
collector's debug exporter, `verbosity: detailed`):

```sh
docker logs --since 5m $(docker ps -q --filter ancestor=otel/opentelemetry-collector:0.111.0) 2>&1 |
  awk '/ResourceMetrics #/ {p=""} /-> host.name:/ {h=$3}
       /-> openvms.process.name:/ {p=$3} /-> Name:/ {n=$3}
       /^NumberDataPoints/ {a=""} /-> .*: Str\(/ {a=a" "$NF}
       /^Value:/ {print h, p, n, a, $2}' |
  grep -E 'memory.usage|process.count|cpu.time|paging.faults|openvms.io'
```

| MONITOR SYSTEM | collector | how they relate |
| --- | --- | --- |
| Free List Size | `system.memory.usage` `free` | bytes ÷ 8192 = pages |
| Mod List Size | `system.memory.usage` `modified` | bytes ÷ 8192 = pages |
| Process states (LEF, HIB, COM, CUR) | `system.process.count` by `openvms.process.state` | equal (x86 shows 2–3 extra HIB; known) |
| Direct I/O Rate | `openvms.io.direct` | difference between two exports ÷ the interval |
| Buffered I/O Rate | `openvms.io.buffered` | the same |
| Page Fault Rate | `system.paging.faults`, major + minor | the same |
| CPU Busy | `system.cpu.time` | 100% less idle's share of the change in all modes |
| SHOW SYSTEM, a chosen process's CPU | `process.cpu.time` for that process | equal, in seconds |
| SHOW SYSTEM, its Pages | `process.memory.usage` | bytes ÷ 8192 = pages |

MONITOR averages over its own two minutes, so rates agree to within the
system's own variation, not exactly; sizes and counts agree closely.
