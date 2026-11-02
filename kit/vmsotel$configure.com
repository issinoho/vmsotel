$! VMSOTEL$CONFIGURE.COM -- set up the vmsotel agent (once, as SYSTEM; safe
$! to run again, and needed again when VMSOTEL.CONF asks for a privilege)
$!
$!   @VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE [account] [uic] [autostart]
$!     account    the agent's account (default VMSOTEL)
$!     uic        its UIC, e.g. [360,1]: a group of its own, above
$!                MAXSYSGROUP. Needed only when the account is new.
$!     autostart  YES: VMSOTEL$STARTUP starts the agent at boot (default)
$!   Missing values are asked for when run interactively.
$!
$! What it does (docs/design.md, "The installation kit"):
$!  - VMSOTEL$ROOT:[CONF]VMSOTEL.CONF from VMSOTEL.CONF_EXAMPLE, if there
$!    is none.
$!  - The account. A new one gets no login class at all (local, dial-up,
$!    remote, network, batch) but is not disusered, since a persona
$!    refuses that; LGICMD NL:, DISCTLY, LOCKPWD, a random password
$!    nobody needs, and the quotas the agent runs with. An existing
$!    account is changed only if this procedure created it (its owner
$!    field says "vmsotel agent"). A system-group UIC is refused.
$!  - Its authorized privileges: NETMBX and TMPMBX, and WORLD or READALL
$!    only if VMSOTEL.CONF needs them now (VMSOTEL_PRIVS.COM).
$!  - The tree: [CONF] and [LOG] belong to the account; [BIN], [COM]
$!    and [DOC] are readable by all, as LOGINOUT needs every directory
$!    on the way to be.
$!  - SYS$MANAGER:VMSOTEL$CONFIG.COM (the account; start at boot or
$!    not), and the system logical VMSOTEL$USER.
$!
$ set noon
$ set process/parse_style=extended
$ say = "write sys$output"
$ interactive = f$mode() .eqs. "INTERACTIVE"
$ if f$trnlnm("VMSOTEL$ROOT", "LNM$SYSTEM_TABLE") .eqs. ""
$ then
$     say "VMSOTEL$CONFIGURE: VMSOTEL$ROOT is not defined (@SYS$STARTUP:VMSOTEL$STARTUP INSTALL)"
$     exit 44
$ endif
$ if .not. f$privilege("SYSPRV,SYSNAM,CMEXEC")
$ then
$     say "VMSOTEL$CONFIGURE: needs SYSPRV, SYSNAM and CMEXEC: run it as SYSTEM"
$     exit 36
$ endif
$!
$! ---- the configuration ----
$ conf = "VMSOTEL$ROOT:[CONF]VMSOTEL.CONF"
$ if f$search(conf) .eqs. ""
$ then
$     copy/nolog VMSOTEL$ROOT:[CONF]VMSOTEL.CONF_EXAMPLE 'conf'
$     say "Created ''conf' from VMSOTEL.CONF_EXAMPLE: set host (the"
$     say "  collector) in it before starting the agent."
$ endif
$ @VMSOTEL$ROOT:[COM]VMSOTEL_PRIVS 'conf'
$ privs = vmsotel_privs
$!
$ vmsotel_account == ""
$ vmsotel_autostart == ""
$ if f$search("SYS$MANAGER:VMSOTEL$CONFIG.COM") .nes. "" then @SYS$MANAGER:VMSOTEL$CONFIG.COM
$ accdef = "VMSOTEL"
$ if vmsotel_account .nes. "" then accdef = vmsotel_account
$ autodef = "YES"
$ if vmsotel_autostart .nes. "" then autodef = vmsotel_autostart
$ account = f$edit(p1, "UPCASE,TRIM")
$ if account .eqs. "" .and. interactive then inquire account "The agent's account [''accdef']"
$ account = f$edit(account, "UPCASE,TRIM")
$ if account .eqs. "" then account = accdef
$ maxsys = f$getsyi("MAXSYSGROUP")
$ maxsys_o = f$fao("!OL", maxsys)        ! octal, as UICs are written
$ uicnum = f$identifier(account, "NAME_TO_NUMBER")
$!
$! AUTHORIZE's input, which may hold the new password: in [CONF], which
$! only SYSTEM and the account can read, protected, and erased after.
$ work = "VMSOTEL$ROOT:[CONF]VMSOTEL$CONFIGURE.TMP"
$! Made with no group or world access from the start, not protected
$! after: the default protection is put back once it is erased.
$ old_prot = f$environment("PROTECTION")
$ set protection=(s:rwd,o:rwd,g,w)/default
$ uaf = f$parse("SYS$SYSTEM:SYSUAF.DAT")
$ quotas = "/astlm=100 /bytlm=200000 /enqlm=500 /fillm=100 /biolm=100" + -
           " /diolm=100 /pgflquota=300000 /tqelm=20 /wsdefault=4096" + -
           " /wsquota=16384 /wsextent=32768"
$ logins = "/nolocal /nodialup /noremote /nonetwork /nobatch"
$ if uicnum .eq. 0 then goto new_account
$!
$! ---- an existing account: change it only if we made it ----
$ if uicnum / 65536 .le. maxsys
$ then
$     say "VMSOTEL$CONFIGURE: ''account' has a system UIC; the agent needs an account of its own"
$     set protection=('old_prot')/default
$     exit 44
$ endif
$ define/user sys$output 'work'
$ define/user sysuaf 'uaf'
$ mcr authorize show 'account'
$ owner = ""
$ open/read f 'work'
$ read_owner:
$   read/end_of_file=owner_done f line
$   at = f$locate("Owner:", line)
$   if at .lt. f$length(line) then -
        owner = f$edit(f$extract(at + 6, 999, line), "TRIM,COMPRESS")
$   goto read_owner
$ owner_done:
$ close f
$ delete/nolog 'work';*
$ if f$edit(owner, "UPCASE") .nes. "VMSOTEL AGENT"
$ then
$     say "VMSOTEL$CONFIGURE: an account ''account' exists, but this procedure did not"
$     say "  create it (its owner field is """, owner, """). Choose another name, so that"
$     say "  a real user's account is not changed."
$     set protection=('old_prot')/default
$     exit 44
$ endif
$ say "Account ''account' exists (", f$fao("!%U", uicnum), "); setting its privileges, quotas and logins"
$ open/write f 'work'
$ write f "modify ''account' /privileges=(''privs') /defprivileges=(''privs') -"
$ write f "  /flags=(nodisuser,disctly,lockpwd,norestricted) /lgicmd=""NL:"" -"
$ write f "  ''logins' -"
$ write f "  ''quotas'"
$ write f "exit"
$ close f
$ goto run_authorize
$!
$! ---- a new account ----
$ new_account:
$ uic = f$edit(p2, "UPCASE,TRIM")
$ if uic .eqs. "" .and. interactive then -
      inquire uic "UIC for ''account', a group of its own above ''maxsys_o' (e.g. [360,1])"
$ uic = f$edit(uic, "UPCASE,TRIM")
$ if uic .eqs. "" .or. f$extract(0, 1, uic) .nes. "[" .or. -
     f$locate(",", uic) .eq. f$length(uic)
$ then
$     say "VMSOTEL$CONFIGURE: ''account' is a new account: give its UIC, e.g. [360,1]"
$     set protection=('old_prot')/default
$     exit 44
$ endif
$ grp = f$integer("%O" + (f$element(0, ",", uic) - "["))
$ if grp .le. maxsys
$ then
$     say "VMSOTEL$CONFIGURE: group ", f$fao("!OL", grp), " is a system group (MAXSYSGROUP is ", -
          maxsys_o, "): choose a group above it"
$     set protection=('old_prot')/default
$     exit 44
$ endif
$! A group of its own: accounts that share a group share its group
$! access to files. [376,*], the obvious-looking choice, is where
$! DECnet puts its server accounts (x86).
$ grp_o = f$fao("!OL", grp)
$ define/user sys$output 'work'
$ define/user sys$error nl:      ! NOSUCHUSER, when the group is free
$ define/user sysuaf 'uaf'
$ mcr authorize show [''grp_o',*]/brief
$ users = ""
$ open/read f 'work'
$ read_group:
$   read/end_of_file=group_done f line
$   if f$locate("[''grp_o',", line) .lt. f$length(line) then -
        users = users + " " + f$element(0, " ", f$edit(f$extract(21, 16, line), "TRIM"))
$   goto read_group
$ group_done:
$ close f
$ delete/nolog 'work';*
$ if users .nes. ""
$ then
$     say "VMSOTEL$CONFIGURE: group ''grp_o' already has accounts:''users'"
$     say "  Choose a group no other account uses."
$     set protection=('old_prot')/default
$     exit 44
$ endif
$! A password nobody needs: 32 random letters and digits from OpenSSL.
$ launch := $VMSOTEL$ROOT:[BIN]VMSOTEL_LAUNCH.EXE
$ define/user sys$output 'work'
$ launch --password
$ open/read f 'work'
$ read/end_of_file=pw_read f password
$ pw_read:
$ close f
$ delete/erase/nolog 'work';*
$ if f$length(password) .ne. 32
$ then
$     say "VMSOTEL$CONFIGURE: could not make a password (VMSOTEL_LAUNCH.EXE --password)"
$     set protection=('old_prot')/default
$     exit 44
$ endif
$ logdev = f$parse("VMSOTEL$ROOT:[LOG]",,,"DEVICE","NO_CONCEAL")
$ logdir = f$parse("VMSOTEL$ROOT:[LOG]",,,"DIRECTORY","NO_CONCEAL") - "][" - "><"
$ say "Creating account ''account' ''uic'"
$ open/write f 'work'
$ write f "add ''account' /uic=''uic' /password=''password' /owner=""vmsotel agent"" -"
$ write f "  /privileges=(''privs') /defprivileges=(''privs') -"
$ write f "  /flags=(nodisuser,disctly,lockpwd) /lgicmd=""NL:"" /pwdlifetime=none -"
$ write f "  ''logins' -"
$ write f "  ''quotas' -"
$ write f "  /device=''logdev' /directory=''logdir'"
$ write f "exit"
$ close f
$ password = ""
$!
$ run_authorize:
$ set file/protection=(s:rwd,o,g,w) 'work'
$ define/user sysuaf 'uaf'
$ define/user sys$input 'work'
$ mcr authorize
$ delete/erase/nolog 'work';*
$ set protection=('old_prot')/default
$ uicnum = f$identifier(account, "NAME_TO_NUMBER")
$ if uicnum .eq. 0
$ then
$     say "VMSOTEL$CONFIGURE: AUTHORIZE did not create ''account' (see above)"
$     exit 44
$ endif
$!
$! ---- the tree ----
$! The root's own directory file, e.g. SYS$COMMON:[000000]VMSOTEL.DIR:
$! split "[A.B.C]" at its last dot into "[A.B]" and "C".
$ rootdev = f$parse("VMSOTEL$ROOT:[000000]",,,"DEVICE","NO_CONCEAL")
$ rootdir = f$parse("VMSOTEL$ROOT:[000000]",,,"DIRECTORY","NO_CONCEAL") - -
            "][" - "><" - ".000000"
$ inner = rootdir - "[" - "]"
$ n = 0
$ count_dots:
$   if f$element(n + 1, ".", inner) .eqs. "." then goto counted
$   n = n + 1
$   goto count_dots
$ counted:
$ last = f$element(n, ".", inner)
$ parent = rootdev + "[000000]"
$ if n .gt. 0 then parent = rootdev + "[" + -
      f$extract(0, f$length(inner) - f$length(last) - 1, inner) + "]"
$ set file/protection=(s:rwe,o:rwe,g:re,w:re) 'parent''last'.DIR
$ set file/protection=(s:rwe,o:rwe,g:re,w:re) VMSOTEL$ROOT:[000000]BIN.DIR,COM.DIR,DOC.DIR
$ set file/protection=(s:rwed,o:rwed,g:re,w:re) VMSOTEL$ROOT:[BIN]*.*;*,[COM]*.*;*,[DOC]*.*;*
$ set file/owner='account'/protection=(s:rwe,o:rwe,g,w) -
      VMSOTEL$ROOT:[000000]CONF.DIR,LOG.DIR
$ if f$search("VMSOTEL$ROOT:[CONF]*.*;*") .nes. "" then -
      set file/owner='account'/protection=(s:rwed,o:rwed,g,w) VMSOTEL$ROOT:[CONF]*.*;*
$ if f$search("VMSOTEL$ROOT:[LOG]*.*;*") .nes. "" then -
      set file/owner='account'/protection=(s:rwed,o:rwed,g,w) VMSOTEL$ROOT:[LOG]*.*;*
$!
$! ---- site settings ----
$ autostart = f$edit(p3, "UPCASE,TRIM")
$ if autostart .eqs. "" .and. interactive then -
      inquire autostart "Start the agent at boot (YES/NO) [''autodef']"
$ autostart = f$edit(autostart, "UPCASE,TRIM")
$ if autostart .eqs. "" then autostart = autodef
$ if autostart .nes. "YES" then autostart = "NO"
$ open/write f SYS$MANAGER:VMSOTEL$CONFIG.COM
$ write f "$! VMSOTEL$CONFIG.COM -- site settings for the vmsotel agent, written by"
$ write f "$! VMSOTEL$ROOT:[COM]VMSOTEL$CONFIGURE.COM (run it again to change them)"
$ write f "$ vmsotel_account == ""''account'"""
$ write f "$ vmsotel_autostart == ""''autostart'"""
$ close f
$ set file/protection=(s:rwed,o:rwed,g:re,w) SYS$MANAGER:VMSOTEL$CONFIG.COM
$ define/system/executive_mode/nolog VMSOTEL$USER 'account'
$!
$ say ""
$ say "The vmsotel agent will run as ''account' ", f$fao("!%U", uicnum), -
      ", authorized ''privs' (start at boot: ''autostart')."
$ say "Next:"
$ say "  1. Check host (the collector) in VMSOTEL$ROOT:[CONF]VMSOTEL.CONF. If it turns"
$ say "     on per-process metrics or a log, run this again: they need a privilege."
$ say "  2. $ @VMSOTEL$ROOT:[COM]VMSOTEL_START"
$ say "  3. Add $ @SYS$STARTUP:VMSOTEL$STARTUP.COM to SYS$MANAGER:SYSTARTUP_VMS.COM (after"
$ say "     TCP/IP) and $ @SYS$STARTUP:VMSOTEL$SHUTDOWN.COM to SYS$MANAGER:SYSHUTDWN.COM"
$ exit 1
