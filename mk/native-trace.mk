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
                                 src/dataflow_dr.h drclient/CMakeLists.txt | $(BUILD)
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
	@$(MAKE) --no-print-directory dr-taint-stress-test
	@$(MAKE) --no-print-directory dr-taint-multirange-test
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

