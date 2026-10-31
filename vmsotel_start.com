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
$! given arguments, with NETMBX and TMPMBX and nothing else: the
$! agent needs no privilege (docs/findings.md). It runs as the UIC that
$! starts it -- SYSTEM, from SYSTARTUP_VMS.COM.
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
$! Quotas are stated rather than left to the PQL defaults: the queue is
$! allocated at start (10 exports of 128 KB by default), and TCP and
$! TLS want buffered I/O and buffer space. These are modest.
$!
$ run/detached sys$system:loginout.exe -
      /input='root'VMSOTEL_RUN.COM -
      /output='root'VMSOTEL_RUN.LOG -
      /process_name=VMSOTEL -
      /privileges=(nosame,netmbx,tmpmbx) -
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
$ say "vmsotel started."
$ say "  log   : TYPE ''root'VMSOTEL.LOG"
$ say "  stop  : @''root'VMSOTEL_STOP"
$ exit 1
