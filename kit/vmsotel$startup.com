$! VMSOTEL$STARTUP.COM -- the vmsotel agent at system startup
$!
$!   @SYS$STARTUP:VMSOTEL$STARTUP          from SYSTARTUP_VMS.COM, after
$!                                         TCP/IP: define VMSOTEL$ROOT and
$!                                         VMSOTEL$USER, and start the agent
$!                                         if SYS$MANAGER:VMSOTEL$CONFIG.COM
$!                                         says AUTOSTART
$!   @SYS$STARTUP:VMSOTEL$STARTUP INSTALL  define VMSOTEL$ROOT only (PCSI,
$!                                         after installing)
$!   @SYS$STARTUP:VMSOTEL$STARTUP REMOVE   stop the agent and deassign both
$!                                         (PCSI, before removing)
$!
$! As SYSTEM. The agent itself runs as its own account (VMSOTEL_START).
$!
$ set noon
$ op = f$edit(p1, "UPCASE,TRIM")
$ if op .eqs. "REMOVE" then goto remove
$!
$! This file is in <destination>[SYS$STARTUP]; the product is in
$! <destination>[VMSOTEL]. Physical names: the destination may itself
$! be a concealed logical (SYS$COMMON).
$ proc = f$environment("PROCEDURE")
$ dev = f$parse(proc,,,"DEVICE","NO_CONCEAL")
$ dir = f$edit(f$parse(proc,,,"DIRECTORY","NO_CONCEAL"), "UPCASE") - "][" - "><"
$ if f$locate("SYS$STARTUP]", dir) .eq. f$length(dir)
$ then
$     write sys$error "VMSOTEL$STARTUP: expected to be in a [SYS$STARTUP] directory, not ''dir'"
$     exit 44
$ endif
$ root = dev + (dir - "SYS$STARTUP]" + "VMSOTEL.]" - ".000000")
$ define/system/executive_mode/translation_attributes=concealed/nolog -
      VMSOTEL$ROOT 'root'
$ if f$search("VMSOTEL$ROOT:[BIN]VMSOTEL_LAUNCH.EXE") .eqs. ""
$ then
$     write sys$error "VMSOTEL$STARTUP: no VMSOTEL$ROOT:[BIN]VMSOTEL_LAUNCH.EXE (root ''root')"
$     exit 44
$ endif
$ if op .eqs. "INSTALL" then exit 1
$!
$! At boot: the site settings, written by VMSOTEL$CONFIGURE, name the
$! account and say whether to start. Global symbols, as that file
$! assigns them: a local one of the same name would hide them.
$ if f$search("SYS$MANAGER:VMSOTEL$CONFIG.COM") .eqs. ""
$ then
$     write sys$output "VMSOTEL$STARTUP: not configured; run @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE"
$     exit 1
$ endif
$ vmsotel_account == ""
$ vmsotel_autostart == "NO"
$ @SYS$MANAGER:VMSOTEL$CONFIG.COM
$ if vmsotel_account .nes. "" then define/system/executive_mode/nolog -
      VMSOTEL$USER 'f$edit(vmsotel_account, "UPCASE,TRIM")'
$ if f$edit(vmsotel_autostart, "UPCASE,TRIM") .nes. "YES"
$ then
$     write sys$output "VMSOTEL$STARTUP: start at boot is off (SYS$MANAGER:VMSOTEL$CONFIG.COM)"
$     exit 1
$ endif
$ @VMSOTEL$ROOT:[COM]VMSOTEL_START
$ exit 1
$!
$ remove:
$! Stop the agent if the account's VMSOTEL process is running; asking
$! one that is not would wait 10 s for an answer.
$ account = f$edit(f$trnlnm("VMSOTEL$USER", "LNM$SYSTEM_TABLE"), "UPCASE,TRIM")
$ running = 0
$ ctx = ""
$ rm_loop:
$   pid = f$pid(ctx)
$   if pid .eqs. "" then goto rm_looked
$   if f$getjpi(pid, "PRCNAM") .nes. "VMSOTEL" then goto rm_loop
$   if f$edit(f$getjpi(pid, "USERNAME"), "TRIM") .nes. account then goto rm_loop
$ running = 1
$ tmp = f$context("PROCESS", ctx, "CANCEL")
$ rm_looked:
$ if f$trnlnm("VMSOTEL$ROOT", "LNM$SYSTEM_TABLE") .nes. ""
$ then
$     if running .and. f$search("VMSOTEL$ROOT:[COM]VMSOTEL_STOP.COM") .nes. "" then -
          @VMSOTEL$ROOT:[COM]VMSOTEL_STOP
$     deassign/system/executive_mode VMSOTEL$ROOT
$ endif
$ if f$trnlnm("VMSOTEL$USER", "LNM$SYSTEM_TABLE") .nes. "" then -
      deassign/system/executive_mode VMSOTEL$USER
$ exit 1
