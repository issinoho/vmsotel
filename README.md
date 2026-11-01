# vmsotel (draft)

An OpenTelemetry agent for OpenVMS: host and per-process metrics, and
(phase 3) the operator log and audit journal as logs. All three phases have run on both machines.
It started inside vmsguard (`issinoho/vmsguard`) and was moved here
with its history; the two share no code.

MIT licensed (`LICENSE`).

- `CLAUDE.md` — working notes for Claude Code sessions
- `docs/design.md` — the design, and the questions the probes answer
- `docs/findings.md` — what the probes showed, per machine
- `docs/prompts.md` — paste-ready prompts for each stage
- `src/model/` — one reading of the system, and 32-to-64-bit counter
  widening
- `src/otlp/` — the OTLP JSON encoder
- `src/platform/` — the platform interface, and its POSIX implementation
- `src/transport/` — HTTP/1.1 POST, TLS over memory BIOs, the export
  queue
- `src/sources/` — the source interface; `fake/` has scripted values
- `src/agent/` — the configuration file and the collect-and-send loop
- `vmsotel.conf.example` — every configuration key, explained
- `tests/` — unit tests and golden payloads
- `probes/` — six probes: four system-service probes, and the OTLP
  export without and with TLS
- `collector/` — collector configuration for docker-nuc
- `tools/fake_collector.py` — a schema-checking stand-in collector for
  Linux testing
- `stub/` — Linux stand-ins for the VMS system services, so the probes
  run here first

## On Linux

```sh
make check
```

That runs the agent's unit tests, checks the encoder's golden
payloads (`tests/golden/`) against the OTLP schema, runs the agent
against the fake collector (retries, `Retry-After`, rejection, a
collector that is down, TLS verification), builds every probe,
runs the four system-service probes against
the stub, and runs `probe_otlp` and `probe_otlp_tls` against the fake
collector — including the cases that must fail (an HTTP 503, an hour
of clock skew, plain HTTP to a TLS port). With the
`opentelemetry-proto` Python package installed
(`PYTHON=/path/to/venv/bin/python make check`) the fake collector
parses every payload against the official schema; without it, only the
JSON syntax is checked, and it says so.

## The agent on Linux

`make` builds `build/vmsotel` (plain HTTP) and `build/vmsotel_tls`.
On Linux they read the fake source, so they send scripted values, not
this machine's. Point a configuration at a collector and run it; it
logs to standard output and runs until stopped:

```sh
cp vmsotel.conf.example my.conf      # edit host and port
build/vmsotel my.conf
build/vmsotel my.conf --cycles 3     # three collections, then stop
```

## On docker-nuc: the collector

The collector runs in Docker on docker-nuc, `192.168.0.218`, which is
where the OTLP probes post: ports 4318 (plain) and 4319 (TLS). It needs
a clone of this repository there; start Docker from inside it, or the
bind mounts below name files that do not exist and Docker creates
empty root-owned directories in their place.

```sh
git clone https://github.com/issinoho/vmsotel.git ~/projects/vmsotel
cd ~/projects/vmsotel/collector
./make-cert.sh
docker run --rm -p 4318:4318 -p 4319:4319 \
  -v "$PWD/collector.yaml:/etc/otelcol/config.yaml:ro" \
  -v "$PWD/collector.crt:/etc/otelcol/collector.crt:ro" \
  -v "$PWD/collector.key:/etc/otelcol/collector.key:ro" \
  otel/opentelemetry-collector:0.111.0
```

If docker-nuc runs a firewall, ports 4318 and 4319 must be open to the
LAN (`sudo ufw allow 4318:4319/tcp` with ufw). Leave the collector's
output in view: each probe's data points appear there, and that is the
other end to watch.

Without Docker, `tools/fake_collector.py --port 4318` will do for the
plain-HTTP probe.

## On OpenVMS

Same commands on both machines. Clone once, from the directory that
holds the vmsguard checkout (type git subcommands in lowercase):

```dcl
$ git clone https://github.com/issinoho/vmsotel.git
$ @[.vmsotel]build_probes/OUTPUT=probes.log RUN
$ type probes.log
```

Later, to pick up changes: `set default [.vmsotel]`, `git pull`,
`set default [-]`, and run it again.

The agent itself is built by `build_vms.com`, from the same directory:

```dcl
$ @[.vmsotel]build_vms/OUTPUT=selftest.log SELFTEST
$ @[.vmsotel]build_vms/OUTPUT=run.log RUN
```

`SELFTEST` builds `[.vmsotel.build]VMSOTEL.EXE` and `VMSOTEL_TLS.EXE`
and prints every value the agent reads beside the `SHOW` or `MONITOR`
command that shows the same number. `RUN` sends three exports to the
collector on docker-nuc, and a TLS one too if `collector.crt` from
docker-nuc has been copied into `[.vmsotel]`. `descrip.mms` builds the
same images with `MMS CLEAN` and `MMS` (capitals: with the extended
parse style a lower-case target is not found); so far run on x86.

To run the agent as a service, see `docs/operations.md`.

That builds the six probes, runs them, and runs `SHOW SYSTEM` and
`SHOW DEVICE D` beside the probes that need comparing with them. Run it
**without** elevated privileges first; whether the probes need any is
one of the questions.

If many items come back "not declared", this lists what the system
headers actually contain:

```dcl
$ @[.vmsotel]build_probes/OUTPUT=headers.log HEADERS
```

## Verified

- All six probes build with `-std=c99 -Wall -Wextra -Werror` on Linux
  (`probe_otlp` with `-pedantic` too). The four system-service probes
  drop `-pedantic` only because it rejects `$` in identifiers, which VMS
  names cannot avoid.
- The system-service probes run against `stub/vmsstub.c`, including the
  refused-item, longword-retry, NOPRIV/SUSPENDED and unmounted-disk
  paths. The CPU-unit check was confirmed to fail when the stub reports
  one CPU's ticks against two active CPUs.
- `probe_otlp` and `probe_otlp_tls` pass all seven fake-collector
  cases, with every payload parsed by protobuf against
  `opentelemetry-proto` 1.27.0, and later 1.45.2.
- The model and encoder build with `-std=c99 -pedantic -Wall -Wextra
  -Werror`, pass their unit tests, and their golden payloads parse
  against `opentelemetry-proto` 1.45.2. Twelve deliberate breaks of the
  code were each caught by a test.

The probes have since run on both VMS machines, and the collector
configuration on docker-nuc. The agent has run on both too: built by
`build_vms.com`, its self-test matched SHOW and MONITOR, it sent
exports the collector decoded, and it ran detached and stopped cleanly
through `vmsotel_start.com` and `vmsotel_stop.com`. `docs/findings.md`
has each result and what has not yet been seen: `descrip.mms` on the
Itanium, a start from `SYSTARTUP_VMS.COM`, TLS from VMS.
