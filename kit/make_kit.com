$! MAKE_KIT.COM -- build vmsotel's PCSI kit for this node's architecture
$!
$!   @dev:[dir.VMSOTEL.KIT]MAKE_KIT          after @[.VMSOTEL]BUILD_VMS
$!
$! Writes ISSINOHO-<base>-VMSOTEL-<version>-1.PCSI to [.KIT_<arch>] in
$! the vmsotel tree. Needs [.BUILD]VMSOTEL.EXE, VMSOTEL_TLS.EXE and
$! VMSOTEL_LAUNCH.EXE, and refuses agent images whose --version is not
$! VO_VERSION in src/model/version.h. PRODUCT PACKAGE needs no
$! privilege.
$!
$! The product description and text module are the templates in
$! [.KIT] with @PRODUCER@, @BASE@, @PCSIVERSION@, @VERSION@ and @ARCH@
$! filled in; README.VMS too. tools/check_kit.py, in make check, checks
$! that every file the description names is gathered here.
$!
$ set noon
$ set process/parse_style=extended
$ say := write sys$output
$ status = 44
$ saved_default = f$environment("DEFAULT")
$ proc = f$environment("PROCEDURE")
$ set default 'f$parse(proc,,,"DEVICE")''f$parse(proc,,,"DIRECTORY")'
$ set default [-]
$ arch = f$edit(f$getsyi("ARCH_NAME"), "UPCASE")
$ base = "I64VMS"
$ archname = "Integrity"
$ if arch .eqs. "X86_64" then base = "X86VMS"
$ if arch .eqs. "X86_64" then archname = "x86-64"
$ producer = "ISSINOHO"
$!
$! ---- the version, from src/model/version.h alone ----
$! The value between the quotes. The quote character is made from its
$! code, and the IFs are blocks: the command after a one-line THEN is
$! parsed again, which ate a level of doubled quotes ("""" became ""),
$! and the version read as a lone quote (x86, the first MAKE_KIT).
$ q = " "
$ q[0,8] = 34
$ version = ""
$ pcsiversion = ""
$ open/read/error=no_version vh [.src.model]version.h
$ vh_loop:
$   read/end_of_file=vh_done vh line
$   if f$locate("#define VO_VERSION ", line) .eq. 0
$   then
$       version = f$element(1, q, line)
$   endif
$   if f$locate("#define VO_PCSI_VERSION ", line) .eq. 0
$   then
$       pcsiversion = f$element(1, q, line)
$   endif
$   goto vh_loop
$ vh_done:
$ close vh
$ no_version:
$ if version .eqs. "" .or. pcsiversion .eqs. "" .or. -
     version .eqs. q .or. pcsiversion .eqs. q
$ then
$     say "MAKE_KIT: no VO_VERSION or VO_PCSI_VERSION in [.src.model]version.h"
$     goto done
$ endif
$!
$! ---- the images, and the version they report ----
$ images = "VMSOTEL,VMSOTEL_TLS,VMSOTEL_LAUNCH"
$ i = 0
$ img_loop:
$   img = f$element(i, ",", images)
$   if img .eqs. "," then goto img_done
$   if f$search("[.BUILD]''img'.EXE") .eqs. ""
$   then
$       say "MAKE_KIT: no [.BUILD]''img'.EXE; @BUILD_VMS first"
$       goto done
$   endif
$   i = i + 1
$   goto img_loop
$ img_done:
$ say "MAKE_KIT: version.h says ''version', PCSI ''pcsiversion'"
$ agents = "VMSOTEL,VMSOTEL_TLS"
$ i = 0
$ ver_loop:
$   img = f$element(i, ",", agents)
$   if img .eqs. "," then goto ver_done
$   agent = "$" + f$search("[.BUILD]''img'.EXE")
$   define/user sys$output [.BUILD]MAKE_KIT_VERSION.TMP
$   agent "--version"
$   got = ""
$   open/read v [.BUILD]MAKE_KIT_VERSION.TMP
$   read/end_of_file=ver_read v got
$   ver_read:
$   close v
$   delete/nolog [.BUILD]MAKE_KIT_VERSION.TMP;*
$   if got .nes. "vmsotel " + version
$   then
$       say "MAKE_KIT: ''img'.EXE says """, got, """; the kit is ", version, ". Rebuild."
$       goto done
$   endif
$   i = i + 1
$   goto ver_loop
$ ver_done:
$!
$! ---- gather the files, flat: PRODUCT PACKAGE finds each by name ----
$ out = "[.KIT_''arch']"
$ mat = "[.KIT_''arch'.MAT]"
$ if f$search("KIT_''arch'.DIR") .eqs. "" then create/directory 'out'
$ if f$search("[.KIT_''arch']MAT.DIR") .eqs. "" then create/directory 'mat'
$ if f$search("''out'*.PCSI;*") .nes. "" then delete/nolog 'out'*.PCSI;*
$ if f$search("''out'*.PCSI$*;*") .nes. "" then delete/nolog 'out'*.PCSI$*;*
$ if f$search("''mat'*.*;*") .nes. "" then delete/nolog 'mat'*.*;*
$ copy/nolog [.BUILD]VMSOTEL.EXE,VMSOTEL_TLS.EXE,VMSOTEL_LAUNCH.EXE 'mat'
$ copy/nolog []VMSOTEL_START.COM,VMSOTEL_RUN.COM,VMSOTEL_STOP.COM,VMSOTEL_PRIVS.COM 'mat'
$ copy/nolog [.KIT]VMSOTEL$CONFIGURE.COM,VMSOTEL$STARTUP.COM,VMSOTEL$SHUTDOWN.COM 'mat'
$ copy/nolog []VMSOTEL.CONF.EXAMPLE 'mat'VMSOTEL.CONF_EXAMPLE
$ copy/nolog [.DOCS]OPERATIONS.MD 'mat'OPERATIONS.MD
$ copy/nolog []LICENSE. 'mat'LICENSE.
$ in = "[.KIT]README.VMS"
$ to = mat + "README.VMS"
$ gosub fill
$ in = "[.KIT]VMSOTEL.PCSI$DESC"
$ to = out + "PRODUCT-''base'.PCSI$DESC"
$ gosub fill
$ in = "[.KIT]VMSOTEL.PCSI$TEXT"
$ to = out + "PRODUCT-''base'.PCSI$TEXT"
$ gosub fill
$ matspec = f$parse(mat,,,"DEVICE","NO_CONCEAL") + -
            f$parse(mat,,,"DIRECTORY","NO_CONCEAL")
$!
$ say "MAKE_KIT: ''producer' ''base' VMSOTEL ''pcsiversion' (vmsotel ''version')"
$ product package VMSOTEL -
      /producer='producer' /base_system='base' /version='pcsiversion' -
      /source='out'PRODUCT-'base'.PCSI$DESC -
      /material='matspec' -
      /destination='out' -
      /format=sequential -
      /options=noconfirm /log
$ status = $status
$ kit = f$search("''out'*.PCSI")
$ if kit .nes. "" then say "MAKE_KIT: kit ", kit
$ if kit .eqs. "" then status = 44
$ done:
$ set default 'saved_default'
$ exit status
$!
$! Copies file "in" to "to", replacing each @NAME@ by its value.
$ fill:
$ open/read fin 'in'
$ open/write fout 'to'
$ fill_line:
$   read/end_of_file=fill_end fin line
$   t = 0
$ fill_token:
$   name = f$element(t, ",", "PRODUCER,BASE,PCSIVERSION,VERSION,ARCH")
$   if name .eqs. "," then goto fill_write
$   tok = "@" + name + "@"
$   if name .eqs. "PRODUCER" then val = producer
$   if name .eqs. "BASE" then val = base
$   if name .eqs. "PCSIVERSION" then val = pcsiversion
$   if name .eqs. "VERSION" then val = version
$   if name .eqs. "ARCH" then val = archname
$ fill_again:
$   at = f$locate(tok, line)
$   if at .eq. f$length(line) then goto fill_next
$   line = f$extract(0, at, line) + val + f$extract(at + f$length(tok), 9999, line)
$   goto fill_again
$ fill_next:
$   t = t + 1
$   goto fill_token
$ fill_write:
$   write fout line
$   goto fill_line
$ fill_end:
$ close fin
$ close fout
$ return
