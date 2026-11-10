#!/usr/bin/env python3
"""A stand-in OTLP/HTTP collector for testing probe_otlp on Linux.

It accepts POST /v1/metrics and /v1/logs with Content-Type
application/json, and checks the body the way a real collector would:
parsed into the official ExportMetricsServiceRequest or
ExportLogsServiceRequest message with protobuf's JSON parser, which rejects unknown fields, wrong types and badly encoded
64-bit integers. That needs the opentelemetry-proto package; without
it only the JSON syntax and the outline are checked, and it says so.

    fake_collector.py --port 4318            serve until interrupted
    fake_collector.py --selftest PLAIN TLS   run both probe images
                                             against it, including the
                                             cases that must fail
    fake_collector.py --validate FILE...     check saved payloads, such
                                             as the encoder's golden
                                             files, the same way
    fake_collector.py --agent-selftest PLAIN TLS [VMSSTUB]
                                             run the agent images against
                                             it: retries, Retry-After,
                                             rejection, a collector that
                                             is down, TLS verification

This is development tooling only. The real test is a real collector:
see collector/collector.yaml.
"""

import argparse
import email.utils
import http.server
import json
import os
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time

try:
    from google.protobuf import json_format
    from opentelemetry.proto.collector.metrics.v1 import metrics_service_pb2
    from opentelemetry.proto.collector.logs.v1 import logs_service_pb2
    HAVE_PROTO = True
except ImportError:
    HAVE_PROTO = False


def validate_logs(body, doc):
    """Return (ok, message, number of log records)."""
    if not HAVE_PROTO:
        if "resourceLogs" not in doc:
            return False, "no resourceLogs", 0
        return True, "JSON syntax only (opentelemetry-proto not installed)", -1
    msg = logs_service_pb2.ExportLogsServiceRequest()
    try:
        json_format.Parse(body, msg)
    except json_format.ParseError as e:
        return False, f"schema: {e}", 0
    n = 0
    for rl in msg.resource_logs:
        for sl in rl.scope_logs:
            for r in sl.log_records:
                if r.observed_time_unix_nano == 0:
                    return False, "a log record with no observedTimeUnixNano", 0
                if not r.body.string_value:
                    return False, "a log record with no body", 0
                n += 1
    if n == 0:
        return False, "no log records", 0
    return True, "valid ExportLogsServiceRequest", n


def validate(body, path=None):
    """Return (ok, message, number of data points or log records). The
    path says which request it must be; without one, the body does."""
    try:
        doc = json.loads(body)
    except ValueError as e:
        return False, f"not JSON: {e}", 0
    if path == "/v1/logs" or (path is None and "resourceLogs" in doc):
        return validate_logs(body, doc)
    if HAVE_PROTO:
        msg = metrics_service_pb2.ExportMetricsServiceRequest()
        try:
            json_format.Parse(body, msg)
        except json_format.ParseError as e:
            return False, f"schema: {e}", 0
        points = 0
        for rm in msg.resource_metrics:
            for sm in rm.scope_metrics:
                for m in sm.metrics:
                    kind = m.WhichOneof("data")
                    if kind is None:
                        return False, f"metric {m.name} has no data", 0
                    dps = getattr(m, kind).data_points
                    for dp in dps:
                        if dp.time_unix_nano == 0:
                            return False, f"{m.name}: zero timeUnixNano", 0
                    points += len(dps)
        return True, "valid ExportMetricsServiceRequest", points
    if "resourceMetrics" not in doc:
        return False, "no resourceMetrics", 0
    return True, "JSON syntax only (opentelemetry-proto not installed)", -1


class State:
    """What the handler should do, and what it saw."""
    status = 200
    date_offset = 0          # seconds added to the Date header
    date_offsets = []        # per response, in order, before date_offset
    last = None              # (ok, message, points) of the last request
    # Responses to give, in order, before falling back to `status`:
    # (status, Retry-After or None, body or None).
    script = []
    # Every request: (monotonic time, status sent, valid, body bytes, path).
    seen = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("collector: " + (fmt % args) + "\n")

    def date_time_string(self, timestamp=None):
        off = (State.date_offsets.pop(0) if State.date_offsets
               else State.date_offset)
        return email.utils.formatdate(time.time() + off, usegmt=True)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n)
        ctype = self.headers.get("Content-Type", "")
        if self.path not in ("/v1/metrics", "/v1/logs"):
            result = (False, f"wrong path {self.path}", 0)
        elif ctype != "application/json":
            result = (False, f"wrong Content-Type {ctype!r}", 0)
        else:
            result = validate(body, self.path)
        State.last = result
        sys.stderr.write(f"collector: {self.path}: {result[1]}, "
                         f"{result[2]} {'records' if self.path == '/v1/logs' else 'points'}\n")

        retry_after, reply = None, None
        if not result[0]:
            status = 400
        elif State.script:
            status, retry_after, reply = State.script.pop(0)
        else:
            status = State.status
        State.seen.append((time.monotonic(), status, result[0], body,
                           self.path))
        if reply is None:
            reply = b"{}" if status == 200 else json.dumps(
                {"code": 3, "message": result[1]}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        if retry_after is not None:
            self.send_header("Retry-After", str(retry_after))
        self.send_header("Content-Length", str(len(reply)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(reply)


def start(tls_dir=None, port=0):
    srv = http.server.HTTPServer(("127.0.0.1", port), Handler)
    if tls_dir:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(os.path.join(tls_dir, "collector.crt"),
                            os.path.join(tls_dir, "collector.key"))
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def make_cert(d):
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-days", "1", "-subj", "/CN=fake-collector",
         "-addext", "subjectAltName=IP:127.0.0.1",
         "-keyout", os.path.join(d, "collector.key"),
         "-out", os.path.join(d, "collector.crt")],
        check=True, capture_output=True)


def run_probe(args):
    p = subprocess.run(args, capture_output=True, text=True, timeout=30)
    return p.returncode, p.stdout


def selftest(plain, tls):
    failures = 0

    def expect(name, args, want_ok, must_contain=()):
        nonlocal failures
        State.last = None
        rc, out = run_probe(args)
        good = (rc == 0) == want_ok and all(s in out for s in must_contain)
        if want_ok and State.last is not None and not State.last[0]:
            good = False
        print(f"{'ok  ' if good else 'FAIL'}  {name}")
        if not good:
            failures += 1
            print(out)

    if not HAVE_PROTO:
        print("note  opentelemetry-proto not installed: payload checked "
              "for JSON syntax only")

    srv, port = start()
    base = ["--host", "127.0.0.1", "--port", str(port)]

    State.status, State.date_offset = 200, 0
    expect("plain export accepted", [plain] + base, True,
           ["HTTP 200", "clock agrees"])

    State.status = 503
    expect("plain export: a 503 is reported as failure", [plain] + base,
           False, ["HTTP status 503"])

    State.status, State.date_offset = 200, 3600
    expect("plain export: an hour of skew is reported as failure",
           [plain] + base, False, ["clock skew: ours minus collector's = -36"])
    State.date_offset = 0

    expect("plain export through inet_addr, without getaddrinfo",
           [plain, "--no-getaddrinfo"] + base, True,
           ["HTTP 200", "through inet_addr/gethostbyname"])

    expect("plain image refuses --tls", [plain, "--tls"] + base, False)
    srv.shutdown()

    with tempfile.TemporaryDirectory() as d:
        make_cert(d)
        srv, port = start(d)
        base = ["--host", "127.0.0.1", "--port", str(port), "--tls"]
        expect("TLS export accepted, unverified without --cafile",
               [tls] + base, True, ["TLS handshake", "not verified"])
        expect("TLS export verified with --cafile",
               [tls] + base + ["--cafile", os.path.join(d, "collector.crt")],
               True, ["certificate verified"])
        expect("plain HTTP to a TLS port fails",
               [plain, "--host", "127.0.0.1", "--port", str(port)], False)
        srv.shutdown()

    print(f"\n{'PASS' if failures == 0 else 'FAILURES'}: {failures}")
    return 1 if failures else 0


def free_port():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


STUB_OPLOG = os.path.join("build", "stub_operator.log")
STUB_JOURNAL = os.path.join("build", "stub_audit.journal")


def stub_message(when, *lines):
    """An OPCOM message as the operator log holds it: an empty record,
    the header, the lines (findings 34)."""
    return ("\n%%%%%%%%%%%  OPCOM  " + when + "  %%%%%%%%%%%\n" +
            "".join(l + "\n" for l in lines))


def stub_logfail(user, vms_time):
    """A LOGFAIL journal record in NSAMSGDEF, length-prefixed as the
    stub's journal stores records."""
    def pkt(kind, data):
        return (len(data) + 4).to_bytes(2, "little") + \
            kind.to_bytes(2, "little") + data
    body = (pkt(6, b"SECURITY") + pkt(45, user.encode()) +
            pkt(44, b"SSH_PASSWORD:192.0.2.77") +
            pkt(50, vms_time.to_bytes(8, "little")))
    size = 20 + len(body)
    hdr = (bytes([5, 0, 5, 0, 0x1c, 0, 4, 0]) + size.to_bytes(2, "little") +
           bytes([0, 3]) + bytes(8))
    rec = hdr + body
    return len(rec).to_bytes(2, "little") + rec


def agent_selftest(plain, tls, vmsstub=None):
    """Runs the agent images against this collector. Each case states
    what must happen at both ends: the agent's exit status and log, and
    what the collector received."""
    failures = 0

    def run(image, conf, cycles, drain=8, timeout=60, env=None):
        with tempfile.NamedTemporaryFile("w", suffix=".conf",
                                         delete=False) as f:
            f.write(conf)
        try:
            p = subprocess.run([image, f.name, "--cycles", str(cycles),
                                "--drain", str(drain)],
                               capture_output=True, text=True,
                               timeout=timeout, env=env)
            return p.returncode, p.stdout
        finally:
            os.unlink(f.name)

    def check(name, good, out=""):
        nonlocal failures
        print(f"{'ok  ' if good else 'FAIL'}  {name}")
        if not good:
            failures += 1
            print(out)

    def reset(script=(), status=200, offset=0, offsets=()):
        State.script = list(script)
        State.status = status
        State.date_offset = offset
        State.date_offsets = list(offsets)
        State.seen = []

    def accepted(path=None):
        return [s for s in State.seen if s[1] == 200 and s[2] and
                (path is None or s[4] == path)]

    def log_records():
        """Every log record the collector took, in arrival order."""
        out = []
        for s in accepted("/v1/logs"):
            for rl in json.loads(s[3])["resourceLogs"]:
                for sl in rl["scopeLogs"]:
                    out.extend(sl["logRecords"])
        return out

    def numbered(recs):
        """The fake log source's sequence numbers, in arrival order."""
        nums = []
        for r in recs:
            m = re.search(r"fake operator message (\d+)|user U(\d+) ",
                          r["body"]["stringValue"])
            nums.append(int(m.group(1) or m.group(2)) if m else -1)
        return nums

    def contiguous(nums):
        return len(nums) > 0 and nums == list(range(1, len(nums) + 1))

    def logs_case(image, port):
        """The VMS log source against the stub's files, written to while
        the agent runs: what was there before is not sent; each new
        message and journal record arrives once; the last message, with
        nothing after it, goes once it has been unchanged for a read; a
        new version of the log is followed from its start."""
        for base in (STUB_OPLOG, STUB_JOURNAL):
            for v in [""] + [f".{i}" for i in range(2, 10)]:
                if os.path.exists(base + v):
                    os.unlink(base + v)
        vms = lambda unix_s: unix_s * 10_000_000 + 35067168000000000

        def writer():
            time.sleep(2.2)
            with open(STUB_OPLOG, "a") as f:
                f.write(stub_message("1-NOV-2026 11:00:01.50",
                                     "Message from user SYSTEM on STUBBY",
                                     "%SYSTEM-W-TEST, first new message") +
                        stub_message("1-NOV-2026 11:00:02.00",
                                     "Message from user IAIN on STUBBY",
                                     "second new message"))
            with open(STUB_JOURNAL, "ab") as f:
                f.write(stub_logfail("ROOT", vms(1793530000)) +
                        stub_logfail("USER", vms(1793530001)))
            time.sleep(3.0)
            with open(STUB_OPLOG + ".2", "w") as f:
                f.write(stub_message("1-NOV-2026 11:00:09.00",
                                     "Logfile has been initialized",
                                     "third message, in version 2"))
        reset()
        th = threading.Thread(target=writer)
        th.start()
        rc, out = run(image, conf(port, "logs.opcom = yes\n"
                                        "logs.audit = yes\n"), 11, drain=4)
        th.join()
        recs = log_records()
        bodies = [r["body"]["stringValue"] for r in recs]
        want = ["Message from user SYSTEM on STUBBY\n"
                "%SYSTEM-W-TEST, first new message",
                "Message from user IAIN on STUBBY\nsecond new message",
                "Login failure: remote user ROOT from SSH_PASSWORD:192.0.2.77",
                "Login failure: remote user USER from SSH_PASSWORD:192.0.2.77",
                "Logfile has been initialized\nthird message, in version 2"]
        files = [next((a["value"]["stringValue"] for a in r["attributes"]
                       if a["key"] == "log.file.path"), "") for r in recs]
        check("logs, VMS source (stub): starts at the end, each new message "
              "and journal record once, the last message once it settles, "
              "version ;2 followed",
              rc == 0 and sorted(bodies) == sorted(want) and
              bodies.index(want[0]) < bodies.index(want[1]) and
              recs[bodies.index(want[0])]["severityText"] == "WARN" and
              recs[bodies.index(want[0])]["timeUnixNano"] ==
              "1793530801500000000" and
              recs[bodies.index(want[2])]["timeUnixNano"] ==
              "1793530000000000000" and
              files[bodies.index(want[4])].endswith("OPERATOR.LOG;2") and
              "starting at the end of STUBBY$DKA0:[SYS0.SYSMGR]OPERATOR.LOG;1"
              in out and "now reading STUBBY$DKA0:[SYS0.SYSMGR]OPERATOR.LOG;2"
              in out, out + "\n" + "\n---\n".join(bodies))

    def conf(port, extra=""):
        return f"host = 127.0.0.1\nport = {port}\ninterval = 1\n" + extra

    def gauge(body, name):
        doc = json.loads(body)
        for m in doc["resourceMetrics"][0]["scopeMetrics"][0]["metrics"]:
            if m["name"] == name:
                return m["gauge"]["dataPoints"][0]["asDouble"]
        return None

    if not HAVE_PROTO:
        print("note  opentelemetry-proto not installed: payloads checked "
              "for JSON syntax only")

    srv, port = start()

    reset()
    rc, out = run(plain, conf(port), 3)
    check("three cycles, three valid exports accepted",
          rc == 0 and len(accepted()) == 3 and
          all(s[2] for s in State.seen) and
          "3 sent, 0 failed attempts, 0 dropped" in out and
          "CLOCK" not in out, out)

    # Backoff doubles from 1 s but never past the interval, so this one
    # runs at 5 s to see it double.
    reset(script=[(503, None, None), (503, None, None)])
    rc, out = run(plain, conf(port).replace("interval = 1", "interval = 5"),
                  2)
    check("503 twice: retried with doubling backoff, nothing lost",
          rc == 0 and len(accepted()) == 2 and
          out.count("export failed, HTTP 503") == 2 and
          "retrying in 1000 ms" in out and "retrying in 2000 ms" in out and
          "export sent again after 2 failed attempts over 3 s" in out and
          "2 sent, 2 failed attempts, 0 dropped" in out, out)

    reset(script=[(429, 3, None)])
    rc, out = run(plain, conf(port), 1)
    gap = (State.seen[1][0] - State.seen[0][0]) if len(State.seen) > 1 else 0
    check(f"429 with Retry-After: 3 honoured (next try after {gap:.1f} s)",
          rc == 0 and len(accepted()) == 1 and 2.9 <= gap < 5 and
          "retrying in 3000 ms" in out, out)

    reset(script=[(400, None, b'{"code":3,"message":"proto: bad"}')])
    rc, out = run(plain, conf(port), 2)
    check("400: dropped loudly, not retried, the next one sent",
          rc == 0 and len(State.seen) == 2 and len(accepted()) == 1 and
          "EXPORT REJECTED, HTTP 400 on /v1/metrics: proto: bad" in out and
          "1 sent, 0 failed attempts, 1 dropped" in out, out)

    reset(script=[(200, None, b'{"partialSuccess":{"rejectedDataPoints":'
                              b'"2","errorMessage":"unit"}}')])
    rc, out = run(plain, conf(port), 1)
    check("200 with rejected points: delivered, and logged",
          rc == 0 and "rejected 2 data points: unit" in out, out)

    reset(offset=3600)
    rc, out = run(plain, conf(port), 2)
    skew = gauge(State.seen[-1][3], "vmsotel.clock.skew") if State.seen else None
    check(f"collector an hour ahead: vmsotel.clock.skew = {skew}, "
          "warned once",
          rc == 0 and skew is not None and -3602 <= skew <= -3598 and
          out.count("CLOCK: this system is 3600 s behind the collector") == 1,
          out)

    # A clock near the edge: the Date header has whole seconds, so a
    # node 2 s off reads 2 or 3 from one export to the next (both VMS
    # machines, findings 44). Warned once, cleared once -- not each time.
    reset(offsets=[4, 2, 4, 2, 0])
    rc, out = run(plain, conf(port), 5)
    check("clock skew around the edge: warned once, cleared once",
          rc == 0 and out.count("CLOCK: this system is") == 1 and
          out.count("clock now within 1 s of the collector") == 1, out)

    reset()
    rc, out = run(plain, conf(port, "max_payload = 4096\n"), 1, drain=1)
    check("payload over max_payload: dropped with the size it needs",
          rc == 0 and len(State.seen) == 0 and
          "more than max_payload 4096; dropped -- raise max_payload" in out,
          out)
    # Phase 3 through the fake log source: a burst of 150 waiting (more
    # than one 64-record read), then one a second. Every record must
    # arrive once, in order, beside the metrics.
    logs_on = "logs.opcom = yes\nlogs.audit = yes\n"
    reset()
    env = dict(os.environ, VMSOTEL_FAKE_LOG_BURST="150")
    rc, out = run(plain, conf(port, logs_on), 4, env=env)
    recs = log_records()
    nums = numbered(recs)
    check(f"logs: a burst of 150 then one a second -- {len(nums)} records, "
          "each once and in order, on /v1/logs beside 4 metrics exports",
          rc == 0 and len(accepted("/v1/metrics")) == 4 and
          contiguous(nums) and len(nums) >= 150 and
          all(r.get("timeUnixNano") for r in recs) and
          any(r["severityText"] == "WARN" and r["severityNumber"] == 13
              for r in recs) and
          "logs: fake logs: 150 waiting" in out and
          f"log records: {len(nums)} queued, 0 too large" in out, out)

    reset()
    env = dict(os.environ, VMSOTEL_FAKE_LOG_BURST="60")
    rc, out = run(plain, conf(port, logs_on + "max_payload = 4096\n"), 2,
                  env=env)
    nums = numbered(log_records())
    sizes = [len(s[3]) for s in accepted("/v1/logs")]
    check(f"logs: max_payload 4096 -- {len(sizes)} log payloads, none "
          "larger, no record lost",
          rc == 0 and len(sizes) > 2 and max(sizes) <= 4096 and
          contiguous(nums) and len(nums) >= 60, out)

    reset()
    env = dict(os.environ, VMSOTEL_FAKE_LOG_FAIL="1",
               VMSOTEL_FAKE_LOG_BURST="4")
    rc, out = run(plain, conf(port, logs_on), 3, env=env)
    recs = log_records()
    check("logs: the journal unreadable -- said once, operator messages "
          "still sent",
          rc == 0 and out.count("logs: cannot read SYS$MANAGER:SECURITY."
                                "AUDIT$JOURNAL: fake: no privilege") == 1 and
          len(recs) >= 4 and contiguous(numbered(recs)) and
          not any("Login failure" in r["body"]["stringValue"] for r in recs),
          out)

    if vmsstub:
        # The VMS source against the stub: the disks it must skip
        # (shadow member DKA100, unmounted DQA0, memory disk LDM9) are
        # absent, and memory is in bytes: the stub's 524288 pages of
        # 8192 and 233216-page page file with 172688 free.
        reset()
        rc, out = run(vmsstub, conf(port), 2)
        sent = b"".join(s[3] for s in State.seen)
        check("VMS source (stub): two exports; skips shadow member, "
              "unmounted and memory disks; memory and page file in bytes",
              rc == 0 and len(accepted()) == 2 and
              b'"STUBBY$DKA0"' in sent and b'"DSA1"' in sent and
              b"DKA100" not in sent and b"DQA0" not in sent and
              b"LDM9" not in sent and
              b'"asInt":"4294967296"' in sent and
              b'"free"}}],"startTimeUnixNano"' in sent and
              b'"asInt":"1414660096"' in sent and
              b"system.cpu.time" in sent and b'"lef"' in sent and
              b"process.pid" not in sent, out)

        # Phase 2 through the VMS source: the stub's OPCOM is chosen;
        # its AUDIT_SERVER is NOPRIV, which the agent logs once.
        reset()
        rc, out = run(vmsstub, conf(port, "process.name = opcom\n"
                                          "process.user = AUDIT*\n"), 2)
        sent = b"".join(s[3] for s in State.seen)
        check("VMS source (stub), process.name = opcom: one process "
              "resource, the NOPRIV one logged once",
              rc == 0 and len(accepted()) == 2 and
              sent.count(b'"openvms.process.name","value":{"stringValue":'
                         b'"OPCOM"}') == 2 and
              b"process.cpu.time" in sent and b"AUDIT_SERVER" not in sent
              and out.count("could not be read (no WORLD privilege?)") == 1,
              out)

        logs_case(vmsstub, port)

    # The clock stepping back an hour mid-run (tests/clockjump.c): what
    # VMS's local-time clock does when summer time ends. Without the
    # agent's guard it stalls for the hour.
    jump = os.path.join(os.path.dirname(os.path.abspath(plain)), "clockjump.so")
    if os.path.exists(jump):
        reset()
        env = dict(os.environ, LD_PRELOAD=jump, CLOCKJUMP_AFTER="3")
        rc, out = run(plain, conf(port).replace("interval = 1", "interval = 2"),
                      5, env=env, timeout=40)
        check("clock steps back an hour: agent notices and keeps collecting",
              rc == 0 and len(accepted()) == 5 and
              "clock went back 35" in out, out)
    else:
        print("note  build/clockjump.so missing: clock-step case skipped")

    # Stopping: the stop file, and SIGTERM, each followed by a final
    # send and a summary in the --log file.
    def stopped_run(image, conf_text, how, after=2.5, env=None):
        d = tempfile.mkdtemp()
        cf, lf, sf = (os.path.join(d, n) for n in ("a.conf", "a.log", "a.stop"))
        with open(cf, "w") as f:
            f.write(conf_text)
        p = subprocess.Popen([image, cf, "--log", lf, "--stop-file", sf],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, env=env)
        time.sleep(after)
        if how == "file":
            open(sf, "w").close()
        else:
            p.terminate()
        try:
            rc = p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = None
        log = open(lf).read() if os.path.exists(lf) else ""
        return rc, log, os.path.exists(sf), p.stdout.read()

    reset()
    rc, log, left, out = stopped_run(plain, conf(port), "file")
    check("stop file: seen, deleted, clean exit, summary in the log file",
          rc == 0 and not left and "stop requested" in log and
          "stopped:" in log and " 0 dropped" in log and
          len(accepted()) >= 2 and out == "", log + out)

    reset()
    rc, log, left, out = stopped_run(plain, conf(port), "signal")
    check("SIGTERM: clean exit with a summary",
          rc == 0 and "stop requested" in log and "stopped:" in log, log)

    srv.shutdown()
    srv.server_close()

    down = free_port()
    rc, out = run(plain, conf(down, "queue = 2\n"), 4, drain=2)
    check("collector down: bounded queue drops the oldest, agent gives up",
          rc != 0 and out.count("queue full (2); oldest metrics payload "
                                "dropped") == 2
          and "Connection refused" in out and
          "stopping with 2 exports unsent; they are lost" in out and
          re.search(r"done: 4 collections, 0 sent, \d+ failed attempts, "
                    r"4 dropped", out) is not None, out)

    later = free_port()
    reset()
    timer = threading.Timer(2.5, lambda: setattr(State,
        "late", start(port=later)))
    timer.start()
    rc, out = run(plain, conf(later), 3, drain=15)
    timer.join()
    late = getattr(State, "late", None)
    check("collector starts late: queued exports delivered once it is up",
          rc == 0 and len(accepted()) == 3 and "export failed" in out and
          "export sent again after" in out and "3 sent" in out, out)
    if late:
        late[0].shutdown()
        late[0].server_close()

    # The same with logs and a backlog bigger than the queue: what could
    # not be queued stays in the file, uncommitted, and comes later.
    later = free_port()
    reset()
    State.late = None
    timer = threading.Timer(3.5, lambda: setattr(State,
        "late", start(port=later)))
    timer.start()
    env = dict(os.environ, VMSOTEL_FAKE_LOG_BURST="300")
    rc, out = run(plain, conf(later, "logs.opcom = yes\nlogs.audit = yes\n"
                              "max_payload = 16384\n"), 8, drain=20, env=env)
    timer.join()
    nums = numbered(log_records())
    full = out.count("queue full (10); oldest metrics payload dropped")
    check(f"logs, collector starts late with 300 waiting: {len(nums)} "
          f"records, each once and in order; the queue filled and dropped "
          f"{full} metrics payloads, no logs",
          rc == 0 and contiguous(nums) and len(nums) >= 300 and full >= 1 and
          "oldest logs payload" not in out and
          len(accepted("/v1/metrics")) + full == 8, out)
    if State.late:
        State.late[0].shutdown()
        State.late[0].server_close()

    nowhere = free_port()
    rc, log, left, out = stopped_run(plain, conf(nowhere), "file", after=3.5)
    lost = re.search(r"stopping with (\d+) exports? unsent", log)
    check(f"stop with the collector down: {lost.group(1) if lost else '?'} "
          "lost exports logged and counted",
          rc == 0 and lost is not None and
          f"{lost.group(1)} dropped" in log and "waiting" in log and
          "retrying" not in log.split("stop requested")[-1], log)

    if vmsstub:
        env = dict(os.environ, STUB_TDF="3600")
        rc, out = run(vmsstub, conf(port), 1, env=env)
        check("clock check: a differential time() disagrees with refuses "
              "to start", rc != 0 and "not starting: clock check failed" in out
              and "3600 s apart" in out, out)
        env = dict(os.environ, STUB_TDF="none")
        rc, out = run(vmsstub, conf(port), 1, env=env)
        check("clock check: no system differential refuses to start",
              rc != 0 and "not defined in LNM$SYSTEM_TABLE" in out, out)

    rc, out = run(plain, conf(port, "tls = yes\n"), 1)
    check("plain image refuses tls = yes",
          rc != 0 and "built without TLS" in out, out)

    rc, out = run(plain, "port = 4318\n", 1)
    check("configuration without a host is refused",
          rc != 0 and "no host given" in out, out)

    with tempfile.TemporaryDirectory() as d:
        make_cert(d)
        srv, port = start(d)
        ca = os.path.join(d, "collector.crt")

        reset()
        rc, out = run(tls, conf(port, f"tls = yes\ncafile = {ca}\n"), 2)
        check("TLS with the collector's CA: verified and accepted",
              rc == 0 and len(accepted()) == 2 and "TLS through" in out, out)

        reset()
        rc, out = run(tls, conf(port, "tls = yes\n"), 1, drain=2)
        check("TLS without that CA: refused, nothing sent",
              rc != 0 and len(State.seen) == 0 and
              "TLS certificate: self-signed certificate" in out, out)

        reset()
        rc, out = run(plain, conf(port), 1, drain=2)
        check("plain HTTP to the TLS port fails",
              rc != 0 and len(accepted()) == 0, out)

        reset()
        rc, out = run(tls, conf(port, f"tls = yes\ncafile = {ca}\n"
                                      "host = localhost\n"), 1, drain=2)
        check("TLS to a name the certificate does not carry: refused",
              rc != 0 and len(State.seen) == 0 and "TLS certificate" in out,
              out)
        srv.shutdown()
        srv.server_close()

    print(f"\n{'PASS' if failures == 0 else 'FAILURES'}: {failures}")
    return 1 if failures else 0


def validate_files(paths):
    """Validate each file as a request body. Exit status 1 if any fail."""
    failures = 0
    if not HAVE_PROTO:
        print("note  opentelemetry-proto not installed: JSON syntax only")
    for path in paths:
        with open(path, "rb") as f:
            body = f.read()
        ok, msg, points = validate(body)
        what = "records" if b'"resourceLogs"' in body else "points"
        print(f"{'ok  ' if ok else 'FAIL'}  {path}: {msg}"
              + (f", {points} {what}" if points >= 0 else ""))
        failures += 0 if ok else 1
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=4318)
    ap.add_argument("--selftest", nargs=2, metavar=("PLAIN", "TLS"))
    ap.add_argument("--validate", nargs="+", metavar="FILE")
    ap.add_argument("--agent-selftest", nargs="+",
                    metavar="IMAGE", help="PLAIN TLS [VMSSTUB]")
    a = ap.parse_args()
    if a.agent_selftest:
        sys.exit(agent_selftest(*a.agent_selftest))
    if a.validate:
        sys.exit(validate_files(a.validate))
    if a.selftest:
        sys.exit(selftest(*a.selftest))
    srv = http.server.HTTPServer(("0.0.0.0", a.port), Handler)
    print(f"fake collector on :{a.port}, schema check "
          f"{'on' if HAVE_PROTO else 'OFF'}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
