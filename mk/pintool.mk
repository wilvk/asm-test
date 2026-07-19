# pintool.mk — XED-decoded Intel Pin trace tier (PIN-2, pin-xed-trace-tier.md).
#
# Included by ../Makefile after mk/native-trace.mk (so HWTRACE_OBJS / DRAPP_KS_OBJ
# / DR_AVAILABLE are already defined for the T6 parity validator link). Knobs live
# in the parent Makefile / mk/docker.mk; edit targets here.
#
# The tier builds pintool/asmtest_pintool.so against a pinned, digest-gated Pin kit
# via Pin's out-of-kit PIN_ROOT make protocol (pintool/makefile), launches a fixture
# under `pin -t`, and asserts the recorded instruction/block offsets are
# byte-identical to the in-process single-step and DynamoRIO backends. Pin decodes
# with XED, so it traces Intel APX where the DynamoRIO decoder aborts (T8).
#
# The tool compiles under PinCRT (-DPIN_CRT=1 -nostdlib -fno-exceptions -fno-rtti,
# injected by the kit's makefile.unix.config), so pintool/asmtest_pintool.cpp sticks
# to the PinCRT subset — plain POSIX open/mmap, no libstdc++ iostream.
#
# REAL gates (CLAUDE.md): the pinned Pin kit is x86-64 gcc-linux, so every runnable
# target self-skips (printed reason + `1..0 # skipped`) on a non-x86-64 or non-Linux
# host — nothing to apt-install, an architecture gate like mk/cli.mk's CLI_ARCH. Pin
# itself IS installable, so on a gate-passing host a missing kit is fetched via
# scripts/fetch-pin.sh rather than skipped.

PINTOOL_ARCH := $(shell uname -m)
PINTOOL_SO   := pintool/obj-intel64/asmtest_pintool.so
PROBE_SO     := pintool/obj-intel64/probe_capture.so

.PHONY: pintool-tool pintool-test pintool-apx-test pin-probe-tool pin-probe-test

ifneq ($(PINTOOL_ARCH),x86_64)

pintool-tool pintool-test pintool-apx-test pin-probe-tool pin-probe-test:
	@echo "# SKIP pintool: Intel Pin is x86-only and this host is $(PINTOOL_ARCH)."
	@echo "1..0 # skipped"

else ifneq ($(UNAME_S),Linux)

pintool-tool pintool-test pintool-apx-test pin-probe-tool pin-probe-test:
	@echo "# SKIP pintool: the pinned Pin kit is gcc-linux; this host is $(UNAME_S)."
	@echo "1..0 # skipped"

else

# Build the out-of-kit tool. A missing Pin kit is an installable dependency, so the
# recipe fetches it (CLAUDE.md rule); PIN_HOME overrides, and the docker lane always
# sets it. Built via the kit's out-of-kit protocol (pintool/makefile).
pintool-tool:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pintool-tool (PIN_ROOT=$$home) =="; \
	cd pintool && $(MAKE) PIN_ROOT=$$home obj-intel64/asmtest_pintool.so
	@echo "built $(PINTOOL_SO)"

# Launch-under-pin fixture: exports the asmtest_trace_begin/_end markers the tool
# resolves by symbol (-rdynamic puts them in the dynamic symbol table, as the DR
# workload rule does), maps the shm channel (-lrt), materializes the shared parity
# ROUTINE, runs it twice. -Ipintool reaches pintool_shm.h. A one-step compile+link
# rule (like $(BUILD)/taint_workload) — it includes pintool_shm.h, not asmtest.h,
# so it does NOT use the generic examples/%.o rule.
$(BUILD)/pin_trace_workload: examples/pin_trace_workload.c pintool/pintool_shm.h \
                             $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Ipintool -rdynamic examples/pin_trace_workload.c -lrt -o $@

# Out-of-process validator: expected-offset assertions + byte-for-byte parity with
# the in-process single-step backend (always on, in-container) and the DynamoRIO
# backend (T7, env-gated via ASMTEST_DRCLIENT). Links the union of the test_hwtrace
# (single-step) and test_drtrace (DR app-side + assembler) link lines; drtrace_app.o
# compiles everywhere (its DR calls are dlopen-based), so this builds without a DR
# install and the DR arm self-skips at runtime. -rdynamic exports the drtrace_app.o
# markers for the in-process DR replay. One-step compile+link (like taint_validator);
# it includes pintool_shm.h, not asmtest.h, so it does NOT use the generic rule.
$(BUILD)/pin_trace_validator: examples/pin_trace_validator.c pintool/pintool_shm.h \
                              include/asmtest_hwtrace.h include/asmtest_trace.h \
                              include/asmtest_drtrace.h \
                              $(HWTRACE_OBJS) $(BUILD)/drtrace_app.o $(DRAPP_KS_OBJ) \
                              $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Ipintool -DPIN_VALIDATOR_DR -rdynamic \
	      examples/pin_trace_validator.c \
	      $(HWTRACE_OBJS) $(BUILD)/drtrace_app.o $(DRAPP_KS_OBJ) \
	      $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -lrt -o $@

# pintool-test — the full parity lane. Builds tool + workload + validator, runs the
# workload under `pin -t asmtest_pintool.so`, then the out-of-process validator.
# Mirrors dr-taint-attach-coop-test (launcher-then-validator). The DR offset-parity
# arm (T7) runs when DynamoRIO is configured (DR_AVAILABLE, set when DYNAMORIO_HOME
# resolves libdynamorio); the docker-pintool image always sets it, a bare host
# without DR self-skips that arm. The single-step parity + expected-offset arms run
# unconditionally on any x86-64 Linux.
PIN_SHM ?= /asmtest_pin_trace_ci
pintool-test:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pintool-test (PIN_ROOT=$$home) =="; \
	$(MAKE) --no-print-directory pintool-tool PIN_HOME=$$home; \
	$(MAKE) --no-print-directory $(BUILD)/pin_trace_workload $(BUILD)/pin_trace_validator; \
	dr_env=""; \
	if [ -n "$(DR_AVAILABLE)" ]; then \
	  $(MAKE) --no-print-directory drtrace-client; \
	  dr_env="ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so) ASMTEST_DR_LIB=$(abspath $(DR_DLLIB))"; \
	fi; \
	rm -f /dev/shm$(PIN_SHM) 2>/dev/null || true; \
	"$$home/pin" -t $(PINTOOL_SO) -shm $(PIN_SHM) -- $(BUILD)/pin_trace_workload $(PIN_SHM); \
	env $$dr_env $(BUILD)/pin_trace_validator $(PIN_SHM)
	@$(MAKE) --no-print-directory pintool-apx-test

# --- T8: APX (EGPR/REX2) fixture — Pin traces it, DynamoRIO decoder-errors -----
# The negative control that proves the tier earns its keep: an APX routine Intel's
# XED decodes on any x86-64 host but the pinned DynamoRIO decoder rejects (DR #6226).

# Ungated XED decode assertion (runs on ANY x86-64 host — never executes the bytes).
# Links the Pin kit's XED. libxed.so pulls in libpincrt.so transitively; DT_RPATH
# (--disable-new-dtags) propagates the rpath to that transitive dep, unlike the
# default DT_RUNPATH. -isystem on the kit headers keeps -Wextra quiet about them.
$(BUILD)/pin_apx_decode: examples/pin_apx_decode.c pintool/pin_apx_fixture.h \
                         $(BUILD)/.build-flags | $(BUILD)
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	xed="$$home/extras/xed-intel64"; \
	$(CC) $(CFLAGS) -Ipintool -isystem $$xed/include examples/pin_apx_decode.c \
	  -L$$xed/lib -lxed -Wl,--disable-new-dtags \
	  -Wl,-rpath,$$xed/lib -Wl,-rpath,$$home/intel64/pinrt/lib -o $@

# APX workload (executes r16/r17 — run ONLY behind the CPUID gate) + validator
# (positive complete-trace check + fork-isolated DR negative control).
$(BUILD)/pin_apx_workload: examples/pin_apx_workload.c pintool/pin_apx_fixture.h \
                           pintool/pintool_shm.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Ipintool -rdynamic examples/pin_apx_workload.c -lrt -o $@

$(BUILD)/pin_apx_validator: examples/pin_apx_validator.c pintool/pin_apx_fixture.h \
                            pintool/pintool_shm.h include/asmtest_drtrace.h \
                            include/asmtest_trace.h $(BUILD)/drtrace_app.o \
                            $(DRAPP_KS_OBJ) $(BUILD)/trace.o \
                            $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Ipintool -rdynamic examples/pin_apx_validator.c \
	      $(BUILD)/drtrace_app.o $(DRAPP_KS_OBJ) $(BUILD)/trace.o \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -lrt -o $@

# pintool-apx-test: the decode assertion runs unconditionally; the execution halves
# (Pin positive trace + DR negative control) run only when CPUID reports APX_F — a
# REAL hardware gate (running APX on a non-APX CPU is a #UD), self-skipping with the
# printed reason otherwise. Appended to pintool-test above.
PIN_APX_SHM ?= /asmtest_pin_apx_ci
pintool-apx-test:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pintool-apx-test (PIN_ROOT=$$home) =="; \
	$(MAKE) --no-print-directory $(BUILD)/pin_apx_decode PIN_HOME=$$home; \
	echo "-- apx XED decode assertion (ungated) --"; \
	$(BUILD)/pin_apx_decode; \
	if $(BUILD)/pin_apx_decode cpuid; then \
	  echo "-- apx execution halves (CPUID APX_F present) --"; \
	  $(MAKE) --no-print-directory pintool-tool PIN_HOME=$$home; \
	  $(MAKE) --no-print-directory $(BUILD)/pin_apx_workload $(BUILD)/pin_apx_validator; \
	  dr_env=""; \
	  if [ -n "$(DR_AVAILABLE)" ]; then \
	    $(MAKE) --no-print-directory drtrace-client; \
	    dr_env="ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so) ASMTEST_DR_LIB=$(abspath $(DR_DLLIB))"; \
	  fi; \
	  rm -f /dev/shm$(PIN_APX_SHM) 2>/dev/null || true; \
	  "$$home/pin" -t $(PINTOOL_SO) -shm $(PIN_APX_SHM) -- $(BUILD)/pin_apx_workload $(PIN_APX_SHM); \
	  env $$dr_env $(BUILD)/pin_apx_validator $(PIN_APX_SHM); \
	else \
	  echo "# SKIP pintool-apx: host CPU lacks APX (hardware gate)"; \
	  echo "1..0 # skipped"; \
	fi

# --- PIN-3: probe-mode argument/return capture (pin-probe-mode-capture.md) -----
# probe_capture.so splices probes at a named routine's entry/exit and records its
# SysV arg/return registers + a pointed-to buffer into the value-trace shm channel
# (include/asmtest_valtrace_shm.h); an out-of-process validator diffs the capture
# against the INDEPENDENT ptrace single-step stepper on the same routine — two
# unrelated producers must agree. Builds on the shared Pin substrate above.

# The probe tool, built via the kit's out-of-kit protocol (pintool/makefile).
pin-probe-tool:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pin-probe-tool (PIN_ROOT=$$home) =="; \
	cd pintool && $(MAKE) PIN_ROOT=$$home obj-intel64/probe_capture.so
	@echo "built $(PROBE_SO)"

# The native capture fixture (plain standalone binary; -Iinclude is in CFLAGS so it
# reaches asmtest_valtrace_shm.h). One-step compile+link.
$(BUILD)/pin_probe_victim: examples/pin_probe_victim.c \
                           include/asmtest_valtrace_shm.h \
                           $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) examples/pin_probe_victim.c -o $@

# The out-of-process validator: drains the shm channel and, in capture mode, runs
# the ptrace reference producer. Links the hwtrace tier (ptrace_backend.o etc, the
# same union attach_trace links), plus -lrt for shm_open. It includes
# asmtest_valtrace_shm.h/asmtest_ptrace.h, not asmtest.h, so it does NOT use the
# generic examples/%.o rule.
$(BUILD)/pin_probe_validator: examples/pin_probe_validator.c \
                              include/asmtest_valtrace_shm.h \
                              include/asmtest_ptrace.h $(HWTRACE_OBJS) \
                              $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) examples/pin_probe_validator.c \
	      $(HWTRACE_OBJS) $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) \
	      $(LINK_LIBBPF) -ldl -lrt -o $@

# pin-probe-test — build tool + victim + validator, then run three scenarios under
# `pin -probe`: capture (full arg/return capture + ptrace cross-check), badptr (the
# invalid-pointer buffer is refused, not faulted), and skip (an un-probeable routine
# reports an explicit reason). The ptrace reference producer needs SYS_PTRACE, which
# the docker-pintool lane grants (--cap-add=SYS_PTRACE); the victim also opts in via
# PR_SET_PTRACER_ANY. Self-skips off x86-64/Linux via the arch gate above.
PIN_PROBE_SHM ?= /asmtest_valtrace_pin_ci
pin-probe-test:
	@home=$${PIN_HOME:-$$(sh scripts/fetch-pin.sh)}; \
	echo "== pin-probe-test (PIN_ROOT=$$home) =="; \
	$(MAKE) --no-print-directory pin-probe-tool PIN_HOME=$$home; \
	$(MAKE) --no-print-directory $(BUILD)/pin_probe_victim $(BUILD)/pin_probe_validator; \
	set -e; \
	for sc in capture badptr skip; do \
	  func=capref; [ $$sc = skip ] && func=tiny; \
	  shm=$(PIN_PROBE_SHM)_$$sc; \
	  rm -f /dev/shm$$shm 2>/dev/null || true; \
	  "$$home/pin" -probe -t $(PROBE_SO) -func $$func -shm $$shm -- \
	    $(BUILD)/pin_probe_victim $$sc; \
	  $(BUILD)/pin_probe_validator $$shm $$sc $(abspath $(BUILD)/pin_probe_victim); \
	done

endif
