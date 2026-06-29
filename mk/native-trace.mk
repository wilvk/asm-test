# native-trace.mk — Native runtime-trace tiers: DynamoRIO (in-process) and hardware (Intel PT / CoreSight).
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

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

# App-side library object (lifecycle + markers + W^X exec memory). No DR headers
# or link dependency (it dlopen()s libdynamorio and declares dr_app_* via dlsym),
# so it always compiles regardless of whether DynamoRIO is installed.
$(BUILD)/drtrace_app.o: src/drtrace_app.c include/asmtest_drtrace.h \
                        include/asmtest_trace.h include/asmtest_assemble.h | $(BUILD)
	$(CC) $(CFLAGS) $(DRAPP_KS_DEF) $(KEYSTONE_CFLAGS) -c $< -o $@

# DynamoRIO client (.so) via CMake. Real target shape: shells out to cmake.
.PHONY: drtrace-client
drtrace-client: $(BUILD)/libasmtest_drclient.so
$(BUILD)/libasmtest_drclient.so: src/drtrace_client.c drclient/CMakeLists.txt | $(BUILD)
ifndef DR_AVAILABLE
	@echo "drtrace-client: DynamoRIO not found (set DYNAMORIO_HOME); skipping"
else
	@mkdir -p $(BUILD)/drclient
	cd $(BUILD)/drclient && cmake -DDynamoRIO_DIR=$(abspath $(DYNAMORIO_DIR)) \
	    -DASMTEST_BUILD_DIR=$(abspath $(BUILD)) $(abspath drclient) >/dev/null
	cmake --build $(BUILD)/drclient >/dev/null
	@echo "drtrace-client: built $@"
endif

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
                            include/asmtest_trace.h include/asmtest_assemble.h | $(BUILD)/pic
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
	    ./$(BUILD)/test_drtrace
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

$(BUILD)/hwtrace.o: src/hwtrace.c include/asmtest_hwtrace.h \
                    include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/pt_backend.o: src/pt_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -c $< -o $@
$(BUILD)/cs_backend.o: src/cs_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(OPENCSD_DEF) $(OPENCSD_CFLAGS) -c $< -o $@
# AMD branch-record reconstruction reuses the Capstone layer (disasm.o) via
# asmtest_disas for instruction lengths — no new decoder lib, just Capstone.
$(BUILD)/amd_backend.o: src/amd_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Single-step (EFLAGS.TF / SIGTRAP) backend: no external library at all, just the
# same Capstone length-decoder (disasm.o) for block normalization. Runs on ANY
# x86-64 Linux host with no PMU/perf/privilege — the universal hardware-tier path.
$(BUILD)/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

HWTRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o $(BUILD)/cs_backend.o \
                $(BUILD)/amd_backend.o $(BUILD)/ss_backend.o \
                $(BUILD)/disasm.o $(BUILD)/trace.o

$(BUILD)/test_hwtrace: $(HWTRACE_OBJS) $(BUILD)/test_hwtrace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) -o $@

.PHONY: hwtrace-test
hwtrace-test: $(BUILD)/test_hwtrace
	@echo "== hwtrace-test =="
	./$(BUILD)/test_hwtrace

# Python language-wrapper test for the native-trace tier (asmtest.drtrace). Builds
# the app lib + DR client, then runs the pytest suite with the lib paths wired up.
# Self-skips when DynamoRIO is absent. Runs on a dev box (DYNAMORIO_HOME=...) and
# is the body of the `make docker-drtrace` container lane.
.PHONY: drtrace-python-test
drtrace-python-test:
ifndef DR_AVAILABLE
	@echo "== drtrace-python-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
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

drtrace-cpp-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,cpp)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-cpp-test =="
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_drtrace.cpp -ldl -o $(BUILD)/test_drtrace_cpp
	$(drtrace_env) ./$(BUILD)/test_drtrace_cpp
endif

drtrace-rust-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,rust)
else
	@$(MAKE) shared-emu $(CORPUS_LIB) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-rust-test =="
	@# Run as an EXAMPLE binary (main thread), NOT `cargo test` (which runs the
	@# test on a worker thread, making the process multi-threaded when dr_app_start
	@# takes over — that crashes; a single-threaded main lets DR take over cleanly).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(drtrace_env) \
	  $(CARGO) run --quiet --example drtrace
endif

drtrace-go-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,go)
else
	@$(MAKE) shared-emu $(CORPUS_LIB) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-go-test =="
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" CGO_CFLAGS="-I$(abspath include)" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) $(drtrace_env) \
	  $(GO) test -run TestDrtrace ./...
endif

drtrace-node-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,node)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-node-test =="
	$(call drtrace_run,cd bindings/node && $(drtrace_env) $(NODE) test_drtrace.js)
endif

drtrace-java-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,java)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-java-test =="
	mkdir -p $(BUILD)/java-drtrace
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java-drtrace \
	  bindings/java/DrTrace.java bindings/java/DrTraceTest.java
	$(call drtrace_run,$(drtrace_env) $(JAVA) --enable-preview --enable-native-access=ALL-UNNAMED -cp $(BUILD)/java-drtrace DrTraceTest)
endif

drtrace-dotnet-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,dotnet)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-dotnet-test =="
	$(call drtrace_run,$(drtrace_env) $(DOTNET) run --project bindings/dotnet/drtrace/drtrace.csproj)
endif

drtrace-ruby-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,ruby)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-ruby-test =="
	cd bindings/ruby && $(drtrace_env) $(RUBY) test_drtrace.rb
endif

drtrace-lua-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,lua)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
	@echo "== drtrace-lua-test =="
	cd bindings/lua && $(drtrace_env) $(LUAJIT) test_drtrace.lua
endif

drtrace-zig-test:
ifndef DR_AVAILABLE
	$(call drtrace_skip,zig)
else
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
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
	$(hwtrace_env) ./$(BUILD)/test_hwtrace_cpp

# rust/go link libasmtest_emu (their wrapper shares the crate/package), so build
# the emu superset + corpus lib too — mirroring the drtrace-rust/go lanes.
hwtrace-rust-test: shared-hwtrace
	@$(MAKE) shared-emu $(CORPUS_LIB)
	@echo "== hwtrace-rust-test =="
	@# --test-threads=1: the single-step backend is process-global (single active
	@# region, a global SIGTRAP handler, per-thread EFLAGS.TF), so the two live
	@# single-step tests must run serially — concurrent stepping crashes (SIGTRAP).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(hwtrace_env) \
	  $(CARGO) test --test hwtrace -- --nocapture --test-threads=1

hwtrace-go-test: shared-hwtrace
	@$(MAKE) shared-emu $(CORPUS_LIB)
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
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java-hwtrace \
	  bindings/java/HwTrace.java bindings/java/HwTraceTest.java
	$(hwtrace_env) $(JAVA) --enable-preview --enable-native-access=ALL-UNNAMED \
	  -cp $(BUILD)/java-hwtrace HwTraceTest

hwtrace-dotnet-test: shared-hwtrace
	@echo "== hwtrace-dotnet-test =="
	$(hwtrace_env) $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj

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
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

shared-hwtrace: $(call shlib_dev,libasmtest_hwtrace)
$(call shlib_real,libasmtest_hwtrace): $(BUILD)/pic/hwtrace.o \
                                       $(BUILD)/pic/pt_backend.o \
                                       $(BUILD)/pic/cs_backend.o \
                                       $(BUILD)/pic/amd_backend.o \
                                       $(BUILD)/pic/ss_backend.o \
                                       $(BUILD)/pic/disasm.o \
                                       $(BUILD)/pic/trace.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_hwtrace) $^ \
	      $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) -o $@
$(call shlib_dev,libasmtest_hwtrace): $(call shlib_real,libasmtest_hwtrace)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_hwtrace)
	ln -sf $(notdir $(call shlib_compat,libasmtest_hwtrace)) $@

