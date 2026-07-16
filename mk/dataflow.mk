# dataflow.mk — data-flow tracing tier (docs/internal/plans/data-flow-tracing-plan.md).
#
# Included by ../Makefile (split out by concern). All knobs (CSTD, WERROR,
# ASM_SYNTAX, BUILD, CAPSTONE_*, UNICORN_*, ...) come from the parent Makefile,
# which reads this file in place after they are defined; edit targets here.
#
# Three layers, on separate dependency tiers so the pure spine builds everywhere:
#   dataflow.o          PURE C — the L0 value-trace sink + L1 def-use + L2 slicer
#                       (no Capstone, no Unicorn). Runs on every host.
#   dataflow_operands.o Capstone operand read/write-set enumerator (detail mode).
#                       Degrades to a no-op without Capstone.
#   dataflow_emu.o      Unicorn L0 producer (Phase 2). Built only when libunicorn
#                       is present; the emulator test self-skips otherwise.
#
# `make dataflow-test` builds + runs the three suites. The pure suites run on any
# host; the emulator suite is gated on libunicorn.

# --- objects ---------------------------------------------------------------
$(BUILD)/dataflow.o: src/dataflow.c include/asmtest_valtrace.h \
                     include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Phase 4 (increment 1): the pure PC -> (method, version) resolver over an L0
# value trace. Same PURE-C tier as dataflow.o (no Capstone, no Unicorn) — runs
# everywhere; the managed-taint prerequisite.
$(BUILD)/dataflow_method.o: src/dataflow_method.c include/asmtest_valtrace.h \
                            include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Phase 4 (increment 2): the pure GC-move address canonicalizer over an L0 value
# trace. Same PURE-C tier as dataflow.o (no Capstone, no Unicorn) — runs
# everywhere; remaps memory addresses across a GC compaction to a stable
# (object, field) identity so def-use survives without false aliasing.
$(BUILD)/dataflow_gcmove.o: src/dataflow_gcmove.c include/asmtest_valtrace.h \
                            include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Phase 4 (increment 3): the pure runtime-helper SUMMARY-EDGE modeler over an L0
# value trace. Same PURE-C tier as dataflow.o (no Capstone, no Unicorn) — runs
# everywhere; rewrites a recognized CoreCLR helper call (alloc / write-barrier /
# generic-dict) into its declared input->output summary and reuses the shared
# asmtest_defuse_build, so caller data-flow connects across the helper body.
$(BUILD)/dataflow_helpers.o: src/dataflow_helpers.c include/asmtest_valtrace.h \
                             include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/dataflow_operands.o: src/dataflow_operands.c include/asmtest_valtrace.h \
                              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# Unicorn producer: needs both Unicorn (engine) and Capstone (operand enumerator).
$(BUILD)/dataflow_emu.o: src/dataflow_emu.c include/asmtest_valtrace.h \
                         include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# Scoped ptrace L0 producer (Phase 3): fills the SAME asmtest_valtrace_t from a live,
# single-stepped tracee (its own fork/attach + PTRACE_SINGLESTEP loop). Ships no header —
# a value-trace PRODUCER is a tier, not part of the shared sink API (its test re-declares
# the entry points, like the emulator producer). Needs Capstone (the operand enumerator);
# off Linux x86-64 / without Capstone it compiles to an ENOSYS stub.
$(BUILD)/dataflow_ptrace.o: src/dataflow_ptrace.c \
                            include/asmtest_valtrace.h include/asmtest_trace.h \
                            include/asmtest_codeimage.h \
                            $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# --- Phase 6: the data-flow ANALYSIS shared lib (libasmtest_dataflow) --------
# The L0 value sink + L1 def-use + L2 slice + method identity + GC-move
# canonicalization + runtime-helper summaries — the pure analysis pipeline, the
# packaging target the language bindings (Phase 6) dlopen. The value-trace
# PRODUCERS (emu / ptrace / DR) are separate tiers and are NOT bundled here.
# Links Capstone (the operand enumerator dataflow_operands uses in detail mode).
$(BUILD)/pic/dataflow.o: src/dataflow.c include/asmtest_valtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/dataflow_method.o: src/dataflow_method.c include/asmtest_valtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/dataflow_gcmove.o: src/dataflow_gcmove.c include/asmtest_valtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/dataflow_helpers.o: src/dataflow_helpers.c include/asmtest_valtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/dataflow_operands.o: src/dataflow_operands.c include/asmtest_valtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -fPIC -c $< -o $@

DATAFLOW_SHLIB_OBJS := $(BUILD)/pic/dataflow.o $(BUILD)/pic/dataflow_operands.o \
                       $(BUILD)/pic/dataflow_method.o $(BUILD)/pic/dataflow_gcmove.o \
                       $(BUILD)/pic/dataflow_helpers.o

.PHONY: shared-dataflow dataflow-python-test
shared-dataflow: $(call shlib_dev,libasmtest_dataflow)
$(call shlib_real,libasmtest_dataflow): $(DATAFLOW_SHLIB_OBJS)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_dataflow) $^ $(CAPSTONE_LIBS) -o $@
$(call shlib_dev,libasmtest_dataflow): $(call shlib_real,libasmtest_dataflow)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_dataflow)
	ln -sf $(notdir $(call shlib_compat,libasmtest_dataflow)) $@

# Phase 6 — the Python data-flow binding (bindings/python/asmtest/dataflow.py).
# Runs the standalone TAP reporter (no pytest dependency) against the freshly
# built analysis lib, so it validates the ctypes wrapper on any host.
dataflow-python-test: shared-dataflow
	ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  python3 bindings/python/tests/test_dataflow.py

# Phase 6 — the C++ data-flow binding (bindings/cpp/asmtest_dataflow.hpp): a
# header-only typed wrapper. Links the two PURE analysis objects it calls
# (gcmove canon + method resolver); no Capstone/Unicorn, so it builds anywhere.
.PHONY: dataflow-cpp-test
dataflow-cpp-test: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                   $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
                   bindings/cpp/asmtest_dataflow.hpp bindings/cpp/test_dataflow.cpp | $(BUILD)
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_dataflow.cpp \
	  $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
	  $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o $(CAPSTONE_LIBS) \
	  -o $(BUILD)/test_dataflow_cpp
	$(BUILD)/test_dataflow_cpp

# Phase 6 — the Node data-flow binding (bindings/node/dataflow.js, koffi). Self-skips
# (available()===false) when the analysis lib isn't built, so it never reddens a general
# node run; this dedicated target builds the lib + points the loader at it.
.PHONY: dataflow-node-test
dataflow-node-test: shared-dataflow
	cd bindings/node && \
	  ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(NODE) test_dataflow.js

# Phase 6 — the Ruby data-flow binding (bindings/ruby/dataflow.rb, Fiddle). Needs a
# Ruby interpreter (the docker bindings image); validated in ubuntu:24.04 locally.
.PHONY: dataflow-ruby-test
dataflow-ruby-test: shared-dataflow
	cd bindings/ruby && \
	  ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(RUBY) test_dataflow.rb

# Phase 6 — the Lua data-flow binding (bindings/lua/dataflow.lua, LuaJIT FFI). Needs
# LuaJIT (the docker bindings image); validated in ubuntu:24.04 locally.
.PHONY: dataflow-lua-test
dataflow-lua-test: shared-dataflow
	cd bindings/lua && \
	  ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(LUAJIT) test_dataflow.lua

# Phase 6 — the Zig data-flow binding (bindings/zig/src/dataflow via std.DynLib). Needs
# zig 0.13.x (the docker bindings image). `-lc` selects the libc dlopen backend (zig's
# own ELF loader wants a hash table the plain shared lib omits — ElfHashTableNotFound).
.PHONY: dataflow-zig-test
dataflow-zig-test: shared-dataflow
	ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(ZIG) run -lc bindings/zig/src/test_dataflow.zig

# Phase 6 — the Rust data-flow binding (bindings/rust/test_dataflow.rs, direct FFI).
# Standalone rustc smoke (no cargo project) linked against the analysis lib; needs
# rustc (the docker bindings image).
RUSTC ?= rustc
.PHONY: dataflow-rust-test
dataflow-rust-test: shared-dataflow
	$(RUSTC) bindings/rust/test_dataflow.rs -L $(BUILD) -l asmtest_dataflow \
	  -o $(BUILD)/rust_dataflow_test
	LD_LIBRARY_PATH=$(abspath $(BUILD)) $(BUILD)/rust_dataflow_test

# Phase 6 — the Go data-flow binding (bindings/go/cmd/dataflowsmoke, cgo dlopen).
# Needs Go + a C toolchain (cgo); validated in golang:1 locally.
.PHONY: dataflow-go-test
dataflow-go-test: shared-dataflow
	cd bindings/go && \
	  ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  GOFLAGS=-mod=mod $(GO) run ./cmd/dataflowsmoke

# Phase 6 — the Java data-flow binding (bindings/java/TestDataflow.java, Project Panama
# FFM). Needs JDK 22+ (the docker bindings image uses openjdk-25); validated in JDK 23.
JAVA ?= java
.PHONY: dataflow-java-test
dataflow-java-test: shared-dataflow
	$(JAVAC) --release 22 -d $(BUILD)/java-df bindings/java/TestDataflow.java
	ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(JAVA) --enable-native-access=ALL-UNNAMED -cp $(BUILD)/java-df TestDataflow

# Phase 6 — the .NET data-flow binding (bindings/dotnet/dataflow_smoke, P/Invoke).
# Needs the .NET SDK (the docker bindings image); validated in dotnet/sdk:8.0.
.PHONY: dataflow-dotnet-test
dataflow-dotnet-test: shared-dataflow
	cd bindings/dotnet/dataflow_smoke && \
	  ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
	  $(DOTNET) run -c Release

# All-bindings convenience target (each self-selects its interpreter/compiler).
.PHONY: dataflow-bindings-test
dataflow-bindings-test: dataflow-python-test dataflow-cpp-test dataflow-node-test \
                        dataflow-ruby-test dataflow-lua-test dataflow-zig-test \
                        dataflow-rust-test dataflow-go-test dataflow-java-test \
                        dataflow-dotnet-test
	@echo "dataflow-bindings-test: all 10 language bindings passed"

# --- test-object compile knobs ---------------------------------------------
# The examples/%.c pattern rule (root Makefile) compiles these with plain CFLAGS;
# the Capstone/Unicorn suites need the extra include paths + the -DASMTEST_HAVE_CAPSTONE
# guard so their #ifdef'd assertions compile. test_dataflow.o needs nothing extra.
$(BUILD)/test_operands.o: CFLAGS += $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF)
$(BUILD)/test_dataflow_emu.o: CFLAGS += $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) \
                                        $(CAPSTONE_DEF)

# --- test binaries (standalone TAP, like test_ibs; no framework runtime) ----
$(BUILD)/test_dataflow: $(BUILD)/dataflow.o $(BUILD)/test_dataflow.o
	$(CC) $(CFLAGS) $^ -o $@

# Phase 4 (increment 1) PC -> (method, version) resolver suite. PURE — links only
# the value-trace sink + the resolver, no framework/Capstone/Unicorn. This EXPLICIT
# rule beats the root Makefile's generic test_% pattern (which would otherwise link
# the framework runtime + a same-named routine object it does not have), so the
# suite builds correctly wherever it is requested — including if the root
# SUITE_EXCLUDES has not yet been updated to keep it out of `make test`.
$(BUILD)/test_dataflow_method: $(BUILD)/dataflow.o $(BUILD)/dataflow_method.o \
                               $(BUILD)/test_dataflow_method.o
	$(CC) $(CFLAGS) $^ -o $@

# Phase 4 (increment 2) GC-move canonicalization suite. PURE — links only the
# value-trace sink + the canonicalizer, no framework/Capstone/Unicorn. Like the
# increment-1 rule above, this EXPLICIT rule beats the root Makefile's generic
# test_% pattern (which would otherwise link the framework runtime + a same-named
# routine object it does not have), so the suite builds correctly wherever it is
# requested — including before the root SUITE_EXCLUDES is updated to keep it out
# of `make test`.
$(BUILD)/test_dataflow_gcmove: $(BUILD)/dataflow.o $(BUILD)/dataflow_gcmove.o \
                               $(BUILD)/test_dataflow_gcmove.o
	$(CC) $(CFLAGS) $^ -o $@

# Phase 4 (increment 3) runtime-helper summary-edge suite. PURE — links the
# value-trace sink + the increment-1 resolver (name identification) + the helper
# modeler, no framework/Capstone/Unicorn. Like the increment-1/2 rules above, this
# EXPLICIT rule beats the root Makefile's generic test_% pattern (which would link
# the framework runtime + a same-named routine object it does not have), so the
# suite builds wherever it is requested — including before the root SUITE_EXCLUDES
# is updated to keep it out of `make test`.
$(BUILD)/test_dataflow_helpers: $(BUILD)/dataflow.o $(BUILD)/dataflow_method.o \
                                $(BUILD)/dataflow_helpers.o \
                                $(BUILD)/test_dataflow_helpers.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_operands: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                        $(BUILD)/test_operands.o
	$(CC) $(CFLAGS) $^ $(CAPSTONE_LIBS) -o $@

$(BUILD)/test_dataflow_emu: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                            $(BUILD)/dataflow_emu.o $(BUILD)/test_dataflow_emu.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

# --- the tier target -------------------------------------------------------
# Gate the emulator suite on libunicorn (like emu-test); the pure suites always run.
DF_HAVE_UNICORN := $(shell pkg-config --exists unicorn 2>/dev/null && echo 1)
ifeq ($(DF_HAVE_UNICORN),1)
DF_EMU_SUITE := $(BUILD)/test_dataflow_emu
endif

# The Phase 3 ptrace suite always builds (Capstone); it CROSS-VALIDATES against the
# emulator oracle only where Unicorn is present (-DDF_HAVE_EMU pulls in dataflow_emu.o +
# libunicorn), and otherwise still runs its live-capture assertions, skipping just the
# oracle comparison. At runtime it self-skips where ptrace is blocked (seccomp).
# Increment 3 adds codeimage.o (the versioned-decode byte source the producer now calls) and
# dataflow_method.o (the PC -> method+version attribution post-pass the suite drives). asmspy
# already links codeimage.o via HWTRACE_OBJS, so this is only the standalone test's link.
ifeq ($(DF_HAVE_UNICORN),1)
$(BUILD)/test_dataflow_ptrace.o: CFLAGS += $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) \
                                           $(UNICORN_CFLAGS) -DDF_HAVE_EMU
$(BUILD)/test_dataflow_ptrace: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                               $(BUILD)/dataflow_method.o $(BUILD)/codeimage.o \
                               $(BUILD)/dataflow_ptrace.o $(BUILD)/dataflow_emu.o \
                               $(BUILD)/test_dataflow_ptrace.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -o $@
else
$(BUILD)/test_dataflow_ptrace.o: CFLAGS += $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF)
$(BUILD)/test_dataflow_ptrace: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                               $(BUILD)/dataflow_method.o $(BUILD)/codeimage.o \
                               $(BUILD)/dataflow_ptrace.o \
                               $(BUILD)/test_dataflow_ptrace.o
	$(CC) $(CFLAGS) $^ $(CAPSTONE_LIBS) $(LINK_LIBBPF) -o $@
endif

# Increment 4 (worker-thread targeting): the multi-thread fixture in test_dataflow_ptrace.c
# spawns pthread workers, so the suite links libpthread. -pthread is additive + position-
# independent (a no-op where glibc folds pthread into libc, glibc >= 2.34); appended so it
# applies to whichever test_dataflow_ptrace link rule (unicorn / non-unicorn) above fires.
$(BUILD)/test_dataflow_ptrace: CFLAGS += -pthread

.PHONY: dataflow-test dataflow-grep-gate
dataflow-test: $(BUILD)/test_dataflow $(BUILD)/test_dataflow_method \
               $(BUILD)/test_dataflow_gcmove $(BUILD)/test_dataflow_helpers \
               $(BUILD)/test_operands $(DF_EMU_SUITE) \
               $(BUILD)/test_dataflow_ptrace
	@echo "== dataflow-test =="
	$(MAKE) --no-print-directory dataflow-grep-gate
	$(BUILD)/test_dataflow
	$(BUILD)/test_dataflow_method
	$(BUILD)/test_dataflow_gcmove
	$(BUILD)/test_dataflow_helpers
	$(BUILD)/test_operands
ifeq ($(DF_HAVE_UNICORN),1)
	$(BUILD)/test_dataflow_emu
else
	@echo "# SKIP test_dataflow_emu: no libunicorn (make deps DEPS_ARGS=--emu)"
endif
	$(BUILD)/test_dataflow_ptrace
# F1 increment 1 (PTRACE_SINGLEBLOCK block-stepping, byte-identical to the single-step oracle)
# shipped its own lane but was left OUT of this aggregate, so it had no gate. Chained as a
# sub-make (like dataflow-grep-gate above) rather than repeated as another ifeq block: the
# dataflow-blockstep-test target ALREADY carries the DF_HAVE_UNICORN gate and its own clean
# SKIP, so calling it keeps that gate in ONE place instead of a copy here that can drift.
	$(MAKE) --no-print-directory dataflow-blockstep-test

# Phase 0 exit-criterion grep gate: the operand enumerator must hold ONE persistent
# csh, never a per-op cs_open in the hot path — so exactly one cs_open call site.
dataflow-grep-gate:
	@n=$$(grep -c 'cs_open(' src/dataflow_operands.c); \
	 if [ "$$n" -ne 1 ]; then \
	   echo "dataflow: expected exactly one cs_open (persistent handle), found $$n"; \
	   exit 1; \
	 fi; \
	 echo "dataflow: operand enumerator holds a persistent csh (1 cs_open) — OK"

# --- F1 increment 1: block-step + emulator-replay value tier ---------------
# src/dataflow_blockstep.c — a lower-perturbation scoped L0 producer: drive the region
# with PTRACE_SINGLEBLOCK (one stop per taken branch) + a full GETREGS at each boundary,
# and REPLAY each pure straight-line block through Unicorn to reconstruct the interior
# values, purity-gated (impure -> single-step fallback) with a coherence canary. Needs
# Linux x86-64 + Capstone (operand enumerator + purity scan) + Unicorn (replay); off any
# of those it compiles to an ENOSYS stub, so the object + explicit link rule are defined
# UNCONDITIONALLY (the explicit rule beats the root Makefile's generic test_% pattern,
# which would otherwise link the framework runtime against this standalone-main suite),
# with the Unicorn cflags/-D and libs toggled by DF_HAVE_UNICORN (set above). Ships no
# header — a value-trace PRODUCER is a tier; its suite re-declares the entry points.
DFB_UNICORN_FLAGS :=
DFB_LINK_LIBS     := $(CAPSTONE_LIBS)
ifeq ($(DF_HAVE_UNICORN),1)
DFB_UNICORN_FLAGS := $(UNICORN_CFLAGS) -DASMTEST_HAVE_UNICORN
DFB_LINK_LIBS     := $(UNICORN_LIBS) $(CAPSTONE_LIBS)
endif

$(BUILD)/dataflow_blockstep.o: src/dataflow_blockstep.c \
                               include/asmtest_valtrace.h include/asmtest_trace.h \
                               $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(DFB_UNICORN_FLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

$(BUILD)/test_dataflow_blockstep: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                                  $(BUILD)/dataflow_blockstep.o \
                                  $(BUILD)/test_dataflow_blockstep.o
	$(CC) $(CFLAGS) $^ $(DFB_LINK_LIBS) -o $@

# Dedicated lane: build + run where libunicorn is present, else a clean SKIP (matching the
# "optional tiers need libunicorn" convention). At runtime the suite ALSO self-skips where
# ptrace is blocked (seccomp/yama) or PTRACE_SINGLEBLOCK is non-functional.
.PHONY: dataflow-blockstep-test
ifeq ($(DF_HAVE_UNICORN),1)
dataflow-blockstep-test: $(BUILD)/test_dataflow_blockstep
	@echo "== dataflow-blockstep-test =="
	$(BUILD)/test_dataflow_blockstep
else
dataflow-blockstep-test:
	@echo "# SKIP dataflow-blockstep-test: no libunicorn (make deps DEPS_ARGS=--emu)"
endif
