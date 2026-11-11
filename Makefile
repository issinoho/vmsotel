# vmsotel, Linux side.
#
#   make            build everything that can be built here
#   make check      run the agent's unit tests, check the encoder's
#                   golden payloads against the official OTLP schema,
#                   and run every probe: the system-service probes
#                   against the stub, probe_otlp against a fake
#                   collector that validates its payload the same way
#   make golden     rewrite tests/golden/ from the encoder; read the
#                   diff before committing it
#
# The system-service probes only mean anything on OpenVMS. Here they
# link against stub/vmsstub.c, which exists so their logic runs before
# it costs a round trip -- the same reason vmsguard runs its gateway
# against a stub libpcap.

CC      ?= cc
STRICT  = -std=c99 -pedantic -Wall -Wextra -Werror
# '$' in identifiers is the one extension VMS code cannot avoid, and
# -pedantic has no switch for it alone, so the VMS-only probes drop
# -pedantic and keep the rest. probe_otlp is portable and keeps it.
VMSISH  = -std=c99 -Wall -Wextra -Werror -fdollars-in-identifiers -Istub
# The probes also build on VAX, whose Compaq C V6.4 is C89 with no
# 64-bit integer at all. Checked here: C89, long long an error. The
# stub is Linux-only and may use long long, so its headers are system
# headers for this check.
# Compaq C on VAX also predefines vms, vax, VMS and VAX as 1 in its
# relaxed mode: a parameter called vms became "1" (the agent's first VAX
# build), so they are defined here too.
VAXISH  = -std=c89 -Wall -Wextra -Werror -Wlong-long \
          -Wdeclaration-after-statement -fdollars-in-identifiers -isystem stub \
          -Dvms=1 -Dvax=1 -DVMS=1 -DVAX=1

PYTHON ?= python3

B = build
VMS_PROBES = $(B)/probe_syi $(B)/probe_rmi $(B)/probe_jpi $(B)/probe_dvi $(B)/probe_logs
PROBES = $(VMS_PROBES) $(B)/probe_otlp $(B)/probe_otlp_tls

# The agent: portable C99, so the full strict flags. platform_posix.c
# asks for POSIX itself.
# Plain file names in #include, with each directory on the path: VSI C
# taking "model/model.h" relative to /INCLUDE_DIRECTORY is unproven.
AGENT_INC = -Isrc/model -Isrc/otlp -Isrc/platform -Isrc/transport \
            -Isrc/sources -Isrc/agent -Itests
# SOFT holds the VAX switches when check-soft builds into build/soft.
AGENT_CFLAGS = $(STRICT) $(SOFT) $(AGENT_INC)
AGENT_HDRS = $(wildcard src/*/*.h) $(wildcard tests/*.h)

# Objects that do not change with TLS, and the two that do.
CORE_OBJ = $(B)/vo_types.o $(B)/model.o $(B)/procfilter.o $(B)/logparse.o \
           $(B)/otlp_json.o \
           $(B)/platform_posix.o $(B)/queue.o $(B)/config.o \
           $(B)/fake_source.o $(B)/fake_logs.o
AGENTS = $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub \
         $(B)/vmsotel_launch_vmsstub $(B)/vmsotel_launch_vax_vmsstub

# The VMS source, built here against stub/ so its logic runs before it
# costs a round trip. Like the probes it takes '$' in identifiers, so
# no -pedantic; the stub headers come first on its include path.
VMS_SRC_CFLAGS = $(VMSISH) $(SOFT) $(AGENT_INC)

TESTS = $(B)/test_types $(B)/test_model $(B)/test_logs $(B)/test_otlp \
        $(B)/test_transport
GOLDEN = tests/golden/full.json tests/golden/minimal.json tests/golden/logs.json

ALL = $(PROBES) $(TESTS) $(AGENTS) $(B)/clockjump.so

all: $(ALL)

$(B):
	mkdir -p $(B)

$(B)/vmsstub.o: stub/vmsstub.c stub/*.h | $(B)
	$(CC) $(VMSISH) -c -o $@ stub/vmsstub.c

$(B)/vmsprobe.o: probes/vmsprobe.c probes/vmsprobe.h | $(B)
	$(CC) $(VMSISH) -c -o $@ probes/vmsprobe.c

$(B)/probe_%: probes/probe_%.c probes/vmsprobe.h probes/items_*.h $(B)/vmsprobe.o $(B)/vmsstub.o | $(B)
	$(CC) $(VMSISH) -o $@ $< $(B)/vmsprobe.o $(B)/vmsstub.o

$(B)/probe_otlp: probes/probe_otlp.c | $(B)
	$(CC) $(STRICT) -o $@ $<

$(B)/probe_otlp_tls: probes/probe_otlp.c | $(B)
	$(CC) $(STRICT) -DWITH_TLS -o $@ $< -lssl -lcrypto

$(B)/vo_types.o: src/model/vo_types.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/model.o: src/model/model.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/procfilter.o: src/model/procfilter.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/logparse.o: src/model/logparse.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/otlp_json.o: src/otlp/otlp_json.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/platform_posix.o: src/platform/platform_posix.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/queue.o: src/transport/queue.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/config.o: src/agent/config.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/fake_source.o: src/sources/fake/fake_source.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/fake_logs.o: src/sources/fake/fake_logs.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/vms_source.o: src/sources/vms/vms_source.c $(AGENT_HDRS) stub/*.h | $(B)
	$(CC) $(VMS_SRC_CFLAGS) -c -o $@ $<
$(B)/vms_logs.o: src/sources/vms/vms_logs.c $(AGENT_HDRS) stub/*.h | $(B)
	$(CC) $(VMS_SRC_CFLAGS) -c -o $@ $<

$(B)/http.o: src/transport/http.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/agent.o: src/agent/agent.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<
$(B)/http_tls.o: src/transport/http.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -DVO_WITH_TLS -c -o $@ $<
$(B)/agent_tls.o: src/agent/agent.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -DVO_WITH_TLS -c -o $@ $<
$(B)/tls.o: src/transport/tls.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -DVO_WITH_TLS -c -o $@ $<

$(B)/vmsotel: $(B)/agent.o $(B)/http.o $(CORE_OBJ)
	$(CC) -o $@ $^
$(B)/vmsotel_tls: $(B)/agent_tls.o $(B)/http_tls.o $(B)/tls.o $(CORE_OBJ)
	$(CC) -o $@ $^ -lssl -lcrypto

# The plain agent with the VMS source in place of the fake one.
VMS_CORE_OBJ = $(filter-out $(B)/fake_source.o $(B)/fake_logs.o,$(CORE_OBJ)) \
               $(B)/vms_source.o $(B)/vms_logs.o
$(B)/vmsotel_vmsstub: $(B)/agent.o $(B)/http.o $(VMS_CORE_OBJ) $(B)/vmsstub.o
	$(CC) -o $@ $^

# The kit's launcher (src/launch): VMS system services, so against the
# stub, which knows one account, VMSOTEL, and prints the process it is
# asked to create. OpenSSL for --password.
$(B)/launch.o: src/launch/launch.c stub/*.h | $(B)
	$(CC) $(VMSISH) -c -o $@ $<
$(B)/vmsotel_launch_vmsstub: $(B)/launch.o $(B)/vmsstub.o
	$(CC) -o $@ $^ -lcrypto
# As VAX builds it (findings V28): C89, no OpenSSL, and the identity
# check. Not VAX's two- and three-argument persona calls, which the
# stub's prototypes do not match; those ran on the VAX.
LAUNCH_VAX = -DVO_LAUNCH_NO_OPENSSL -DVO_LAUNCH_CHECK_ID
$(B)/launch_vax.o: src/launch/launch.c stub/*.h | $(B)
	$(CC) $(subst -std=c99,-std=c89 -Wlong-long,$(VMSISH)) $(LAUNCH_VAX) -c -o $@ $<
$(B)/vmsotel_launch_vax_vmsstub: $(B)/launch_vax.o $(B)/vmsstub.o
	$(CC) -o $@ $^

# Test-only: makes CLOCK_MONOTONIC step back an hour, for the agent
# self-test. Linux and glibc; not part of the agent, not strict C99.
$(B)/clockjump.so: tests/clockjump.c | $(B)
	$(CC) -shared -fPIC -o $@ $< -ldl

$(B)/fixture.o: tests/fixture.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

# The test compares the formatter with glibc's snprintf, whose output
# gcc thinks may be truncated; it is the point of the test.
$(B)/test_types: tests/test_types.c $(B)/vo_types.o
	$(CC) $(AGENT_CFLAGS) -Wno-format-truncation -o $@ $< $(B)/vo_types.o

$(B)/test_model: tests/test_model.c $(B)/fixture.o $(B)/model.o $(B)/procfilter.o $(B)/vo_types.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o $(B)/procfilter.o $(B)/vo_types.o

$(B)/test_logs: tests/test_logs.c $(B)/logparse.o $(B)/vo_types.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/logparse.o $(B)/vo_types.o

$(B)/test_otlp: tests/test_otlp.c $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o $(B)/vo_types.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o $(B)/vo_types.o

$(B)/test_transport: tests/test_transport.c $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o $(B)/procfilter.o $(B)/vo_types.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o $(B)/procfilter.o $(B)/vo_types.o

# The generated tables are committed; regenerate them when the lists
# in gen_items.py change.
items:
	cd probes && python3 -I gen_items.py
	cd stub && python3 -I gen_stub_headers.py

golden: $(B)/test_otlp
	mkdir -p tests/golden
	$(B)/test_otlp --write tests/golden

check: all
	$(B)/test_model
	$(B)/test_logs
	$(B)/test_otlp tests/golden
	$(B)/test_transport vmsotel.conf.example
	$(PYTHON) -I tools/fake_collector.py --validate $(GOLDEN)
	python3 -I tools/check_build_files.py Makefile build_vms.com descrip.mms
	python3 -I tools/check_kit.py
	$(B)/vmsotel --version | grep -qx 'vmsotel 0.1.0'
	$(B)/vmsotel_launch_vmsstub VMSOTEL DKA0:[X.COM]R.COM DKA0:[X.LOG]R.LOG vmsotel > $(B)/launch.out
	grep -q 'as persona 7, input DKA0:\[X.COM\]R.COM, output DKA0:\[X.LOG\]R.LOG, error DKA0:\[X.LOG\]R.LOG, name VMSOTEL, priority 4, flags 0x200, quotas 1=100 3=200000 12=500 6=100 2=100 5=100 7=300000 9=20 10=4096 11=16384 13=32768$$' $(B)/launch.out
	grep -q '^vmsotel_launch: process 02001234 started as VMSOTEL$$' $(B)/launch.out
	! $(B)/vmsotel_launch_vmsstub vmsotel_x a.com a.log vmsotel > $(B)/launch.out
	grep -q 'cannot take on the account VMSOTEL_X' $(B)/launch.out
	! STUB_CREPRC_FAIL=1 $(B)/vmsotel_launch_vmsstub vmsotel a.com a.log vmsotel > $(B)/launch.out
	grep -q 'cannot create the process as VMSOTEL' $(B)/launch.out
	! $(B)/vmsotel_launch_vmsstub vmsotel a.com a.log process_name_too_long > /dev/null
	$(B)/vmsotel_launch_vmsstub --password > $(B)/launch.out
	$(B)/vmsotel_launch_vmsstub --password >> $(B)/launch.out
	test "$$(grep -cxE '[A-Z0-9]{32}' $(B)/launch.out)" = 2
	test "$$(sort -u $(B)/launch.out | wc -l)" = 2
	$(B)/vmsotel_launch_vax_vmsstub VMSOTEL DKA0:[X.COM]R.COM DKA0:[X.LOG]R.LOG vmsotel > $(B)/launch.out
	grep -q '^vmsotel_launch: process 02001234 started as VMSOTEL$$' $(B)/launch.out
	! STUB_PERSONA_IGNORED=1 $(B)/vmsotel_launch_vax_vmsstub VMSOTEL a.com a.log vmsotel > $(B)/launch.out
	grep -q 'refusing to start the agent as anyone else' $(B)/launch.out
	! grep -q CREPRC $(B)/launch.out
	$(B)/vmsotel_launch_vax_vmsstub --password > $(B)/launch.out
	$(B)/vmsotel_launch_vax_vmsstub --password >> $(B)/launch.out
	test "$$(grep -cxE '[A-Z0-9]{32}' $(B)/launch.out)" = 2
	test "$$(sort -u $(B)/launch.out | wc -l)" = 2
	$(B)/vmsotel_vmsstub --selftest
	STUB_RMI_MEM_LONG=1 $(B)/vmsotel_vmsstub --selftest > $(B)/selftest_memlong.out
	grep -q 'RMI\$$_FRLIST  *400000 ' $(B)/selftest_memlong.out
	$(PYTHON) -I tools/fake_collector.py --agent-selftest $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub
	for f in probes/vmsprobe.c probes/probe_*.c; do \
	    $(CC) $(VAXISH) -c -o /dev/null $$f || exit 1; done
	rm -f $(B)/stub_operator.log $(B)/stub_audit.journal
	for p in $(VMS_PROBES); do echo "== $$p"; $$p || exit 1; done
	rm -f $(B)/stub_operator.log
	$(B)/probe_logs --request | grep -q 'tailing works'
	$(B)/probe_logs | grep -q 'markers from earlier runs of this probe: [1-9]'
	STUB_NO_TAIL=1 $(B)/probe_logs --request > $(B)/probe_logs.out
	grep -q 'tail by reopening' $(B)/probe_logs.out
	grep -q 'ok    reopened at the last record read, by its RFA' $(B)/probe_logs.out
	STUB_NO_TAIL=1 STUB_BAD_RFA=1 $(B)/probe_logs --request > $(B)/probe_logs.out || true
	grep -q 'FAIL  go back to the last record by its RFA' $(B)/probe_logs.out
	grep -q 'tail by reopening' $(B)/probe_logs.out
	rm -f $(B)/stub_audit.journal
	! STUB_BAD_AUDIT=1 $(B)/probe_logs | grep -q 'packets add up'
	rm -f $(B)/stub_audit.journal
	$(PYTHON) -I tools/fake_collector.py --selftest $(B)/probe_otlp $(B)/probe_otlp_tls
	$(MAKE) check-soft

# The agent as VAX builds it (docs/design.md, "VAX"): vo_u64 as two
# longwords, select(), inet_addr and FIONBIO for the network, RMI$_MODES for CPU
# time. Built into build/soft and put through the same tests, golden
# payloads and agent self-tests as the native build, so the VAX paths
# are tested here and not only on the VAX.
SOFT_DEFS = -DVO_SOFT64 -DVO_NET_SELECT -DVO_NET_HOSTENT -DVO_NET_IOCTL \
            -DVO_RMI_MODES

# Every agent source the VAX compiles, as C89 with no long long; and
# no snprintf anywhere in the agent, which VAX lacks (vo_snprintf).
VAX_SRCS = src/model/vo_types.c src/model/model.c src/model/procfilter.c \
           src/model/logparse.c src/otlp/otlp_json.c \
           src/platform/platform_posix.c src/transport/queue.c \
           src/transport/http.c src/agent/config.c src/agent/agent.c \
           src/sources/vms/vms_source.c src/sources/vms/vms_logs.c

check-soft:
	$(MAKE) B=$(B)/soft SOFT="$(SOFT_DEFS)" soft-run

soft-run: $(TESTS) $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub $(B)/clockjump.so
	$(B)/test_types | grep -q 'two longwords'
	$(B)/test_types
	$(B)/test_model
	$(B)/test_logs
	$(B)/test_otlp tests/golden
	$(B)/test_transport vmsotel.conf.example
	$(B)/vmsotel_vmsstub --selftest | tee $(B)/selftest.out
	grep -q 'CPU modes from RMI\$$_MODES' $(B)/selftest.out
	grep -q 'ticks per CPU-second: 100.0' $(B)/selftest.out
	$(PYTHON) -I tools/fake_collector.py --agent-selftest $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub
	for f in $(VAX_SRCS); do \
	    $(CC) $(VAXISH) $(SOFT_DEFS) $(AGENT_INC) -c -o /dev/null $$f || exit 1; done
	! grep -n -E '(^|[^_a-z])v?snprintf *\(' $(VAX_SRCS)

clean:
	rm -rf $(B)

.PHONY: all check check-soft soft-run clean items golden
