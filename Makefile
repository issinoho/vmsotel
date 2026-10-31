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

# The agent: portable C99, so the full strict flags.
# Plain file names in #include, with each directory on the path: VSI C
# taking "model/model.h" relative to /INCLUDE_DIRECTORY is unproven.
AGENT_CFLAGS = $(STRICT) -Isrc/model -Isrc/otlp -Itests
MODEL_OBJ = $(B)/model.o
OTLP_OBJ  = $(B)/otlp_json.o
TESTS = $(B)/test_model $(B)/test_otlp
GOLDEN = tests/golden/full.json tests/golden/minimal.json

ALL = $(PROBES) $(TESTS)

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

$(B)/model.o: src/model/model.c src/model/model.h src/model/vo_types.h | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

$(B)/otlp_json.o: src/otlp/otlp_json.c src/otlp/otlp_json.h src/model/model.h src/model/vo_types.h | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

$(B)/fixture.o: tests/fixture.c tests/fixture.h src/model/model.h src/otlp/otlp_json.h | $(B)
	$(CC) $(AGENT_CFLAGS) -c -o $@ $<

$(B)/test_model: tests/test_model.c tests/check.h $(B)/fixture.o $(MODEL_OBJ) | $(B)
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(MODEL_OBJ)

$(B)/test_otlp: tests/test_otlp.c tests/check.h $(B)/fixture.o $(MODEL_OBJ) $(OTLP_OBJ) | $(B)
	$(CC) $(AGENT_CFLAGS) -o $@ $< $(B)/fixture.o $(MODEL_OBJ) $(OTLP_OBJ)

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
	$(PYTHON) -I tools/fake_collector.py --validate $(GOLDEN)
	for p in $(VMS_PROBES); do echo "== $$p"; $$p || exit 1; done
	$(PYTHON) -I tools/fake_collector.py --selftest $(B)/probe_otlp $(B)/probe_otlp_tls

clean:
	rm -rf $(B)

.PHONY: all check clean items golden
