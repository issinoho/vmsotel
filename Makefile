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
AGENT_CFLAGS = $(STRICT) $(AGENT_INC)
AGENT_HDRS = $(wildcard src/*/*.h) $(wildcard tests/*.h)

# Objects that do not change with TLS, and the two that do.
CORE_OBJ = $(B)/model.o $(B)/procfilter.o $(B)/logparse.o $(B)/otlp_json.o \
           $(B)/platform_posix.o $(B)/queue.o $(B)/config.o \
           $(B)/fake_source.o $(B)/fake_logs.o
AGENTS = $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub \
         $(B)/vmsotel_launch_vmsstub

# The VMS source, built here against stub/ so its logic runs before it
# costs a round trip. Like the probes it takes '$' in identifiers, so
# no -pedantic; the stub headers come first on its include path.
VMS_SRC_CFLAGS = $(VMSISH) $(AGENT_INC)

TESTS = $(B)/test_model $(B)/test_logs $(B)/test_otlp $(B)/test_transport
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

# Test-only: makes CLOCK_MONOTONIC step back an hour, for the agent
# self-test. Linux and glibc; not part of the agent, not strict C99.
$(B)/clockjump.so: tests/clockjump.c | $(B)
	$(CC) -shared -fPIC -o $@ $< -ldl

$(B)/fixture.o: tests/fixture.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

$(B)/test_model: tests/test_model.c $(B)/fixture.o $(B)/model.o $(B)/procfilter.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o $(B)/procfilter.o

$(B)/test_logs: tests/test_logs.c $(B)/logparse.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/logparse.o

$(B)/test_otlp: tests/test_otlp.c $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o

$(B)/test_transport: tests/test_transport.c $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o $(B)/procfilter.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o $(B)/procfilter.o

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
	$(B)/vmsotel_vmsstub --selftest
	$(PYTHON) -I tools/fake_collector.py --agent-selftest $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub
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

clean:
	rm -rf $(B)

.PHONY: all check clean items golden
