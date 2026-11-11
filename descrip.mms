! descrip.mms : MMS description file for the vmsotel agent
!
! Usage, from the vmsotel directory:
!     $ MMS                build VMSOTEL.EXE, VMSOTEL_TLS.EXE and
!                          VMSOTEL_LAUNCH.EXE
!     $ MMS/MACRO=VAX=1    on VAX: VMSOTEL.EXE and VMSOTEL_LAUNCH.EXE
!     $ MMS CLEAN          remove the agent's objects and images
!
! Type the target in capitals. With SET PROCESS/PARSE_STYLE=EXTENDED,
! DCL passes "clean" through in lower case, and MMS does not match it
! to CLEAN (%MMS-F-BADTARG, x86, findings 42).
!
! build_vms.com is the path to try first: it finds the OpenSSL kit,
! runs the self-test, and checks everything it does. This file builds
! the same images from the same flags; it has built them on both
! machines (findings 42).
!
! Keep it in step with build_vms.com and the Makefile: a source file
! added to one must be added to all three. vmsguard learned that each
! of its three MMS fixes was a rule, an object or an include directory
! the other two files had and this one lacked.
!
! Comments use "!", and a hyphen as the last character of any line,
! comment included, continues it -- so no comment here ends in one.
!
! MMS cannot search for OpenSSL, so the kit is named here: SSL3, the
! 32-bit-pointer images, which build_probes.com found on both machines.
! VO_EXPECT_OPENSSL_3 below makes tls.c refuse to compile if the
! headers OPENSSL selects are not 3.x. Change all three together.

SSL_INCLUDE = SSL3$INCLUDE
SSL_LIBRARY = SYS$LIBRARY:SSL3$LIBSSL_SHR32
CRYPTO_LIBRARY = SYS$LIBRARY:SSL3$LIBCRYPTO_SHR32
SSL_EXPECT = VO_EXPECT_OPENSSL_3

! PREFIX_LIBRARY_ENTRIES=ALL_ENTRIES, or every POSIX name (socket,
! poll, getaddrinfo) is left a bare symbol that fails to link. No
! /POINTER_SIZE here: the system-service code's item lists need 32-bit
! pointers. Only tls.obj and launch.obj, which call the ..._SHR32
! images, state it.

! VAX (findings, "VAX"): Compaq C V6.4 refuses /STANDARD=C99, so the
! agent is C89 there (RELAXED_ANSI89 keeps '$' in names), and there is
! no OpenSSL, so no VMSOTEL_TLS.EXE, and the launcher is built without
! it and without /POINTER_SIZE (findings V28). MMS V3.2 on VAX has no
! .IF and does not predefine __VAX__ (MMS$ARCH_NAME is there, but
! .IFDEF cannot test a value), so the VAX is named by hand, and .FIRST
! stops a build whose VAX macro does not match the machine.

.IFDEF VAX
VAX_BUILD = 1
CSTD = /STANDARD=RELAXED_ANSI89
IMAGES = [.build]vmsotel.exe, [.build]vmsotel_launch.exe
.ELSE
VAX_BUILD = 0
CSTD = /STANDARD=C99
IMAGES = [.build]vmsotel.exe, [.build]vmsotel_tls.exe, [.build]vmsotel_launch.exe
.ENDIF

INCLUDES = /INCLUDE_DIRECTORY=([.src.model],[.src.otlp],[.src.platform],-
[.src.transport],[.src.sources],[.src.agent])
CFLAGS = $(CSTD)/PREFIX_LIBRARY_ENTRIES=ALL_ENTRIES$(INCLUDES)

OPT = [.build]vmsotel_ssl.opt

CORE_OBJS = [.build]vo_types.obj,-
[.build]model.obj,[.build]procfilter.obj,[.build]logparse.obj,-
[.build]otlp_json.obj,-
[.build]platform_posix.obj,[.build]queue.obj,[.build]config.obj,-
[.build]vms_source.obj,[.build]vms_logs.obj

HEADERS = [.src.model]model.h,[.src.model]vo_types.h,[.src.model]version.h,-
[.src.model]procfilter.h,[.src.model]logparse.h,-
[.src.otlp]otlp_json.h,[.src.platform]platform.h,[.src.transport]http.h,-
[.src.transport]tls.h,[.src.transport]queue.h,[.src.sources]source.h,-
[.src.sources]logsource.h,-
[.src.agent]config.h

! [.build] must exist before the first compile, and OPENSSL must name
! the kit: VSI C turns <openssl/ssl.h> into openssl:ssl.h, so that
! logical, not the include list, picks the headers. /JOB because MMS
! runs each action in a subprocess.

.FIRST
    @ IF (F$GETSYI("ARCH_NAME") .EQS. "VAX") .NE. $(VAX_BUILD) THEN WRITE SYS$OUTPUT "vmsotel: on VAX type MMS/MACRO=VAX=1, and only there"
    @ IF (F$GETSYI("ARCH_NAME") .EQS. "VAX") .NE. $(VAX_BUILD) THEN EXIT %X10000002
    @- IF F$SEARCH("BUILD.DIR;1") .EQS. "" THEN CREATE/DIRECTORY [.build]
    @ DEFINE/JOB/NOLOG OPENSSL $(SSL_INCLUDE):

! .LAST runs only when MMS succeeds; after a failed build, DEASSIGN/JOB
! OPENSSL by hand.

.LAST
    @- IF F$TRNLNM("OPENSSL","LNM$JOB") .NES. "" THEN DEASSIGN/JOB OPENSSL

ALL : $(IMAGES)
    @ WRITE SYS$OUTPUT "build complete: $(IMAGES)"

! ==== the agent's objects ====

[.build]vo_types.obj : [.src.model]vo_types.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.model]vo_types.c

[.build]model.obj : [.src.model]model.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.model]model.c

[.build]procfilter.obj : [.src.model]procfilter.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.model]procfilter.c

[.build]logparse.obj : [.src.model]logparse.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.model]logparse.c

[.build]otlp_json.obj : [.src.otlp]otlp_json.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.otlp]otlp_json.c

! _SOCKADDR_LEN selects the BSD 4.4 socket structures getaddrinfo needs.
! On VAX platform_posix.c undefines it (see there).

[.build]platform_posix.obj : [.src.platform]platform_posix.c, $(HEADERS)
    $(CC)$(CFLAGS)/DEFINE=(_SOCKADDR_LEN)/OBJECT=$(MMS$TARGET) -
[.src.platform]platform_posix.c

[.build]queue.obj : [.src.transport]queue.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.transport]queue.c

[.build]config.obj : [.src.agent]config.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.agent]config.c

[.build]vms_source.obj : [.src.sources.vms]vms_source.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.sources.vms]vms_source.c

[.build]vms_logs.obj : [.src.sources.vms]vms_logs.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.sources.vms]vms_logs.c

[.build]http.obj : [.src.transport]http.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.transport]http.c

[.build]agent.obj : [.src.agent]agent.c, $(HEADERS)
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.agent]agent.c

! The TLS image's three: http.c and agent.c again with VO_WITH_TLS, and
! tls.c with the pointer size of the OpenSSL images.

[.build]http_tls.obj : [.src.transport]http.c, $(HEADERS)
    $(CC)$(CFLAGS)/DEFINE=(VO_WITH_TLS)/OBJECT=$(MMS$TARGET) -
[.src.transport]http.c

[.build]agent_tls.obj : [.src.agent]agent.c, $(HEADERS)
    $(CC)$(CFLAGS)/DEFINE=(VO_WITH_TLS)/OBJECT=$(MMS$TARGET) -
[.src.agent]agent.c

[.build]tls.obj : [.src.transport]tls.c, $(HEADERS)
    $(CC)$(CFLAGS)/POINTER_SIZE=32/DEFINE=(VO_WITH_TLS,$(SSL_EXPECT))-
/OBJECT=$(MMS$TARGET) [.src.transport]tls.c

! The kit's launcher: persona services, $CREPRC, and OpenSSL's
! generator, so the pointer size of the ..._SHR32 images too. On VAX,
! neither OpenSSL nor /POINTER_SIZE; that rule comes first, so that
! tools/check_build_files.py compares the other.

.IFDEF VAX
[.build]launch.obj : [.src.launch]launch.c
    $(CC)$(CFLAGS)/OBJECT=$(MMS$TARGET) [.src.launch]launch.c
.ELSE

[.build]launch.obj : [.src.launch]launch.c
    $(CC)$(CFLAGS)/POINTER_SIZE=32/OBJECT=$(MMS$TARGET) [.src.launch]launch.c
.ENDIF

! ==== linker options ====

$(OPT) :
    @ OPEN/WRITE OPTF $(OPT)
    @ WRITE OPTF "$(SSL_LIBRARY)/SHAREABLE"
    @ WRITE OPTF "$(CRYPTO_LIBRARY)/SHAREABLE"
    @ CLOSE OPTF

! ==== images ====
!
! Each link is followed by a severity test: an undefined symbol is a
! warning to LINK, which writes the image anyway and leaves $SEVERITY
! at 0. %X10000002 is an error status, which stops MMS. vmsguard
! confirmed this check both ways.

[.build]vmsotel.exe : [.build]agent.obj, [.build]http.obj, $(CORE_OBJS)
    $(LINK)/EXECUTABLE=$(MMS$TARGET) [.build]agent.obj,[.build]http.obj,-
$(CORE_OBJS)
    @ IF $SEVERITY .NE. 1 THEN EXIT %X10000002

[.build]vmsotel_tls.exe : [.build]agent_tls.obj, [.build]http_tls.obj, -
[.build]tls.obj, $(CORE_OBJS), $(OPT)
    $(LINK)/EXECUTABLE=$(MMS$TARGET) [.build]agent_tls.obj,-
[.build]http_tls.obj,[.build]tls.obj,$(CORE_OBJS),$(OPT)/OPTIONS
    @ IF $SEVERITY .NE. 1 THEN EXIT %X10000002

.IFDEF VAX
[.build]vmsotel_launch.exe : [.build]launch.obj
    $(LINK)/EXECUTABLE=$(MMS$TARGET) [.build]launch.obj
    @ IF $SEVERITY .NE. 1 THEN EXIT %X10000002
.ELSE
[.build]vmsotel_launch.exe : [.build]launch.obj, $(OPT)
    $(LINK)/EXECUTABLE=$(MMS$TARGET) [.build]launch.obj,$(OPT)/OPTIONS
    @ IF $SEVERITY .NE. 1 THEN EXIT %X10000002
.ENDIF

! ==== housekeeping ====
!
! The agent's products only; the probes' objects and images in [.build]
! are left alone. No lower-case alias: see the note at the top.

CLEAN :
    @- IF F$SEARCH("[.build]vmsotel*.exe") .NES. "" THEN DELETE/NOCONFIRM [.build]vmsotel*.exe;*
    @- IF F$SEARCH("[.build]vmsotel_ssl.opt") .NES. "" THEN DELETE/NOCONFIRM [.build]vmsotel_ssl.opt;*
    @- IF F$SEARCH("[.build]vo_types.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]vo_types.obj;*
    @- IF F$SEARCH("[.build]model.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]model.obj;*
    @- IF F$SEARCH("[.build]procfilter.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]procfilter.obj;*
    @- IF F$SEARCH("[.build]logparse.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]logparse.obj;*
    @- IF F$SEARCH("[.build]otlp_json.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]otlp_json.obj;*
    @- IF F$SEARCH("[.build]platform_posix.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]platform_posix.obj;*
    @- IF F$SEARCH("[.build]queue.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]queue.obj;*
    @- IF F$SEARCH("[.build]config.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]config.obj;*
    @- IF F$SEARCH("[.build]vms_source.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]vms_source.obj;*
    @- IF F$SEARCH("[.build]vms_logs.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]vms_logs.obj;*
    @- IF F$SEARCH("[.build]http*.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]http*.obj;*
    @- IF F$SEARCH("[.build]agent*.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]agent*.obj;*
    @- IF F$SEARCH("[.build]tls.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]tls.obj;*
    @- IF F$SEARCH("[.build]launch.obj") .NES. "" THEN DELETE/NOCONFIRM [.build]launch.obj;*
