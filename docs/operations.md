# Running vmsotel

The operator's page: install, configure, start, stop, and what the log
says. vmsotel sends this system's CPU, memory, paging, process, disk and
I/O figures to an OpenTelemetry collector over OTLP/HTTP, every minute
by default. It needs no privilege beyond NETMBX and TMPMBX.

## Install

The vmsotel tree can live anywhere; everything below is found relative
to it. From the directory that is to hold it:

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
`cafile`, `interval`, `timeout`, `queue`, `max_payload`, and
`resource.NAME = value` for extra attributes on every export. With
`tls = yes` the collector's certificate is always verified, against
`cafile` if given; there is no setting to skip that.

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
| `CLOCK: this system is N s behind the collector …` | timestamps are off by N s; fix NTP. Logged once, and again when it recovers |
| `cpu mode interrupt is unusable (…); not reported` | x86: that counter reads as negative there; expected, at start |
| `export failed: …; retrying in N ms; M waiting` | the collector did not take it; it is kept and retried |
| `queue full (10); oldest payload dropped` | the collector has been away a while; the oldest export is lost |
| `EXPORT REJECTED, HTTP 400: …` | the collector refused the export outright; a bug, or a wrong `path` |
| `collector accepted the export but rejected N data points` | partly refused; the message says why |
| `payload needs N bytes, more than max_payload …` | raise `max_payload` |
| `stopping with N exports unsent; they are lost` | at stop, the collector could not be reached |
| `stopped: …` | the summary: collections, sent, failed attempts, dropped |

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
host, metric, attributes, value -- for the last five minutes (the
collector's debug exporter, `verbosity: detailed`):

```sh
docker logs --since 5m $(docker ps -q --filter ancestor=otel/opentelemetry-collector:0.111.0) 2>&1 |
  awk '/-> host.name:/ {h=$3} /-> Name:/ {n=$3} /^NumberDataPoints/ {a=""}
       /-> .*: Str\(/ {a=a" "$NF} /^Value:/ {print h, n, a, $2}' |
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

MONITOR averages over its own two minutes, so rates agree to within the
system's own variation, not exactly; sizes and counts agree closely.
