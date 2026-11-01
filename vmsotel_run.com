$! vmsotel_run.com -- the detached process's command procedure
$!
$! Not run by hand: VMSOTEL_START.COM gives it to LOGINOUT. It runs the
$! agent with its configuration, log and stop file, and exits when the
$! agent does. They are found as VMSOTEL_START finds them: beside this
$! file in the source tree, or in the kit's [CONF] and [LOG].
$!
$! The privileges VMSOTEL.CONF needs (VMSOTEL_PRIVS.COM) are the only
$! ones enabled. In the source tree VMSOTEL_START gave exactly those; in
$! the kit they are the account's authorized privileges, which
$! VMSOTEL$CONFIGURE set when it last read VMSOTEL.CONF -- so a
$! configuration that has since asked for more is reported here.
$!
$! The TLS image is preferred: it also speaks plain HTTP when the
$! configuration says tls = no.
$!
$ set noon
$ proc = f$environment("PROCEDURE")
$ dev = f$parse(proc,,,"DEVICE")
$ dir = f$parse(proc,,,"DIRECTORY")
$ root = dev + dir
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
$ set default 'logd'
$!
$ @'com'VMSOTEL_PRIVS 'conf'
$ set process/privileges=(noall,'vmsotel_privs')
$ missing = ""
$ i = 0
$ priv_loop:
$   p = f$element(i, ",", vmsotel_privs)
$   if p .eqs. "," then goto priv_done
$   if .not. f$privilege(p) then missing = missing + " " + p
$   i = i + 1
$   goto priv_loop
$ priv_done:
$ if missing .nes. ""
$ then
$     write sys$output f$time(), " vmsotel_run: VMSOTEL.CONF needs", -
          missing, ", which ", f$edit(f$getjpi("", "USERNAME"), "TRIM"), -
          " is not authorized; what needs it will not be read."
$     if kit then write sys$output "  Run @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE again, as SYSTEM."
$ endif
$!
$ exe = bin + "VMSOTEL_TLS.EXE"
$ if f$search(exe) .eqs. "" then exe = bin + "VMSOTEL.EXE"
$ write sys$output f$time(), " vmsotel_run: ", exe, " as ", -
      f$edit(f$getjpi("", "USERNAME"), "TRIM")
$ show process/privileges
$!
$! Quoted, so that DCL passes them as written.
$ vmsotel := $'exe'
$ vmsotel "''conf'" -
          "--log" "''logd'VMSOTEL.LOG" -
          "--stop-file" "''logd'VMSOTEL.STOP"
$ st = $status
$ write sys$output f$time(), " vmsotel_run: agent exited, status ", st
$ exit st
