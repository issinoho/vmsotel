$! MAKE_KIT.COM -- build vmsotel's PCSI kit for this node's architecture
$!
$!   @dev:[dir.VMSOTEL.KIT]MAKE_KIT          after @[.VMSOTEL]BUILD_VMS
$!
$! Writes ISSINOHO-<base>-VMSOTEL-<version>-1.PCSI to [.KIT_<arch>] in
$! the vmsotel tree. Needs [.BUILD]VMSOTEL.EXE, VMSOTEL_TLS.EXE and
$! VMSOTEL_LAUNCH.EXE -- on VAX, which has no OpenSSL, not
$! VMSOTEL_TLS.EXE -- and refuses agent images whose --version is not
$! VO_VERSION in src/model/version.h. PRODUCT PACKAGE needs no
$! privilege.
$!
$! The product description and text module are the templates in
$! [.KIT] with @PRODUCER@, @BASE@, @PCSIVERSION@, @VERSION@, @ARCH@,
$! @SSL3MIN@ and @NEEDS@ filled in, and @NOTVAX@: nothing, or on VAX "-- not on VAX: ", which
$! makes the rest of that line a comment; README.VMS too. tools/check_kit.py, in make check, checks
$! that every file the description names is gathered here.
$!
$ set noon
$! Not on VAX: V7.3 there has no PARSE_STYLE (findings V29).
$ if f$getsyi("ARCH_NAME") .nes. "VAX" then set process/parse_style=extended
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
$ if arch .eqs. "ALPHA" then base = "AXPVMS"
$! VAX: base VMS, as the family's other VAX kits (vms-zlib, vms-hass)
$! and the family site expect: ISSINOHO-VMS-VMSOTEL-... PCSI takes
$! VAXVMS as well; the first VAX kit used it (findings V29).
$ if arch .eqs. "VAX" then base = "VMS"
$ if arch .eqs. "X86_64" then archname = "x86-64"
$ if arch .eqs. "ALPHA" then archname = "Alpha"
$ if arch .eqs. "VAX" then archname = "VAX"
$ notvax = ""
$ if arch .eqs. "VAX" then notvax = "-- not on VAX: "
$ producer = "ISSINOHO"
$!
$! ---- the version, from src/model/version.h alone ----
$! The value between the quotes on the line that starts with the
$! #define. Compared with f$extract, not f$locate(...) .eq. 0: f$locate
$! returns the length when it finds nothing, which for an empty line is
$! 0, so every blank line matched and the last set the version to
$! f$element's "not there", the delimiter itself -- a lone quote (x86,
$! the first two MAKE_KITs).
$ q = " "
$ q[0,8] = 34
$ version = ""
$ pcsiversion = ""
$ open/read/error=no_version vh [.src.model]version.h
$ vh_loop:
$   read/end_of_file=vh_done vh line
$   if f$extract(0, 19, line) .eqs. "#define VO_VERSION "
$   then
$       version = f$element(1, q, line)
$   endif
$   if f$extract(0, 24, line) .eqs. "#define VO_PCSI_VERSION "
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
$ if arch .eqs. "VAX" then images = "VMSOTEL,VMSOTEL_LAUNCH"
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
$ if arch .eqs. "VAX" then agents = "VMSOTEL"
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
$! ---- the SSL3 the kit needs: the one its images were linked against ----
$! A linked image asks for SSL3$LIBSSL_SHR32's minor id or later
$! (LESS/EQUAL), and that id is the kit's last number: SSL3 V3.0-22's
$! image is ident "SSL 3.0-22", minor id 22. On an older SSL3 the kit
$! installed, and VMSOTEL$CONFIGURE then failed with SHRIDMISMAT (a
$! tester's node with V3.0-18, x86 images linked against 3.0-21). So
$! the description's dependency says so, and PRODUCT INSTALL refuses an
$! older one. Read from the installed image, which BUILD_VMS has just
$! linked against.
$ ssl3min = ""
$ if arch .eqs. "VAX" then goto ssl3_done
$ define/user sys$output [.BUILD]MAKE_KIT_SSL3.TMP
$ analyze/image/select=ident sys$share:ssl3$libssl_shr32.exe
$ open/read/error=ssl3_done s [.BUILD]MAKE_KIT_SSL3.TMP
$ ssl3_loop:
$   read/end_of_file=ssl3_read s line
$   at = f$locate(q + "SSL 3.", line)
$   if at .eq. f$length(line) then goto ssl3_loop
$   ssl3min = "V" + f$element(0, q, f$extract(at + 5, 99, line))
$ ssl3_read:
$ close s
$ delete/nolog [.BUILD]MAKE_KIT_SSL3.TMP;*
$ ssl3_done:
$ if arch .nes. "VAX" .and. f$locate("-", ssl3min) .eq. f$length(ssl3min)
$ then
$     say "MAKE_KIT: cannot read SSL3$LIBSSL_SHR32's ident (got """, ssl3min, """)"
$     goto done
$ endif
$ if arch .nes. "VAX" then say "MAKE_KIT: needs SSL3 ''ssl3min' or later"
$! README.VMS's requirements, for this architecture alone.
$ needs = "VSI TCP/IP Services and VSI SSL3 " + ssl3min + " or later"
$ if arch .eqs. "VAX" then needs = "TCP/IP Services; VAX has no OpenSSL, so no TLS"
$!
$! ---- gather the files, flat: PRODUCT PACKAGE finds each by name ----
$ out = "[.KIT_''arch']"
$ mat = "[.KIT_''arch'.MAT]"
$ if f$search("KIT_''arch'.DIR") .eqs. "" then create/directory 'out'
$ if f$search("[.KIT_''arch']MAT.DIR") .eqs. "" then create/directory 'mat'
$ if f$search("''out'*.PCSI;*") .nes. "" then delete/nolog 'out'*.PCSI;*
$ if f$search("''out'*.PCSI$*;*") .nes. "" then delete/nolog 'out'*.PCSI$*;*
$ if f$search("''mat'*.*;*") .nes. "" then delete/nolog 'mat'*.*;*
$ copy/nolog [.BUILD]VMSOTEL.EXE,VMSOTEL_LAUNCH.EXE 'mat'
$ if arch .nes. "VAX" then copy/nolog [.BUILD]VMSOTEL_TLS.EXE 'mat'
$ copy/nolog []VMSOTEL_START.COM,VMSOTEL_RUN.COM,VMSOTEL_STOP.COM,VMSOTEL_PRIVS.COM 'mat'
$ copy/nolog [.KIT]VMSOTEL$CONFIGURE.COM,VMSOTEL$STARTUP.COM,VMSOTEL$SHUTDOWN.COM 'mat'
$! VAX's ODS-2 disks allow one dot in a name, so there the source tree
$! has the example as VMSOTEL.CONF_EXAMPLE, the kit's own name for it.
$ if arch .nes. "VAX" then copy/nolog []VMSOTEL.CONF.EXAMPLE 'mat'VMSOTEL.CONF_EXAMPLE
$ if arch .eqs. "VAX" then copy/nolog []VMSOTEL.CONF_EXAMPLE 'mat'VMSOTEL.CONF_EXAMPLE
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
$! PCSI V7.3-100 on VAX has no /OPTIONS, and does not ask.
$ pkgopt = "/options=noconfirm"
$ if arch .eqs. "VAX" then pkgopt = ""
$ say "MAKE_KIT: ''producer' ''base' VMSOTEL ''pcsiversion' (vmsotel ''version')"
$ product package VMSOTEL -
      /producer='producer' /base_system='base' /version='pcsiversion' -
      /source='out'PRODUCT-'base'.PCSI$DESC -
      /material='matspec' -
      /destination='out' -
      /format=sequential -
      'pkgopt' /log
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
$   name = f$element(t, ",", "PRODUCER,BASE,PCSIVERSION,VERSION,ARCH,NOTVAX,SSL3MIN,NEEDS")
$   if name .eqs. "," then goto fill_write
$   tok = "@" + name + "@"
$   if name .eqs. "PRODUCER" then val = producer
$   if name .eqs. "BASE" then val = base
$   if name .eqs. "PCSIVERSION" then val = pcsiversion
$   if name .eqs. "VERSION" then val = version
$   if name .eqs. "ARCH" then val = archname
$   if name .eqs. "NOTVAX" then val = notvax
$   if name .eqs. "SSL3MIN" then val = ssl3min
$   if name .eqs. "NEEDS" then val = needs
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
