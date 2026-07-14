# native-trace.mk — Native runtime-trace tiers: DynamoRIO (in-process) and hardware (Intel PT / CoreSight).
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

# CORPUS_LIB is the conformance fixture lib the per-binding hwtrace test lanes below
# link (via each crate's build.rs / cgo). It is fully defined in mk/bindings.mk, but
# that file is included AFTER this one, so a bare `$(CORPUS_LIB)` in a prerequisite
# here would expand to EMPTY (Make expands prerequisites when the rule is read) — the
# lane would then never build the fixture and cargo/cgo would fail to link
# -lasmtest_corpus. Define it here too (identical `:=`; bindings.mk redefines it
# harmlessly) so the `hwtrace-rust-test` / `hwtrace-go-test` corpus prereq resolves.
ifeq ($(UNAME_S),Darwin)
CORPUS_LIB := $(BUILD)/libasmtest_corpus.dylib
else
CORPUS_LIB := $(BUILD)/libasmtest_corpus.so
endif

# --- Optional DynamoRIO in-process native-trace tier -----------------------
# `make drtrace-test` traces code running NATIVELY in-process via DynamoRIO's
# Application Interface (the emulator tier traces isolated guest bytes instead).
# Two artifacts: the app library (drtrace_app.o / libasmtest_drapp) and the DR
# client (libasmtest_drclient.so, built by CMake — the lone CMake-driven
# sub-build, since DynamoRIO ships no pkg-config and needs find_package).
#
# Gated on DynamoRIO, located via two NEW knobs (every other optional dep uses
# pkg-config; DynamoRIO can't, so this new shape is deliberate):
#   DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>   runtime root (libdynamorio)
#   DYNAMORIO_DIR=$(DYNAMORIO_HOME)/cmake           find_package config dir
# When absent, drtrace-test self-skips with a clear message; drtrace_app.o still
# compiles (its dr_app_* calls are #ifdef'd out, returning ASMTEST_DR_ENODR).
DYNAMORIO_HOME ?=
DYNAMORIO_DIR  ?= $(DYNAMORIO_HOME)/cmake
DR_LIBDIR      := $(DYNAMORIO_HOME)/lib64/release
DR_DLLIB       := $(DR_LIBDIR)/libdynamorio.so
ifneq ($(wildcard $(DR_DLLIB)),)
DR_AVAILABLE := 1
endif
# The app library dlopen()s libdynamorio at runtime (its constructor reads
# DYNAMORIO_OPTIONS, which must be set first), so it links libdl, NOT libdynamorio.

# Keystone lets the host-native exec path assemble text (asm_exec_native);
# without it that one entry point returns ASMTEST_DR_ENOSYS and the rest works.
# DRAPP_KEYSTONE=0 forces it off even when Keystone is installed: assemble.o also
# carries the emulator's in-line-assembler bridges (emu_*_call_asm), whose emu_*_call
# targets are NOT linked into the standalone libasmtest_drapp, so a Keystone-enabled
# drapp .so has unresolved emu symbols and won't dlopen. The language-wrapper test
# lanes exercise only the raw-bytes path (asmtest_exec_alloc), so they build drapp
# Keystone-less and stay loadable.
DRAPP_KEYSTONE ?= 1
ifeq ($(DRAPP_KEYSTONE)$(shell pkg-config --exists keystone 2>/dev/null && echo 1),11)
DRAPP_KS_DEF     := -DASMTEST_HAVE_KEYSTONE
DRAPP_KS_OBJ     := $(BUILD)/assemble.o
DRAPP_KS_PIC_OBJ := $(BUILD)/pic/assemble.o
DRAPP_KS_LIBS    := $(KEYSTONE_LIBS)
endif

# Flag-identity tripwire for drtrace_app.o. The object is compiled with
# $(DRAPP_KS_DEF) (Keystone on/off), but that flag is NOT among the rule's
# prerequisites, so flipping DRAPP_KEYSTONE between sub-makes in the SAME build tree
# would silently reuse a stale object. That is not hypothetical: the per-binding
# lanes below invoke `$(MAKE) shared-drtrace ... DRAPP_KEYSTONE=0` because a
# Keystone-enabled drapp .so has unresolved emu_* symbols and won't dlopen — so a
# reused Keystone-on object breaks exactly those lanes (CI only escapes it by running
# each in a clean tree). Record the full compile-flag string in a sentinel that
# changes only when the flags do (Keystone, and by extension SAN/COV via CFLAGS), and
# make the objects depend on it so they rebuild precisely when a knob flips.
DRAPP_FLAGS := $(strip $(CFLAGS) $(DRAPP_KS_DEF) $(KEYSTONE_CFLAGS))
$(BUILD)/.drapp-flags: FORCE | $(BUILD)
	@printf '%s\n' '$(DRAPP_FLAGS)' | cmp -s - $@ || printf '%s\n' '$(DRAPP_FLAGS)' > $@
.PHONY: FORCE
FORCE:

# App-side library object (lifecycle + markers + W^X exec memory). No DR headers
# or link dependency (it dlopen()s libdynamorio and declares dr_app_* via dlsym),
# so it always compiles regardless of whether DynamoRIO is installed.
$(BUILD)/drtrace_app.o: src/drtrace_app.c include/asmtest_drtrace.h \
                        include/asmtest_trace.h include/asmtest_assemble.h \
                        $(BUILD)/.drapp-flags | $(BUILD)
	$(CC) $(CFLAGS) $(DRAPP_KS_DEF) $(KEYSTONE_CFLAGS) -c $< -o $@

# DynamoRIO client (.so) via CMake. Real target shape: shells out to cmake. The one
# cmake --build produces BOTH clients declared in drclient/CMakeLists.txt: the control
# client (libasmtest_drclient.so) and the data-flow L0 value client
# (libasmtest_drval_client.so, Phase 5). The value client's .so is tied to this rule as
# a grouped output below so a target that needs it triggers this same build.
.PHONY: drtrace-client
drtrace-client: $(BUILD)/libasmtest_drclient.so
$(BUILD)/libasmtest_drclient.so: src/drtrace_client.c src/dataflow_dr_client.c \
                                 src/dataflow_dr_client_inlined.c src/dataflow_dr.h \
                                 include/asmtest_taint.h drclient/CMakeLists.txt | $(BUILD)
ifndef DR_AVAILABLE
	@echo "drtrace-client: DynamoRIO not found (set DYNAMORIO_HOME); skipping"
else
	@mkdir -p $(BUILD)/drclient
	cd $(BUILD)/drclient && cmake -DDynamoRIO_DIR=$(abspath $(DYNAMORIO_DIR)) \
	    -DASMTEST_BUILD_DIR=$(abspath $(BUILD)) $(abspath drclient) >/dev/null
	cmake --build $(BUILD)/drclient >/dev/null
	@echo "drtrace-client: built $@ + $(BUILD)/libasmtest_drval_client.so"
endif
# The value client is emitted by the SAME cmake --build as the control client (both are
# add_library targets), so it need not re-run cmake: an empty recipe ties it to the rule
# above (the grouped-output idiom, avoiding GNU make 4.3's `&:` for portability).
$(BUILD)/libasmtest_drval_client.so: $(BUILD)/libasmtest_drclient.so ;
# The inlined value client (taint-tier Increment 3) is emitted by the SAME cmake
# --build (a third add_library target), so tie it to the rule above too.
$(BUILD)/libasmtest_drval_client_inlined.so: $(BUILD)/libasmtest_drclient.so ;
# The taint client (taint-tier Increment 4) is the SAME source built -DASMTEST_TAINT (a
# fourth add_library target), emitted by the SAME cmake --build; tie it in too.
$(BUILD)/libasmtest_drtaint_client.so: $(BUILD)/libasmtest_drclient.so ;

# App-side shared library (libasmtest_drapp) for the language bindings.
shared-drtrace: $(call shlib_dev,libasmtest_drapp)
$(call shlib_real,libasmtest_drapp): $(BUILD)/pic/drtrace_app.o \
                                     $(BUILD)/pic/trace.o $(DRAPP_KS_PIC_OBJ)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_drapp) $^ \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
$(call shlib_dev,libasmtest_drapp): $(call shlib_real,libasmtest_drapp)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_drapp)
	ln -sf $(notdir $(call shlib_compat,libasmtest_drapp)) $@

$(BUILD)/pic/drtrace_app.o: src/drtrace_app.c include/asmtest_drtrace.h \
                            include/asmtest_trace.h include/asmtest_assemble.h \
                            $(BUILD)/.drapp-flags | $(BUILD)/pic
	$(CC) $(CFLAGS) $(DRAPP_KS_DEF) $(KEYSTONE_CFLAGS) -fPIC -c $< -o $@

# Standalone smoke harness (run directly: --no-fork, single job). -rdynamic puts
# the marker symbols in the executable's dynamic symbol table so the DR client can
# resolve their PCs with dr_get_proc_address (the shared libasmtest_drapp exports
# them by default visibility; an executable needs --export-dynamic).
$(BUILD)/test_drtrace: $(BUILD)/drtrace_app.o $(BUILD)/trace.o \
                       $(DRAPP_KS_OBJ) $(BUILD)/test_drtrace.o
	$(CC) $(CFLAGS) -rdynamic $^ $(DRAPP_KS_LIBS) -ldl -lpthread -o $@

.PHONY: drtrace-test
drtrace-test:
ifndef DR_AVAILABLE
	@echo "== drtrace-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/test_drtrace
	@echo "== drtrace-test =="
	ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/test_drtrace
	@$(MAKE) --no-print-directory dr-valtrace-test
	@$(MAKE) --no-print-directory dr-valtrace-inlined-test
	@$(MAKE) --no-print-directory dr-taint-native-test
	@$(MAKE) --no-print-directory dr-taint-launch-test
	@$(MAKE) --no-print-directory dr-taint-prod-test
	@$(MAKE) --no-print-directory dr-taint-markerless-test
	@$(MAKE) --no-print-directory dr-taint-attach-coop-test
	@$(MAKE) --no-print-directory dr-taint-stress-test
	@$(MAKE) --no-print-directory dr-taint-multirange-test
	@$(MAKE) --no-print-directory dr-taint-gcremap-test
	@$(MAKE) --no-print-directory dr-taint-simd-test
endif

# --- Data-flow L0 VALUE producer (Phase 5, increment 1) --------------------
# The in-band, whole-process DynamoRIO analog of the scoped ptrace value producer:
# src/dataflow_dr.c (app side, reuses the drtrace lifecycle + a dedicated value client)
# fills the SAME asmtest_valtrace_t as the emulator/ptrace producers, cross-validated
# against the emulator L0 oracle. Runs inside the `make docker-drtrace` lane (drtrace-
# test invokes dr-valtrace-test above) and self-skips cleanly without DynamoRIO.
#
# dataflow_dr.o needs Capstone (the operand enumerator, asmtest_operands) exactly as
# dataflow_ptrace.o does; without it the file compiles to an ENOSYS stub that self-skips.
$(BUILD)/dataflow_dr.o: src/dataflow_dr.c src/dataflow_dr.h \
                        include/asmtest_valtrace.h include/asmtest_drtrace.h \
                        include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# The value-producer test links the emulator L0 as its ORACLE where libunicorn is
# present (-DDF_HAVE_EMU), mirroring the dataflow-ptrace suite; otherwise it runs the
# in-band capture assertions alone. Probe unicorn here — dataflow.mk, which owns the
# canonical DF_HAVE_UNICORN, is included AFTER this file.
DRVAL_HAVE_UNICORN := $(shell pkg-config --exists unicorn 2>/dev/null && echo 1)

# dr_valtrace links the pure sink + operand enumerator + the DR value producer + the
# drtrace app-side lifecycle (drtrace_app.o/trace.o, plus the Keystone bridge object
# when drapp is Keystone-enabled). -rdynamic exports the value marker so the client
# resolves its PC (as test_drtrace does for its markers).
ifeq ($(DRVAL_HAVE_UNICORN),1)
$(BUILD)/dr_valtrace.o: examples/dr_valtrace.c include/asmtest_valtrace.h \
                        $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -DDF_HAVE_EMU -c $< -o $@
$(BUILD)/dr_valtrace: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                      $(BUILD)/dataflow_dr.o $(BUILD)/dataflow_emu.o \
                      $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                      $(BUILD)/dr_valtrace.o
	$(CC) $(CFLAGS) -rdynamic $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
else
$(BUILD)/dr_valtrace.o: examples/dr_valtrace.c include/asmtest_valtrace.h \
                        $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/dr_valtrace: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                      $(BUILD)/dataflow_dr.o \
                      $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                      $(BUILD)/dr_valtrace.o
	$(CC) $(CFLAGS) -rdynamic $^ $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
endif

.PHONY: dr-valtrace-test
dr-valtrace-test:
ifndef DR_AVAILABLE
	@echo "== dr-valtrace-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/dr_valtrace
	@echo "== dr-valtrace-test (DynamoRIO L0 value producer) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drval_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_valtrace
endif

# --- Inlined value client (taint-tier Increment 3) -------------------------
# The re-platform of the clean-call value client onto inlined drmgr/drreg/drx_buf
# instrumentation (src/dataflow_dr_client_inlined.c). Runs the SAME dr_valtrace
# oracle harness — the app side (dataflow_dr.c) picks the client from
# ASMTEST_DRVAL_CLIENT, so pointing it at the inlined .so re-runs the identical
# 14-check + emulator-oracle cross-validation against the inlined producer. This
# is the A/B gate: the inlined client must pass identically to the clean-call
# client (byte-identical for every def-use-consumed field; see the client header
# for the rflags/dead-register clean-call-only divergences). Self-skips without DR.
.PHONY: dr-valtrace-inlined-test
dr-valtrace-inlined-test:
ifndef DR_AVAILABLE
	@echo "== dr-valtrace-inlined-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/dr_valtrace
	@echo "== dr-valtrace-inlined-test (inlined drmgr/drreg/drx_buf client vs emulator oracle) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drval_client_inlined.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_valtrace
endif

# --- Taint tier (Increment 4): in-band inline dst_tag = union(src_tags) -----
# The taint app driver is dataflow_dr.c built -DASMTEST_TAINT (adds the seed marker +
# asmtest_dataflow_dr_taint_run); it fills the SAME asmtest_valtrace_t PLUS a parallel
# per-step taint witness. The oracle harness (examples/dr_taint.c) diffs the client
# taint set against asmtest_slice_forward from the emulator L0, + a negative control.
# Self-skips cleanly without DynamoRIO; the body of the `make docker-taint-native` lane.
$(BUILD)/dataflow_dr_taint.o: src/dataflow_dr.c src/dataflow_dr.h \
                              include/asmtest_taint.h include/asmtest_valtrace.h \
                              include/asmtest_drtrace.h include/asmtest_trace.h \
                              $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# dr_taint links the pure sink + operand enumerator + the TAINT app driver + the drtrace
# lifecycle (+ the emulator oracle where Unicorn is present, -DDF_HAVE_EMU). -rdynamic
# exports the value + seed markers so the client resolves their PCs.
ifeq ($(DRVAL_HAVE_UNICORN),1)
$(BUILD)/dr_taint.o: examples/dr_taint.c include/asmtest_valtrace.h \
                     include/asmtest_taint.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -DDF_HAVE_EMU -c $< -o $@
$(BUILD)/dr_taint: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                   $(BUILD)/dataflow_dr_taint.o $(BUILD)/dataflow_emu.o \
                   $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                   $(BUILD)/dr_taint.o
	$(CC) $(CFLAGS) -rdynamic $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
else
$(BUILD)/dr_taint.o: examples/dr_taint.c include/asmtest_valtrace.h \
                     include/asmtest_taint.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/dr_taint: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                   $(BUILD)/dataflow_dr_taint.o \
                   $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                   $(BUILD)/dr_taint.o
	$(CC) $(CFLAGS) -rdynamic $^ $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
endif

.PHONY: dr-taint-native-test
dr-taint-native-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-native-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/dr_taint
	@$(MAKE) --no-print-directory dr-taint-inline-gate
	@echo "== dr-taint-native-test (seeded: inline taint set vs emulator forward slice) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint
	@echo "== dr-taint-native-test (negative control: unseeded => empty taint set) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint negative
	@echo "== dr-taint-native-test (sink: tainted flag reaches a branch => at_taint_hit_t) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint sink
	@echo "== dr-taint-native-test (sink negative control: unseeded => zero hits) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint sink-negative
	@echo "== dr-taint-native-test (create-on-touch: taint through a fresh-heap store) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint heapstore
	@echo "== dr-taint-native-test (per-byte union: high-byte-only seed reaches the load) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint highbyte
endif

# --- Taint tier (Increment 8): XMM/YMM (SSE/AVX) SIMD taint ------------------
# dr_taint_simd is the SIMD analog of dr_taint: the SAME taint client + app driver, driven by
# examples/dr_taint_simd.c whose fixtures flow taint THROUGH an XMM register AND an SSE 16-byte
# vectorized copy (movdqu/movdqa/movq), AND — the YMM/AVX slice — through a YMM register AND an
# AVX 32-byte vectorized copy (vmovdqu/vmovdqa), oracle-diffed against asmtest_slice_forward +
# negative controls + branch-condition sinks. The client's per-byte vector lane tags (32/YMM slot,
# XMM = low 16) + 16/32-byte SSE/AVX memory shadow are additive under -DASMTEST_TAINT (the flag-off
# value client is byte-identical). The YMM modes are AVX-gated and skip cleanly on a non-AVX CPU.
# Runs in the `make docker-taint-native` lane and self-skips cleanly without DynamoRIO.
ifeq ($(DRVAL_HAVE_UNICORN),1)
$(BUILD)/dr_taint_simd.o: examples/dr_taint_simd.c include/asmtest_valtrace.h \
                          include/asmtest_taint.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -DDF_HAVE_EMU -c $< -o $@
$(BUILD)/dr_taint_simd: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                        $(BUILD)/dataflow_dr_taint.o $(BUILD)/dataflow_emu.o \
                        $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                        $(BUILD)/dr_taint_simd.o
	$(CC) $(CFLAGS) -rdynamic $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
else
$(BUILD)/dr_taint_simd.o: examples/dr_taint_simd.c include/asmtest_valtrace.h \
                          include/asmtest_taint.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/dr_taint_simd: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                        $(BUILD)/dataflow_dr_taint.o \
                        $(BUILD)/drtrace_app.o $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                        $(BUILD)/dr_taint_simd.o
	$(CC) $(CFLAGS) -rdynamic $^ $(CAPSTONE_LIBS) \
	      $(DRAPP_KS_LIBS) -ldl -lpthread -o $@
endif

.PHONY: dr-taint-simd-test
dr-taint-simd-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-simd-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/dr_taint_simd
	@$(MAKE) --no-print-directory dr-taint-inline-gate
	@echo "== dr-taint-simd-test (copy: XMM + SSE 16-byte copy taint set vs forward slice) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd
	@echo "== dr-taint-simd-test (negative control: unseeded => empty SIMD taint set) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd negative
	@echo "== dr-taint-simd-test (sink: seeded XMM lane reaches a branch => at_taint_hit_t) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd sink
	@echo "== dr-taint-simd-test (sink negative control: unseeded => zero hits) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd sink-negative
	@echo "== dr-taint-simd-test (ymm-copy: YMM + AVX 32-byte copy taint set vs forward slice) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd ymm-copy
	@echo "== dr-taint-simd-test (ymm-negative control: unseeded => empty YMM taint set) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd ymm-negative
	@echo "== dr-taint-simd-test (ymm-sink: seeded YMM lane reaches a branch => at_taint_hit_t) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd ymm-sink
	@echo "== dr-taint-simd-test (ymm-sink negative control: unseeded => zero hits) =="
	ASMTEST_DRVAL_CLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/dr_taint_simd ymm-sink-negative
endif

# Inscount/inline sanity (an exit criterion): the taint client emits propagation INLINE —
# the ONLY dr_insert_clean_call sites are rare + off the per-instruction path: on_marker
# (region), on_seed (seed paint), on_sink_register (report), on_sink (per watched
# conditional branch), and on_store_slow (a store-tag FIRST-TOUCH slowpath, taken at most
# once per 1 MiB page — the fast path is an inline store). The per-instruction
# union/broadcast never calls out.
.PHONY: dr-taint-inline-gate
dr-taint-inline-gate:
	@n=$$(grep -c 'dr_insert_clean_call(' src/dataflow_dr_client_inlined.c); \
	 if [ "$$n" -ne 5 ]; then \
	   echo "dr-taint: expected exactly 5 clean calls (markers + per-branch sink + store first-touch slowpath), found $$n"; \
	   exit 1; \
	 fi; \
	 echo "dr-taint: propagation is inline (clean calls only at markers + sink + store slowpath) — OK"

# --- Taint tier Increment 5 (first slice): launch-under-DR native de-risk ---
# `drrun -c <taint client>.so -- ./taint_workload` runs a native workload under DR from a
# CLEAN START (not the in-process dr_inject path); the client instruments it and writes
# the sink hit into a POSIX shared-memory channel; a SEPARATE ./taint_validator process
# drains + oracle-diffs it OUT OF PROCESS. De-risks the launcher mechanics (does the same
# client work under drrun -c?), the shm transport, and the out-of-process validator — all
# WITHOUT dotnet/JIT (that is the next slice). Self-skips without DynamoRIO. The client is
# UNCHANGED from the in-process build (the build-mode question resolves to a single build).
$(BUILD)/taint_workload: examples/taint_workload.c include/asmtest_taint.h \
                         include/asmtest_taint_shm.h src/dataflow_dr.h \
                         $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc -rdynamic examples/taint_workload.c -lrt -o $@

# The validator includes asmtest_taint_shm.h, which embeds at_drval_t/at_vstep_t
# (src/dataflow_dr.h) — so it compiles -DASMTEST_TAINT -Isrc to agree with the workload
# on the shm struct layout (at_drval_t gains step_taint under the flag).
ifeq ($(DRVAL_HAVE_UNICORN),1)
$(BUILD)/taint_validator: examples/taint_validator.c include/asmtest_taint.h \
                          include/asmtest_taint_shm.h include/asmtest_valtrace.h \
                          src/dataflow_dr.h $(BUILD)/dataflow.o \
                          $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
                          $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc $(UNICORN_CFLAGS) -DDF_HAVE_EMU \
	      examples/taint_validator.c \
	      $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
	      $(UNICORN_LIBS) $(CAPSTONE_LIBS) -lrt -o $@
else
$(BUILD)/taint_validator: examples/taint_validator.c include/asmtest_taint.h \
                          include/asmtest_taint_shm.h include/asmtest_valtrace.h \
                          src/dataflow_dr.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc examples/taint_validator.c -lrt -o $@
endif

DR_DRRUN ?= $(DYNAMORIO_HOME)/bin64/drrun
LAUNCH_SHM ?= /asmtest_taint_launch_ci
.PHONY: dr-taint-launch-test
dr-taint-launch-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-launch-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_workload $(BUILD)/taint_validator
	@echo "== dr-taint-launch-test (drrun -c taint-client -- native workload; shm; out-of-proc validator) =="
	@rm -f /dev/shm$(LAUNCH_SHM) 2>/dev/null || true
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	    $(abspath $(BUILD)/taint_workload) $(LAUNCH_SHM)
	$(BUILD)/taint_validator $(LAUNCH_SHM)
endif

# --- Taint tier Increment 9 (lever 1): record-free PRODUCTION propagation -----
# `drrun -c <client>.so prod -- ./taint_workload` runs the SAME launched sink fixture under the
# RECORD-FREE production path (event_insert -> emit_taint_phase_prod): NO drx_buf record, NO GP
# snapshot, NO memory value stores, NO step_taint witness — memory-source EAs are computed inline
# via lea for the shadow read, keeping only the tag shadow + the guarded sink. Because there is no
# witness, correctness is SINK-based (a seed reaching the sink is the end-to-end proof taint
# propagated): the validator's `prod` mode drains the shm sink report and asserts the seeded run
# reports exactly one tainted kind=1 branch hit, and the `noseed` negative reports ZERO (no phantom
# taint). The value-trace / emulator-oracle / taint-SET checks are SKIPPED under prod (nothing to
# diff). This is the Increment-9 overhead lever — see dr-taint-overhead-test for the ~187x -> ~75x
# measurement. Self-skips without DynamoRIO.
PROD_SHM ?= /asmtest_taint_prod_ci
.PHONY: dr-taint-prod-test
dr-taint-prod-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-prod-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_workload $(BUILD)/taint_validator
	@echo "== dr-taint-prod-test (Increment 9 lever 1: record-free production propagation; sink-validated) =="
	@rm -f /dev/shm$(PROD_SHM) 2>/dev/null || true
	@echo "-- seeded (record-free prod: a seed still reaches the sink; NO value trace / witness) --"
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) prod -- \
	    $(abspath $(BUILD)/taint_workload) $(PROD_SHM)
	$(BUILD)/taint_validator $(PROD_SHM) prod
	@rm -f /dev/shm$(PROD_SHM) 2>/dev/null || true
	@echo "-- negative control (unseeded => zero hits under prod: no phantom taint) --"
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) prod -- \
	    $(abspath $(BUILD)/taint_workload) $(PROD_SHM) noseed
	$(BUILD)/taint_validator $(PROD_SHM) prod noseed
endif

# --- DR ATTACH tier, Increment 1 (first slice): cooperative attach + detach --
# The launch tier owns a process from a CLEAN START (drrun -c ... -- app); this lane instead
# takes over a process that is ALREADY RUNNING NATIVELY and lets it go again — the attach
# tier's headline lifecycle, on the PROVEN, non-experimental dr_app_* API (external foreign-
# PID injection is the experimental Increment 2 probe, deferred). examples/taint_attach_coop.c
# is started as a PLAIN native process (NOT under drrun): it runs the seed->derive->branch-sink
# fixture NATIVELY (DR absent, no capture), then dr_app_setup_and_start brings DR + the
# UNCHANGED libasmtest_drtaint_client.so up on itself, arms a scoped taint window (the client
# captures, sink hit written synchronously to POSIX-shm), then dr_app_stop_and_cleanup DETACHES
# (its exit event flushes the value/taint trace) and the fixture runs NATIVELY once more with
# nothing new captured. The workload emits TAP for the attach-lifecycle assertions
# (under_dynamorio() false->true->false + capture-only-while-armed); the SAME out-of-process
# taint_validator then oracle-diffs the captured window. Self-skips without DynamoRIO. The
# client is reused VERBATIM — this lane adds only the native launcher + the detach lifecycle.
$(BUILD)/taint_attach_coop.o: examples/taint_attach_coop.c include/asmtest_drtrace.h \
                              include/asmtest_taint.h include/asmtest_taint_shm.h \
                              include/asmtest_trace.h src/dataflow_dr.h \
                              $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc -c $< -o $@

# Links the drtrace app-side lifecycle (drtrace_app.o = dr_app_setup/start/stop_and_cleanup +
# W^X asmtest_exec_alloc; trace.o + the Keystone bridge object when drapp is Keystone-enabled),
# exactly as $(BUILD)/test_drtrace and $(BUILD)/dr_taint do. -rdynamic exports the marker
# symbols so the client resolves their PCs; -lrt for shm_open.
$(BUILD)/taint_attach_coop: $(BUILD)/taint_attach_coop.o $(BUILD)/drtrace_app.o \
                            $(BUILD)/trace.o $(DRAPP_KS_OBJ)
	$(CC) $(CFLAGS) -rdynamic $^ $(DRAPP_KS_LIBS) -ldl -lpthread -lrt -o $@

ATTACH_SHM ?= /asmtest_taint_attach_ci
.PHONY: dr-taint-attach-coop-test
dr-taint-attach-coop-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-attach-coop-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_attach_coop $(BUILD)/taint_validator
	@echo "== dr-taint-attach-coop-test (native -> dr_app_* self-attach -> armed capture -> detach -> native; shm; out-of-proc validator) =="
	@rm -f /dev/shm$(ATTACH_SHM) 2>/dev/null || true
	ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
	    $(BUILD)/taint_attach_coop $(ATTACH_SHM)
	$(BUILD)/taint_validator $(ATTACH_SHM)
endif

# --- DR ATTACH tier, Increment 2: external-attach empirical probe (go/no-go) ---
# The extension-load-probe move, for ATTACH. Unlike Increment 1's COOPERATIVE self-attach
# (dr_app_*, no experimental API), this probes DR's EXPERIMENTAL EXTERNAL attach: it starts a
# PLAIN native victim (examples/attach_probe_victim — a bounded heartbeat loop), lets it run
# natively, then injects DR + a minimal counting client (drclient/attach_probe.c) into the RUNNING
# process via `drrun -attach <pid>`. It records the yes/no that gates Increments 3-5: did DR take
# the running process over (the client's bb event fired over live code -> non-zero instrumented
# instructions EXECUTED = ATTACH_PROBE_TAKEOVER_OK), did the victim KEEP RUNNING (heartbeats
# continued past the attach), and did it exit native. Prints `ATTACH PROBE OK` (GO) iff all hold,
# else `ATTACH PROBE NO-GO`. Needs SYS_PTRACE for the ptrace-seize (the docker lane adds the cap).
# THROWAWAY diagnostic (not a product artifact, not wired into the main CI gate — a no-go is a
# valid research finding, recorded in docs/internal/analysis/dr-attach-probe-findings.md).
$(BUILD)/attach_probe_victim: examples/attach_probe_victim.c $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) examples/attach_probe_victim.c -o $@

.PHONY: dr-taint-attach-probe
dr-taint-attach-probe:
ifndef DR_AVAILABLE
	@echo "== dr-taint-attach-probe =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@mkdir -p $(BUILD)/attach_probe
	cd $(BUILD)/attach_probe && cmake -DDynamoRIO_DIR=$(abspath $(DYNAMORIO_DIR)) \
	    -DASMTEST_BUILD_DIR=$(abspath $(BUILD)) -DASMTEST_BUILD_ATTACH_PROBE=ON \
	    $(abspath drclient) >/dev/null
	cmake --build $(BUILD)/attach_probe >/dev/null
	@$(MAKE) $(BUILD)/attach_probe_victim
	@echo "== dr-taint-attach-probe (DR EXTERNAL attach to a running native process — Increment 2 go/no-go) =="
	@vlog=$(BUILD)/attach_victim.log; alog=$(BUILD)/attach_drrun.log; rm -f $$vlog $$alog; \
	 $(BUILD)/attach_probe_victim 2>$$vlog & vpid=$$!; \
	 sleep 2; \
	 pre=$$(grep -c VICTIM_HEARTBEAT $$vlog 2>/dev/null || true); [ -z "$$pre" ] && pre=0; \
	 echo "# victim pid=$$vpid ($$pre heartbeats native, pre-attach); attaching via 'drrun -attach' ..."; \
	 timeout 90 $(DR_DRRUN) -attach $$vpid -c $(abspath $(BUILD)/libasmtest_attach_probe.so) >$$alog 2>&1; arc=$$?; \
	 wait $$vpid 2>/dev/null; vrc=$$?; \
	 echo "# --- drrun -attach output (rc=$$arc) ---"; head -20 $$alog | sed 's/^/#   /'; \
	 echo "# --- victim log tail ---"; tail -6 $$vlog | sed 's/^/#   /'; \
	 post=$$(grep -c VICTIM_HEARTBEAT $$vlog 2>/dev/null || true); [ -z "$$post" ] && post=0; \
	 reached=$$(cat $$vlog $$alog 2>/dev/null | grep -c 'dr_client_main reached' || true); [ -z "$$reached" ] && reached=0; \
	 takeover=$$(cat $$vlog $$alog 2>/dev/null | grep -c ATTACH_PROBE_TAKEOVER_OK || true); [ -z "$$takeover" ] && takeover=0; \
	 ended=$$(grep -c VICTIM_END $$vlog 2>/dev/null || true); [ -z "$$ended" ] && ended=0; \
	 echo "# SUMMARY: client_reached=$$reached takeover_ok=$$takeover pre_beats=$$pre post_beats=$$post victim_end=$$ended attach_rc=$$arc victim_rc=$$vrc"; \
	 if [ "$$reached" -ge 1 ] && [ "$$takeover" -ge 1 ] && [ "$$post" -gt "$$pre" ] && [ "$$ended" -ge 1 ]; then \
	   echo "ATTACH PROBE OK — GO: DR external attach took over the running victim (non-zero instrumentation executed), the victim SURVIVED (heartbeats continued past attach) and exited native."; \
	 else \
	   echo "ATTACH PROBE NO-GO — external attach did not fully hold (see SUMMARY). Gates attach-tier Increments 3-5; record the mode in dr-attach-probe-findings.md."; \
	 fi
endif

# --- DR ATTACH tier, Increment 6: MANAGED-attach go/no-go probe (research-gated) --------------
# The managed analog of the Increment-2 native external-attach probe. Increment 6 is a SPIKE with a
# KILL CRITERION: the managed default deliberately stays launch-under-DR / ptrace (a clean managed
# attach wants GC-safepoint coordination — prior managed+tracing SIGTRAP history is a live warning).
# This lane empirically answers the go/no-go: can a trivial, already-RUNNING .NET process SURVIVE DR
# EXTERNAL attach + detach without swallowing a .NET SIGSEGV/SIGTRAP or crashing? It starts
# examples/managed_attach_victim (a plain dotnet process, NOT under drrun — a long-running managed
# heartbeat loop whose hot method tiers up), injects DR + the MINIMAL counting client
# (drclient/attach_probe.c, reused VERBATIM from the native probe) via `drrun -attach <pid>` mid-run,
# DETACHES via `drconfig -detach <pid>` (reaping the lingering injector — the Increment-5 lesson),
# and records whether the managed process TOOK the instrumentation (non-zero executed count =
# takeover), SURVIVED (heartbeats continued while attached), returned to NATIVE after detach
# (heartbeats advanced past detach) and exited clean with NO fatal .NET signal. Prints
# `MANAGED ATTACH PROBE OK` (GO) iff all hold, else `MANAGED ATTACH PROBE NO-GO` with the failure
# mode. Needs SYS_PTRACE (ptrace-seize) + the .NET SDK (the docker lane provides both). THROWAWAY
# diagnostic — a no-go is a valid research finding (recorded in
# docs/internal/analysis/dr-managed-attach-probe-findings.md); NOT wired into the main CI gate.
MANAGED_ATTACH_OUT ?= $(BUILD)/managed_attach_victim_out
# Increment-6 Option-1 sweep knobs (bounded experiments to see if a DR option/version flips the
# NO-GO): PROBE_DROPS = DR runtime options placed in the [DR options] slot before -c (e.g.
# `-no_mangle_app_seg` — the %fs stack-canary hypothesis — or `-thread_private`); PROBE_CLIENT_ARGS
# = client options after the client path (e.g. `noinstr` = the seize-only control: take over with
# ZERO instrumentation to isolate the seize from the per-instruction clean call). Both default empty
# (the baseline counting-client run reproduced above).
PROBE_DROPS       ?=
PROBE_CLIENT_ARGS ?=
.PHONY: dr-taint-managed-attach-probe
dr-taint-managed-attach-probe:
ifndef DR_AVAILABLE
	@echo "== dr-taint-managed-attach-probe =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { \
	  echo "== dr-taint-managed-attach-probe =="; echo "# SKIP: dotnet SDK not found"; \
	  echo "1..0 # skipped"; exit 0; }
	@mkdir -p $(BUILD)/attach_probe
	cd $(BUILD)/attach_probe && cmake -DDynamoRIO_DIR=$(abspath $(DYNAMORIO_DIR)) \
	    -DASMTEST_BUILD_DIR=$(abspath $(BUILD)) -DASMTEST_BUILD_ATTACH_PROBE=ON \
	    $(abspath drclient) >/dev/null
	cmake --build $(BUILD)/attach_probe >/dev/null
	@rm -rf $(MANAGED_ATTACH_OUT)
	@$(DOTNET) build -c Release examples/managed_attach_victim/managed_attach_victim.csproj \
	    -o $(MANAGED_ATTACH_OUT) >$(BUILD)/managed_attach_build.log 2>&1 \
	  || { echo "# dotnet build failed:"; tail -20 $(BUILD)/managed_attach_build.log; exit 1; }
	@echo "== dr-taint-managed-attach-probe (DR EXTERNAL attach to a running .NET process — Increment 6 go/no-go) [DROPS='$(PROBE_DROPS)' CLIENT_ARGS='$(PROBE_CLIENT_ARGS)'] =="
	@vlog=$(BUILD)/managed_attach_victim.log; alog=$(BUILD)/managed_attach_drrun.log; \
	 rm -f $$vlog $$alog; \
	 $(DOTNET) $(abspath $(MANAGED_ATTACH_OUT)/managed_attach_victim.dll) 2>$$vlog & lpid=$$!; \
	 vpid=""; \
	 for t in 1 2 3 4 5 6 7 8 9 10 11 12; do \
	   vpid=$$(sed -n 's/.*MANAGED_VICTIM_START pid=\([0-9]*\).*/\1/p' $$vlog 2>/dev/null | head -1); \
	   [ -n "$$vpid" ] && break; sleep 1; \
	 done; \
	 if [ -z "$$vpid" ]; then echo "not ok - managed victim never reported its pid (did not start)"; kill $$lpid 2>/dev/null; echo "MANAGED ATTACH PROBE NO-GO — victim failed to start"; exit 1; fi; \
	 sleep 3; \
	 pre=$$(grep -c MANAGED_VICTIM_HEARTBEAT $$vlog 2>/dev/null || true); [ -z "$$pre" ] && pre=0; \
	 echo "# managed victim pid=$$vpid ($$pre heartbeats native, pre-attach; JIT warmed); attaching via 'drrun -attach' ..."; \
	 timeout 120 $(DR_DRRUN) -attach $$vpid $(PROBE_DROPS) -c $(abspath $(BUILD)/libasmtest_attach_probe.so) $(PROBE_CLIENT_ARGS) >$$alog 2>&1 & apid=$$!; \
	 sleep 5; \
	 beats_attach=$$(grep -c MANAGED_VICTIM_HEARTBEAT $$vlog 2>/dev/null || true); [ -z "$$beats_attach" ] && beats_attach=0; \
	 echo "# ~5 s attached; detaching via 'drconfig -detach' ..."; \
	 $(DR_DRCONFIG) -detach $$vpid >$(BUILD)/managed_attach_cfg.log 2>&1 || true; \
	 sleep 1; kill $$apid 2>/dev/null || true; wait $$apid 2>/dev/null || true; \
	 sleep 3; \
	 beats_detach=$$(grep -c MANAGED_VICTIM_HEARTBEAT $$vlog 2>/dev/null || true); [ -z "$$beats_detach" ] && beats_detach=0; \
	 alive=$$(kill -0 $$vpid 2>/dev/null && echo 1 || echo 0); \
	 wait $$lpid 2>/dev/null; vrc=$$?; \
	 reached=$$(cat $$vlog $$alog 2>/dev/null | grep -c 'dr_client_main reached' || true); [ -z "$$reached" ] && reached=0; \
	 takeover=$$(cat $$vlog $$alog 2>/dev/null | grep -c ATTACH_PROBE_TAKEOVER_OK || true); [ -z "$$takeover" ] && takeover=0; \
	 ended=$$(grep -c MANAGED_VICTIM_END $$vlog 2>/dev/null || true); [ -z "$$ended" ] && ended=0; \
	 crash=$$(cat $$vlog $$alog 2>/dev/null | grep -icE 'stack smashing|SIGSEGV|SIGTRAP|SIGILL|SIGABRT|Segmentation fault|Fatal error|double free|corrupt|core dumped' || true); [ -z "$$crash" ] && crash=0; \
	 signame=none; if [ "$$vrc" -gt 128 ]; then n=$$((vrc-128)); \
	   case $$n in 11) signame=SIGSEGV;; 6) signame=SIGABRT;; 5) signame=SIGTRAP;; 4) signame=SIGILL;; 8) signame=SIGFPE;; *) signame=SIG$$n;; esac; fi; \
	 fatal=0; if [ "$$vrc" -ne 0 ] || [ "$$crash" -ge 1 ]; then fatal=1; fi; \
	 echo "# --- drrun -attach output (head) ---"; head -15 $$alog | sed 's/^/#   /'; \
	 echo "# --- victim log tail ---"; tail -8 $$vlog | sed 's/^/#   /'; \
	 echo "# SUMMARY: client_reached=$$reached takeover_ok=$$takeover pre_beats=$$pre attach_beats=$$beats_attach detach_beats=$$beats_detach alive_after_detach=$$alive victim_end=$$ended crash_text=$$crash fatal=$$fatal victim_rc=$$vrc ($$signame)"; \
	 if [ "$$reached" -ge 1 ] && [ "$$takeover" -ge 1 ] && [ "$$beats_detach" -gt "$$beats_attach" ] && [ "$$ended" -ge 1 ] && [ "$$fatal" -eq 0 ]; then \
	   echo "MANAGED ATTACH PROBE OK — GO: DR external attach took over a running .NET process (non-zero instrumentation executed), it SURVIVED takeover + detach (no fatal .NET signal), returned to native after detach and exited clean. Record GO in dr-managed-attach-probe-findings.md; a managed seed->sink is the follow-on."; \
	 else \
	   echo "MANAGED ATTACH PROBE NO-GO — a trivial .NET process did not survive DR external attach+detach cleanly (client_reached=$$reached, then rc=$$vrc/$$signame; see SUMMARY). Kill criterion: record the concrete failure mode in dr-managed-attach-probe-findings.md and keep managed on launch-under-DR / ptrace."; \
	 fi
endif

# --- DR ATTACH tier, Increment 3: MARKER-LESS seed/sink/region config ---------
# An attached foreign target fires NO taint markers, so the client learns what to instrument, what
# to seed, and where to report ENTIRELY from client OPTIONS + runtime module+offset resolution.
# examples/taint_markerless_victim carries the taint_sink_chain fixture + the seed buffer as STATIC
# globals (nm-resolvable offsets, PIE so offset == module offset), calls no markers; the lane passes
# `region=<victim>+0x<fixture_off>,<len> seed=<victim>+0x<seedbuf_off>,8,<color> shm=/<name>` and the
# client resolves them against the victim module's runtime base (event_module_load), OWNS the shm
# (opened via bare syscalls), captures, and a separate taint_validator (markerless mode) oracle-diffs
# the taint SET + sink hit out of process — the launch tier's oracle discipline with config from
# OUTSIDE. Seeded: a tainted kind=1 sink hit + taint set == emulator forward slice; noseed control:
# ZERO hits. Validates the config path under LAUNCH (Increment 4 reuses it over `drrun -attach`).
# Self-skips without DynamoRIO.
$(BUILD)/taint_markerless_victim: examples/taint_markerless_victim.c $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) examples/taint_markerless_victim.c -o $@

MARKERLESS_SHM ?= /asmtest_taint_markerless_ci
.PHONY: dr-taint-markerless-test
dr-taint-markerless-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-markerless-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_markerless_victim $(BUILD)/taint_validator
	@echo "== dr-taint-markerless-test (Increment 3: marker-less region/seed/shm config via client options) =="
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 seedoff=$$(nm $$vbin | awk '/ [BbDd] g_seedbuf$$/ {print "0x"$$1}' | head -1); \
	 echo "# victim=taint_markerless_victim  fixture_off=$$fixoff  seedbuf_off=$$seedoff (PIE, module-relative)"; \
	 if [ -z "$$fixoff" ] || [ -z "$$seedoff" ]; then echo "not ok - could not resolve g_fixture/g_seedbuf offsets via nm"; exit 1; fi; \
	 rm -f /dev/shm$(MARKERLESS_SHM) 2>/dev/null || true; \
	 echo "-- seeded (client configured ENTIRELY by options: region + seed + shm; the victim calls NO markers) --"; \
	 $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	   region=taint_markerless_victim+$$fixoff,0x16 \
	   seed=taint_markerless_victim+$$seedoff,0x8,0x1 shm=$(MARKERLESS_SHM) -- \
	   $$vbin 2>&1 | grep -E 'MARKERLESS_VICTIM|ASMTEST_TAINT_INSCOUNT' || true; \
	 $(BUILD)/taint_validator $(MARKERLESS_SHM) markerless
	@rm -f /dev/shm$(MARKERLESS_SHM) 2>/dev/null || true
	@echo "-- negative control (NO seed= option => zero hits) --"
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	   region=taint_markerless_victim+$$fixoff,0x16 shm=$(MARKERLESS_SHM) -- \
	   $$vbin 2>&1 | grep -E 'MARKERLESS_VICTIM' || true; \
	 $(BUILD)/taint_validator $(MARKERLESS_SHM) markerless noseed
endif

# --- DR ATTACH tier, Increment 4: EXTERNAL attach data-flow/taint end-to-end ---
# Composes Increment 2 (external attach GO) + Increment 3 (marker-less config): the taint client
# is injected into a SEPARATE, already-RUNNING native victim via `drrun -attach <pid>` and
# configured entirely by options (region/seed/shm by module+offset) — a producer ATTACHED to a
# process it did not start. The victim (taint_markerless_victim `attach`) loops the seeded
# fixture for ~12 s; the client seizes it mid-run, seeds + registers the region, and its
# post-seed runs trip the branch-condition sink into the client-owned shm, drained + checked by a
# separate taint_validator (`attach` mode: >=1 tainted kind=1 hit — SINK-based, since the attach
# window captures a VARIABLE number of runs). The victim SURVIVES attach + detach (exits native).
# Negative control: attach WITHOUT seed= => zero hits. Needs SYS_PTRACE for the ptrace-seize (the
# docker lane adds the cap, mirroring the attach probe). Self-skips without DynamoRIO.
ATTACH_EXT_SHM ?= /asmtest_taint_attach_ext_ci
.PHONY: dr-taint-attach-test
dr-taint-attach-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-attach-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_markerless_victim $(BUILD)/taint_validator
	@echo "== dr-taint-attach-test (Increment 4: EXTERNAL attach to a running native process + marker-less taint capture) =="
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 seedoff=$$(nm $$vbin | awk '/ [BbDd] g_seedbuf$$/ {print "0x"$$1}' | head -1); \
	 echo "# fixture_off=$$fixoff seedbuf_off=$$seedoff"; \
	 rm -f /dev/shm$(ATTACH_EXT_SHM) 2>/dev/null || true; \
	 echo "-- seeded: attach to the RUNNING victim + marker-less region/seed/shm; expect >=1 tainted sink hit --"; \
	 $$vbin attach 2>$(BUILD)/attach_ext_victim.log & vpid=$$!; \
	 sleep 2; \
	 timeout 90 $(DR_DRRUN) -attach $$vpid -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	   region=taint_markerless_victim+$$fixoff,0x16 \
	   seed=taint_markerless_victim+$$seedoff,0x8,0x1 shm=$(ATTACH_EXT_SHM) >$(BUILD)/attach_ext_drrun.log 2>&1; arc=$$?; \
	 wait $$vpid 2>/dev/null; vrc=$$?; \
	 hb=$$(grep -c "MARKERLESS_VICTIM heartbeat" $(BUILD)/attach_ext_victim.log 2>/dev/null || true); [ -z "$$hb" ] && hb=0; \
	 end=$$(grep -c "MARKERLESS_VICTIM done" $(BUILD)/attach_ext_victim.log 2>/dev/null || true); [ -z "$$end" ] && end=0; \
	 echo "# attach_rc=$$arc victim_rc=$$vrc heartbeats=$$hb victim_done=$$end (victim survived attach + detach, exited native)"; \
	 if [ "$$end" -lt 1 ]; then echo "not ok - victim did not exit native after attach (survival failed)"; exit 1; fi; \
	 $(BUILD)/taint_validator $(ATTACH_EXT_SHM) attach
	@rm -f /dev/shm$(ATTACH_EXT_SHM) 2>/dev/null || true
	@echo "-- negative control: attach WITHOUT seed= => zero hits --"
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 $$vbin attach 2>/dev/null & vpid=$$!; \
	 sleep 2; \
	 timeout 90 $(DR_DRRUN) -attach $$vpid -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	   region=taint_markerless_victim+$$fixoff,0x16 shm=$(ATTACH_EXT_SHM) >/dev/null 2>&1; \
	 wait $$vpid 2>/dev/null; \
	 $(BUILD)/taint_validator $(ATTACH_EXT_SHM) attach noseed
endif

# --- DR ATTACH tier, Increment 5 (first slice): DETACH correctness (return to native) ---
# Attach's take-over-and-LET-GO contract (replacing launch's one-lifecycle-per-process). Unlike
# dr-taint-attach-test (which stays attached until the victim exits), this attaches to the running
# victim, captures a window, then DETACHES MID-RUN via `drconfig -detach <pid>` and asserts the
# victim RETURNS TO NATIVE: its heartbeats keep advancing AFTER the detach (more beats at exit than
# at detach), and it exits cleanly (uncorrupted). Proof the tier can seize a process, capture, and
# release it, leaving it running native. The attach-window capture is checked too (>=1 tainted hit).
# Needs SYS_PTRACE (attach + detach both ptrace); the external-attach docker image adds the cap.
# The K-round cycling + the shadow/TLS leak assertion land in dr-taint-cycle-test below.
DR_DRCONFIG ?= $(DYNAMORIO_HOME)/bin64/drconfig
DETACH_SHM ?= /asmtest_taint_detach_ci
.PHONY: dr-taint-detach-test
dr-taint-detach-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-detach-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_markerless_victim $(BUILD)/taint_validator
	@echo "== dr-taint-detach-test (Increment 5: attach -> capture -> DETACH mid-run -> victim continues NATIVE -> exits clean) =="
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 seedoff=$$(nm $$vbin | awk '/ [BbDd] g_seedbuf$$/ {print "0x"$$1}' | head -1); \
	 vlog=$(BUILD)/detach_victim.log; rm -f $$vlog /dev/shm$(DETACH_SHM) 2>/dev/null || true; \
	 $$vbin attach 2>$$vlog & vpid=$$!; \
	 sleep 2; \
	 echo "# attaching (background) to running victim pid=$$vpid, capturing ~4s, then detaching mid-run ..."; \
	 timeout 90 $(DR_DRRUN) -attach $$vpid -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	   region=taint_markerless_victim+$$fixoff,0x16 \
	   seed=taint_markerless_victim+$$seedoff,0x8,0x1 shm=$(DETACH_SHM) >$(BUILD)/detach_drrun.log 2>&1 & apid=$$!; \
	 sleep 4; \
	 beats_detach=$$(grep -c "MARKERLESS_VICTIM heartbeat" $$vlog 2>/dev/null || true); [ -z "$$beats_detach" ] && beats_detach=0; \
	 $(DR_DRCONFIG) -detach $$vpid >$(BUILD)/detach_cfg.log 2>&1 || true; \
	 wait $$apid 2>/dev/null; \
	 sleep 3; \
	 wait $$vpid 2>/dev/null; vrc=$$?; \
	 beats_exit=$$(grep -c "MARKERLESS_VICTIM heartbeat" $$vlog 2>/dev/null || true); [ -z "$$beats_exit" ] && beats_exit=0; \
	 end=$$(grep -c "MARKERLESS_VICTIM done" $$vlog 2>/dev/null || true); [ -z "$$end" ] && end=0; \
	 echo "# beats_at_detach=$$beats_detach  beats_at_exit=$$beats_exit  victim_done=$$end  victim_rc=$$vrc"; \
	 $(BUILD)/taint_validator $(DETACH_SHM) attach; \
	 if [ "$$beats_exit" -gt "$$beats_detach" ] && [ "$$end" -ge 1 ]; then \
	   echo "ok - victim RETURNED TO NATIVE after mid-run detach ($$beats_detach -> $$beats_exit heartbeats past detach) and exited clean [DETACH -> NATIVE]"; \
	 else echo "not ok - victim did not continue native after detach or exit clean (beats $$beats_detach->$$beats_exit end=$$end)"; exit 1; fi
endif

# --- DR ATTACH tier, Increment 5 (completion): K-round attach/detach CYCLING + leak assertion ---
# The detach first slice proved ONE take-over-and-let-go; this proves the tier can do it REPEATEDLY
# on the SAME pid — the re-attach reliability the taint tier flagged as UNRELIABLE for the
# in-process path (asmtest_drtrace.h:87: "DynamoRIO's in-process re-attach is unreliable"). Attach
# is where it must either work or be documented as bounded. A single long-lived native victim
# (taint_markerless_victim attach <secs>) is seized K times: each round `drrun -attach <pid>` takes
# it over marker-less (region/seed/shm by module+offset), captures a window into a per-round
# client-owned shm, then `drconfig -detach <pid>` releases it — and the lane asserts, PER ROUND,
# (a) the attached window captured a tainted kind=1 sink hit (attach worked), (b) the victim's
# heartbeats ADVANCE after the detach (it returned to native between rounds), and (c) the surviving
# NATIVE victim does not ACCUMULATE memory across rounds — the explicit shadow/TLS/drx LEAK
# assertion. event_exit (fires on each detach) frees the reg-tag TLS + drx buffers + the 1 GiB
# shadow DIRECTORY *and* every installed 1 MiB shadow LEAF (the Increment-5 leaf-free fix; without
# it each detach orphaned the touched leaves, ~2 MiB/round). So the check has two teeth: round 1
# must not jump by a shadow-directory scale (~1 GiB, the coarse backstop), and rounds >= 2 must not
# grow by a shadow-LEAF scale (~1 MiB) over the prior round (a one-time fixed DR-attach VA footprint
# is paid in round 1, so rounds 2+ are flat iff leaves are freed). After K rounds the victim exits
# clean natively (uncorrupted, rc=0). Needs SYS_PTRACE (attach + detach both ptrace); the
# external-attach docker image adds the cap.
CYCLE_SHM    ?= /asmtest_taint_cycle_ci
CYCLE_ROUNDS ?= 3
# Round-1 backstop: native VmSize vs the pre-attach baseline must stay under this (kB). A leaked
# 1 GiB shadow directory = +1048576 kB, so 512 MiB catches it while tolerating the fixed one-time
# DR-attach VA footprint (~2-3 MiB) that external detach does not fully return.
CYCLE_LEAK_SLACK_KB ?= 524288
# Per-round (round >= 2) growth cap (kB): with leaves freed each detach the native victim is flat
# between rounds (measured ~68 kB/round residue); a single orphaned 1 MiB leaf = ~1028 kB, so a
# 512 kB cap cleanly separates freed (pass) from leaked (fail) with wide margin either side.
CYCLE_GROWTH_SLACK_KB ?= 512
.PHONY: dr-taint-cycle-test
dr-taint-cycle-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-cycle-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_markerless_victim $(BUILD)/taint_validator
	@echo "== dr-taint-cycle-test (Increment 5: K attach->capture->detach rounds on ONE pid; native + leak-free between) =="
	@vbin=$(abspath $(BUILD)/taint_markerless_victim); \
	 fixoff=$$(nm $$vbin | awk '/ [BbDd] g_fixture$$/ {print "0x"$$1}' | head -1); \
	 seedoff=$$(nm $$vbin | awk '/ [BbDd] g_seedbuf$$/ {print "0x"$$1}' | head -1); \
	 if [ -z "$$fixoff" ] || [ -z "$$seedoff" ]; then echo "not ok - could not resolve g_fixture/g_seedbuf via nm"; exit 1; fi; \
	 K=$(CYCLE_ROUNDS); dur=$$((K * 10 + 10)); \
	 vlog=$(BUILD)/cycle_victim.log; rm -f $$vlog; \
	 for r in $$(seq 1 $$K); do rm -f /dev/shm$(CYCLE_SHM)_$$r 2>/dev/null || true; done; \
	 $$vbin attach $$dur 2>$$vlog & vpid=$$!; \
	 sleep 2; \
	 base_vsz=$$(awk '/^VmSize:/{print $$2}' /proc/$$vpid/status 2>/dev/null || echo 0); [ -z "$$base_vsz" ] && base_vsz=0; \
	 echo "# victim pid=$$vpid  native VmSize baseline=$${base_vsz} kB  rounds=$$K  victim_secs=$$dur  round1_slack=$(CYCLE_LEAK_SLACK_KB) kB  per_round_slack=$(CYCLE_GROWTH_SLACK_KB) kB"; \
	 fail=0; prev_vsz=$$base_vsz; \
	 for r in $$(seq 1 $$K); do \
	   shm=$(CYCLE_SHM)_$$r; \
	   timeout 60 $(DR_DRRUN) -attach $$vpid -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) \
	     region=taint_markerless_victim+$$fixoff,0x16 \
	     seed=taint_markerless_victim+$$seedoff,0x8,0x1 shm=$$shm >$(BUILD)/cycle_drrun_$$r.log 2>&1 & apid=$$!; \
	   sleep 3; \
	   beats_d=$$(grep -c "MARKERLESS_VICTIM heartbeat" $$vlog 2>/dev/null || true); [ -z "$$beats_d" ] && beats_d=0; \
	   $(DR_DRCONFIG) -detach $$vpid >$(BUILD)/cycle_cfg_$$r.log 2>&1; drc=$$?; \
	   sleep 1; \
	   kill $$apid 2>/dev/null || true; wait $$apid 2>/dev/null || true; \
	   sleep 2; \
	   alive=$$(kill -0 $$vpid 2>/dev/null && echo 1 || echo 0); \
	   beats_n=$$(grep -c "MARKERLESS_VICTIM heartbeat" $$vlog 2>/dev/null || true); [ -z "$$beats_n" ] && beats_n=0; \
	   vsz=$$(awk '/^VmSize:/{print $$2}' /proc/$$vpid/status 2>/dev/null || echo 0); [ -z "$$vsz" ] && vsz=0; \
	   grew=$$((vsz - base_vsz)); growth=$$((vsz - prev_vsz)); \
	   cap=$$($(BUILD)/taint_validator $$shm attach >$(BUILD)/cycle_val_$$r.log 2>&1 && echo ok || echo BAD); \
	   echo "# round $$r: detach_rc=$$drc  capture=$$cap  alive_after_detach=$$alive  beats(detach->native)=$$beats_d->$$beats_n  native_VmSize=$${vsz} kB (+$${grew} vs base, +$${growth} vs prior round)"; \
	   if [ "$$cap" != ok ]; then echo "not ok - round $$r: externally-attached window captured NO tainted sink hit (attach/capture failed)"; fail=1; fi; \
	   if [ "$$alive" -ne 1 ]; then echo "not ok - round $$r: victim NOT alive after detach (detach corrupted/killed it) — return-to-native failed"; fail=1; fi; \
	   if [ "$$beats_n" -le "$$beats_d" ]; then echo "not ok - round $$r: victim did NOT advance natively after detach ($$beats_d -> $$beats_n) — return-to-native failed"; fail=1; fi; \
	   if [ "$$grew" -ge $(CYCLE_LEAK_SLACK_KB) ]; then echo "not ok - round $$r: native VmSize +$${grew} kB vs base (>= $(CYCLE_LEAK_SLACK_KB)) — shadow DIRECTORY leaked across detach"; fail=1; fi; \
	   if [ "$$r" -ge 2 ] && [ "$$growth" -ge $(CYCLE_GROWTH_SLACK_KB) ]; then echo "not ok - round $$r: native VmSize grew +$${growth} kB over the prior round (>= $(CYCLE_GROWTH_SLACK_KB)) — a shadow LEAF was orphaned (not freed) across detach"; fail=1; fi; \
	   prev_vsz=$$vsz; \
	 done; \
	 wait $$vpid 2>/dev/null; vrc=$$?; \
	 end=$$(grep -c "MARKERLESS_VICTIM done" $$vlog 2>/dev/null || true); [ -z "$$end" ] && end=0; \
	 final_vsz=$$(awk '/^VmSize:/{print $$2}' /proc/$$vpid/status 2>/dev/null || echo "(exited)"); \
	 echo "# after $$K rounds: victim_done=$$end victim_rc=$$vrc  final native VmSize=$${final_vsz}"; \
	 if [ "$$end" -lt 1 ] || [ "$$vrc" -ne 0 ]; then echo "not ok - victim did not exit clean natively after $$K attach/detach cycles (done=$$end rc=$$vrc)"; fail=1; fi; \
	 if [ "$$fail" -ne 0 ]; then echo "not ok - dr-taint-cycle-test: attach/detach cycling FAILED (see rounds above)"; exit 1; fi; \
	 echo "ok - $$K attach/capture/detach rounds on one pid: each captured, returned to native, and freed the shadow+leaves (VmSize flat round-over-round); victim exited clean [CYCLE OK]"
endif

# --- Taint tier Increment 5: concurrent-writer shadow stress ---------------
# `drrun -c <taint client>.so -- ./taint_stress` launches an N-thread native workload
# whose threads, released together by a barrier, ALL seed a disjoint buffer + run the
# branch-sink fixture at once — stressing the process-global tag shadow's concurrent
# leaf-CAS installs + single-byte tag stores and the atomic sink-report append. Validates
# the Increment-4 race policy: exactly N correct sink hits, no crash/hang, no false
# clean->tainted flip. Self-skips without DynamoRIO; needs no Capstone/Unicorn.
$(BUILD)/taint_stress: examples/taint_stress.c include/asmtest_taint.h \
                       src/dataflow_dr.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc -rdynamic examples/taint_stress.c \
	      -lpthread -o $@

.PHONY: dr-taint-stress-test
dr-taint-stress-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-stress-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_stress
	@echo "== dr-taint-stress-test (drrun -c taint-client -- N-thread concurrent process-global shadow stress) =="
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	    $(abspath $(BUILD)/taint_stress)
endif

# --- Taint tier Increment 6: multi-range / method-range scoping (native) -----
# `drrun -c <taint client>.so -- ./taint_multirange` launches a native workload whose
# fixture is ONE contiguous blob but is registered as TWO disjoint instrumented ranges with
# an un-instrumented GAP between them; the taint is carried across the gap through the
# process-global stack shadow (store in range A, reload in range B). Proves:
#   (1) range-count > 1: the client auto-appends a SET of ranges (regions=2 in its stderr);
#   (2) the boundary policy: the sink fires + the taint set oracle-diffs across the gap;
#   (3) the cost bound: scope=whole (instrument the whole window, gap included) instruments
#       strictly MORE instructions than scope=ranges (the default) — an inscount delta.
# Self-skips without DynamoRIO; the taint-set oracle self-skips without libunicorn (the
# structural checks + range-count + inscount delta still run). Body of docker-taint-native.
$(BUILD)/taint_multirange: examples/taint_multirange.c examples/taint_multirange_fixture.h \
                           include/asmtest_taint.h include/asmtest_taint_shm.h \
                           src/dataflow_dr.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc -rdynamic examples/taint_multirange.c -lrt -o $@

ifeq ($(DRVAL_HAVE_UNICORN),1)
$(BUILD)/taint_multirange_validator: examples/taint_multirange_validator.c \
                          examples/taint_multirange_fixture.h include/asmtest_taint.h \
                          include/asmtest_taint_shm.h include/asmtest_valtrace.h \
                          src/dataflow_dr.h $(BUILD)/dataflow.o \
                          $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
                          $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc $(UNICORN_CFLAGS) -DDF_HAVE_EMU \
	      examples/taint_multirange_validator.c \
	      $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
	      $(UNICORN_LIBS) $(CAPSTONE_LIBS) -lrt -o $@
else
$(BUILD)/taint_multirange_validator: examples/taint_multirange_validator.c \
                          examples/taint_multirange_fixture.h include/asmtest_taint.h \
                          include/asmtest_taint_shm.h include/asmtest_valtrace.h \
                          src/dataflow_dr.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc examples/taint_multirange_validator.c -lrt -o $@
endif

MULTIRANGE_SHM ?= /asmtest_taint_multirange_ci
.PHONY: dr-taint-multirange-test
dr-taint-multirange-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-multirange-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_multirange $(BUILD)/taint_multirange_validator
	@echo "== dr-taint-multirange-test (scope=ranges: 2 ranges + un-instrumented gap; cross-gap seed->sink; oracle diff) =="
	@rm -f /dev/shm$(MULTIRANGE_SHM) /dev/shm$(MULTIRANGE_SHM)_whole 2>/dev/null || true
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	    $(abspath $(BUILD)/taint_multirange) $(MULTIRANGE_SHM) 2> $(BUILD)/mr_ranges.log
	@grep -Eq 'ASMTEST_TAINT_INSCOUNT .*regions=2 scope=ranges' $(BUILD)/mr_ranges.log \
	  && echo "ok - client auto-registered a range SET (range-count > 1: regions=2)" \
	  || { echo "not ok - expected regions=2 scope=ranges in client output"; cat $(BUILD)/mr_ranges.log; exit 1; }
	$(BUILD)/taint_multirange_validator $(MULTIRANGE_SHM)
	@echo "== dr-taint-multirange-test (scope=whole vs scope=ranges: instrumented-instruction-count delta) =="
	$(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) scope=whole -- \
	    $(abspath $(BUILD)/taint_multirange) $(MULTIRANGE_SHM)_whole 2> $(BUILD)/mr_whole.log
	@rm -f /dev/shm$(MULTIRANGE_SHM)_whole 2>/dev/null || true
	@ir=$$(grep -oE 'inscount=[0-9]+ regions=2 scope=ranges' $(BUILD)/mr_ranges.log | grep -oE 'inscount=[0-9]+' | grep -oE '[0-9]+' | head -1); \
	 iw=$$(grep -oE 'inscount=[0-9]+ regions=2 scope=whole'  $(BUILD)/mr_whole.log  | grep -oE 'inscount=[0-9]+' | grep -oE '[0-9]+' | head -1); \
	 echo "# instrumented instructions: scope=ranges=$$ir  scope=whole=$$iw"; \
	 if [ -n "$$ir" ] && [ -n "$$iw" ] && [ "$$ir" -lt "$$iw" ]; then \
	   echo "ok - method-range scoping reduced instrumented-instruction count ($$ir < $$iw) — cost bound is real"; \
	 else echo "not ok - expected inscount(ranges) < inscount(whole) [ranges=$$ir whole=$$iw]"; exit 1; fi
endif

# --- Taint tier Increment 7: GC-move shadow remap (synthetic-triple unit test) -------------
# `drrun -c <gcremap client>.so gcremap_selftest -- /bin/true` runs, at client init, the
# synthetic-triple GC-move remap unit test: hand-provided {old,new,len} triples copied
# through the SAME BSD 2-level create-on-touch tag shadow, asserting the tag is readable at
# the NEW address and absent at the OLD one. T1-T4 exercise at_gc_remap (the DR-API remap,
# serialized under g_lock + SEQ_CST fences per the Increment-4 concurrency policy) — disjoint
# move, per-byte colour fidelity, a negative (unseeded => no phantom taint), and an OVERLAPPING
# slide. T5-T7 exercise the LIVE, DR-API-FREE at_gc_remap_live the profiler drives at the GC
# fence, proving Slice 2's raw-mmap leaf allocator (tag_ptr_create_raw, a bare mmap syscall)
# carries a tag into a NEVER-TOUCHED destination leaf — the case Slice 1 conservatively dropped.
# The remap path lives behind the DISABLED compile flag ASMTEST_TAINT_GCREMAP (a SEPARATE .so),
# so the default value/taint clients are byte-identical and this adds no clean call.
# STILL DEFERRED: the managed seed->move->sink choreography + coherence canary over a real
# forced .NET GC (the profiler-fed live path landed in dr-gcmove-live-test). TAP + a grep-able
# `ASMTEST_GCREMAP_SELFTEST checks=.. fails=..` land on stderr; the lane asserts fails=0 and
# checks>0. Self-skips cleanly without DynamoRIO.
.PHONY: dr-taint-gcremap-test
dr-taint-gcremap-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-gcremap-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@echo "== dr-taint-gcremap-test (Increment 7 partial: synthetic {old,new,len} triples; remap behind ASMTEST_TAINT_GCREMAP) =="
	@out=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_gcremap_client.so) \
	          gcremap_selftest -- /bin/true 2>&1); \
	 printf '%s\n' "$$out"; \
	 line=$$(printf '%s\n' "$$out" | grep 'ASMTEST_GCREMAP_SELFTEST'); \
	 checks=$$(printf '%s\n' "$$line" | sed -n 's/.*checks=\([0-9]*\).*/\1/p'); \
	 fails=$$(printf '%s\n' "$$line" | sed -n 's/.*fails=\([0-9]*\).*/\1/p'); \
	 if [ -n "$$fails" ] && [ "$$fails" -eq 0 ] && [ -n "$$checks" ] && [ "$$checks" -gt 0 ]; then \
	   echo "dr-taint-gcremap: $$checks synthetic-triple checks passed, 0 failed — OK"; \
	 else \
	   echo "dr-taint-gcremap: FAILED (checks=$$checks fails=$$fails)"; exit 1; \
	 fi
endif

# --- Taint tier Increment 5: dotnet launch — JIT / code-cache coexistence ---
# `drrun -c <taint client>.so -- dotnet taint_hello.dll` runs a trivial MANAGED workload
# under DR. Its hot method tiers up (tier-0 -> tier-1) mid-run, so DR's code cache must
# handle .NET's tiered JIT rewriting live code — the plan's RISK CONCENTRATION, and the
# first in-tree demonstration of DR coexisting with .NET tiered-JIT. Success = the
# workload runs to completion (prints HELLO_TAINT_DOTNET, exits 0) with no swallowed .NET
# SIGSEGV, no SIGTRAP/crash, no hang. Self-skips without DynamoRIO OR without the .NET SDK.
# The client is UNCHANGED from the native launch (single build). This slice is the
# coexistence smoke; a managed seed->sink over a managed buffer is the next slice.
DOTNET ?= dotnet
.PHONY: dr-taint-dotnet-test
dr-taint-dotnet-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-dotnet-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { \
	  echo "== dr-taint-dotnet-test =="; echo "# SKIP: dotnet SDK not found"; \
	  echo "1..0 # skipped"; exit 0; }
	@$(MAKE) drtrace-client
	@echo "== dr-taint-dotnet-test (drrun -c taint-client -- dotnet: .NET tiered-JIT / DR code-cache coexistence) =="
	@rm -rf $(BUILD)/taint_hello_out
	@$(DOTNET) build -c Release examples/taint_hello/taint_hello.csproj \
	    -o $(BUILD)/taint_hello_out >$(BUILD)/taint_hello_build.log 2>&1 \
	  || { echo "# dotnet build failed:"; tail -20 $(BUILD)/taint_hello_build.log; exit 1; }
	@out=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	          $(DOTNET) $(abspath $(BUILD)/taint_hello_out/taint_hello.dll) 2>&1); \
	  rc=$$?; printf '%s\n' "$$out"; \
	  if [ $$rc -eq 0 ]; then \
	    echo "ok 1 - dotnet workload ran to completion under drrun + taint client (rc=0, no SIGTRAP/SIGSEGV/crash/hang)"; \
	  else echo "not ok 1 - dotnet under drrun crashed/hung (rc=$$rc)"; fi; \
	  if printf '%s' "$$out" | grep -q "HELLO_TAINT_DOTNET"; then \
	    echo "ok 2 - managed tiered-JIT'd code executed correctly under DR's code cache"; \
	  else echo "not ok 2 - expected managed output missing (coexistence failure)"; fi; \
	  echo "1..2"; \
	  [ $$rc -eq 0 ] && printf '%s' "$$out" | grep -q "HELLO_TAINT_DOTNET"
endif

# --- Taint tier Increment 6: dotnet method-range auto-registration ----------
# DOTNET_PerfMapEnabled=1 drrun -c <taint client>.so methodscan=Hot -- dotnet taint_methods.dll
# The .NET runtime streams /tmp/perf-<pid>.map as it JITs; the client's perfmap poller thread
# auto-registers every JIT'd method whose symbol contains "Hot" (HotAlpha + HotBeta) as an
# instrumented range with NO C region marker — so range-count > 1 arises purely from .NET
# method-load, and the client instruments REAL JIT'd managed code (the Increment-6 dotnet exit
# criterion; unblocks Increment 5's managed seed->sink). Asserts: the workload completes
# (HELLO_TAINT_METHODS, no crash/hang), the client reports regions >= 2, and it instrumented a
# non-zero instruction count. Self-skips without DynamoRIO OR without the .NET SDK. The client
# is UNCHANGED from the native launch (single build; the poller is armed by the client option).
.PHONY: dr-taint-methods-test
dr-taint-methods-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-methods-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { \
	  echo "== dr-taint-methods-test =="; echo "# SKIP: dotnet SDK not found"; \
	  echo "1..0 # skipped"; exit 0; }
	@$(MAKE) drtrace-client
	@echo "== dr-taint-methods-test (drrun methodscan=Hot -- dotnet: perfmap method-load auto-registration) =="
	@rm -rf $(BUILD)/taint_methods_out
	@$(DOTNET) build -c Release examples/taint_methods/taint_methods.csproj \
	    -o $(BUILD)/taint_methods_out >$(BUILD)/taint_methods_build.log 2>&1 \
	  || { echo "# dotnet build failed:"; tail -20 $(BUILD)/taint_methods_build.log; exit 1; }
	@out=$$(DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 \
	          $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) methodscan=Hot -- \
	          $(DOTNET) $(abspath $(BUILD)/taint_methods_out/taint_methods.dll) 2>&1); \
	  rc=$$?; printf '%s\n' "$$out"; \
	  reg=$$(printf '%s' "$$out" | grep -oE 'regions=[0-9]+' | grep -oE '[0-9]+' | sort -rn | head -1); \
	  ins=$$(printf '%s' "$$out" | grep -oE 'inscount=[0-9]+' | grep -oE '[0-9]+' | sort -rn | head -1); \
	  [ -z "$$reg" ] && reg=0; [ -z "$$ins" ] && ins=0; \
	  if [ $$rc -eq 0 ] && printf '%s' "$$out" | grep -q "HELLO_TAINT_METHODS"; then \
	    echo "ok 1 - managed workload ran to completion under drrun + taint client (no crash/hang)"; \
	  else echo "not ok 1 - dotnet workload crashed/hung or output missing (rc=$$rc)"; fi; \
	  if [ "$$reg" -ge 2 ]; then \
	    echo "ok 2 - auto-registered range-count > 1 from .NET method-load (regions=$$reg)"; \
	  else echo "not ok 2 - expected regions >= 2 auto-registered from perfmap (got $$reg)"; fi; \
	  if [ "$$ins" -gt 0 ]; then \
	    echo "ok 3 - client instrumented real JIT'd managed code (inscount=$$ins)"; \
	  else echo "not ok 3 - expected inscount > 0 over the registered method ranges (got $$ins)"; fi; \
	  echo "1..3"; \
	  [ $$rc -eq 0 ] && printf '%s' "$$out" | grep -q "HELLO_TAINT_METHODS" && [ "$$reg" -ge 2 ] && [ "$$ins" -gt 0 ]
endif

# --- Taint tier Increment 5 (exit crit 3): MANAGED seed->sink over JIT'd .NET code ---
# The last Increment-5 exit criterion — a taint seed on a buffer flows through REAL JIT'd
# managed code to a branch-condition sink, REPORTED OUT OF PROCESS over shm — composing
# Increment 6's method-range auto-registration with Increment 5's shm channel + validator.
# A native shim (libtaint_managed_shim.so, P/Invoked by the managed workload) exports the
# seed/sink marker symbols the client resolves by PC, maps the shm channel, and holds a
# NATIVE seed buffer (a stable address — painting a GC-movable managed object is Increment 7).
# The client's methodscan=Hot poller auto-registers the JIT'd HotSeedSink; its seeded
# load->cmp->branch trips the branch-condition sink; a SEPARATE taint_managed_validator drains
# the hit. Seeded run must report a tainted branch hit (kind=1); the unseeded negative control
# must report ZERO. Self-skips without DynamoRIO OR without the .NET SDK.
$(BUILD)/libtaint_managed_shim.so: examples/taint_managed_shim.c include/asmtest_taint.h \
                          include/asmtest_taint_shm.h src/dataflow_dr.h \
                          $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc -shared -fPIC \
	      examples/taint_managed_shim.c -lrt -o $@

$(BUILD)/taint_managed_validator: examples/taint_managed_validator.c include/asmtest_taint.h \
                          include/asmtest_taint_shm.h src/dataflow_dr.h \
                          $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_TAINT -Isrc examples/taint_managed_validator.c -lrt -o $@

MANAGED_SHM ?= /asmtest_taint_managed_ci
.PHONY: dr-taint-managed-test
dr-taint-managed-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-managed-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { \
	  echo "== dr-taint-managed-test =="; echo "# SKIP: dotnet SDK not found"; \
	  echo "1..0 # skipped"; exit 0; }
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/libtaint_managed_shim.so $(BUILD)/taint_managed_validator
	@echo "== dr-taint-managed-test (drrun methodscan=Hot -- dotnet: managed seed->sink over JIT'd code) =="
	@rm -rf $(BUILD)/taint_managed_out
	@$(DOTNET) build -c Release examples/taint_managed/taint_managed.csproj \
	    -o $(BUILD)/taint_managed_out >$(BUILD)/taint_managed_build.log 2>&1 \
	  || { echo "# dotnet build failed:"; tail -20 $(BUILD)/taint_managed_build.log; exit 1; }
	@rm -f /dev/shm$(MANAGED_SHM) 2>/dev/null || true
	@echo "-- seeded run (expect a tainted branch-condition sink hit) --"
	@LD_LIBRARY_PATH=$(abspath $(BUILD)) DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 \
	    $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) methodscan=Hot -- \
	    $(DOTNET) $(abspath $(BUILD)/taint_managed_out/taint_managed.dll) seed $(MANAGED_SHM) 2>&1 | \
	  grep -E 'HELLO_TAINT_MANAGED|taint_managed_shim:|ASMTEST_TAINT_INSCOUNT' || true
	@$(BUILD)/taint_managed_validator seed $(MANAGED_SHM)
	@rm -f /dev/shm$(MANAGED_SHM) 2>/dev/null || true
	@echo "-- negative control (unseeded => zero hits) --"
	@LD_LIBRARY_PATH=$(abspath $(BUILD)) DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 \
	    $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) methodscan=Hot -- \
	    $(DOTNET) $(abspath $(BUILD)/taint_managed_out/taint_managed.dll) noseed $(MANAGED_SHM) 2>&1 | \
	  grep -E 'HELLO_TAINT_MANAGED|taint_managed_shim:' || true
	@$(BUILD)/taint_managed_validator noseed $(MANAGED_SHM)
endif

# --- Taint tier Increment 9: overhead measurement + HARD BAND GATE ----------
# The in-repo overhead NUMBER for the taint tier — every figure in the plan is external literature.
# Times a hot loop (seeded load + arithmetic + data-dependent branch — the ops the client
# instruments) and DECOMPOSES the cost: bare native, DR code-cache baseline (regions=0), the value
# client (value capture), the full-taint client (value + taint), and the record-free PRODUCTION
# client (`prod`). Also a separate AVX2/YMM SIMD variant (Increment 8). Reports the ratios against
# the plan's ~10-50x band, and — the Increment-9 exit criterion — HARD-GATES on it: ok 3 build-FAILS
# if the record-free production overhead regresses past the band (prod-taint > BAND_MAX x bare). The
# monotonic wall-clock facts (ok 1) stay noise-tolerant structural checks (as the Increment-3
# microbench notes), but the band gate has ~4.5x headroom over the measured ~11x, so it flags a real
# regression (e.g. re-adding the drx_buf record -> ~187x) without flaking. Self-skips without DynamoRIO.
$(BUILD)/taint_overhead: examples/taint_overhead.c include/asmtest_taint.h \
                         src/dataflow_dr.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -O2 -DASMTEST_TAINT -Isrc -rdynamic examples/taint_overhead.c -o $@

OVERHEAD_ITERS ?= 5000000
# Increment-9 HARD BAND GATE: production taint overhead must stay within the plan's ~10-50x band
# or the build fails. Set at the band's upper bound (50x bare); the record-free `prod` path measures
# ~11x, so this has ~4.5x headroom for CI noise while still catching a real regression (e.g. someone
# re-introducing the drx_buf record round-trip, which put prod at ~187x). The ratio prod/bare is
# runner-speed-independent (bare and prod scale together), so an absolute x-bare threshold is stable.
BAND_MAX ?= 50
.PHONY: dr-taint-overhead-test
dr-taint-overhead-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-overhead-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/taint_overhead
	@echo "== dr-taint-overhead-test (hot loop, 4-way decomposition; Increment 9 overhead number) =="
	@bare=$$($(BUILD)/taint_overhead nomark $(OVERHEAD_ITERS) | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 dr=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	        $(abspath $(BUILD)/taint_overhead) nomark $(OVERHEAD_ITERS) 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 val=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drval_client_inlined.so) -- \
	        $(abspath $(BUILD)/taint_overhead) mark $(OVERHEAD_ITERS) 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 taint=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	        $(abspath $(BUILD)/taint_overhead) mark $(OVERHEAD_ITERS) 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 prod=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) prod -- \
	        $(abspath $(BUILD)/taint_overhead) mark $(OVERHEAD_ITERS) 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 [ -z "$$bare" ] && bare=0; [ -z "$$dr" ] && dr=0; [ -z "$$val" ] && val=0; [ -z "$$taint" ] && taint=0; [ -z "$$prod" ] && prod=0; \
	 echo "# hotns: bare=$$bare  DR-baseline=$$dr  value-capture=$$val  full-taint=$$taint  PROD-taint=$$prod  (iters=$(OVERHEAD_ITERS))"; \
	 awk -v b=$$bare -v d=$$dr -v v=$$val -v t=$$taint -v p=$$prod 'BEGIN{ if (b>0) printf \
	   "# DR-baseline = %.1fx bare;  value-capture = %.1fx bare;  full-taint (value+taint) = %.1fx bare;  PROD-taint (record-free) = %.1fx bare\n", \
	   d/b, v/b, t/b, p/b }'; \
	 echo "# FINDING: DR steady-state on a hot loop is ~1x (near-native code cache). The FULL-tier cost is"; \
	 echo "#   DOMINATED by the L0 VALUE-capture recording (a full register-file snapshot per insn — an"; \
	 echo "#   oracle-validation feature), NOT by taint. The PRODUCTION build (client option 'prod', Increment"; \
	 echo "#   9 lever 1) takes a RECORD-FREE path: no drx_buf record, no GP snapshot, no value stores, no"; \
	 echo "#   step_taint witness — only the tag shadow + the guarded sink, each mem-source EA computed inline"; \
	 echo "#   via lea. Taint semantics are identical (a seed still reaches a sink — dr-taint-prod-test 6/6"; \
	 echo "#   seeded + 3/3 negative). RESULT: PROD is now ~11x bare — IN the plan's ~10-50x band. Removing the"; \
	 echo "#   drx_buf record ALONE reached the band (it was the bulk of production cost, more than the earlier"; \
	 echo "#   ~60% estimate); a direct-mapped shadow (lever 2) is a further optimization, not needed to enter"; \
	 echo "#   the band. The band is now ENFORCED (ok 3 below: prod-taint <= $(BAND_MAX)x bare, a HARD gate)."; \
	 if [ "$$taint" -ge "$$prod" ] && [ "$$prod" -gt "$$dr" ] && [ "$$dr" -ge "$$bare" ] && [ "$$bare" -gt 0 ]; then \
	   echo "ok 1 - overhead measured + monotonic (full-taint >= prod-taint > DR-baseline >= bare > 0)"; \
	 else echo "not ok 1 - expected full-taint >= prod-taint > DR-baseline >= bare > 0 (bare=$$bare dr=$$dr prod=$$prod taint=$$taint)"; fi; \
	 sbare=$$($(abspath $(BUILD)/taint_overhead) nomark $(OVERHEAD_ITERS) simd 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 sdr=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	        $(abspath $(BUILD)/taint_overhead) nomark $(OVERHEAD_ITERS) simd 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 staint=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	        $(abspath $(BUILD)/taint_overhead) mark $(OVERHEAD_ITERS) simd 2>/dev/null | grep -oE 'hotns=[0-9]+' | grep -oE '[0-9]+'); \
	 [ -z "$$sbare" ] && sbare=0; [ -z "$$sdr" ] && sdr=0; [ -z "$$staint" ] && staint=0; \
	 echo "# SIMD (AVX2/YMM) hotns: bare=$$sbare  DR-baseline=$$sdr  full-taint=$$staint  (iters=$(OVERHEAD_ITERS))"; \
	 awk -v b=$$sbare -v d=$$sdr -v t=$$staint 'BEGIN{ if (b>0) printf \
	   "# SIMD-on vs SIMD-off: DR-baseline = %.1fx bare;  SIMD full-taint = %.1fx bare (reported SEPARATELY from the scalar band)\n", d/b, t/b }'; \
	 echo "# SIMD TRADEOFF (Increment 8): whole-register per-byte lane taint MULTIPLIES the vector reg-tag traffic — a 32-byte YMM"; \
	 echo "#   source/dest is 32 per-byte union/broadcast ops + a 32-byte shadow access vs 1 for a GP reg, so SIMD taint runs"; \
	 echo "#   COSTLIER per instrumented vector op than the scalar band (still on the inline no-clean-call path). Reported here so"; \
	 echo "#   the cost is EXPLICIT rather than silently blowing the scalar band; lane-precise (sub-register) SIMD taint is deferred."; \
	 if [ "$$sbare" -eq 0 ]; then \
	   echo "ok 2 - SIMD overhead SKIPPED (no AVX2 on this CPU)"; sok=1; \
	 elif [ "$$staint" -gt "$$sbare" ]; then \
	   echo "ok 2 - SIMD taint overhead measured + costlier than bare (SIMD full-taint > bare > 0)"; sok=1; \
	 else echo "not ok 2 - expected SIMD full-taint > bare > 0 (sbare=$$sbare staint=$$staint)"; sok=0; fi; \
	 bmax=$(BAND_MAX); \
	 if [ "$$bare" -gt 0 ] && [ "$$prod" -gt 0 ] && [ "$$prod" -le $$(( bmax * bare )) ]; then \
	   echo "ok 3 - PRODUCTION taint overhead within the ~10-50x band (prod-taint <= $${bmax}x bare) [HARD BAND GATE]"; bandok=1; \
	 else echo "not ok 3 - PRODUCTION taint overhead REGRESSED PAST the band (limit $${bmax}x bare; bare=$$bare prod=$$prod) [HARD BAND GATE]"; bandok=0; fi; \
	 echo "1..3"; \
	 [ "$$taint" -ge "$$prod" ] && [ "$$prod" -gt "$$dr" ] && [ "$$dr" -ge "$$bare" ] && [ "$$bare" -gt 0 ] && [ "$$sok" -eq 1 ] && [ "$$bandok" -eq 1 ]
endif

# --- GC-move-range extraction: DR + ICorProfiler coexistence PROBE (go/no-go) -----
# The go/no-go for the recommended Phase-4/Increment-7 mechanism: extract .NET compacting-GC
# object-move {old,new,len} ranges via an in-process ICorProfilerCallback4::MovedReferences2
# profiler and feed them to the DR taint client's shadow remap (at_gc_remap). The one untested
# risk (docs/internal/analysis/gc-move-range-extraction-findings.md) is whether a CLR profiler
# .so coexists with a process running under DynamoRIO on Linux. This lane builds a MINIMAL
# profiler (examples/gcprofiler_probe/gcprofiler.cpp), a workload that forces compacting GCs
# (gcmover), and runs it BOTH natively and under `drrun -c <taint client>`, asserting the
# profiler loads + Initializes and MovedReferences2 delivers ranges UNDER DR with no crash/hang.
# The CoreCLR profiler headers (corprof.h/cor.h + PAL) are fetched once from dotnet/runtime
# (pinned tag) — self-skips without DynamoRIO OR the .NET SDK OR git.
GCPROBE_CLSID  ?= {A4B2C1D0-1111-2222-3333-444455556666}
GCPROBE_RT_TAG ?= v8.0.8
GCPROBE_RT     := $(BUILD)/coreclr-headers
.PHONY: dr-gcprofiler-probe
dr-gcprofiler-probe:
ifndef DR_AVAILABLE
	@echo "== dr-gcprofiler-probe =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { echo "== dr-gcprofiler-probe =="; echo "# SKIP: dotnet SDK not found"; echo "1..0 # skipped"; exit 0; }
	@command -v git >/dev/null 2>&1 || { echo "== dr-gcprofiler-probe =="; echo "# SKIP: git not found (needed to fetch CoreCLR profiler headers)"; echo "1..0 # skipped"; exit 0; }
	@if [ ! -f $(GCPROBE_RT)/src/coreclr/pal/prebuilt/inc/corprof.h ]; then \
	   echo "# fetching CoreCLR profiler headers (dotnet/runtime $(GCPROBE_RT_TAG))..."; \
	   rm -rf $(GCPROBE_RT); \
	   git clone --depth 1 --filter=blob:none --sparse -b $(GCPROBE_RT_TAG) https://github.com/dotnet/runtime $(GCPROBE_RT) >/dev/null 2>&1 \
	     && git -C $(GCPROBE_RT) sparse-checkout set src/coreclr/pal/inc src/coreclr/pal/prebuilt/inc src/coreclr/inc >/dev/null 2>&1 \
	     || { echo "== dr-gcprofiler-probe =="; echo "# SKIP: could not fetch CoreCLR headers (no network?)"; echo "1..0 # skipped"; exit 0; }; \
	 fi
	@R=$(abspath $(GCPROBE_RT))/src/coreclr; \
	 $(CXX) -std=c++17 -shared -fPIC -o $(BUILD)/libgcprobe.so examples/gcprofiler_probe/gcprofiler.cpp \
	   -DPAL_STDCPP_COMPAT -DHOST_UNIX -DHOST_64BIT -DHOST_AMD64 -DTARGET_UNIX -DTARGET_64BIT -DTARGET_AMD64 \
	   -DBIT64 -DUNIX -DPLATFORM_UNIX -DFEATURE_PAL \
	   -I$$R/pal/inc/rt -I$$R/pal/inc -I$$R/pal/prebuilt/inc -I$$R/inc -Iinclude \
	   || { echo "# profiler build failed"; exit 1; }
	@$(MAKE) drtrace-client
	@rm -rf $(BUILD)/gcmover_out
	@$(DOTNET) build -c Release examples/gcprofiler_probe/gcmover/gcmover.csproj -o $(BUILD)/gcmover_out \
	    >$(BUILD)/gcmover_build.log 2>&1 || { echo "# dotnet build failed:"; tail -15 $(BUILD)/gcmover_build.log; exit 1; }
	@echo "== dr-gcprofiler-probe (ICorProfilerCallback4::MovedReferences2 under DynamoRIO) =="
	@P="CORECLR_ENABLE_PROFILING=1 CORECLR_PROFILER=$(GCPROBE_CLSID) CORECLR_PROFILER_PATH=$(abspath $(BUILD)/libgcprobe.so)"; \
	 base=$$(env $$P $(DOTNET) $(abspath $(BUILD)/gcmover_out/gcmover.dll) 2>&1 | grep -c "MovedReferences2 ranges"); \
	 out=$$(env $$P $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) -- \
	         $(DOTNET) $(abspath $(BUILD)/gcmover_out/gcmover.dll) 2>&1); rc=$$?; \
	 dr=$$(printf '%s\n' "$$out" | grep -c "MovedReferences2 ranges"); \
	 init=$$(printf '%s\n' "$$out" | grep -c "Initialize OK"); \
	 hello=$$(printf '%s' "$$out" | grep -c "HELLO_GC_MOVER"); \
	 crash=$$(printf '%s\n' "$$out" | grep -icE "SIGSEGV|Segmentation|SIGTRAP|fatal error"); \
	 echo "# baseline(no-DR) MovedReferences2=$$base ;  under-DR MovedReferences2=$$dr ;  init=$$init hello=$$hello crash=$$crash rc=$$rc"; \
	 printf '%s\n' "$$out" | grep -m1 "MovedReferences2 ranges" | sed 's/^GCPROBE: /# sample: /'; \
	 if [ "$$init" -ge 1 ] && [ "$$rc" -eq 0 ] && [ "$$hello" -ge 1 ] && [ "$$crash" -eq 0 ]; then \
	   echo "ok 1 - profiler loaded + Initialize + workload completed UNDER DR (no crash/hang)"; \
	 else echo "not ok 1 - DR/profiler coexistence failed (init=$$init rc=$$rc hello=$$hello crash=$$crash)"; fi; \
	 if [ "$$dr" -ge 1 ]; then \
	   echo "ok 2 - MovedReferences2 delivered {old,new,len} ranges UNDER DR ($$dr calls)"; \
	 else echo "not ok 2 - no MovedReferences2 under DR (got $$dr)"; fi; \
	 echo "1..2"; \
	 [ "$$init" -ge 1 ] && [ "$$rc" -eq 0 ] && [ "$$hello" -ge 1 ] && [ "$$crash" -eq 0 ] && [ "$$dr" -ge 1 ]
endif

# --- Taint tier Increment 7 (Slice 1): LIVE GC-move remap driven by the profiler ---
# Composes the go/no-go probe's proven pieces into the actual Increment-7 capability wiring: the
# ICorProfilerCallback4 profiler (built with the gc_move handshake), the compacting-GC workload,
# and the DR taint client's in-tree at_gc_remap_live. Under `drrun -c <taint client> gcmove --
# dotnet gcmover` + the profiler, the client publishes gc_move's address (POSIX-shm handshake)
# and the profiler feeds every MovedReferences2 {old,new,len} range to at_gc_remap_live AT THE
# GC FENCE. Load-bearing finding baked into at_gc_remap_live: DR heap/lock APIs (dr_mutex,
# dr_global_alloc, dr_raw_mem_alloc) CRASH from the profiler's app-code thread, so the live remap
# is DR-API-FREE (plain spinlock + static snapshot + tag_ptr_lookup, no leaf create). Asserts the
# remap ran on real moves (remapped_ranges > 0), the workload completed, no crash/hang. The full
# seed->move->sink SURVIVAL is Slice 2 (needs a raw-syscall leaf allocator so a move into an
# untouched destination leaf carries the tag). Self-skips without DynamoRIO/dotnet/git.
.PHONY: dr-gcmove-live-test
dr-gcmove-live-test:
ifndef DR_AVAILABLE
	@echo "== dr-gcmove-live-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { echo "== dr-gcmove-live-test =="; echo "# SKIP: dotnet SDK not found"; echo "1..0 # skipped"; exit 0; }
	@command -v git >/dev/null 2>&1 || { echo "== dr-gcmove-live-test =="; echo "# SKIP: git not found (CoreCLR profiler headers)"; echo "1..0 # skipped"; exit 0; }
	@if [ ! -f $(GCPROBE_RT)/src/coreclr/pal/prebuilt/inc/corprof.h ]; then \
	   rm -rf $(GCPROBE_RT); \
	   git clone --depth 1 --filter=blob:none --sparse -b $(GCPROBE_RT_TAG) https://github.com/dotnet/runtime $(GCPROBE_RT) >/dev/null 2>&1 \
	     && git -C $(GCPROBE_RT) sparse-checkout set src/coreclr/pal/inc src/coreclr/pal/prebuilt/inc src/coreclr/inc >/dev/null 2>&1 \
	     || { echo "== dr-gcmove-live-test =="; echo "# SKIP: could not fetch CoreCLR headers"; echo "1..0 # skipped"; exit 0; }; \
	 fi
	@R=$(abspath $(GCPROBE_RT))/src/coreclr; \
	 $(CXX) -std=c++17 -shared -fPIC -o $(BUILD)/libgcprobe.so examples/gcprofiler_probe/gcprofiler.cpp \
	   -DPAL_STDCPP_COMPAT -DHOST_UNIX -DHOST_64BIT -DHOST_AMD64 -DTARGET_UNIX -DTARGET_64BIT -DTARGET_AMD64 \
	   -DBIT64 -DUNIX -DPLATFORM_UNIX -DFEATURE_PAL \
	   -I$$R/pal/inc/rt -I$$R/pal/inc -I$$R/pal/prebuilt/inc -I$$R/inc -Iinclude \
	   || { echo "# profiler build failed"; exit 1; }
	@$(MAKE) drtrace-client
	@rm -rf $(BUILD)/gcmover_out
	@$(DOTNET) build -c Release examples/gcprofiler_probe/gcmover/gcmover.csproj -o $(BUILD)/gcmover_out \
	    >$(BUILD)/gcmover_build.log 2>&1 || { echo "# dotnet build failed:"; tail -15 $(BUILD)/gcmover_build.log; exit 1; }
	@echo "== dr-gcmove-live-test (profiler drives at_gc_remap_live on real GC moves under DR) =="
	@rm -f /dev/shm/asmtest_taint_gcmove; \
	 P="CORECLR_ENABLE_PROFILING=1 CORECLR_PROFILER=$(GCPROBE_CLSID) CORECLR_PROFILER_PATH=$(abspath $(BUILD)/libgcprobe.so)"; \
	 env $$P $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) gcmove -- \
	   $(DOTNET) $(abspath $(BUILD)/gcmover_out/gcmover.dll) >$(BUILD)/gcmove_live.out 2>&1; rc=$$?; \
	 hs=$$(grep -c "handshake found" $(BUILD)/gcmove_live.out); \
	 rr=$$(grep -oE 'remapped_ranges=[0-9]+' $(BUILD)/gcmove_live.out | grep -oE '[0-9]+' | tail -1); [ -z "$$rr" ] && rr=0; \
	 hello=$$(grep -c HELLO_GC_MOVER $(BUILD)/gcmove_live.out); \
	 crash=$$(grep -icE "SIGSEGV|Segmentation|SIGTRAP|fatal error" $(BUILD)/gcmove_live.out); \
	 echo "# handshake=$$hs  remapped_ranges=$$rr  hello=$$hello  crash=$$crash  rc=$$rc"; \
	 if [ "$$hs" -ge 1 ] && [ "$$rc" -eq 0 ] && [ "$$hello" -ge 1 ] && [ "$$crash" -eq 0 ]; then \
	   echo "ok 1 - gc_move handshake + workload completed UNDER DR (no crash/hang)"; \
	 else echo "not ok 1 - live gc_move path failed (hs=$$hs rc=$$rc hello=$$hello crash=$$crash)"; fi; \
	 if [ "$$rr" -ge 1 ]; then \
	   echo "ok 2 - at_gc_remap_live drove real compacting-GC move ranges under DR ($$rr ranges)"; \
	 else echo "not ok 2 - no live remaps (remapped_ranges=$$rr)"; fi; \
	 echo "1..2"; \
	 [ "$$hs" -ge 1 ] && [ "$$rc" -eq 0 ] && [ "$$hello" -ge 1 ] && [ "$$crash" -eq 0 ] && [ "$$rr" -ge 1 ]
endif

# --- Taint tier Increment 7 (Slice 2): MANAGED GC-move SURVIVAL (seed->move->sink) ---------
# The end-to-end proof that a taint seed on a GC-MOVABLE managed object SURVIVES a compacting GC
# that relocates it — closing what the Increment-5 managed lane deferred (it seeds a stable NATIVE
# buffer). Composes: the in-process MovedReferences2 profiler (libgcprobe.so) feeding real moved
# ranges to the client's DR-API-FREE live remap (at_gc_remap_live) at the GC fence; Slice 2's
# raw-syscall (bare mmap) leaf allocator (tag_ptr_create_raw) carrying the tag into a never-touched
# destination leaf; the shim's shim_seed_at painting the object at its briefly-pinned current
# address; and methodscan=MoveSink auto-registering the JIT'd GcMoveSink whose tainted
# load->cmp->branch trips the sink at the object's NEW address. This is the FIRST run that
# exercises tag_ptr_create_raw from the profiler's dcontext-less app-code thread on a REAL fence —
# the load-bearing de-risk of the bare-mmap claim: a crash here would mean raw mmap is not safe
# there. Seeded run: assert the object actually moved (moved=1) AND a tainted branch hit (kind=1)
# crossed the shm channel (seed survived to the new address); negative control (noseed): ZERO hits
# (no phantom taint conjured at the freshly-mmap'd destination leaf). The shadow-level both-sides
# coherence (present-at-new + absent-at-old) is proven deterministically by the synthetic T5-T7
# in dr-taint-gcremap-test; this lane proves the live end-to-end survival. Self-skips without
# DynamoRIO / dotnet / git (CoreCLR profiler headers).
GCMOVE_SURV_SHM ?= /asmtest_taint_gcmove_surv_ci
.PHONY: dr-taint-gcmove-survival-test
dr-taint-gcmove-survival-test:
ifndef DR_AVAILABLE
	@echo "== dr-taint-gcmove-survival-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@command -v $(DOTNET) >/dev/null 2>&1 || { echo "== dr-taint-gcmove-survival-test =="; echo "# SKIP: dotnet SDK not found"; echo "1..0 # skipped"; exit 0; }
	@command -v git >/dev/null 2>&1 || { echo "== dr-taint-gcmove-survival-test =="; echo "# SKIP: git not found (CoreCLR profiler headers)"; echo "1..0 # skipped"; exit 0; }
	@if [ ! -f $(GCPROBE_RT)/src/coreclr/pal/prebuilt/inc/corprof.h ]; then \
	   rm -rf $(GCPROBE_RT); \
	   git clone --depth 1 --filter=blob:none --sparse -b $(GCPROBE_RT_TAG) https://github.com/dotnet/runtime $(GCPROBE_RT) >/dev/null 2>&1 \
	     && git -C $(GCPROBE_RT) sparse-checkout set src/coreclr/pal/inc src/coreclr/pal/prebuilt/inc src/coreclr/inc >/dev/null 2>&1 \
	     || { echo "== dr-taint-gcmove-survival-test =="; echo "# SKIP: could not fetch CoreCLR headers"; echo "1..0 # skipped"; exit 0; }; \
	 fi
	@R=$(abspath $(GCPROBE_RT))/src/coreclr; \
	 $(CXX) -std=c++17 -shared -fPIC -o $(BUILD)/libgcprobe.so examples/gcprofiler_probe/gcprofiler.cpp \
	   -DPAL_STDCPP_COMPAT -DHOST_UNIX -DHOST_64BIT -DHOST_AMD64 -DTARGET_UNIX -DTARGET_64BIT -DTARGET_AMD64 \
	   -DBIT64 -DUNIX -DPLATFORM_UNIX -DFEATURE_PAL \
	   -I$$R/pal/inc/rt -I$$R/pal/inc -I$$R/pal/prebuilt/inc -I$$R/inc -Iinclude \
	   || { echo "# profiler build failed"; exit 1; }
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/libtaint_managed_shim.so $(BUILD)/taint_managed_validator
	@rm -rf $(BUILD)/taint_gcmove_managed_out
	@$(DOTNET) build -c Release examples/taint_gcmove_managed/taint_gcmove_managed.csproj \
	    -o $(BUILD)/taint_gcmove_managed_out >$(BUILD)/taint_gcmove_managed_build.log 2>&1 \
	  || { echo "# dotnet build failed:"; tail -20 $(BUILD)/taint_gcmove_managed_build.log; exit 1; }
	@echo "== dr-taint-gcmove-survival-test (seed on a GC-movable object survives a compacting GC; sink fires at the NEW address) =="
	@rm -f /dev/shm$(GCMOVE_SURV_SHM) /dev/shm/asmtest_taint_gcmove 2>/dev/null || true
	@echo "-- seeded run (expect moved=1 + a tainted branch-condition sink hit at the moved address) --"
	@P="CORECLR_ENABLE_PROFILING=1 CORECLR_PROFILER=$(GCPROBE_CLSID) CORECLR_PROFILER_PATH=$(abspath $(BUILD)/libgcprobe.so)"; \
	 out=$$(env $$P LD_LIBRARY_PATH=$(abspath $(BUILD)) DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 \
	    $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) gcmove methodscan=MoveSink -- \
	    $(DOTNET) $(abspath $(BUILD)/taint_gcmove_managed_out/taint_gcmove_managed.dll) seed $(GCMOVE_SURV_SHM) 2>&1); \
	 printf '%s\n' "$$out" | grep -E 'GCMOVE_MANAGED|HELLO_GCMOVE_MANAGED|taint_managed_shim:|remapped_ranges' | head -8; \
	 printf '%s\n' "$$out" | grep -q 'moved=1' \
	   && echo "# object relocated by a compacting GC (moved=1) — the sink at the new address is a real cross-move test" \
	   || { echo "# FAIL: object did not move (moved=0); GC-move survival is inconclusive"; exit 1; }
	@$(BUILD)/taint_managed_validator seed $(GCMOVE_SURV_SHM)
	@rm -f /dev/shm$(GCMOVE_SURV_SHM) /dev/shm/asmtest_taint_gcmove 2>/dev/null || true
	@echo "-- negative control (unseeded => zero hits; no phantom taint at the moved-into leaf) --"
	@P="CORECLR_ENABLE_PROFILING=1 CORECLR_PROFILER=$(GCPROBE_CLSID) CORECLR_PROFILER_PATH=$(abspath $(BUILD)/libgcprobe.so)"; \
	 env $$P LD_LIBRARY_PATH=$(abspath $(BUILD)) DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 \
	    $(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drtaint_client.so) gcmove methodscan=MoveSink -- \
	    $(DOTNET) $(abspath $(BUILD)/taint_gcmove_managed_out/taint_gcmove_managed.dll) noseed $(GCMOVE_SURV_SHM) 2>&1 | \
	  grep -E 'GCMOVE_MANAGED|HELLO_GCMOVE_MANAGED|taint_managed_shim:' | head -4 || true
	@$(BUILD)/taint_managed_validator noseed $(GCMOVE_SURV_SHM)
endif

# --- Inlined-vs-clean-call microbenchmark (taint-tier Increment 3) ----------
# Times one whole asmtest_dataflow_dr_run over a LOOPING fixture under each value
# client across fresh processes and reports the per-instruction capture-cost
# delta (DR init + app-side replay are identical, so the delta is the asymmetric
# capture cost). Also a correctness stress the tiny oracle fixture skips (a
# back-edge, a flag-dependent branch, many drx_buf flushes). No Unicorn needed.
$(BUILD)/dr_valtrace_bench.o: examples/dr_valtrace_bench.c \
                             include/asmtest_valtrace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/dr_valtrace_bench: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                           $(BUILD)/dataflow_dr.o $(BUILD)/drtrace_app.o \
                           $(BUILD)/trace.o $(DRAPP_KS_OBJ) \
                           $(BUILD)/dr_valtrace_bench.o
	$(CC) $(CFLAGS) -rdynamic $^ $(CAPSTONE_LIBS) $(DRAPP_KS_LIBS) \
	      -ldl -lpthread -o $@

BENCH_ITERS   ?= 40000
BENCH_SAMPLES ?= 5
.PHONY: dr-valtrace-bench
dr-valtrace-bench:
ifndef DR_AVAILABLE
	@echo "== dr-valtrace-bench =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@$(MAKE) drtrace-client
	@$(MAKE) $(BUILD)/dr_valtrace_bench
	@echo "== dr-valtrace-bench (per-instruction capture cost: clean-call vs inlined) =="
	@ITERS=$(BENCH_ITERS) SAMPLES=$(BENCH_SAMPLES) \
	  CLEAN=$(abspath $(BUILD)/libasmtest_drval_client.so) \
	  INLINED=$(abspath $(BUILD)/libasmtest_drval_client_inlined.so) \
	  DRLIB=$(abspath $(DR_DLLIB)) BIN=$(abspath $(BUILD)/dr_valtrace_bench) \
	  bash scripts/dr_valtrace_bench.sh
endif

# --- DR extension-load probe (taint tier, Increment 2) ---------------------
# THROWAWAY diagnostic, not a product artifact. Builds the opt-in probe client
# (drclient/probe_extensions.c — the sole client that use_DynamoRIO_extension()s)
# and runs it under `drrun` over /bin/true, asserting each of the BSD-clean stack
# drmgr/drreg/drx loads under DR's private loader (the never-tested blocker gating
# the whole Phase-5 taint re-platform) and that a non-zero instruction count was
# instrumented. Self-skips without DynamoRIO. Body of the `make docker-drext-probe`
# lane. See docs/internal/analysis/dr-extension-load-probe-findings.md.
#
# Set PROBE_DRWRAP=1 and/or PROBE_UMBRA=1 to ALSO load-check the LGPL-2.1 extensions
# (drwrap is a DR core ext/ extension; umbra ships in the Dr. Memory Framework). Both
# paths are informational only and deliberately NOT the committed CI gate; the
# packaging conveys either compliantly (scripts/collect-licenses.sh + licenses/LGPL-2.1.txt).
DR_DRRUN := $(DYNAMORIO_HOME)/bin64/drrun
PROBE_UMBRA ?=
PROBE_DRWRAP ?=
DREXT_PROBE_CMAKE_ARGS :=
ifeq ($(PROBE_DRWRAP),1)
DREXT_PROBE_CMAKE_ARGS += -DPROBE_DRWRAP=ON
endif
ifeq ($(PROBE_UMBRA),1)
DREXT_PROBE_CMAKE_ARGS += -DPROBE_UMBRA=ON \
    -DDrMemoryFramework_DIR=$(abspath $(DYNAMORIO_HOME)/drmemory/drmf)
endif

.PHONY: drext-probe
drext-probe:
ifndef DR_AVAILABLE
	@echo "== drext-probe =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
	@echo "1..0 # skipped"
else
	@mkdir -p $(BUILD)/drext_probe
	cd $(BUILD)/drext_probe && cmake -DDynamoRIO_DIR=$(abspath $(DYNAMORIO_DIR)) \
	    -DASMTEST_BUILD_DIR=$(abspath $(BUILD)) -DASMTEST_BUILD_DREXT_PROBE=ON \
	    $(DREXT_PROBE_CMAKE_ARGS) $(abspath drclient) >/dev/null
	cmake --build $(BUILD)/drext_probe >/dev/null
	@echo "== drext-probe (DR extension private-loader check) =="
	@out=$$($(DR_DRRUN) -c $(abspath $(BUILD)/libasmtest_drext_probe.so) -- /bin/true 2>&1); \
	  rc=$$?; printf '%s\n' "$$out"; \
	  if [ $$rc -ne 0 ]; then echo "drext-probe: FAIL (drrun rc=$$rc)"; exit 1; fi; \
	  printf '%s' "$$out" | grep -q "drext-probe: PROBE OK" \
	    || { echo "drext-probe: FAIL (no PROBE OK line — an extension did not load)"; exit 1; }
	@echo "drext-probe: PASS (drmgr+drreg+drx loaded under the private loader)"
endif

# --- Optional hardware-assisted native-trace tier (Intel PT / CoreSight) ---
# `make hwtrace-test` records native coverage with near-zero capture overhead via
# the CPU's branch-trace unit (Intel PT / ARM CoreSight) + a software decoder
# (libipt / OpenCSD) that replays the registered bytes. It self-skips (the common
# case) unless on a bare-metal Intel-PT host (or CoreSight board) with the PMU
# present and perf_event privilege lowered — never on AMD, VMs, or standard CI.
#
# Decoders auto-detect via pkg-config, mirroring CAPSTONE_*: when found, the
# backend is built -DASMTEST_HAVE_LIBIPT / -DASMTEST_HAVE_OPENCSD and linked; when
# absent the same objects compile decoder-free and asmtest_hwtrace_available()
# reports the decoder missing so the tier self-skips.
# Intel PT decoder (libipt). Unlike the other optional deps it ships NO
# pkg-config file on common distros (Ubuntu's libipt-dev installs intel-pt.h on
# the default include path and libipt.so), so detect by probing the header and
# link with -lipt. Override LIBIPT_CFLAGS / LIBIPT_LIBS for a non-standard
# location, or `make ... LIBIPT_DEF=` to force-disable.
LIBIPT_CFLAGS ?=
ifeq ($(shell $(CC) $(LIBIPT_CFLAGS) -E -include intel-pt.h -xc /dev/null >/dev/null 2>&1 && echo 1),1)
LIBIPT_DEF  ?= -DASMTEST_HAVE_LIBIPT
LIBIPT_LIBS ?= -lipt
else
LIBIPT_LIBS ?=
endif

# ARM CoreSight decoder (OpenCSD). Its pkg-config module is `libopencsd` (NOT
# `opencsd`), and ships -lopencsd.
OPENCSD_CFLAGS ?= $(shell pkg-config --cflags libopencsd 2>/dev/null)
OPENCSD_LIBS   ?= $(shell pkg-config --libs libopencsd 2>/dev/null)
ifeq ($(shell pkg-config --exists libopencsd 2>/dev/null && echo 1),1)
OPENCSD_DEF := -DASMTEST_HAVE_OPENCSD
endif

# LbrExtV2 speculation flags (AMD Phase 4): struct perf_branch_entry gained the
# `spec:2` bitfield (PERF_BR_SPEC_WRONG_PATH) in Linux 6.1. Older
# <linux/perf_event.h> lack it, so probe the member (a `-fsyntax-only` semantic
# check, no link) and only then build amd_backend.c -DASMTEST_HAVE_PERF_BR_SPEC.
# Without the define the wrong-path filter compiles out — a no-op, exactly as on
# Zen 3 BRS / non-Linux where there are no spec bits. Mirrors the LIBIPT_DEF probe.
# The header is injected with -include rather than a literal `#include` line: an
# unescaped `#` inside $(shell ...) is a comment to GNU make < 4.3 (macOS ships
# 3.81), truncating the line, while `\#` breaks make >= 4.3 the other way.
PERF_BR_SPEC_DEF := $(shell printf 'int f(struct perf_branch_entry *e){return (int)e->spec;}\n' | $(CC) -fsyntax-only -include linux/perf_event.h -xc - >/dev/null 2>&1 && echo -DASMTEST_HAVE_PERF_BR_SPEC)

# Optional eBPF JIT code-emission detector (libbpf CO-RE) — the sideband accelerator for
# the code-image recorder (src/codeimage.c): it detects mprotect/mmap PROT_EXEC +
# memfd_create for a target PID and raises events into a bpf_ringbuf the recorder drains.
# The .bpf.o + libbpf skeleton are built ONLY when clang + bpftool + libbpf are ALL present
# (the `make docker-hwtrace-codeimage` lane). On a bare host without them CODEIMAGE_SKEL is
# empty, so codeimage.o has no skeleton prerequisite, clang/bpftool are never invoked, the
# TU compiles WITHOUT -DASMTEST_HAVE_LIBBPF, and asmtest_codeimage_bpf_available() returns 0
# (self-skip) — exactly how an empty LIBIPT_DEF makes pt_backend.o compile decoder-free.
CLANG         ?= clang
BPFTOOL       ?= bpftool
LIBBPF_CFLAGS ?= $(shell pkg-config --cflags libbpf 2>/dev/null)
LIBBPF_LIBS   ?= $(shell pkg-config --libs libbpf 2>/dev/null)
ifeq ($(shell pkg-config --exists libbpf 2>/dev/null && command -v $(CLANG) >/dev/null 2>&1 && command -v $(BPFTOOL) >/dev/null 2>&1 && echo 1),1)
LIBBPF_DEF     := -DASMTEST_HAVE_LIBBPF
CODEIMAGE_SKEL := $(BUILD)/codeimage.skel.h
BRANCHSNAP_SKEL := $(BUILD)/branchsnap.skel.h
CODEIMAGE_INC  := -I$(BUILD)
LINK_LIBBPF    := $(LIBBPF_LIBS)
endif

# CO-RE build chain (only reached when CODEIMAGE_SKEL is non-empty). vmlinux.h comes from
# the running kernel's BTF (the container shares the host kernel at `docker run` time);
# fall back to the checked-in minimal header. clang is BPF's only front end; -g emits the
# BTF CO-RE relocations need, -O2 is required by the verifier.
$(BUILD)/vmlinux.h: | $(BUILD)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@ 2>/dev/null \
	  || cp bpf/vmlinux_min.h $@
$(BUILD)/codeimage.bpf.o: bpf/codeimage.bpf.c bpf/codeimage_event.h $(BUILD)/vmlinux.h | $(BUILD)
	$(CLANG) -target bpf -D__TARGET_ARCH_x86 -g -O2 -Wall -I$(BUILD) -Ibpf -c $< -o $@
$(BUILD)/codeimage.skel.h: $(BUILD)/codeimage.bpf.o | $(BUILD)
	$(BPFTOOL) gen skeleton $< > $@

# AMD-P0 deterministic LBR snapshot: the branchsnap BPF program + its skeleton, built on
# the SAME BPF-toolchain gate as codeimage (BRANCHSNAP_SKEL is set only inside the ifeq
# above, so without the toolchain branchsnap.o has no skeleton prereq and compiles its stub).
$(BUILD)/branchsnap.bpf.o: bpf/branchsnap.bpf.c bpf/branchsnap_event.h $(BUILD)/vmlinux.h | $(BUILD)
	$(CLANG) -target bpf -D__TARGET_ARCH_x86 -g -O2 -Wall -I$(BUILD) -Ibpf -c $< -o $@
$(BUILD)/branchsnap.skel.h: $(BUILD)/branchsnap.bpf.o | $(BUILD)
	$(BPFTOOL) gen skeleton $< > $@


$(BUILD)/hwtrace.o: src/hwtrace.c include/asmtest_hwtrace.h \
                    include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/pt_backend.o: src/pt_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -c $< -o $@
$(BUILD)/cs_backend.o: src/cs_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(OPENCSD_DEF) $(OPENCSD_CFLAGS) -c $< -o $@
# AMD branch-record reconstruction reuses the Capstone layer (disasm.o) via
# asmtest_disas for instruction lengths — no new decoder lib, just Capstone.
# $(PERF_BR_SPEC_DEF) enables the LbrExtV2 wrong-path filter where the header has it.
$(BUILD)/amd_backend.o: src/amd_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(PERF_BR_SPEC_DEF) -c $< -o $@
# Single-step (EFLAGS.TF / SIGTRAP) backend: no external library at all, just the
# same Capstone length-decoder (disasm.o) for block normalization. Runs on ANY
# x86-64 Linux OR macOS host with no PMU/perf/privilege — the universal hardware-tier
# path (both deliver the #DB single-step trap as an in-process SIGTRAP).
$(BUILD)/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# AMD MSR-direct LBR snapshot (src/msr_lbr.c): reads the LbrExtV2 FROM/TO MSRs via
# /dev/cpu/N/msr; no external library, decodes through amd_backend.o's asmtest_amd_decode.
$(BUILD)/msr_lbr.o: src/msr_lbr.c include/asmtest_hwtrace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Env-gated AMD/hwtrace debug logging (src/debug.c, Phase 4): no external library, no
# public header; ASMTEST_HWTRACE_DEBUG / ASMTEST_AMD_DEBUG gate stderr diagnostics.
$(BUILD)/debug.o: src/debug.c src/debug.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Cross-tier orchestrator (asmtest_trace_auto.h): no external library; calls
# asmtest_hwtrace_available() directly and dlopen-probes libasmtest_drapp (-ldl).
$(BUILD)/trace_auto.o: src/trace_auto.c include/asmtest_trace_auto.h \
                       include/asmtest_hwtrace.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Out-of-process ptrace single-step backend (W2): no external library, just the
# same Capstone length-decoder (disasm.o) for block normalization.
$(BUILD)/ptrace_backend.o: src/ptrace_backend.c include/asmtest_ptrace.h \
                          include/asmtest_trace.h include/asmtest_codeimage.h \
                          include/asmtest_descent_internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Call-descent handle (asmtest_descent_t): edges + nested frames read through scalar
# accessors. No external library (plain malloc pools); sibling of src/trace.c.
$(BUILD)/descent.o: src/descent.c include/asmtest_ptrace.h \
                    include/asmtest_descent_internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Time-aware code-image recorder (asmtest_codeimage.h): a userspace soft-dirty timeline
# (the foreign-JIT byte source) + an OPTIONAL eBPF emission detector. The userspace path
# needs no external library; the eBPF half is compiled in only when built
# -DASMTEST_HAVE_LIBBPF (the LIBBPF_* probe below, which the bare host leaves empty so the
# detector self-skips). $(LIBBPF_DEF)/$(LIBBPF_CFLAGS)/$(CODEIMAGE_INC) are empty unless
# that probe fires (see Phase C).
$(BUILD)/codeimage.o: src/codeimage.c include/asmtest_codeimage.h $(CODEIMAGE_SKEL) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) $(CODEIMAGE_INC) -c $< -o $@
# AMD-P0 deterministic LBR snapshot (src/branchsnap.c). Same libbpf gate as codeimage:
# with the toolchain it compiles the real bpf_get_branch_snapshot loader (needs the
# skeleton + -Ibpf for branchsnap_event.h); without it, the self-skipping #else stub.
$(BUILD)/branchsnap.o: src/branchsnap.c include/asmtest_hwtrace.h $(BRANCHSNAP_SKEL) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) -I$(BUILD) -Ibpf -c $< -o $@
# §D3 concealed ptrace-stealth stepper: the shared stepping body + bundled-binary
# discovery (dladdr-sibling, mirroring drtrace_app.c's dr_bundled_lib). Compiled
# into BOTH libasmtest_hwtrace (the in-process forked-child fallback path) and the
# standalone asmtest-stealth-helper binary below (the bundled path).
$(BUILD)/stealth_helper.o: src/stealth_helper.c src/stealth_helper.h \
                           include/asmtest_hwtrace.h include/asmtest_ptrace.h \
                           include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stealth_helper_main.o: src/stealth_helper_main.c src/stealth_helper.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Statistical AMD IBS-Op edge lane (src/ibs_backend.c, asmtest_ibs.h): the one
# hardware branch source on Zen 2, where every branch-STACK path self-skips. No
# external library and no Capstone (Phase 0-1) — raw perf_event_open + a pure decoder
# that is host-independent (unit-tested on every CI host). A SEPARATE statistical
# producer, never a member of the exact insns[]/blocks[] parity cascade.
$(BUILD)/ibs_backend.o: src/ibs_backend.c include/asmtest_ibs.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

HWTRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o $(BUILD)/cs_backend.o \
                $(BUILD)/amd_backend.o $(BUILD)/ss_backend.o \
                $(BUILD)/trace_auto.o $(BUILD)/ptrace_backend.o \
                $(BUILD)/descent.o $(BUILD)/stealth_helper.o \
                $(BUILD)/codeimage.o $(BUILD)/branchsnap.o \
                $(BUILD)/msr_lbr.o $(BUILD)/ibs_backend.o $(BUILD)/debug.o \
                $(BUILD)/disasm.o $(BUILD)/trace.o

# The test builds a synthetic wrong-path perf_branch_entry (test_amd_spec_filter), so
# it needs $(PERF_BR_SPEC_DEF) too — it guards the .spec field the same way amd_backend.c does.
$(BUILD)/test_hwtrace.o: CFLAGS += $(PERF_BR_SPEC_DEF)
$(BUILD)/test_hwtrace: $(HWTRACE_OBJS) $(BUILD)/test_hwtrace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

# AMD deterministic LBR-snapshot test: drives the public asmtest_amd_snapshot_trace()
# (in branchsnap.o, part of HWTRACE_OBJS above). Self-skips (exit 0) without the BPF
# toolchain / privilege / AMD substrate. Defined AFTER HWTRACE_OBJS so the prerequisite
# is non-empty (Make expands prerequisites when the rule is read).
$(BUILD)/test_branchsnap: $(HWTRACE_OBJS) $(BUILD)/test_branchsnap.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@
.PHONY: branchsnap-test
branchsnap-test: $(BUILD)/test_branchsnap
	$(BUILD)/test_branchsnap

# Statistical AMD IBS-Op edge lane. test_ibs's PURE decoder checks (synthetic raw
# records) run and pass on ANY host; its live out-of-band capture self-skips (exit 0)
# off AMD IBS-Op. Links only ibs_backend.o (a self-contained producer, no Capstone /
# PT / OpenCSD decoders) + -lpthread for the test's worker thread. ibs_probe is the
# standalone capability probe. Both are wired into hwtrace-test below.
#
# test_ibs.c / ibs_probe.c include the INTERNAL src/ibs_backend.h — the Phase 7
# IBS-Fetch front-end lane (asmtest_ibs_fetch_*) lives there, kept off the public
# asmtest_ibs.h surface — so they compile with -Isrc. Explicit object rules override
# the generic examples pattern rule (which is -Iinclude only); they depend on the
# internal header + .build-flags so an edit / knob flip forces a rebuild.
$(BUILD)/test_ibs.o: examples/test_ibs.c include/asmtest_ibs.h src/ibs_backend.h \
                     $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@
$(BUILD)/ibs_probe.o: examples/ibs_probe.c include/asmtest_ibs.h src/ibs_backend.h \
                      $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c $< -o $@
$(BUILD)/test_ibs: $(BUILD)/ibs_backend.o $(BUILD)/test_ibs.o
	$(CC) $(CFLAGS) $^ -lpthread -o $@
$(BUILD)/ibs_probe: $(BUILD)/ibs_backend.o $(BUILD)/ibs_probe.o
	$(CC) $(CFLAGS) $^ -o $@
.PHONY: ibs-test
ibs-test: $(BUILD)/test_ibs $(BUILD)/ibs_probe
	@echo "== ibs-test =="
	$(BUILD)/ibs_probe
	$(BUILD)/test_ibs

# §D3 bundled stepper: a real separate process the managed packages ship (vs. the
# in-process forked child). Links only the ptrace stepper + Capstone length-decoder
# (NOT the PT/CoreSight/AMD decoders), so a bundled copy carries no libipt/OpenCSD
# runtime dependency. Found at run time via the dladdr-sibling lookup in
# stealth_helper.c (or the ASMTEST_STEALTH_HELPER path override).
STEALTH_HELPER_OBJS := $(BUILD)/stealth_helper_main.o $(BUILD)/stealth_helper.o \
    $(BUILD)/ptrace_backend.o $(BUILD)/descent.o $(BUILD)/codeimage.o \
    $(BUILD)/disasm.o $(BUILD)/trace.o
$(BUILD)/asmtest-stealth-helper: $(STEALTH_HELPER_OBJS)
	$(CC) $(CFLAGS) $^ $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

.PHONY: stealth-helper
stealth-helper: $(BUILD)/asmtest-stealth-helper

# hwtrace-test builds the bundled helper alongside the C harness so the §D3 test's
# bundled sub-case (which finds asmtest-stealth-helper as test_hwtrace's sibling in
# $(BUILD)/) exercises the real exec'd-binary path, not only the forked child.
.PHONY: hwtrace-test
hwtrace-test: $(BUILD)/test_hwtrace $(BUILD)/asmtest-stealth-helper \
              $(BUILD)/test_ibs $(BUILD)/ibs_probe
	@echo "== hwtrace-test =="
	$(BUILD)/test_hwtrace
	@echo "== ibs (statistical IBS-Op edge lane) =="
	$(BUILD)/ibs_probe
	$(BUILD)/test_ibs

# Code-image recorder self-test (same-address-different-bytes temporal proof; runs live on
# any Linux with soft-dirty — no privilege). When built with libbpf it additionally
# exercises the eBPF emission detector, which self-skips without CAP_BPF.
$(BUILD)/test_codeimage: $(HWTRACE_OBJS) $(BUILD)/test_codeimage.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

.PHONY: codeimage-test
codeimage-test: $(BUILD)/test_codeimage
	@echo "== codeimage-test =="
	$(BUILD)/test_codeimage

# Real managed-runtime trace: attach to a LIVE JIT runtime and trace a genuine
# JIT-compiled method out of band (resolve from the runtime's perf-map -> attach ->
# run_to -> step: block-step preferred, per-instruction fallback — F18). One
# argv-driven harness drives all three runtimes (V8, CoreCLR,
# HotSpot); each self-skips (never hangs/flakes) when the runtime is absent, ptrace is
# denied, or the JIT re-tiered the code. Driven in plain containers by
# `make docker-hwtrace-jit{,-dotnet,-java}`.
$(BUILD)/jit_trace: $(HWTRACE_OBJS) $(BUILD)/jit_trace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

# §D3 whole-window capture over the cross-process JIT-address channel: a self-contained
# demo (no managed runtime) proving the stepper records methods it learns only through the
# shared channel after forking. Runs on any ptrace-capable x86-64 Linux; self-skips where
# ptrace is denied. `make windowed-trace` (and the docker-hwtrace lane below runs it).
$(BUILD)/windowed_trace: $(HWTRACE_OBJS) $(BUILD)/windowed_trace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

.PHONY: windowed-trace
windowed-trace: $(BUILD)/windowed_trace
	$(BUILD)/windowed_trace

# "Trace a process asm-test did NOT start": attach_victim runs as a wholly SEPARATE
# process; attach_trace attaches to it BY PID, resolves the hot function from its
# symbol table, run_to's the entry, and steps one call — the foreign-attach
# path (resolve -> PTRACE_ATTACH -> run_to -> trace_attached -> DETACH), no managed
# runtime needed. The tracer links the hwtrace tier (Capstone gives the disassembly);
# the victim is a plain standalone binary. Runs on any ptrace-capable x86-64/AArch64
# Linux; the victim opts in with PR_SET_PTRACER_ANY so it works under a plain
# container even at Yama ptrace_scope=1. `make docker-hwtrace-attach-demo` runs it in
# the plain (unprivileged) hwtrace container.
$(BUILD)/attach_trace: $(HWTRACE_OBJS) $(BUILD)/attach_trace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

$(BUILD)/attach_victim: $(BUILD)/attach_victim.o
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: hwtrace-attach-demo
hwtrace-attach-demo: $(BUILD)/attach_trace $(BUILD)/attach_victim
	@echo "== hwtrace-attach-demo (trace a separate, already-running process) =="
	BUILD=$(BUILD) sh examples/attach_demo.sh

# DATA-logging sibling of the attach demo: syscall_log attaches to a SEPARATE
# process BY PID and logs its syscalls WITH the data crossing the kernel boundary
# (write/read buffers, openat paths) via PTRACE_SYSCALL + process_vm_readv — a
# minimal strace on the same attach seam as attach_trace, but linking only libc
# (data logging needs the ptrace seam, not asm-test's decoder). syscall_victim does
# real file I/O so there is data to capture. Both are plain standalone binaries.
# x86-64 Linux; the victim opts in with PR_SET_PTRACER_ANY so it runs under a plain
# container at Yama ptrace_scope=1. `make docker-hwtrace-syscall-log` runs it there.
$(BUILD)/syscall_log: $(BUILD)/syscall_log.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/syscall_victim: $(BUILD)/syscall_victim.o
	$(CC) $(CFLAGS) $^ -o $@

.PHONY: hwtrace-syscall-log
hwtrace-syscall-log: $(BUILD)/syscall_log $(BUILD)/syscall_victim
	@echo "== hwtrace-syscall-log (log a separate process's syscalls + data) =="
	BUILD=$(BUILD) sh examples/syscall_demo.sh

.PHONY: hwtrace-jit hwtrace-jit-node hwtrace-jit-dotnet hwtrace-jit-java \
        hwtrace-jit-java-jitdump hwtrace-jit-jitdump hwtrace-jit-dotnet-jitdump \
        hwtrace-jit-dotnet-bcl hwtrace-jit-java-bcl
hwtrace-jit: hwtrace-jit-node # back-compat alias for the default (Node.js) lane

# The perf JVMTI agent (libperf-jvmti.so, from linux-tools) — HotSpot's de-facto jitdump
# encoder. Userspace, so a kernel-version-mismatched copy still runs; resolve by glob.
PERF_JVMTI := $(firstword $(wildcard /usr/lib/linux-tools*/*/libperf-jvmti.so \
                                     /usr/lib/linux-tools/*/libperf-jvmti.so \
                                     /usr/lib/*/libperf-jvmti.so))

# Node.js (V8): trace the optimized `asmtjit` body (needs node + Capstone).
hwtrace-jit-node: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-node (real Node.js V8 JIT method) =="
	$(BUILD)/jit_trace node

# Binary jitdump path (asmtest_jitdump_find) against a real V8 jit-<pid>.dump
# (node --perf-prof): recover a method's recorded bytes and validate them vs the perf-map
# address and the live code. Needs node + Capstone.
hwtrace-jit-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-jitdump (real V8 jitdump byte recovery) =="
	$(BUILD)/jit_trace jitdump

# .NET (CoreCLR): build the bare console app (offline, from the SDK packs) and trace its
# `Program::Add` body. DOTNET_TieredCompilation=0 (set by the harness) gives a stable
# single-compilation address. Needs the dotnet SDK + Capstone.
hwtrace-jit-dotnet: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet (real .NET CoreCLR JIT method) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	$(BUILD)/jit_trace dotnet $(BUILD)/jit_dotnet/jit_dotnet.dll

# .NET (CoreCLR) FRAMEWORK method: trace System.Console::WriteLine — BCL code that ships as
# ReadyToRun precompiled native, so the JIT never emits it by default. The harness sets
# DOTNET_ReadyToRun=0 to force the whole BCL to JIT on demand, then steps WriteLine
# like any user method. Reuses the jit_dotnet app (invoked with the "bcl" arg). Needs the
# dotnet SDK + Capstone.
hwtrace-jit-dotnet-bcl: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-bcl (real .NET CoreCLR BCL method: Console.WriteLine) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	$(BUILD)/jit_trace dotnet-bcl $(BUILD)/jit_dotnet/jit_dotnet.dll

# Binary jitdump path against a THIRD producer: .NET CoreCLR. Unlike HotSpot, CoreCLR writes
# a real /tmp/jit-<pid>.dump NATIVELY (no agent) under DOTNET_PerfMapEnabled=1 (set by the
# harness), naming the method identically in the perf-map and the jitdump — so it reuses the
# same trace_jitdump path as V8. Recovers `Program::Add`'s bytes and validates them vs the
# perf-map and the live code. The dll path is absolute (the harness chdirs to /tmp). Needs
# the dotnet SDK + Capstone.
hwtrace-jit-dotnet-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-jitdump (real .NET CoreCLR jitdump byte recovery) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	$(BUILD)/jit_trace dotnet-jitdump $(abspath $(BUILD)/jit_dotnet/jit_dotnet.dll)

# OpenJDK (HotSpot): compile the one-method hot loop and trace its `Hot.asmtjit` C2 body.
# -XX:-TieredCompilation + CompileCommand dontinline (set by the harness) give a stable,
# standalone nmethod; the harness drives `jcmd <pid> Compiler.perfmap` to materialize the
# perf-map on a live process (HotSpot does not stream one). Needs the JDK (javac + jcmd) +
# Capstone.
hwtrace-jit-java: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java (real OpenJDK HotSpot JIT method) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	$(BUILD)/jit_trace java $(BUILD)/jit_java

# --- Call-descent demo lanes (Phase 8 of docs/internal/archive/plans/call-descent-plan.md) ---------------
# `<lane>-descend` runs the same live trace at descent level 2 (DESCEND_KNOWN): the tracer
# descends INTO the runtime's own sibling JIT methods the traced method calls (the .NET
# `dotnet-bcl-descend` lane descends Console.WriteLine's `get_Out`) while still stepping OVER
# libc/PLT. `<lane>-descend-all` runs at level 3 (DESCEND_ALL) with a conservative instruction
# budget + the backend watchdog and is EXPECTED to self-skip (truncate) when it trips a guard
# — it asserts the guards fire, never that L3 is transparent. All watchdog-bounded and
# self-skipping like the plain lanes. Needs the runtime SDK + Capstone.
.PHONY: hwtrace-jit-dotnet-descend hwtrace-jit-dotnet-descend-all \
        hwtrace-jit-dotnet-bcl-descend hwtrace-jit-dotnet-bcl-descend-all \
        hwtrace-jit-java-descend hwtrace-jit-java-descend-all
hwtrace-jit-dotnet-descend hwtrace-jit-dotnet-descend-all: hwtrace-jit-dotnet-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-$* (descend into CoreCLR sibling JIT methods) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	$(BUILD)/jit_trace dotnet-$* $(BUILD)/jit_dotnet/jit_dotnet.dll
hwtrace-jit-dotnet-bcl-descend hwtrace-jit-dotnet-bcl-descend-all: hwtrace-jit-dotnet-bcl-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-bcl-$* (descend Console.WriteLine -> get_Out) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	$(BUILD)/jit_trace dotnet-bcl-$* $(BUILD)/jit_dotnet/jit_dotnet.dll
hwtrace-jit-java-descend hwtrace-jit-java-descend-all: hwtrace-jit-java-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-$* (descend into HotSpot sibling JIT methods) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	$(BUILD)/jit_trace java-$* $(BUILD)/jit_java

# OpenJDK (HotSpot) LIBRARY method: trace java.lang.Math::floorDiv. Unlike .NET, HotSpot JITs
# JDK methods on demand BY DEFAULT — no precompilation-disable flag — so this needs only the
# same dontinline nudge as the user-method lane (added by the harness) plus the jcmd perf-map
# refresh. Reuses Hot (invoked with the "bcl" arg). Needs the JDK (javac + jcmd) + Capstone.
hwtrace-jit-java-bcl: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-bcl (real OpenJDK HotSpot JDK method: Math.floorDiv) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	$(BUILD)/jit_trace java-bcl $(BUILD)/jit_java

# Binary jitdump path against a SECOND producer: OpenJDK HotSpot via the perf JVMTI agent
# (libperf-jvmti.so). The agent records each C2 method's bytes to a real jitdump; the lane
# recovers asmtjit's bytes with asmtest_jitdump_find and validates them against the live
# code and HotSpot's own jcmd perf-map. Needs the JDK, Capstone, and libperf-jvmti.so
# (self-skips if the agent is absent). Drives jcmd, so it also needs jcmd on PATH.
hwtrace-jit-java-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-jitdump (real HotSpot jitdump byte recovery) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	@rm -rf /tmp/.debug/jit 2>/dev/null || true
	$(BUILD)/jit_trace java-jitdump $(BUILD)/jit_java "$(PERF_JVMTI)"

# Python language-wrapper test for the native-trace tier (asmtest.drtrace). Builds
# the app lib + DR client, then runs the pytest suite with the lib paths wired up.
# Self-skips when DynamoRIO is absent. Runs on a dev box (DYNAMORIO_HOME=...) and
# is the body of the `make docker-drtrace` container lane.
.PHONY: drtrace-python-test
drtrace-python-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	@echo "== drtrace-python-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
else
	@echo "== drtrace-python-test =="
	@# One DR lifecycle per process (in-process re-attach is unreliable), so run
	@# each native-trace test file as its OWN pytest invocation.
	cd bindings/python && \
	  export ASMTEST_DRAPP_LIB=$(abspath $(BUILD)/libasmtest_drapp.so) \
	         ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so) \
	         ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) && \
	  python3 -m pytest tests/test_drtrace.py -v && \
	  python3 -m pytest tests/test_drgate.py -v
endif

# Per-language native-trace wrappers (parity with drtrace-python-test). Every
# binding ships a `drtrace` wrapper that dlopens libasmtest_drapp at run time and
# self-skips when DynamoRIO is absent, so these targets degrade to a SKIP message
# off a DynamoRIO host exactly like the C and Python lanes. Each builds the app
# lib + DR client, then runs that binding's standalone drtrace test with the lib
# paths wired up. rust/go additionally link libasmtest_emu (their wrapper lives in
# the same crate/package), so they also build shared-emu + the corpus lib; the
# dlopen-only bindings (cpp/node/java/dotnet/ruby/lua/zig) need neither.
# DRTRACE_BINDING_LANGS is defined up in the Docker section (used by both lanes).

# Env every binding wrapper reads to find the app lib, the DR client, and (for the
# app's lazy dlopen) libdynamorio. Mirrors the drtrace-python-test recipe.
drtrace_env = ASMTEST_DRAPP_LIB=$(abspath $(BUILD)/libasmtest_drapp.so) \
              ASMTEST_DRCLIENT=$(abspath $(BUILD)/libasmtest_drclient.so) \
              ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
              LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH"

# $(call drtrace_skip,<lang>) — the shared "DynamoRIO absent" SKIP message body.
define drtrace_skip
	@echo "== drtrace-$(1)-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
endef

# $(call drtrace_run,<shell command>) — run a wrapper's drtrace test, but downgrade
# DynamoRIO's "can't take over a multi-threaded runtime" abort to a SKIP. In-process
# DynamoRIO can't reliably seize a JIT/GC runtime's background threads, so Node, .NET
# and (intermittently) the JVM are best-effort — the Intel PT backend is the path
# there (docs/native-tracing.md). Verified live on cpp/ruby/java/lua/zig/rust/go.
# Any OTHER nonzero exit (a real failure) still propagates.
define drtrace_run
	@out=$$($(1) 2>&1); rc=$$?; printf '%s\n' "$$out"; \
	if [ $$rc -ne 0 ] && printf '%s' "$$out" | grep -q "Failed to take over all threads"; then \
	  echo "# SKIP: in-process DynamoRIO can't take over this runtime's threads (best-effort; prefer the Intel PT backend — docs/native-tracing.md)"; \
	elif [ $$rc -ne 0 ]; then exit $$rc; fi
endef

.PHONY: drtrace-bindings-test $(addprefix drtrace-,$(addsuffix -test,$(DRTRACE_BINDING_LANGS)))

# Run every language wrapper's native-trace test (plus the Python lane).
drtrace-bindings-test: drtrace-python-test \
	$(addprefix drtrace-,$(addsuffix -test,$(DRTRACE_BINDING_LANGS)))

# Shared native-trace artifacts, built ONCE before the per-language fan-out (B7).
# Every drtrace-*-test lane used to run its own `$(MAKE) shared-drtrace
# drtrace-client DRAPP_KEYSTONE=0` (a recursive sub-make — needed because the
# DRAPP_KEYSTONE=0 override is per-lane, not global). Under `make -j` two lanes
# then built the same build/pic/*.o + libasmtest_drapp + drclient into one shared
# tree concurrently → torn writes / cmake collisions. Making it a single shared
# prerequisite of every lane makes GNU make build it exactly once and every lane
# wait on it. Guarded by DR_AVAILABLE so it stays a no-op (and the lanes SKIP) off
# a DynamoRIO host.
#
# The base prep builds ONLY the DR essentials (libasmtest_drapp + the DR client):
# eight lanes and the CI `drtrace` job dlopen just libasmtest_drapp, and that job
# installs no emulator dep, so pulling shared-emu in here would fail on the missing
# libunicorn/unicorn.h. rust/go additionally load libasmtest_emu and link the
# conformance corpus fixture, so they depend on drtrace-shared-prep-emu, which layers
# shared-emu + $(CORPUS_LIB) on top. Ordered AFTER the base prep, so the shared
# pic/*.o are already built once before the emu sub-make runs — no -j torn writes.
.PHONY: drtrace-shared-prep
drtrace-shared-prep:
ifdef DR_AVAILABLE
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
endif

.PHONY: drtrace-shared-prep-emu
drtrace-shared-prep-emu: drtrace-shared-prep
ifdef DR_AVAILABLE
	@$(MAKE) shared-emu $(CORPUS_LIB) DRAPP_KEYSTONE=0
endif

drtrace-cpp-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,cpp)
else
	@echo "== drtrace-cpp-test =="
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_drtrace.cpp -ldl -o $(BUILD)/test_drtrace_cpp
	$(drtrace_env) $(BUILD)/test_drtrace_cpp
endif

drtrace-rust-test: drtrace-shared-prep-emu
ifndef DR_AVAILABLE
	$(call drtrace_skip,rust)
else
	@echo "== drtrace-rust-test =="
	@# Run as an EXAMPLE binary (main thread), NOT `cargo test` (which runs the
	@# test on a worker thread, making the process multi-threaded when dr_app_start
	@# takes over — that crashes; a single-threaded main lets DR take over cleanly).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(drtrace_env) \
	  $(CARGO) run --quiet --example drtrace
endif

drtrace-go-test: drtrace-shared-prep-emu
ifndef DR_AVAILABLE
	$(call drtrace_skip,go)
else
	@echo "== drtrace-go-test =="
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" CGO_CFLAGS="-I$(abspath include)" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) $(drtrace_env) \
	  $(GO) test -run TestDrtrace ./...
endif

drtrace-node-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,node)
else
	@echo "== drtrace-node-test =="
	$(call drtrace_run,cd bindings/node && $(drtrace_env) $(NODE) test_drtrace.js)
endif

drtrace-java-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,java)
else
	@echo "== drtrace-java-test =="
	mkdir -p $(BUILD)/java-drtrace
	$(JAVAC) --release 22 -d $(BUILD)/java-drtrace \
	  bindings/java/DrTrace.java bindings/java/DrTraceTest.java
	$(call drtrace_run,$(drtrace_env) $(JAVA) --enable-native-access=ALL-UNNAMED -cp $(BUILD)/java-drtrace DrTraceTest)
endif

drtrace-dotnet-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,dotnet)
else
	@echo "== drtrace-dotnet-test =="
	$(call drtrace_run,$(drtrace_env) $(DOTNET) run --project bindings/dotnet/drtrace/drtrace.csproj)
endif

drtrace-ruby-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,ruby)
else
	@echo "== drtrace-ruby-test =="
	cd bindings/ruby && $(drtrace_env) $(RUBY) test_drtrace.rb
endif

drtrace-lua-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,lua)
else
	@echo "== drtrace-lua-test =="
	cd bindings/lua && $(drtrace_env) $(LUAJIT) test_drtrace.lua
endif

drtrace-zig-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,zig)
else
	@echo "== drtrace-zig-test =="
	cd bindings/zig && $(drtrace_env) \
	  $(ZIG) build drtrace-test -Dincdir=$(abspath include) -Dlibdir=$(abspath $(BUILD))
endif

# --- Hardware-trace language wrappers (single-step / PT / AMD) --------------
# The counterpart of the drtrace-<lang> lanes for the hardware-trace tier. Unlike
# DynamoRIO, the tier's portable SINGLESTEP backend needs NO external engine and NO
# perf privilege, so there is no DR_AVAILABLE gate: each target always builds
# shared-hwtrace and runs the binding's test, which self-skips internally only off
# x86-64 Linux or without Capstone. Every wrapper dlopens libasmtest_hwtrace and
# reads $ASMTEST_HWTRACE_LIB; the single-step backend runs live here and on CI/in a
# plain container (no privilege), making these the tier's first cross-language
# regression that actually executes rather than self-skipping.
HWTRACE_BINDING_LANGS := cpp rust go node java dotnet ruby lua zig

hwtrace_env = ASMTEST_HWTRACE_LIB=$(abspath $(call shlib_dev,libasmtest_hwtrace)) \
              LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH"

# .NET lanes only: pin CoreCLR's tiered-compilation background worker resident
# (0x36EE80 ms = 1 h; DOTNET_* DWORDs parse as hex). The suites arm in-process
# EFLAGS.TF windows on managed threads; if the worker idle-exits (default ~4 s)
# and is respawned INSIDE such a window, glibc's pthread_create blocks all
# signals around clone() and the very next #DB is a SIGTRAP delivered while
# blocked — the kernel force-resets it to SIG_DFL and kills the process (exit
# 133). A fast host never steps a window long enough to hit it; GitHub-hosted
# runners died deterministically (dmesg: RIP inside pthread_create, EFLAGS.TF
# set). See docs/guides/tracing/hardware-tracing.md ("per-thread footgun").
hwtrace_dotnet_env = DOTNET_TC_BackgroundWorkerTimeoutMs=36EE80

.PHONY: hwtrace-python-test hwtrace-bindings-test \
        $(addprefix hwtrace-,$(addsuffix -test,$(HWTRACE_BINDING_LANGS)))

# Run every language wrapper's hardware-trace test (plus the Python lane).
hwtrace-bindings-test: hwtrace-python-test \
	$(addprefix hwtrace-,$(addsuffix -test,$(HWTRACE_BINDING_LANGS)))

hwtrace-python-test: shared-hwtrace
	@echo "== hwtrace-python-test =="
	cd bindings/python && $(hwtrace_env) python3 -m pytest tests/test_hwtrace.py -v

hwtrace-cpp-test: shared-hwtrace
	@echo "== hwtrace-cpp-test =="
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_hwtrace.cpp -ldl -o $(BUILD)/test_hwtrace_cpp
	$(hwtrace_env) $(BUILD)/test_hwtrace_cpp

# rust/go link libasmtest_emu (their wrapper shares the crate/package), so build
# the emu superset + corpus lib too — mirroring the drtrace-rust/go lanes.
hwtrace-rust-test: shared-hwtrace shared-emu $(CORPUS_LIB)
	@echo "== hwtrace-rust-test =="
	@# --test-threads=1: the single-step backend is process-global (single active
	@# region, a global SIGTRAP handler, per-thread EFLAGS.TF), so the two live
	@# single-step tests must run serially — concurrent stepping crashes (SIGTRAP).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(hwtrace_env) \
	  $(CARGO) test --test hwtrace -- --nocapture --test-threads=1

hwtrace-go-test: shared-hwtrace shared-emu $(CORPUS_LIB)
	@echo "== hwtrace-go-test =="
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" CGO_CFLAGS="-I$(abspath include)" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) $(hwtrace_env) \
	  $(GO) test -run TestHwtrace ./...

hwtrace-node-test: shared-hwtrace
	@echo "== hwtrace-node-test =="
	cd bindings/node && $(hwtrace_env) $(NODE) test_hwtrace.js

hwtrace-java-test: shared-hwtrace
	@echo "== hwtrace-java-test =="
	mkdir -p $(BUILD)/java-hwtrace
	$(JAVAC) --release 22 -d $(BUILD)/java-hwtrace \
	  bindings/java/HwTrace.java bindings/java/HwTraceTest.java
	$(hwtrace_env) $(JAVA) --enable-native-access=ALL-UNNAMED \
	  -cp $(BUILD)/java-hwtrace HwTraceTest

hwtrace-dotnet-test: shared-hwtrace
	@echo "== hwtrace-dotnet-test =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj

# Slow-host crash-avoidance stress (managed-singlestep-lazy-arm-plan "Sharpening 1"):
# the ONE lane that must run with the tiering worker UNPINNED — no $(hwtrace_dotnet_env)
# — so CoreCLR's worker idle-exits and is respawned via pthread_create adjacent to the
# lazy-arm windows. Under the old stepped-DynamicInvoke Invoke this died (exit 133) on
# loaded runners; under lazy-arm the process must survive with every capture intact.
# Meaningful on a slow/loaded host (CI); on a fast dev box it simply passes quickly.
.PHONY: hwtrace-dotnet-stress
hwtrace-dotnet-stress: shared-hwtrace
	@echo "== hwtrace-dotnet-stress (tiering worker UNPINNED) =="
	$(hwtrace_env) ASMTEST_METHOD_STRESS=60 $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj

# Forward-runtime drift check: the same self-test on .NET 9. The images carry only
# dotnet-sdk-8.0, so install net9 user-local via the official dotnet-install script
# into a scratch dir, flip the TFM in a scratch COPY of the project (the tree is
# never modified), and run. Guards the diagnostics-IPC (DOTNET_IPC_V1) and
# MethodLoadVerbose surfaces against runtime drift — the two places a new runtime
# would silently regress the rundown/labelling paths. Network-dependent (dot.net):
# self-skips, never fails, when the toolchain cannot be fetched.
.PHONY: hwtrace-dotnet9-test
hwtrace-dotnet9-test: shared-hwtrace
	@echo "== hwtrace-dotnet9-test (forward-runtime drift check) =="
	@set -e; d9=$$(mktemp -d); trap 'rm -rf "$$d9"' EXIT; \
	if ! curl -fsSL --max-time 60 https://dot.net/v1/dotnet-install.sh -o $$d9/di.sh; then \
	  echo "# SKIP net9: dotnet-install.sh unreachable"; exit 0; fi; \
	if ! bash $$d9/di.sh --channel 9.0 --install-dir $$d9/dotnet >$$d9/install.log 2>&1; then \
	  echo "# SKIP net9: install failed"; tail -3 $$d9/install.log; exit 0; fi; \
	cp -r bindings/dotnet $$d9/dn && rm -rf $$d9/dn/*/obj $$d9/dn/*/bin; \
	sed -i 's#<TargetFramework>net8.0</TargetFramework>#<TargetFramework>net9.0</TargetFramework>#' \
	  $$d9/dn/hwtrace/hwtrace.csproj; \
	cd $$d9/dn/hwtrace && $(hwtrace_env) $(hwtrace_dotnet_env) \
	  DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  $$d9/dotnet/dotnet run

# Runnable demos of the scoped-trace facility (§Z0/§Z1) on the single-step WEAK tier:
# one project per report — see examples/dotnet/README.md for what each shows.
.PHONY: hwtrace-dotnet-example
hwtrace-dotnet-example: shared-hwtrace
	@echo "== hwtrace-dotnet-example (wholewindow) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/wholewindow/wholewindow.csproj
	@echo "== hwtrace-dotnet-example (region) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/region/region.csproj
	@echo "== hwtrace-dotnet-example (methods) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/methods/methods.csproj
	@echo "== hwtrace-dotnet-example (rundown) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/rundown/rundown.csproj
	@echo "== hwtrace-dotnet-example (localscope) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope/localscope.csproj
	@echo "== hwtrace-dotnet-example (localscope_oop) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope_oop/localscope_oop.csproj
	@echo "== hwtrace-dotnet-example (localscope_oop_managed) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope_oop_managed/localscope_oop_managed.csproj
	@echo "== hwtrace-dotnet-example (windowhybrid) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/windowhybrid/windowhybrid.csproj
	@echo "== hwtrace-dotnet-example (amdhot) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amdhot/amdhot.csproj
	@echo "== hwtrace-dotnet-example (amdlbr) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amdlbr/amdlbr.csproj
	@echo "== hwtrace-dotnet-example (assemblies) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/assemblies/assemblies.csproj
	@echo "== hwtrace-dotnet-example (annotated) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/annotated/annotated.csproj
	@echo "== hwtrace-dotnet-example (tiers) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/tiers/tiers.csproj
	@echo "== hwtrace-dotnet-example (hotspots) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/hotspots/hotspots.csproj
	@echo "== hwtrace-dotnet-example (coverage) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/coverage/coverage.csproj
	@echo "== hwtrace-dotnet-example (callgraph) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/callgraph/callgraph.csproj
	@echo "== hwtrace-dotnet-example (ptrace_native) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/ptrace_native/ptrace_native.csproj
	@echo "== hwtrace-dotnet-example (blockstep) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/blockstep/blockstep.csproj
	@echo "== hwtrace-dotnet-example (ptrace_dotnet) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/ptrace_dotnet/ptrace_dotnet.csproj
	@echo "== hwtrace-dotnet-example (flatprofile) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/flatprofile/flatprofile.csproj
	@echo "== hwtrace-dotnet-example (amplification) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amplification/amplification.csproj
	@echo "== hwtrace-dotnet-example (runtimegaps) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/runtimegaps/runtimegaps.csproj
	@echo "== hwtrace-dotnet-example (footprint) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/footprint/footprint.csproj
	@echo "== hwtrace-dotnet-example (runtimebuckets) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/runtimebuckets/runtimebuckets.csproj
	@echo "== hwtrace-dotnet-example (instructionmix) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/instructionmix/instructionmix.csproj
	@echo "== hwtrace-dotnet-example (perfannotate) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/perfannotate/perfannotate.csproj
	@echo "== hwtrace-dotnet-example (loops) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/loops/loops.csproj
	@echo "== hwtrace-dotnet-example (descent) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descent/descent.csproj
	@echo "== hwtrace-dotnet-example (descent_dotnet) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descent_dotnet/descent_dotnet.csproj
	@echo "== hwtrace-dotnet-example (single-method) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/single-method/single-method.csproj
	@echo "== hwtrace-dotnet-example (perf-triage-drill) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/perf-triage-drill/perf-triage-drill.csproj
	@echo "== hwtrace-dotnet-example (concurrent-isolation) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/concurrent-isolation/concurrent-isolation.csproj
	@echo "== hwtrace-dotnet-example (async-stitch) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/async-stitch/async-stitch.csproj
	@echo "== hwtrace-dotnet-example (trace-diff) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/trace-diff/trace-diff.csproj
	@echo "== hwtrace-dotnet-example (coverage-guided-fuzz) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/coverage-guided-fuzz/coverage-guided-fuzz.csproj
	@echo "== hwtrace-dotnet-example (trace-cost-overhead) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/trace-cost-overhead/trace-cost-overhead.csproj
	@echo "== hwtrace-dotnet-example (descend-all) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descend-all/descend-all.csproj
	@echo "== hwtrace-dotnet-example (crashproof-showdown) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/crashproof-showdown/crashproof-showdown.csproj
	@echo "== hwtrace-dotnet-example (crashproof-survey) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/crashproof-survey/crashproof-survey.csproj
	@echo "== hwtrace-dotnet-example (tier-ladder) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/tier-ladder/tier-ladder.csproj
	@echo "== hwtrace-dotnet-example (amd-period-sweep) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amd-period-sweep/amd-period-sweep.csproj
	@echo "== hwtrace-dotnet-example (amd-snapshot) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amd-snapshot/amd-snapshot.csproj
	@echo "== hwtrace-dotnet-example (codeimage) =="
	$(hwtrace_env) $(DOTNET) run --project examples/dotnet/codeimage/codeimage.csproj

hwtrace-ruby-test: shared-hwtrace
	@echo "== hwtrace-ruby-test =="
	cd bindings/ruby && $(hwtrace_env) $(RUBY) test_hwtrace.rb

hwtrace-lua-test: shared-hwtrace
	@echo "== hwtrace-lua-test =="
	cd bindings/lua && $(hwtrace_env) $(LUAJIT) test_hwtrace.lua

hwtrace-zig-test: shared-hwtrace
	@echo "== hwtrace-zig-test =="
	cd bindings/zig && $(hwtrace_env) \
	  $(ZIG) build hwtrace-test -Dincdir=$(abspath include) -Dlibdir=$(abspath $(BUILD))

# PIC variants + the standalone shared lib (never linked by core/superset).
$(BUILD)/pic/hwtrace.o: src/hwtrace.c include/asmtest_hwtrace.h \
                        include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/pt_backend.o: src/pt_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/cs_backend.o: src/cs_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(OPENCSD_DEF) $(OPENCSD_CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/amd_backend.o: src/amd_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(PERF_BR_SPEC_DEF) -fPIC -c $< -o $@
$(BUILD)/pic/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/trace_auto.o: src/trace_auto.c include/asmtest_trace_auto.h \
                           include/asmtest_hwtrace.h include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/ptrace_backend.o: src/ptrace_backend.c include/asmtest_ptrace.h \
                               include/asmtest_trace.h include/asmtest_codeimage.h \
                               include/asmtest_descent_internal.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/descent.o: src/descent.c include/asmtest_ptrace.h \
                        include/asmtest_descent_internal.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/codeimage.o: src/codeimage.c include/asmtest_codeimage.h \
                          $(CODEIMAGE_SKEL) | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) $(CODEIMAGE_INC) -fPIC -c $< -o $@
$(BUILD)/pic/branchsnap.o: src/branchsnap.c include/asmtest_hwtrace.h \
                           $(BRANCHSNAP_SKEL) | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) -I$(BUILD) -Ibpf -fPIC -c $< -o $@
$(BUILD)/pic/stealth_helper.o: src/stealth_helper.c src/stealth_helper.h \
                               include/asmtest_hwtrace.h include/asmtest_ptrace.h \
                               include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/msr_lbr.o: src/msr_lbr.c include/asmtest_hwtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/debug.o: src/debug.c src/debug.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
# ibs_backend.o must be in the shared lib too: since the Zen-2 F6 fallback,
# hwtrace.c calls asmtest_ibs_window_begin/_end, so omitting it left
# libasmtest_hwtrace.so with an undefined symbol at dlopen (every binding's
# trace_call_auto/resolve_tiers path loads unconditionally, on any host).
$(BUILD)/pic/ibs_backend.o: src/ibs_backend.c include/asmtest_ibs.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# K5: rebuild the hardware/native-trace object tree on a build-knob flip
# (SAN=1/COV=1/CSTD), like the core tree. drtrace_app.o already tracks its own
# .drapp-flags (Keystone + CFLAGS); the rest gain the shared .build-flags
# sentinel here, so `make shared-hwtrace && make SAN=1 shared-hwtrace` no longer
# relinks stale, uninstrumented objects. codeimage.o is included (its bytecode
# skeleton is regenerated separately, but the host object still tracks CFLAGS).
NATIVE_TRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o \
    $(BUILD)/cs_backend.o $(BUILD)/amd_backend.o $(BUILD)/ss_backend.o \
    $(BUILD)/trace_auto.o $(BUILD)/ptrace_backend.o $(BUILD)/descent.o \
    $(BUILD)/stealth_helper.o $(BUILD)/codeimage.o $(BUILD)/branchsnap.o \
    $(BUILD)/msr_lbr.o $(BUILD)/ibs_backend.o $(BUILD)/debug.o
$(NATIVE_TRACE_OBJS) $(patsubst $(BUILD)/%,$(BUILD)/pic/%,$(NATIVE_TRACE_OBJS)): \
    $(BUILD)/.build-flags

shared-hwtrace: $(call shlib_dev,libasmtest_hwtrace)
$(call shlib_real,libasmtest_hwtrace): $(BUILD)/pic/hwtrace.o \
                                       $(BUILD)/pic/pt_backend.o \
                                       $(BUILD)/pic/cs_backend.o \
                                       $(BUILD)/pic/amd_backend.o \
                                       $(BUILD)/pic/ss_backend.o \
                                       $(BUILD)/pic/trace_auto.o \
                                       $(BUILD)/pic/ptrace_backend.o \
                                       $(BUILD)/pic/descent.o \
                                       $(BUILD)/pic/stealth_helper.o \
                                       $(BUILD)/pic/codeimage.o \
                                       $(BUILD)/pic/branchsnap.o \
                                       $(BUILD)/pic/msr_lbr.o \
                                       $(BUILD)/pic/ibs_backend.o \
                                       $(BUILD)/pic/debug.o \
                                       $(BUILD)/pic/disasm.o \
                                       $(BUILD)/pic/trace.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_hwtrace) $^ \
	      $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@
$(call shlib_dev,libasmtest_hwtrace): $(call shlib_real,libasmtest_hwtrace)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_hwtrace)
	ln -sf $(notdir $(call shlib_compat,libasmtest_hwtrace)) $@

# K6: install the native-trace shared libs + their headers, so an installed-prefix
# consumer can compile against the hardware-trace / DynamoRIO tiers (the guide's
# `#include "asmtest_drtrace.h"` and packaging.md's "installs the libs"). `make
# install` installs the headers; these add the shared lib + soname/dev symlinks
# (mirroring install-shared-emu). No pkg-config template ships for these tiers —
# a consumer links by name (-lasmtest_hwtrace / -lasmtest_drapp).
.PHONY: install-shared-hwtrace install-shared-drtrace install-stealth-helper
# §D3 bundled stepper: install the standalone helper NEXT TO libasmtest_hwtrace so
# the dladdr-sibling discovery in stealth_helper.c finds it with no env/opts — the
# same "sibling of the payload" placement the managed packages use. It is an
# executable in $(libdir) by design (that is where the sibling lookup resolves).
install-stealth-helper: stealth-helper
	mkdir -p $(libdir)
	cp $(BUILD)/asmtest-stealth-helper $(libdir)/
	@echo "installed asmtest-stealth-helper to $(libdir)"

install-shared-hwtrace: shared-hwtrace
	mkdir -p $(libdir) $(incdir)
	cp $(call shlib_real,libasmtest_hwtrace) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest_hwtrace)) \
	       $(libdir)/$(call shlib_soname,libasmtest_hwtrace)
	ln -sf $(call shlib_soname,libasmtest_hwtrace) \
	       $(libdir)/$(notdir $(call shlib_dev,libasmtest_hwtrace))
	cp include/asmtest_hwtrace.h include/asmtest_ptrace.h \
	   include/asmtest_codeimage.h include/asmtest_trace_auto.h \
	   include/asmtest_trace.h $(incdir)/
	@echo "installed shared libasmtest_hwtrace $(ASMTEST_VERSION) to $(libdir)"

install-shared-drtrace: shared-drtrace
	mkdir -p $(libdir) $(incdir)
	cp $(call shlib_real,libasmtest_drapp) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest_drapp)) \
	       $(libdir)/$(call shlib_soname,libasmtest_drapp)
	ln -sf $(call shlib_soname,libasmtest_drapp) \
	       $(libdir)/$(notdir $(call shlib_dev,libasmtest_drapp))
	cp include/asmtest_drtrace.h include/asmtest_trace.h $(incdir)/
	@echo "installed shared libasmtest_drapp $(ASMTEST_VERSION) to $(libdir)"

