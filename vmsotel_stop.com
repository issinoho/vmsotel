$! vmsotel_stop.com -- ask the agent to stop, and wait for it
$!
$!     @dev:[dir.VMSOTEL]VMSOTEL_STOP
$!
$! Creates VMSOTEL.STOP, which the agent looks for once a second. It
$! deletes the file -- the acknowledgement this waits for -- makes one
$! more attempt to send what it has queued, logs anything it could not,
$! writes a summary to VMSOTEL.LOG and exits.
$!
$! Not STOP/IDENTIFICATION: that deletes the process without running
$! anything, so whatever was queued is lost and the log never says so.
$! If this procedure reports no answer, STOP/ID is the fallback, and the
$! last "waiting" line in VMSOTEL.LOG is what it lost.
$!
$ set noon
$ say := write sys$output
$ proc = f$environment("PROCEDURE")
$ root = f$parse(proc,,,"DEVICE") + f$parse(proc,,,"DIRECTORY")
$ stop = root + "VMSOTEL.STOP"
$!
$ create 'stop'
$ say "vmsotel: stop requested"
$!
$! The acknowledgement: within a second or two if it is running.
$ count = 0
$ ack_loop:
$   if f$search(stop) .eqs. "" then goto acknowledged
$   wait 00:00:01
$   count = count + 1
$   if count .lt. 10 then goto ack_loop
$ say "vmsotel: no acknowledgement in 10 s. Is it running?"
$ say "  SHOW SYSTEM/PROCESS=VMSOTEL"
$ say "The stop file is left in place; VMSOTEL_START.COM deletes it,"
$ say "so it will not stop the next start."
$ ! %X10000002: an error in $STATUS, but with the message inhibited,
$ ! since the lines above say what is wrong (a bare 2 prints NOMSG).
$ exit %X10000002
$!
$! Then the exit, after its last attempt to send: up to the configured
$! timeout per queued export when the collector is down.
$ acknowledged:
$ say "vmsotel: acknowledged; waiting for it to finish sending"
$ count = 0
$ exit_loop:
$   ctx = ""
$ find_loop:
$     pid = f$pid(ctx)
$     if pid .eqs. "" then goto gone
$     if f$getjpi(pid, "PRCNAM") .eqs. "VMSOTEL" then goto still_there
$     goto find_loop
$ still_there:
$   tmp = f$context("PROCESS", ctx, "CANCEL")
$   wait 00:00:01
$   count = count + 1
$   if count .lt. 60 then goto exit_loop
$ say "vmsotel: still running after 60 s, process ''pid'. See"
$ say "  TYPE ''root'VMSOTEL.LOG"
$ exit %X10000002
$ gone:
$ say "vmsotel: stopped. Its summary is the last line of"
$ say "  ''root'VMSOTEL.LOG"
$ exit 1
