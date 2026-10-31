$! vmsotel_run.com -- the detached process's command procedure
$!
$! Not run by hand: VMSOTEL_START.COM gives it to LOGINOUT. It runs the
$! agent with its configuration, log and stop file, all in this
$! directory, and exits when the agent does.
$!
$! The TLS image is preferred: it also speaks plain HTTP when the
$! configuration says tls = no.
$!
$ set noon
$ proc = f$environment("PROCEDURE")
$ dev = f$parse(proc,,,"DEVICE")
$ dir = f$parse(proc,,,"DIRECTORY")
$ root = dev + dir
$ build = dev + f$extract(0, f$length(dir) - 1, dir) + ".BUILD]"
$ set default 'root'
$!
$ exe = build + "VMSOTEL_TLS.EXE"
$ if f$search(exe) .eqs. "" then exe = build + "VMSOTEL.EXE"
$ write sys$output f$time(), " vmsotel_run: ", exe
$ show process/privileges
$!
$! Quoted, so that DCL passes them as written.
$ vmsotel := $'exe'
$ vmsotel "''root'VMSOTEL.CONF" -
          "--log" "''root'VMSOTEL.LOG" -
          "--stop-file" "''root'VMSOTEL.STOP"
$ st = $status
$ write sys$output f$time(), " vmsotel_run: agent exited, status ", st
$ exit st
