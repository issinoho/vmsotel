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
VMS_PROBES = $(B)/probe_syi $(B)/probe_rmi $(B)/probe_jpi $(B)/probe_dvi
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
CORE_OBJ = $(B)/model.o $(B)/otlp_json.o $(B)/platform_posix.o \
           $(B)/queue.o $(B)/config.o $(B)/fake_source.o
AGENTS = $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub

# The VMS source, built here against stub/ so its logic runs before it
# costs a round trip. Like the probes it takes '$' in identifiers, so
# no -pedantic; the stub headers come first on its include path.
VMS_SRC_CFLAGS = $(VMSISH) $(AGENT_INC)

TESTS = $(B)/test_model $(B)/test_otlp $(B)/test_transport
GOLDEN = tests/golden/full.json tests/golden/minimal.json

ALL = $(PROBES) $(TESTS) $(AGENTS)

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
$(B)/vms_source.o: src/sources/vms/vms_source.c $(AGENT_HDRS) stub/*.h | $(B)
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
VMS_CORE_OBJ = $(filter-out $(B)/fake_source.o,$(CORE_OBJ)) $(B)/vms_source.o
$(B)/vmsotel_vmsstub: $(B)/agent.o $(B)/http.o $(VMS_CORE_OBJ) $(B)/vmsstub.o
	$(CC) -o $@ $^

$(B)/fixture.o: tests/fixture.c $(AGENT_HDRS) | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

$(B)/test_model: tests/test_model.c $(B)/fixture.o $(B)/model.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o

$(B)/test_otlp: tests/test_otlp.c $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(B)/model.o $(B)/otlp_json.o

$(B)/test_transport: tests/test_transport.c $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/http.o $(B)/queue.o $(B)/config.o $(B)/platform_posix.o

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
	$(B)/test_otlp tests/golden
	$(B)/test_transport vmsotel.conf.example
	$(PYTHON) -I tools/fake_collector.py --validate $(GOLDEN)
	python3 -I tools/check_build_files.py Makefile build_vms.com descrip.mms
	$(B)/vmsotel_vmsstub --selftest
	$(PYTHON) -I tools/fake_collector.py --agent-selftest $(B)/vmsotel $(B)/vmsotel_tls $(B)/vmsotel_vmsstub
	for p in $(VMS_PROBES); do echo "== $$p"; $$p || exit 1; done
	$(PYTHON) -I tools/fake_collector.py --selftest $(B)/probe_otlp $(B)/probe_otlp_tls

clean:
	rm -rf $(B)

.PHONY: all check clean items golden
