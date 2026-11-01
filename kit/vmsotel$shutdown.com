$! VMSOTEL$SHUTDOWN.COM -- stop the vmsotel agent at system shutdown
$!
$!   @SYS$STARTUP:VMSOTEL$SHUTDOWN         from SYS$MANAGER:SYSHUTDWN.COM
$!
$! As SYSTEM. VMSOTEL_STOP asks the agent to stop, so that it makes one
$! more attempt to send what it has queued and logs what it could not;
$! the process deletion at shutdown would lose it silently.
$!
$ set noon
$ if f$trnlnm("VMSOTEL$ROOT", "LNM$SYSTEM_TABLE") .eqs. "" then exit 1
$ if f$search("VMSOTEL$ROOT:[COM]VMSOTEL_STOP.COM") .eqs. "" then exit 1
$ @VMSOTEL$ROOT:[COM]VMSOTEL_STOP
$ exit 1
