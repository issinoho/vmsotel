$! vmsotel_start.com -- start the vmsotel agent as a detached process
$!
$!     @dev:[dir.VMSOTEL]VMSOTEL_START       by hand, or from
$!                                           SYS$MANAGER:SYSTARTUP_VMS.COM
$!     @VMSOTEL$ROOT:[COM]VMSOTEL_START      installed from the kit; at
$!                                           boot, SYS$STARTUP:VMSOTEL$STARTUP
$!
$! Everything is found relative to this file's own directory, in one of
$! two layouts (docs/design.md, "The installation kit"):
$!
$!   the source tree            the kit, VMSOTEL$ROOT:
$!   VMSOTEL.CONF               [CONF]VMSOTEL.CONF    the configuration
$!   VMSOTEL.LOG                [LOG]VMSOTEL.LOG      the agent's log
$!   VMSOTEL_RUN.LOG            [LOG]VMSOTEL_RUN.LOG  DCL's, before the agent
$!   VMSOTEL.STOP               [LOG]VMSOTEL.STOP     VMSOTEL_STOP's request
$!   [.BUILD]VMSOTEL*.EXE       [BIN]                 from build_vms
$!   VMSOTEL_RUN, _PRIVS.COM    [COM]
$!
$! It is the kit's when this file is in a [COM] directory with
$! [BIN]VMSOTEL_LAUNCH.EXE beside it.
$!
$! The source tree: RUN/DETACHED of LOGINOUT, so that the process has
$! DCL and can be given arguments, running as the UIC that starts it --
$! SYSTEM, from SYSTARTUP_VMS.COM -- with NETMBX and TMPMBX, plus WORLD
$! and READALL only when VMSOTEL.CONF needs them (VMSOTEL_PRIVS.COM).
$!
$! The kit: never as SYSTEM, whose system-group UIC reaches nearly every
$! file whatever its privileges. The account is the system logical
$! VMSOTEL$USER, set by VMSOTEL$CONFIGURE.COM; it must exist and have a
$! UIC above MAXSYSGROUP. VMSOTEL_LAUNCH.EXE starts the process as that
$! account by persona, since RUN/DETACHED/UIC would keep this process's
$! username and authorized privileges. That needs IMPERSONATE, and
$! WORLD to see whether the account's agent is already running: run it
$! as SYSTEM.
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
$! The kit's VMSOTEL$ROOT is a concealed rooted logical: the physical
$! name of [COM] ends in ".COM]" whichever way this was called.
$ pdir = f$parse(proc,,,"DIRECTORY","NO_CONCEAL") - "][" - "><"
$ top = f$parse(proc,,,"DEVICE","NO_CONCEAL") + -
        f$extract(0, f$length(pdir) - 5, pdir)
$ kit = f$extract(f$length(pdir) - 5, 5, pdir) .eqs. ".COM]" .and. -
        f$search(top + ".BIN]VMSOTEL_LAUNCH.EXE") .nes. ""
$ if kit
$ then
$     conf = top + ".CONF]VMSOTEL.CONF"
$     logd = top + ".LOG]"
$     bin  = top + ".BIN]"
$     com  = top + ".COM]"
$ else
$     conf = root + "VMSOTEL.CONF"
$     logd = root
$     bin  = dev + f$extract(0, f$length(dir) - 1, dir) + ".BUILD]"
$     com  = root
$ endif
$!
$ if f$search(conf) .eqs. ""
$ then
$     say "vmsotel: no configuration. Create ''conf', for"
$     if kit then say "example by @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE, then start again."
$     if .not. kit then say "example from ''root'VMSOTEL.CONF.EXAMPLE, then start again."
$     ! %X10000002: an error in $STATUS, but with the message inhibited,
$     ! since the lines above say what is wrong (a bare 2 prints NOMSG).
$     exit %X10000002
$ endif
$ if f$search(bin + "VMSOTEL_TLS.EXE") .eqs. "" .and. -
     f$search(bin + "VMSOTEL.EXE") .eqs. ""
$ then
$     say "vmsotel: not built. Run @''root'BUILD_VMS first."
$     exit %X10000002
$ endif
$ if f$search(com + "VMSOTEL_RUN.COM") .eqs. "" .or. -
     f$search(com + "VMSOTEL_PRIVS.COM") .eqs. ""
$ then
$     say "vmsotel: cannot find ''com'VMSOTEL_RUN.COM or VMSOTEL_PRIVS.COM"
$     exit %X10000002
$ endif
$!
$ account = ""
$ if kit
$ then
$     ! From the system table, in executive mode, only: not a job or
$     ! group table another user could write.
$     account = f$edit(f$trnlnm("VMSOTEL$USER", "LNM$SYSTEM_TABLE",, -
                                "EXECUTIVE"), "UPCASE,TRIM")
$     if account .eqs. ""
$     then
$         say "vmsotel: no account. Run @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE"
$         say "  once, as SYSTEM; it creates the account and defines VMSOTEL$USER."
$         exit %X10000002
$     endif
$     uicnum = f$identifier(account, "NAME_TO_NUMBER")
$     if uicnum .eq. 0
$     then
$         say "vmsotel: VMSOTEL$USER names ''account', which is not an account."
$         exit %X10000002
$     endif
$     if uicnum / 65536 .le. f$getsyi("MAXSYSGROUP")
$     then
$         say "vmsotel: refusing to run the agent as ''account', which has a"
$         say "  system UIC, ", f$fao("!%U", uicnum), ". It needs an account of its own"
$         say "  (@VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE)."
$         exit %X10000002
$     endif
$     if .not. f$privilege("IMPERSONATE") .or. .not. f$privilege("WORLD")
$     then
$         say "vmsotel: starting the agent as ''account' needs IMPERSONATE, and"
$         say "  WORLD to see whether it is already running: run this as SYSTEM."
$         exit %X10000002
$     endif
$ endif
$!
$! One agent per system: process names are unique within a UIC group,
$! so a second start would fail anyway, less clearly. In the kit the
$! name and the account must both match, so that another user's process
$! called VMSOTEL is not taken for it.
$!
$ ctx = ""
$ find_loop:
$   pid = f$pid(ctx)
$   if pid .eqs. "" then goto not_running
$   if f$getjpi(pid, "PRCNAM") .nes. "VMSOTEL" then goto find_loop
$   if kit .and. f$edit(f$getjpi(pid, "USERNAME"), "TRIM") .nes. account then -
        goto find_loop
$ tmp = f$context("PROCESS", ctx, "CANCEL")
$ say "vmsotel: already running, process ''pid'. Stop it first with"
$ if kit then say "  @VMSOTEL$ROOT:[COM]VMSOTEL_STOP"
$ if .not. kit then say "  @''root'VMSOTEL_STOP"
$ exit %X10000002
$ not_running:
$!
$! A stop request left over from last time would stop the new agent at
$! once.
$ if f$search(logd + "VMSOTEL.STOP") .nes. "" then -
      delete/nolog 'logd'VMSOTEL.STOP;*
$!
$ @'com'VMSOTEL_PRIVS 'conf'
$ privs = vmsotel_privs
$ if kit then goto launch
$!
$! Quotas are stated rather than left to the PQL defaults: the queue is
$! allocated at start (10 exports of 128 KB by default), and TCP and
$! TLS want buffered I/O and buffer space. These are modest, and
$! VMSOTEL_LAUNCH.EXE gives the same.
$!
$ run/detached sys$system:loginout.exe -
      /input='com'VMSOTEL_RUN.COM -
      /output='logd'VMSOTEL_RUN.LOG -
      /process_name=VMSOTEL -
      /privileges=(NOSAME,'privs') -
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
$ say "vmsotel started, privileges NOSAME,''privs'."
$ say "  log   : TYPE ''root'VMSOTEL.LOG"
$ say "  stop  : @''root'VMSOTEL_STOP"
$ exit 1
$!
$! The kit: as the account, by persona. The process gets the account's
$! authorized privileges, which VMSOTEL$CONFIGURE set from the same
$! scan; VMSOTEL_RUN enables those the configuration needs, and says so
$! in VMSOTEL_RUN.LOG if one is not authorized.
$ launch:
$ if f$search(logd + "VMSOTEL_RUN.LOG") .nes. "" then -
      purge/nolog/keep=5 'logd'VMSOTEL_RUN.LOG
$ launch := $'bin'VMSOTEL_LAUNCH.EXE
$ launch "''account'" "''com'VMSOTEL_RUN.COM" "''logd'VMSOTEL_RUN.LOG" "VMSOTEL"
$ st = $status
$ if (st .and. 1) .eq. 0
$ then
$     say "vmsotel: the launcher failed; nothing is running."
$     exit st
$ endif
$! LOGINOUT can still fail as the account (a directory it cannot reach,
$! a disusered account): then the process is gone in a moment.
$ wait 00:00:03
$ ctx = ""
$ check_loop:
$   pid = f$pid(ctx)
$   if pid .eqs. "" then goto not_started
$   if f$getjpi(pid, "PRCNAM") .nes. "VMSOTEL" then goto check_loop
$   if f$edit(f$getjpi(pid, "USERNAME"), "TRIM") .nes. account then -
        goto check_loop
$ tmp = f$context("PROCESS", ctx, "CANCEL")
$ say "vmsotel started as ''account', process ''pid', privileges ''privs'."
$ say "  log   : TYPE VMSOTEL$ROOT:[LOG]VMSOTEL.LOG"
$ say "  stop  : @VMSOTEL$ROOT:[COM]VMSOTEL_STOP"
$ exit 1
$ not_started:
$ say "vmsotel: the process as ''account' did not stay up. See"
$ say "  TYPE ''logd'VMSOTEL_RUN.LOG, and ACCOUNTING/SINCE=-0:5 for its status."
$ exit %X10000002
