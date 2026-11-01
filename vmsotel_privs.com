$! vmsotel_privs.com -- the privileges a configuration needs
$!
$!     @VMSOTEL_PRIVS config-file
$!
$! Sets the global symbol VMSOTEL_PRIVS to NETMBX,TMPMBX, plus WORLD
$! for a line that begins "process." and READALL for "logs.opcom" or
$! "logs.audit" set to yes (or true, on, 1) -- after any spaces, and not
$! a comment. Read line by line rather than SEARCHed, so that a
$! commented-out example grants nothing.
$!
$! One scan for three callers, so they cannot disagree: VMSOTEL_START
$! (the source tree: RUN/DETACHED/PRIVILEGES), VMSOTEL_RUN (enables
$! them in the agent's process) and VMSOTEL$CONFIGURE (the kit: the
$! account's authorized privileges). Other accounts' processes need
$! WORLD; the operator log and the audit journal need READALL
$! (findings 12, 33, 36).
$!
$ set noon
$ vmsotel_privs == "NETMBX,TMPMBX"
$ want_world = 0
$ want_readall = 0
$ open/read/error=privs_done cf 'p1'
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
$ if want_world then vmsotel_privs == vmsotel_privs + ",WORLD"
$ if want_readall then vmsotel_privs == vmsotel_privs + ",READALL"
$ exit 1
