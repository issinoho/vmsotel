$! build_vms.com -- build the vmsotel agent on OpenVMS
$!
$! Usage, from any directory:
$!
$!     @[.vmsotel]build_vms              build VMSOTEL.EXE and VMSOTEL_TLS.EXE
$!     @[.vmsotel]build_vms SELFTEST     build, then the self-test beside
$!                                       SHOW MEMORY, SHOW DEVICE D, MONITOR
$!     @[.vmsotel]build_vms RUN          build, then three exports to the
$!                                       collector on docker-nuc
$!     @[.vmsotel]build_vms CLEAN        delete [.build]VMSOTEL*.EXE and
$!                                       the agent's objects
$!
$! Capture everything for pasting back with /OUTPUT:
$!
$!     @[.vmsotel]build_vms/OUTPUT=selftest.log SELFTEST
$!
$! A second parameter forces an OpenSSL family for VMSOTEL_TLS.EXE, as
$! build_probes.com allows:  @[.vmsotel]build_vms "" SSL111
$!
$! What the compiles and links rest on, all proven on both machines by
$! build_probes.com or vmsguard: /STANDARD=C99, PREFIX_LIBRARY_ENTRIES=
$! ALL_ENTRIES (or every POSIX name fails to link), _SOCKADDR_LEN for
$! the socket code, no /POINTER_SIZE for the system-service code (its
$! item lists are item_list_3 only with 32-bit pointers), and
$! /POINTER_SIZE=32 for the code that calls the OpenSSL ..._SHR32
$! images. The headers are chosen by the OPENSSL logical, defined /JOB
$! to match the image and deassigned on the way out.
$!
$! Every compile and every LINK is checked. An undefined symbol is only
$! a warning to LINK, and the image it writes runs until it reaches the
$! missing routine, so an image whose link was not clean is deleted.
$!
$! Keep this file, descrip.mms and the Makefile in step: a source file
$! added to one must be added to all three.
$!
$ set noon
$ say := write sys$output
$ old_default = f$environment("DEFAULT")
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$!
$ say "vmsotel build, ", f$getsyi("NODENAME"), ", OpenVMS ", -
      f$getsyi("VERSION"), " ", f$getsyi("ARCH_NAME")
$!
$ if p1 .eqs. "CLEAN" then goto clean
$!
$ if f$search("build.dir") .eqs. "" then create/directory [.build]
$!
$!---------------------------------------------------------------------
$! Compile
$!---------------------------------------------------------------------
$!
$! Built by concatenation, not one continued string: a "-" inside quotes
$! would fold the next line's spaces into the list.
$ cc_base = "/STANDARD=C99/PREFIX_LIBRARY_ENTRIES=ALL_ENTRIES" + -
            "/INCLUDE_DIRECTORY=([.src.model],[.src.otlp],[.src.platform]," + -
            "[.src.transport],[.src.sources],[.src.agent])"
$ failed = ""
$ warned = ""
$ built = ""
$!
$ say ""
$ say "compiling"
$ cobj = "model"
$ csrc = "[.src.model]model.c"
$ cflags = ""
$ gosub compile
$ cobj = "procfilter"
$ csrc = "[.src.model]procfilter.c"
$ gosub compile
$ cobj = "logparse"
$ csrc = "[.src.model]logparse.c"
$ gosub compile
$ cobj = "otlp_json"
$ csrc = "[.src.otlp]otlp_json.c"
$ gosub compile
$ cobj = "queue"
$ csrc = "[.src.transport]queue.c"
$ gosub compile
$ cobj = "config"
$ csrc = "[.src.agent]config.c"
$ gosub compile
$ cobj = "http"
$ csrc = "[.src.transport]http.c"
$ gosub compile
$ cobj = "agent"
$ csrc = "[.src.agent]agent.c"
$ gosub compile
$ cobj = "vms_source"
$ csrc = "[.src.sources.vms]vms_source.c"
$ gosub compile
$ cobj = "vms_logs"
$ csrc = "[.src.sources.vms]vms_logs.c"
$ gosub compile
$! _SOCKADDR_LEN selects the BSD 4.4 socket structures getaddrinfo needs.
$ cobj = "platform_posix"
$ csrc = "[.src.platform]platform_posix.c"
$ cflags = "/DEFINE=(_SOCKADDR_LEN)"
$ gosub compile
$ cflags = ""
$!
$ core = "[.build]model.obj,[.build]procfilter.obj,[.build]logparse.obj," + -
         "[.build]otlp_json.obj,[.build]platform_posix.obj," + -
         "[.build]queue.obj,[.build]config.obj,[.build]vms_source.obj," + -
         "[.build]vms_logs.obj"
$ if failed .nes. "" then goto summary
$!
$!---------------------------------------------------------------------
$! Link VMSOTEL.EXE: plain HTTP
$!---------------------------------------------------------------------
$!
$ say "linking vmsotel.exe"
$ link /EXECUTABLE=[.build]vmsotel.exe [.build]agent.obj,[.build]http.obj,'core'
$ if $severity .ne. 1
$ then
$     failed = failed + " vmsotel.exe"
$     if f$search("[.build]vmsotel.exe") .nes. "" then -
          delete/nolog [.build]vmsotel.exe;*
$ else
$     built = built + " vmsotel.exe"
$ endif
$!
$!---------------------------------------------------------------------
$! OpenSSL for VMSOTEL_TLS.EXE
$!---------------------------------------------------------------------
$!
$! The ssl image, the crypto image beside it and the headers must all
$! come from one family; this is build_probes.com's search, which found
$! SSL3 on both machines.
$!
$ ssl_candidates = "SYS$LIBRARY:SSL3$LIBSSL_SHR32,SYS$SHARE:SSL3$LIBSSL_SHR32," + -
                   "SYS$LIBRARY:SSL31$LIBSSL_SHR32,SYS$SHARE:SSL31$LIBSSL_SHR32," + -
                   "SYS$LIBRARY:SSL111$LIBSSL_SHR32,SYS$SHARE:SSL111$LIBSSL_SHR32," + -
                   "SYS$LIBRARY:SSL1$LIBSSL_SHR32,SYS$SHARE:SSL1$LIBSSL_SHR32"
$ if p2 .nes. "" then ssl_candidates = "SYS$LIBRARY:" + p2 + "$LIBSSL_SHR32," + -
                                       "SYS$SHARE:" + p2 + "$LIBSSL_SHR32"
$ ssl_lib = ""
$ i = 0
$ ssl_loop:
$   name = f$element(i, ",", ssl_candidates)
$   if name .eqs. "," then goto ssl_done
$   if f$search(name + ".EXE") .nes. ""
$   then
$       ssl_lib = name
$       goto ssl_done
$   endif
$   i = i + 1
$   goto ssl_loop
$ ssl_done:
$ if ssl_lib .eqs. ""
$ then
$     say "no 32-bit libssl image found; vmsotel_tls.exe not built. Installed:"
$     directory/size/date sys$library:*libssl*.exe,sys$share:*libssl*.exe
$     failed = failed + " vmsotel_tls.exe"
$     goto summary
$ endif
$ colon = f$locate(":", ssl_lib)
$ at_lib = f$locate("$LIBSSL", ssl_lib)
$ ssl_family = f$extract(colon + 1, at_lib - colon - 1, ssl_lib)
$ crypto_lib = f$extract(0, at_lib, ssl_lib) + "$LIBCRYPTO_SHR32"
$ ssl_include = ssl_family + "$INCLUDE"
$ if f$search(crypto_lib + ".EXE") .eqs. "" .or. f$trnlnm(ssl_include) .eqs. ""
$ then
$     say "found ''ssl_lib' but not ''crypto_lib' or ''ssl_include';"
$     say "not building vmsotel_tls.exe rather than mixing OpenSSL versions"
$     show logical SSL*$INCLUDE
$     failed = failed + " vmsotel_tls.exe"
$     goto summary
$ endif
$! tls.c refuses to compile if the headers OPENSSL selects are not the
$! family this link names.
$ ssl_expect = "VO_EXPECT_OPENSSL_3"
$ if ssl_family .eqs. "SSL111" .or. ssl_family .eqs. "SSL1" then -
      ssl_expect = "VO_EXPECT_OPENSSL_1"
$ say "OpenSSL: ''ssl_lib', ''crypto_lib', headers ''ssl_include'"
$ define/job/nolog OPENSSL 'ssl_include':
$ open/write opt [.build]vmsotel_ssl.opt
$ write opt ssl_lib + "/SHAREABLE"
$ write opt crypto_lib + "/SHAREABLE"
$ close opt
$!
$ say "compiling with TLS"
$ failed_before = failed
$ cobj = "http_tls"
$ csrc = "[.src.transport]http.c"
$ cflags = "/DEFINE=(VO_WITH_TLS)"
$ gosub compile
$ cobj = "agent_tls"
$ csrc = "[.src.agent]agent.c"
$ gosub compile
$! The one file that calls OpenSSL, so the one that states the pointer
$! size of the ..._SHR32 images it links against.
$ cobj = "tls"
$ csrc = "[.src.transport]tls.c"
$ cflags = "/POINTER_SIZE=32/DEFINE=(VO_WITH_TLS," + ssl_expect + ")"
$ gosub compile
$ cflags = ""
$ if failed .nes. failed_before then goto summary
$!
$ say "linking vmsotel_tls.exe"
$ link /EXECUTABLE=[.build]vmsotel_tls.exe -
      [.build]agent_tls.obj,[.build]http_tls.obj,[.build]tls.obj,'core', -
      [.build]vmsotel_ssl.opt/OPTIONS
$ if $severity .ne. 1
$ then
$     failed = failed + " vmsotel_tls.exe"
$     if f$search("[.build]vmsotel_tls.exe") .nes. "" then -
          delete/nolog [.build]vmsotel_tls.exe;*
$ else
$     built = built + " vmsotel_tls.exe"
$ endif
$!
$ summary:
$ say ""
$ say "built:''built'"
$ if warned .nes. "" then say "compiled with warnings:''warned'  -- paste them"
$ if failed .nes. "" then say "FAILED:''failed'  -- paste the messages above"
$ if p1 .eqs. "SELFTEST" then goto selftest
$ if p1 .eqs. "RUN" then goto run_agent
$ goto finish
$!
$!---------------------------------------------------------------------
$! SELFTEST: each value beside the command that shows it
$!---------------------------------------------------------------------
$!
$ selftest:
$ if f$search("[.build]vmsotel.exe") .eqs. "" then goto finish
$ say ""
$ say "privileges:"
$ show process/privileges
$ vmsotel := $sys$disk:[.build]vmsotel.exe
$ say ""
$ say "=== vmsotel --selftest"
$ vmsotel --selftest
$ say "=== exit status ", $status
$!  Memory first: these settle whether the source's memory and page-file
$!  values are pages or pagelets.
$ say ""
$ say "=== SHOW MEMORY/PHYSICAL"
$ show memory/physical
$ say "=== SHOW MEMORY/FILES"
$ show memory/files
$ say "=== SHOW SYSTEM/NOPROCESS"
$ show system/noprocess
$ say "=== SHOW DEVICE D"
$ show device d
$ say "=== MONITOR MODES, PAGE, IO, STATES for 5 s"
$ monitor modes,page,io,states/nodisplay/summary=[.build]monitor.sum/ending="+0:00:05"
$ type [.build]monitor.sum
$ goto finish
$!
$!---------------------------------------------------------------------
$! RUN: three exports to the collector on docker-nuc
$!---------------------------------------------------------------------
$!
$ run_agent:
$ if f$search("[.build]vmsotel.exe") .eqs. "" then goto finish
$ open/write cf [.build]docker-nuc.conf
$ write cf "! written by build_vms.com RUN"
$ write cf "host = 192.168.0.218"
$ write cf "port = 4318"
$ write cf "interval = 10"
$ close cf
$ vmsotel := $sys$disk:[.build]vmsotel.exe
$ say ""
$ say "=== vmsotel, plain HTTP to 192.168.0.218:4318, three exports"
$ vmsotel [.build]docker-nuc.conf --cycles 3
$ say "=== exit status ", $status
$!
$! TLS needs the collector's certificate to verify against: there is no
$! way to skip verification. Copy collector.crt from docker-nuc into
$! this directory to include the TLS run.
$ ca = f$search("collector.crt")
$ if ca .eqs. "" .or. f$search("[.build]vmsotel_tls.exe") .eqs. ""
$ then
$     say ""
$     say "=== TLS run skipped: needs collector.crt here and vmsotel_tls.exe"
$     goto finish
$ endif
$ ca = f$parse(ca,,,"DEVICE") + f$parse(ca,,,"DIRECTORY") + -
       f$parse(ca,,,"NAME") + f$parse(ca,,,"TYPE")
$ open/write cf [.build]docker-nuc-tls.conf
$ write cf "! written by build_vms.com RUN"
$ write cf "host = 192.168.0.218"
$ write cf "port = 4319"
$ write cf "tls = yes"
$ write cf "cafile = ''ca'"
$ write cf "interval = 10"
$ close cf
$ vmsotel_tls := $sys$disk:[.build]vmsotel_tls.exe
$ say ""
$ say "=== vmsotel_tls, TLS to 192.168.0.218:4319, verified against ''ca'"
$ vmsotel_tls [.build]docker-nuc-tls.conf --cycles 1
$ say "=== exit status ", $status
$ goto finish
$!
$!---------------------------------------------------------------------
$! One compile. A warning is reported and the build goes on, as in
$! build_probes.com; an error or worse stops anything linking it.
$!---------------------------------------------------------------------
$!
$ compile:
$   cc 'cc_base' 'cflags' /OBJECT=[.build]'cobj'.obj 'csrc'
$   sev = $severity
$   if sev .eq. 2 .or. sev .eq. 4
$   then
$       failed = failed + " " + cobj
$   else
$       if sev .eq. 0 then warned = warned + " " + cobj
$   endif
$   return
$!
$! CLEAN: the agent's images and objects only; the probes' stay.
$ clean:
$ if f$search("[.build]vmsotel*.exe") .nes. "" then -
      delete/nolog [.build]vmsotel*.exe;*
$ agent_objs = "model,procfilter,logparse,otlp_json,platform_posix,queue,config,http," + -
               "agent,vms_source,vms_logs,http_tls,agent_tls,tls"
$ i = 0
$ clean_loop:
$   o = f$element(i, ",", agent_objs)
$   if o .eqs. "," then goto clean_done
$   if f$search("[.build]''o'.obj") .nes. "" then -
        delete/nolog [.build]'o'.obj;*
$   i = i + 1
$   goto clean_loop
$ clean_done:
$ say "cleaned"
$!
$ finish:
$ if f$trnlnm("OPENSSL", "LNM$JOB") .nes. "" then deassign/job OPENSSL
$ set default 'old_default'
$ exit 1
