$! vmsotel_start.com -- start the vmsotel agent as a detached process
$!
$!     @dev:[dir.VMSOTEL]VMSOTEL_START       by hand, or from
$!                                           SYS$MANAGER:SYSTARTUP_VMS.COM
$!
$! Everything is found relative to this file's own directory, the
$! vmsotel tree:
$!
$!     VMSOTEL.CONF        the configuration -- copy VMSOTEL.CONF.EXAMPLE
$!     VMSOTEL.LOG         the agent's log, one line per event
$!     VMSOTEL_RUN.LOG     DCL's output, for problems before the agent runs
$!     VMSOTEL.STOP        created by VMSOTEL_STOP.COM to ask it to stop
$!     [.BUILD]            VMSOTEL_TLS.EXE, or VMSOTEL.EXE, from build_vms
$!
$! RUN/DETACHED of LOGINOUT, so that the process has DCL and can be
$! given arguments, with NETMBX and TMPMBX: the system metrics need no
$! privilege (docs/findings.md). WORLD is added only when VMSOTEL.CONF
$! asks for per-process metrics (process.name or process.user), since
$! without it a process scan sees only the agent's own account's.
$! READALL is added only when it asks for logs (logs.opcom or
$! logs.audit = yes): the operator log and the audit journal are
$! SYSTEM's, (RWED,RWED,RE,), and cannot be opened without it (findings
$! 33, 36). It runs as the UIC that starts it -- SYSTEM, from
$! SYSTARTUP_VMS.COM.
$!
$! It does not restart itself. An agent that came back silently after
$! failing would hide the failure; VMSOTEL.LOG says why it stopped.
$!
$ set noon
$ say := write sys$output
$ proc = f$environment("PROCEDURE")
$ dev = f$parse(proc,,,"DEVICE")
$ dir = f$parse(proc,,,"DIRECTORY")
$ root = dev + dir
$ build = dev + f$extract(0, f$length(dir) - 1, dir) + ".BUILD]"
$!
$ if f$search(root + "VMSOTEL.CONF") .eqs. ""
$ then
$     say "vmsotel: no configuration. Create ''root'VMSOTEL.CONF, for"
$     say "example from ''root'VMSOTEL.CONF.EXAMPLE, then start again."
$     exit 2
$ endif
$ if f$search(build + "VMSOTEL_TLS.EXE") .eqs. "" .and. -
     f$search(build + "VMSOTEL.EXE") .eqs. ""
$ then
$     say "vmsotel: not built. Run @''root'BUILD_VMS first."
$     exit 2
$ endif
$ if f$search(root + "VMSOTEL_RUN.COM") .eqs. ""
$ then
$     say "vmsotel: cannot find ''root'VMSOTEL_RUN.COM"
$     exit 2
$ endif
$!
$! One agent per system: process names are unique within a UIC group,
$! so a second RUN/DETACHED would fail anyway, less clearly.
$!
$ ctx = ""
$ find_loop:
$   pid = f$pid(ctx)
$   if pid .eqs. "" then goto not_running
$   if f$getjpi(pid, "PRCNAM") .eqs. "VMSOTEL" then goto already
$   goto find_loop
$ already:
$ tmp = f$context("PROCESS", ctx, "CANCEL")
$ say "vmsotel: already running, process ''pid'. Stop it first with"
$ say "  @''root'VMSOTEL_STOP"
$ exit 2
$ not_running:
$!
$! A stop request left over from last time would stop the new agent at
$! once.
$ if f$search(root + "VMSOTEL.STOP") .nes. "" then -
      delete/nolog 'root'VMSOTEL.STOP;*
$!
$! WORLD for a line that begins "process."; READALL for "logs.opcom"
$! or "logs.audit" set to yes (or true, on, 1) -- after any spaces, and
$! not a comment. Read line by line rather than SEARCHed, so that a
$! commented-out example grants nothing.
$ privs = "NOSAME,NETMBX,TMPMBX"
$ want_world = 0
$ want_readall = 0
$ open/read/error=privs_done cf 'root'VMSOTEL.CONF
$ privs_loop:
$   read/end_of_file=privs_close cf line
$   line = f$edit(line, "COLLAPSE,UPCASE")
$   if f$extract(0, 8, line) .eqs. "PROCESS." then want_world = 1
$   key = f$extract(0, 11, line)
$   if key .nes. "LOGS.OPCOM=" .and. key .nes. "LOGS.AUDIT=" then -
        goto privs_loop
$   val = f$extract(11, 8, line)
$   if val .eqs. "YES" .or. val .eqs. "TRUE" .or. val .eqs. "ON" .or. -
       val .eqs. "1" then want_readall = 1
$   goto privs_loop
$ privs_close:
$ close cf
$ privs_done:
$ if want_world then privs = privs + ",WORLD"
$ if want_readall then privs = privs + ",READALL"
$!
$! Quotas are stated rather than left to the PQL defaults: the queue is
$! allocated at start (10 exports of 128 KB by default), and TCP and
$! TLS want buffered I/O and buffer space. These are modest.
$!
$ run/detached sys$system:loginout.exe -
      /input='root'VMSOTEL_RUN.COM -
      /output='root'VMSOTEL_RUN.LOG -
      /process_name=VMSOTEL -
      /privileges=('privs') -
      /ast_limit=100 /buffer_limit=200000 /enqueue_limit=500 -
      /file_limit=100 /io_buffered=100 /io_direct=100 -
      /page_file=300000 /queue_limit=20 -
      /working_set=4096 /maximum_working_set=16384 /extent=32768
$ st = $status
$!
$! SET NOON is on, so the status has to be looked at, or a failed RUN
$! would be followed by a message saying all is well.
$ if (st .and. 1) .eq. 0
$ then
$     say "vmsotel: RUN/DETACHED failed; nothing is running."
$     exit st
$ endif
$ say "vmsotel started, privileges ''privs'."
$ say "  log   : TYPE ''root'VMSOTEL.LOG"
$ say "  stop  : @''root'VMSOTEL_STOP"
$ exit 1
