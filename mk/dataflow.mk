# dataflow.mk — data-flow tracing tier (docs/internal/archive/plans/data-flow-tracing-plan.md).
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

# Phase 4 (increment 4): the pure OBJECT-identity transform over an L0 value
# trace. Same PURE-C tier as dataflow.o (no Capstone, no Unicorn) — runs
# everywhere; REFINES increment 2 by inverting its forward canon against a heap
# snapshot ({addr,size,type_id} nodes), so a record keys on (object, offset)
# where the snapshot has evidence and degrades to address identity where it does
# not. Links against dataflow_gcmove.o for that forward canon.
$(BUILD)/dataflow_objid.o: src/dataflow_objid.c include/asmtest_valtrace.h \
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

# --- Phase 6: the data-flow shared lib (libasmtest_dataflow) -----------------
# The L0 value sink + L1 def-use + L2 slice + method identity + GC-move
# canonicalization + runtime-helper summaries — the pure analysis pipeline, the
# packaging target the language bindings (Phase 6) dlopen.
# Links Capstone (the operand enumerator dataflow_operands uses in detail mode).
#
# F7 (live-attach-dataflow-followup-plan.md) ADDS the scoped ptrace L0 producer
# (dataflow_ptrace.o + the codeimage.o byte source it calls) to this lib. That
# reverses this rule's original "PRODUCERS are separate tiers and are NOT bundled
# here" line, deliberately and for one reason: a binding can only wrap what it can
# dlopen. F7's exit criterion is the language bindings capturing data flow over an
# ATTACHED PID, and the attach entry points live in the producer, so a lib without
# it leaves nothing to wrap. The three arguments the original split rested on do
# not survive the move:
#   - "it would not build everywhere" — it does. Off Linux x86-64 / without
#     Capstone, src/dataflow_ptrace.c compiles to ENOSYS stubs by its own #if, so
#     the lib links on every host and the bindings' live_attach_available() reports
#     the truth instead of failing to load.
#   - "it drags in Capstone" — this lib ALREADY links Capstone for the operand
#     enumerator, so the producer adds no new dependency tier.
#   - "it drags in libbpf" — codeimage.o's libbpf use is already optional and
#     $(LINK_LIBBPF)-gated; libasmtest_hwtrace bundles the same pic/codeimage.o on
#     the same terms.
# The pure/impure SPLIT still exists where it earns its keep — at the OBJECT level
# (dataflow.o has no Capstone), which is what lets the pure suites run everywhere.
# It was never load-bearing at the .so level. The emulator producer stays out: it
# needs libunicorn, a genuinely absent-by-default dependency, and no binding wraps
# it.
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
# F7: the scoped ptrace L0 PRODUCER, PIC — the live-attach entry points the
# bindings wrap (asmtest_dataflow_ptrace_attach_pid / _pid_tid / _jit). Same
# cflags as the static $(BUILD)/dataflow_ptrace.o above; without Capstone / off
# Linux x86-64 the file's own #if compiles it to ENOSYS stubs, so this object
# exists on every host and the lib always links.
$(BUILD)/pic/dataflow_ptrace.o: src/dataflow_ptrace.c include/asmtest_valtrace.h \
                                include/asmtest_codeimage.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -fPIC -c $< -o $@

# pic/codeimage.o is native-trace.mk's (included before this file); the producer
# calls asmtest_codeimage_bytes_at for its versioned decode, so the lib needs it.
DATAFLOW_SHLIB_OBJS := $(BUILD)/pic/dataflow.o $(BUILD)/pic/dataflow_operands.o \
                       $(BUILD)/pic/dataflow_method.o $(BUILD)/pic/dataflow_gcmove.o \
                       $(BUILD)/pic/dataflow_helpers.o \
                       $(BUILD)/pic/dataflow_ptrace.o $(BUILD)/pic/codeimage.o

.PHONY: shared-dataflow dataflow-python-test
shared-dataflow: $(call shlib_dev,libasmtest_dataflow)
$(call shlib_real,libasmtest_dataflow): $(DATAFLOW_SHLIB_OBJS)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_dataflow) $^ \
	  $(CAPSTONE_LIBS) $(LINK_LIBBPF) -o $@
$(call shlib_dev,libasmtest_dataflow): $(call shlib_real,libasmtest_dataflow)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_dataflow)
	ln -sf $(notdir $(call shlib_compat,libasmtest_dataflow)) $@

# --- F7: the shared live-attach victim -------------------------------------
# The process every binding's live-attach test ATTACHES to (bindings/dataflow_victim.c
# — see its header for the stdout base= handshake and the survival counter). ONE
# fixture for all ten lanes: they must differ in the FFI under test and nothing else.
# Plain CFLAGS — no Capstone, no libbpf; it is a victim, not a tracer.
$(BUILD)/dataflow_victim: bindings/dataflow_victim.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# What every dataflow-<lang>-test lane exports: the lib to dlopen + the victim to
# spawn. Defined once so a lane cannot drift into pointing at a stale copy of either.
DATAFLOW_LIVE_DEPS := shared-dataflow $(BUILD)/dataflow_victim
dataflow_live_env = ASMTEST_DATAFLOW_LIB=$(abspath $(call shlib_dev,libasmtest_dataflow)) \
                    ASMTEST_DATAFLOW_VICTIM=$(abspath $(BUILD)/dataflow_victim)

# Phase 6 — the Python data-flow binding (bindings/python/asmtest/dataflow.py).
# Runs the standalone TAP reporter (no pytest dependency) against the freshly
# built analysis lib, so it validates the ctypes wrapper on any host.
dataflow-python-test: $(DATAFLOW_LIVE_DEPS)
	$(dataflow_live_env) python3 bindings/python/tests/test_dataflow.py

# Phase 6 — the C++ data-flow binding (bindings/cpp/asmtest_dataflow.hpp): a
# header-only typed wrapper. Unlike the nine dlopen bindings this one LINKS the
# objects it calls, so F7's live attach adds dataflow_ptrace.o (the producer) and
# codeimage.o (the versioned-decode byte source it calls) + $(LINK_LIBBPF) here,
# rather than riding the shlib. It still needs the victim to attach to.
.PHONY: dataflow-cpp-test
dataflow-cpp-test: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                   $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
                   $(BUILD)/dataflow_ptrace.o $(BUILD)/codeimage.o \
                   $(BUILD)/dataflow_victim \
                   bindings/cpp/asmtest_dataflow.hpp bindings/cpp/test_dataflow.cpp | $(BUILD)
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_dataflow.cpp \
	  $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
	  $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
	  $(BUILD)/dataflow_ptrace.o $(BUILD)/codeimage.o \
	  $(CAPSTONE_LIBS) $(LINK_LIBBPF) \
	  -o $(BUILD)/test_dataflow_cpp
	$(dataflow_live_env) $(BUILD)/test_dataflow_cpp

# Phase 6 — the Node data-flow binding (bindings/node/dataflow.js, koffi). Self-skips
# (available()===false) when the analysis lib isn't built, so it never reddens a general
# node run; this dedicated target builds the lib + points the loader at it.
.PHONY: dataflow-node-test
dataflow-node-test: $(DATAFLOW_LIVE_DEPS)
	cd bindings/node && $(dataflow_live_env) $(NODE) test_dataflow.js

# Phase 6 — the Ruby data-flow binding (bindings/ruby/dataflow.rb, Fiddle). Needs a
# Ruby interpreter (the docker bindings image); validated in ubuntu:24.04 locally.
.PHONY: dataflow-ruby-test
dataflow-ruby-test: $(DATAFLOW_LIVE_DEPS)
	cd bindings/ruby && $(dataflow_live_env) $(RUBY) test_dataflow.rb

# Phase 6 — the Lua data-flow binding (bindings/lua/dataflow.lua, LuaJIT FFI). Needs
# LuaJIT (the docker bindings image); validated in ubuntu:24.04 locally.
.PHONY: dataflow-lua-test
dataflow-lua-test: $(DATAFLOW_LIVE_DEPS)
	cd bindings/lua && $(dataflow_live_env) $(LUAJIT) test_dataflow.lua

# Phase 6 — the Zig data-flow binding (bindings/zig/src/dataflow via std.DynLib). Needs
# zig 0.13.x (the docker bindings image). `-lc` selects the libc dlopen backend (zig's
# own ELF loader wants a hash table the plain shared lib omits — ElfHashTableNotFound).
.PHONY: dataflow-zig-test
dataflow-zig-test: $(DATAFLOW_LIVE_DEPS)
	$(dataflow_live_env) $(ZIG) run -lc -I$(abspath include) bindings/zig/src/test_dataflow.zig

# Phase 6 — the Rust data-flow binding (bindings/rust/test_dataflow.rs, direct FFI).
# Standalone rustc smoke (no cargo project) linked against the analysis lib; needs
# rustc (the docker bindings image).
RUSTC ?= rustc
.PHONY: dataflow-rust-test
dataflow-rust-test: $(DATAFLOW_LIVE_DEPS)
	$(RUSTC) bindings/rust/test_dataflow.rs -L $(BUILD) -l asmtest_dataflow \
	  -o $(BUILD)/rust_dataflow_test
	LD_LIBRARY_PATH=$(abspath $(BUILD)) $(dataflow_live_env) $(BUILD)/rust_dataflow_test

# Phase 6 — the Go data-flow binding (bindings/go/cmd/dataflowsmoke, cgo dlopen).
# Needs Go + a C toolchain (cgo); validated in golang:1 locally.
.PHONY: dataflow-go-test
dataflow-go-test: $(DATAFLOW_LIVE_DEPS)
	cd bindings/go && $(dataflow_live_env) \
	  GOFLAGS=-mod=mod $(GO) run ./cmd/dataflowsmoke

# Phase 6 — the Java data-flow binding (bindings/java/TestDataflow.java, Project Panama
# FFM). Needs JDK 22+ (the docker bindings image uses openjdk-25); validated in JDK 23.
JAVA ?= java
.PHONY: dataflow-java-test
dataflow-java-test: $(DATAFLOW_LIVE_DEPS)
	$(JAVAC) --release 22 -d $(BUILD)/java-df bindings/java/TestDataflow.java
	$(dataflow_live_env) \
	  $(JAVA) --enable-native-access=ALL-UNNAMED -cp $(BUILD)/java-df TestDataflow

# Phase 6 — the .NET data-flow binding (bindings/dotnet/dataflow_smoke, P/Invoke).
# Needs the .NET SDK (the docker bindings image); validated in dotnet/sdk:8.0.
.PHONY: dataflow-dotnet-test
dataflow-dotnet-test: $(DATAFLOW_LIVE_DEPS)
	cd bindings/dotnet/dataflow_smoke && $(dataflow_live_env) $(DOTNET) run -c Release

# All-bindings convenience target (each self-selects its interpreter/compiler).
.PHONY: dataflow-bindings-test
dataflow-bindings-test: dataflow-python-test dataflow-cpp-test dataflow-node-test \
                        dataflow-ruby-test dataflow-lua-test dataflow-zig-test \
                        dataflow-rust-test dataflow-go-test dataflow-java-test \
                        dataflow-dotnet-test
	@echo "dataflow-bindings-test: all 10 language bindings passed"

# --- F7: the DOCKER lanes for the ten binding tests ------------------------
# `make dataflow-<lang>-test` needs that language's toolchain, which the host has
# for at most a couple of them — so each lane gets a pinned-toolchain image, the
# same way every other binding lane in this repo does. These reuse
# bindings/Dockerfile.lang and docker.mk's per-language knobs (DOCKER_APT_<lang> /
# DOCKER_SETUP_<lang> / DOCKER_RUNENV_<lang>, all defined before this file is
# included) — one Dockerfile, no toolchain drift from the docker-<lang> images.
#
# They tag asmtest-dataflow-<lang>, NOT asmtest-<lang>: the latter is the shared
# per-language image several other lanes build FROM, and a lane must not repoint a
# tag its neighbours depend on.
#
# --cap-add=SYS_PTRACE: F7's whole subject is attaching to a live pid. Measured, not
# assumed — the lanes also pass under a PLAIN `docker run` with no caps and no
# seccomp override (verified in the exact shape ci.yml's dataflow-bindings matrix
# uses: `docker run --rm -v $PWD:/w -w /w ubuntu:24.04 ... make dataflow-ruby-test`
# -> 36/36). They can, because the victim is the test's own child (same uid, a
# descendant) AND calls PR_SET_PTRACER_ANY, and docker's default seccomp profile has
# allowed ptrace since 19.03. So the CI lanes, which cannot pass extra flags, keep
# working. The cap is added HERE anyway so this lane does not silently depend on all
# of that staying true: the failure it prevents is ETRACE, which these suites treat
# as a hard FAILURE rather than a skip, so a lane that lost ptrace goes red — never
# quietly green.
DATAFLOW_DOCKER_LANGS := python cpp node ruby lua zig rust go java dotnet

.PHONY: docker-dataflow-bindings $(addprefix docker-dataflow-,$(DATAFLOW_DOCKER_LANGS))

define dataflow_docker_lang_rule
docker-dataflow-$(1): docker-bindings-base
	$$(DOCKER) build $$(_docker_plat) -f bindings/Dockerfile.lang \
	  --build-arg BASE_IMAGE=$$(DOCKER_BINDINGS_BASE) \
	  --build-arg APT_PKGS='$$(DOCKER_APT_$(1))' \
	  --build-arg SETUP='$$(DOCKER_SETUP_$(1))' \
	  --build-arg TARGET=$(1) -t asmtest-dataflow-$(1) .
	$$(DOCKER) run --rm --cap-add=SYS_PTRACE $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) \
	  asmtest-dataflow-$(1) make dataflow-$(1)-test
endef
$(foreach L,$(DATAFLOW_DOCKER_LANGS),$(eval $(call dataflow_docker_lang_rule,$(L))))

docker-dataflow-bindings: $(addprefix docker-dataflow-,$(DATAFLOW_DOCKER_LANGS))
	@echo "docker-dataflow-bindings: all 10 language lanes passed in their pinned images"

# --- test-object compile knobs ---------------------------------------------
# The examples/%.c pattern rule (root Makefile) compiles these with plain CFLAGS;
# the Capstone/Unicorn suites need the extra include paths + the -DASMTEST_HAVE_CAPSTONE
# guard so their #ifdef'd assertions compile. test_dataflow.o needs nothing extra.
$(BUILD)/test_operands.o: CFLAGS += $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF)
$(BUILD)/test_dataflow_emu.o: CFLAGS += $(UNICORN_CFLAGS) $(CAPSTONE_CFLAGS) \
                                        $(CAPSTONE_DEF)
# F5 (PT + code-image + Unicorn replay): the suite drives everything through the producer's
# re-declared C entry points (no unicorn/capstone HEADERS, exactly like test_dataflow_emu), so it
# needs only the LIBIPT_DEF — so its `#ifdef ASMTEST_HAVE_LIBIPT` synthetic-fixture case compiles
# IN where libipt is present (the docker-dataflow-pt image) and self-skips where it is absent.
$(BUILD)/test_dataflow_pt.o: CFLAGS += $(LIBIPT_DEF) $(LIBIPT_CFLAGS)

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

# Phase 4 (increment 4) object-identity suite. PURE — links the value-trace sink
# + the increment-2 canonicalizer (objid REFINES its forward canon, so it links
# dataflow_gcmove.o) + the objid transform, no framework/Capstone/Unicorn. Like
# the increment-1/2/3 rules above, this EXPLICIT rule beats the root Makefile's
# generic test_% pattern (which would link the framework runtime + a same-named
# routine object it does not have), so the suite builds wherever it is requested
# — including before the root SUITE_EXCLUDES is updated to keep it out of `make
# test`.
$(BUILD)/test_dataflow_objid: $(BUILD)/dataflow.o $(BUILD)/dataflow_gcmove.o \
                              $(BUILD)/dataflow_objid.o \
                              $(BUILD)/test_dataflow_objid.o
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
               $(BUILD)/test_dataflow_gcmove $(BUILD)/test_dataflow_objid \
               $(BUILD)/test_dataflow_helpers \
               $(BUILD)/test_operands $(DF_EMU_SUITE) \
               $(BUILD)/test_dataflow_ptrace
	@echo "== dataflow-test =="
	$(MAKE) --no-print-directory dataflow-grep-gate
	$(BUILD)/test_dataflow
	$(BUILD)/test_dataflow_method
	$(BUILD)/test_dataflow_gcmove
	$(BUILD)/test_dataflow_objid
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
# F5 (dataflow-pt-replay-tier): the PT + code-image + Unicorn-replay value tier. Chained as a
# sub-make (like dataflow-blockstep-test above) so its DF_HAVE_UNICORN gate + clean SKIP live in
# ONE place. Its synthetic-AUX decode->replay bridge exercises fully in the libipt+Unicorn image
# (docker-dataflow-pt / docker-dataflow-attach, which now carry libipt-dev); where libipt is
# absent the T1/T3/T5 replay-core + equivalence cases still run (Unicorn only) and only the T2
# fixture-bridge case self-skips.
	$(MAKE) --no-print-directory dataflow-pt-test

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

# --- F5: PT + code-image + Unicorn-replay value tier -----------------------
# src/dataflow_pt.c (dataflow-pt-replay-tier) — the LEAST-perturbing L0 value producer: fully
# OUT OF BAND (no PTRACE_SINGLESTEP, no PTRACE_SINGLEBLOCK — zero stops of the target). Decode an
# Intel PT trace to the executed offset path, materialize the region bytes live at trace time from
# the code-image, and REPLAY that exact path through Unicorn to fill the SAME asmtest_valtrace_t
# the emulator L0 fills — matching it byte-for-byte on a deterministic region. Reuses the blockstep
# purity/replayability verdicts (T3, no second scanner). Needs Linux x86-64 + Capstone + Unicorn
# (the replay); the decode BRIDGE additionally needs libipt (its call site is ASMTEST_HAVE_LIBIPT-
# gated, so a Unicorn-only build still links). Off-platform it compiles to a DF_PT_ENOSYS stub, so
# the object + explicit link rule are UNCONDITIONAL (the explicit rule beats the root Makefile's
# generic test_% pattern, which would link the framework runtime against this standalone-main
# suite). Ships no header — a value-trace PRODUCER is a tier; its suite re-declares the entries.
#
# This tier opens NO perf event and adds NO PT capture code (doc-set position 9): the synthetic
# AUX comes from asmtest_pt_encode_fixture (pt_backend.o, libipt's own encoder — no PT PMU), and
# real foreign-pid capture is CONSUMED from intel-pt-attach-foreign-pid (now landed): the T4 live
# case links its asmtest_hwtrace_pt_attach_* entry (in HWTRACE_OBJS below) and runtime-self-skips
# off Intel PT silicon — F5 still adds no capture code of its own.
$(BUILD)/dataflow_pt.o: src/dataflow_pt.c include/asmtest_valtrace.h \
                        include/asmtest_trace.h include/asmtest_codeimage.h \
                        $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) $(DFB_UNICORN_FLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) \
	  $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -c $< -o $@

# The equivalence suite links: the F5 producer + the pure sink/L1/L2 (dataflow.o) + the operand
# enumerator (dataflow_operands.o) + the emulator L0 ORACLE it cross-checks against (dataflow_emu.o)
# + the block-step verdicts it reuses AND (T4) the force_singlestep single-step oracle
# (dataflow_blockstep.o) + HWTRACE_OBJS. HWTRACE_OBJS supplies the T4 live case's CONSUMED capture —
# asmtest_hwtrace_pt_attach_* (hwtrace.o) and its closure (pt_backend.o PT decode, codeimage.o byte
# source, trace.o sink, disasm.o, and the rest of the hwtrace backends the one PT arm drags in) — and
# already CONTAINS pt_backend.o + codeimage.o + trace.o, so those are NOT listed separately (a
# duplicate object on the link line is a multiple-definition error). dataflow_blockstep.o is NOT in
# HWTRACE_OBJS, so it stays explicit. HWTRACE_OBJS is defined in mk/native-trace.mk, included before
# this file. Libs: Unicorn + Capstone + libipt + OpenCSD (empty where absent) + the optional libbpf
# codeimage.o may reference + -ldl -lpthread the hwtrace backends need (mirrors the test_hwtrace link).
$(BUILD)/test_dataflow_pt: $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                           $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_blockstep.o \
                           $(BUILD)/dataflow_pt.o \
                           $(HWTRACE_OBJS) $(BUILD)/test_dataflow_pt.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) $(LIBIPT_LIBS) \
	  $(OPENCSD_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

# Dedicated lane: build + run where libunicorn is present (the replay engine), else a clean SKIP —
# the "optional tiers need libunicorn" convention. At runtime the synthetic decode->replay bridge
# runs wherever libipt is present (the docker lane); the LIVE foreign-pid half self-skips off
# bare-metal Intel PT silicon — its ONLY remaining gate now that the consumed intel-pt-attach-
# foreign-pid capture (asmtest_hwtrace_pt_attach_*) has landed and is linked in.
.PHONY: dataflow-pt-test
ifeq ($(DF_HAVE_UNICORN),1)
dataflow-pt-test: $(BUILD)/test_dataflow_pt
	@echo "== dataflow-pt-test =="
	$(BUILD)/test_dataflow_pt
else
dataflow-pt-test:
	@echo "# SKIP dataflow-pt-test: no libunicorn (make deps DEPS_ARGS=--emu)"
endif

# T4 fail-not-skip: the target a bare-metal Intel PT runner invokes. ASMTEST_REQUIRE_PT=1 turns the
# live-replay availability SKIP into a CHECK FAILURE, so a supposed-PT box whose intel_pt PMU is
# silently hidden goes RED instead of quietly green — exactly as intel-pt-whole-window-substrate#T5's
# hwtrace-pt-live does. Deliberately NOT chained into dataflow-test/aggregate: on every host without
# Intel PT silicon it FAILS by design (missing PMU), which is the whole point of a fail-not-skip
# target. The sibling capture dep is satisfied (intel-pt-attach-foreign-pid ☑5/5), so silicon is the
# only thing left that keeps it red.
.PHONY: dataflow-pt-live
ifeq ($(DF_HAVE_UNICORN),1)
dataflow-pt-live: $(BUILD)/test_dataflow_pt
	@echo "== dataflow-pt-live (ASMTEST_REQUIRE_PT=1: fail-not-skip) =="
	ASMTEST_REQUIRE_PT=1 $(BUILD)/test_dataflow_pt
else
dataflow-pt-live:
	@echo "# SKIP dataflow-pt-live: no libunicorn (make deps DEPS_ARGS=--emu)"
endif
