# asm-test build.
#
#   make test                    build and run the example suites (green)
#   make demo-fail               build and run the intentional-failure demo
#   make bench                   build and run the Phase 9 benchmark demo
#   make docs                    build the Sphinx/Read the Docs HTML docs
#   make clean                   remove build artifacts
#   make ASM_SYNTAX=nasm test    use the NASM (Intel-syntax) backend instead
#
# Default backend (gas): CC (clang on macOS, gcc on Linux) assembles the
# GAS-syntax .s sources through the C preprocessor (-x assembler-with-cpp) so
# they can #include "asm.h". Supports x86-64 and AArch64, Linux and macOS.
#
# NASM backend (ASM_SYNTAX=nasm): nasm assembles the Intel-syntax .asm sources.
# x86-64 only (NASM has no AArch64 target).

CC         ?= cc
# Pin the C standard for reproducible builds across gcc/clang versions and the
# four native targets. gnu11 (not c11) keeps the GNU extensions the runtime and
# the assembler-with-cpp shim rely on. Override with `make CSTD=c11 ...`.
CSTD       ?= gnu11
CFLAGS     := -std=$(CSTD) -Wall -Wextra -O0 -g -Iinclude
ASFLAGS    := -x assembler-with-cpp -Iinclude
BUILD      := build
ASM_SYNTAX ?= gas

# Treat warnings as errors. Off by default (a newer compiler can invent a new
# warning and break an otherwise-fine build); CI turns it on with WERROR=1 over
# the controlled toolchain so a fresh warning fails the build instead of slipping
# through. Flows through CFLAGS, so it covers both the C compile and the link.
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif

# Quality tooling (Track D). These flow through CFLAGS, which both the compile
# and link rules use (so the sanitizer/coverage runtimes link in); clang/gcc
# accept them as no-ops on the assembler step.
#   make SAN=1 ...   build with AddressSanitizer + UndefinedBehaviorSanitizer
#   make COV=1 ...   build with gcov/llvm-cov coverage instrumentation
# See the `sanitize`, `coverage`, and `tidy` convenience targets below.
ifeq ($(SAN),1)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
endif
ifeq ($(COV),1)
CFLAGS += --coverage
endif

# Framework runtime: C runner + the asm capture trampoline.
FRAMEWORK_OBJS := $(BUILD)/asmtest.o $(BUILD)/capture.o
SUITES         := $(BUILD)/test_arith $(BUILD)/test_mem $(BUILD)/test_capture \
                  $(BUILD)/test_fp $(BUILD)/test_simd $(BUILD)/test_args \
                  $(BUILD)/test_struct $(BUILD)/test_structparam \
                  $(BUILD)/test_fpover $(BUILD)/test_refmatch \
                  $(BUILD)/test_callback $(BUILD)/test_qmul \
                  $(BUILD)/test_checked $(BUILD)/test_qadd

.PHONY: all help test check demo-fail clean
.PHONY: lib install uninstall amalgamate pc
.PHONY: shared shared-emu manifest manifest-win64 install-shared install-shared-emu conformance conformance-asm
.PHONY: python-test cpp-test rust-test zig-test
.PHONY: ruby-test lua-test node-test java-test dotnet-test go-test
.PHONY: sanitize coverage tidy fmt fmt-check
.PHONY: deps usecases usecases-emu
.PHONY: drtrace-test drtrace-client shared-drtrace hwtrace-test shared-hwtrace
all: test

# Self-documenting target list. `make` / `make all` still runs the test suites;
# `make help` prints the common targets grouped by area (a curated summary — see
# the section banners below for the full set and the knobs each accepts).
help:
	@echo 'asm-test — make targets (default: test). Knobs: ASM_SYNTAX=nasm, SAN=1, COV=1, WERROR=1, CSTD=c11, PREFIX=...'
	@echo ''
	@echo 'Core:'
	@echo '  test            build + run the example suites (default)'
	@echo '  check           run the framework self-tests (tests/expect.sh)'
	@echo '  usecases        "unusual use case" suites (bit tricks, RPN VM)'
	@echo '  demo-fail       intentional-failure demo (exits nonzero by design)'
	@echo '  demo-robust     crash/hang containment demo'
	@echo '  bench           run the benchmark (BENCH) cases'
	@echo '  clean           remove build artifacts'
	@echo ''
	@echo 'Optional tiers (need libunicorn / libkeystone):'
	@echo '  emu-test        Unicorn-backed emulator suite'
	@echo '  asm-test        in-line assembler (Keystone) suite'
	@echo '  usecases-emu    emulator-as-sandbox / cross-ISA suite'
	@echo ''
	@echo 'Native runtime trace tiers (optional, Linux x86-64; self-skip if absent):'
	@echo '  drtrace-test    in-process DynamoRIO native trace (set DYNAMORIO_HOME)'
	@echo '  drtrace-bindings-test  per-language DynamoRIO wrapper tests (all bindings)'
	@echo '  hwtrace-test    hardware trace: single-step (any x86-64 Linux) / PT / AMD LBR'
	@echo '  hwtrace-bindings-test  per-language hardware-trace wrapper tests (all bindings)'
	@echo ''
	@echo 'Packaging & install:'
	@echo '  lib             build the static libasmtest.a'
	@echo '  shared          build the core shared lib'
	@echo '  shared-emu      build the emulator shared lib'
	@echo '  manifest        emit asmtest_abi.json (ABI layout)'
	@echo '  amalgamate      generate the single-header asmtest_single.h'
	@echo '  install         install headers + static lib + pkg-config'
	@echo '  deps            bootstrap the optional toolchain (DEPS_ARGS=...)'
	@echo '  packages        build every language package (needs all toolchains)'
	@echo '  package-libs    stage the host shared libs into build/dist/native/<plat>'
	@echo '  package-libs-verify  check a collected native tree has both libs per platform'
	@echo '  sync-version    write VERSION into every binding manifest'
	@echo '  check-version   verify every manifest matches VERSION (CI)'
	@echo ''
	@echo 'Quality (Track D/E):'
	@echo '  sanitize        build + run under ASan + UBSan'
	@echo '  coverage        gcov of the runner'
	@echo '  tidy            clang-tidy static analysis'
	@echo '  fmt             reformat the C sources with clang-format (in place)'
	@echo '  fmt-check       report clang-format drift (informational; no gate)'
	@echo '  valgrind        memcheck the routines under test (Linux/x86-64)'
	@echo ''
	@echo 'Language bindings (per-language; need libunicorn):'
	@echo '  python-test cpp-test rust-test zig-test node-test'
	@echo '  java-test dotnet-test ruby-test lua-test go-test'
	@echo '  conformance     regenerate the cross-language corpus.json'
	@echo ''
	@echo 'Docker (Linux CI lanes):'
	@echo '  docker-test docker-ci      example/self-test matrix in a container'
	@echo '  docker-bindings            build + run every language image'
	@echo '  docker-<lang>              one language image (e.g. docker-rust)'
	@echo '  docker-drtrace             DynamoRIO native-trace tier (C + Python) in a container'
	@echo '  docker-drtrace-bindings    DynamoRIO native-trace wrapper tests for every language'
	@echo ''
	@echo 'Native Win64 (cross-compile + Wine):'
	@echo '  win64-check     substrate smoke + capture + runner-port slices'
	@echo '  win64-msabi-test  fast native lane (no Wine; x86-64)'
	@echo ''
	@echo 'Docs (Sphinx):'
	@echo '  docs docs-serve docs-linkcheck docs-clean'

# "Unusual use case" demo suites (Track F): self-contained examples that show
# off a framework feature on a less-obvious target — property-tested bit hacks,
# a stateful RPN bytecode interpreter, and the emulator used as a security
# sandbox / cross-ISA equivalence checker. Kept out of the default SUITES (and
# thus the CI matrix) so they are opt-in via `make usecases` / `usecases-emu`.
USECASE_SUITES := $(BUILD)/test_bittricks $(BUILD)/test_vm

# Framework self-tests (Track A): the meta-suites driven by tests/expect.sh.
SELFTESTS := $(BUILD)/tests_positive $(BUILD)/tests_negative

# --- Packaging & installation (Track B) ------------------------------------
# `make lib`        build libasmtest.a (framework runtime + capture trampoline)
# `make install`    install headers + lib + pkg-config (honors PREFIX/DESTDIR)
# `make amalgamate` generate the single-header asmtest_single.h
AR              ?= ar
PREFIX          ?= /usr/local
DESTDIR         ?=
# Single source of truth: the VERSION file at the repo root. `make sync-version`
# propagates it into every binding manifest; `make check-version` verifies they
# match (run in CI). See scripts/sync-version.sh.
ASMTEST_VERSION := $(strip $(shell cat VERSION))
ASMTEST_VER_MAJOR := $(word 1,$(subst ., ,$(ASMTEST_VERSION)))

.PHONY: print-version sync-version check-version
print-version:
	@echo $(ASMTEST_VERSION)
sync-version:
	@scripts/sync-version.sh
check-version:
	@scripts/sync-version.sh --check

incdir := $(DESTDIR)$(PREFIX)/include/asmtest
libdir := $(DESTDIR)$(PREFIX)/lib
pcdir  := $(DESTDIR)$(PREFIX)/lib/pkgconfig
pc_subst := sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(ASMTEST_VERSION)|g'

# --- Shared-library naming (Track 0: multi-language bindings substrate) ------
# Platform-correct versioned filenames, soname/install-name, and the dev
# symlink, parameterised by library stem so the core and emulator libs share one
# definition. $(call shlib_real,libasmtest) is the real linker output;
# shlib_soname is the embedded name; shlib_dev is the unversioned dev symlink.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
shlib_real    = $(BUILD)/$(1).$(ASMTEST_VERSION).dylib
shlib_soname  = $(1).$(ASMTEST_VER_MAJOR).dylib
shlib_compat  = $(BUILD)/$(1).$(ASMTEST_VER_MAJOR).dylib
shlib_dev     = $(BUILD)/$(1).dylib
shlib_ldflags = -dynamiclib -install_name $(libdir)/$(call shlib_soname,$(1)) \
                -current_version $(ASMTEST_VERSION) \
                -compatibility_version $(ASMTEST_VER_MAJOR)
else
shlib_real    = $(BUILD)/$(1).so.$(ASMTEST_VERSION)
shlib_soname  = $(1).so.$(ASMTEST_VER_MAJOR)
shlib_compat  = $(BUILD)/$(1).so.$(ASMTEST_VER_MAJOR)
shlib_dev     = $(BUILD)/$(1).so
shlib_ldflags = -shared -Wl,-soname,$(call shlib_soname,$(1))
endif

$(BUILD):
	mkdir -p $(BUILD)

# C objects (backend-independent).
$(BUILD)/asmtest.o: src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: examples/%.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Framework self-tests (Track A) live in tests/; same compile, different dir.
$(BUILD)/%.o: tests/%.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

ifeq ($(ASM_SYNTAX),nasm)
# --- NASM backend (x86-64 only) -------------------------------------------
NASM ?= nasm
ifeq ($(shell uname -s),Darwin)
NASMFLAGS := -f macho64 -Iinclude/
else
NASMFLAGS := -f elf64 -Iinclude/
endif

$(BUILD)/capture.o: src/capture.asm include/asm_nasm.inc | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/%.o: examples/%.asm include/asm_nasm.inc | $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@
else
# --- GAS backend (default; x86-64 and AArch64) ----------------------------
$(BUILD)/capture.o: src/capture.s include/asm.h | $(BUILD)
	$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@

$(BUILD)/%.o: examples/%.s include/asm.h | $(BUILD)
	$(CC) $(CFLAGS) $(ASFLAGS) -c $< -o $@
endif

# One test binary per suite: framework + routine(s) + test cases.
$(BUILD)/test_arith: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/test_arith.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_mem: $(FRAMEWORK_OBJS) $(BUILD)/mem.o $(BUILD)/test_mem.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_capture: $(FRAMEWORK_OBJS) $(BUILD)/flags.o $(BUILD)/test_capture.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_fp: $(FRAMEWORK_OBJS) $(BUILD)/fp.o $(BUILD)/test_fp.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_simd: $(FRAMEWORK_OBJS) $(BUILD)/simd.o $(BUILD)/test_simd.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_args: $(FRAMEWORK_OBJS) $(BUILD)/args.o $(BUILD)/test_args.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_struct: $(FRAMEWORK_OBJS) $(BUILD)/structs.o $(BUILD)/test_struct.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_structparam: $(FRAMEWORK_OBJS) $(BUILD)/structparam.o \
                           $(BUILD)/test_structparam.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_fpover: $(FRAMEWORK_OBJS) $(BUILD)/fpover.o $(BUILD)/test_fpover.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_refmatch: $(FRAMEWORK_OBJS) $(BUILD)/refmatch.o \
                        $(BUILD)/test_refmatch.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_callback: $(FRAMEWORK_OBJS) $(BUILD)/callback.o \
                        $(BUILD)/test_callback.o
	$(CC) $(CFLAGS) $^ -o $@

# Real-world kernels for the by-audience examples (see docs/examples.md):
# a DSP Q15 multiply, a runtime checked-add (overflow flag), and a codec
# per-byte saturating add. Portable GAS sources with AArch64 bodies, plus NASM
# counterparts for the x86-64 Intel-syntax lane.
$(BUILD)/test_qmul: $(FRAMEWORK_OBJS) $(BUILD)/qmul.o $(BUILD)/test_qmul.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_checked: $(FRAMEWORK_OBJS) $(BUILD)/checked.o $(BUILD)/test_checked.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_qadd: $(FRAMEWORK_OBJS) $(BUILD)/qadd.o $(BUILD)/test_qadd.o
	$(CC) $(CFLAGS) $^ -o $@

test: $(SUITES)
	@set -e; for t in $(SUITES); do echo "== $$t =="; ./$$t; done

# --- "Unusual use case" suites (Track F) -----------------------------------
# Property-tested branchless bit hacks and a stateful RPN bytecode interpreter.
# Both build on either backend (GAS or NASM, x86-64); the GAS sources also carry
# AArch64 bodies. Run with `make usecases` (or `make ASM_SYNTAX=nasm usecases`).
$(BUILD)/test_bittricks: $(FRAMEWORK_OBJS) $(BUILD)/bittricks.o \
                         $(BUILD)/test_bittricks.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_vm: $(FRAMEWORK_OBJS) $(BUILD)/vm.o $(BUILD)/test_vm.o
	$(CC) $(CFLAGS) $^ -o $@

usecases: $(USECASE_SUITES)
	@set -e; for t in $(USECASE_SUITES); do echo "== $$t =="; ./$$t; done

# Framework self-tests (Track A). The meta-suites are pure C (register/flag/
# vector cases build a regs_t by hand), linked against the framework runtime;
# tests/expect.sh drives them and checks exit codes + diagnostics.
$(BUILD)/tests_positive: $(FRAMEWORK_OBJS) $(BUILD)/positive.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/tests_negative: $(FRAMEWORK_OBJS) $(BUILD)/negative.o
	$(CC) $(CFLAGS) $^ -o $@

check: $(SELFTESTS)
	@BUILD=$(BUILD) sh tests/expect.sh

# Static library: the framework runtime + the asm capture trampoline. The
# optional emulator tier (emu.o, -lunicorn) is intentionally left out.
$(BUILD)/libasmtest.a: $(FRAMEWORK_OBJS)
	$(AR) rcs $@ $^

lib: $(BUILD)/libasmtest.a

# --- Shared libraries + ABI manifest (Track 0) -----------------------------
# Loadable artifacts for FFI bindings (Python/Rust/...) that dlopen()/dlsym()
# the framework. The static libasmtest.a (Track B) stays the primary C path;
# these add the position-independent shared objects bindings consume. Build
# requires nothing the static path doesn't (shared-emu also needs libunicorn).
#   make shared      libasmtest.{so,dylib} (+ versioned name + dev symlink)
#   make shared-emu  libasmtest_emu.{so,dylib} (adds emu.o, links -lunicorn)
#   make manifest    asmtest_abi.json — machine-readable struct layout for the
#                    active host arch (consumed by every binding's generator)
# The capture trampoline + runtime + the opaque-handle FFI helpers (ffi.o) + the
# engine-neutral trace substrate (trace.o: the trace allocate/report/coverage
# helpers + handle accessors, used by the dynamic-language bindings; no Unicorn
# dependency).
PIC_OBJS := $(BUILD)/pic/asmtest.o $(BUILD)/pic/capture.o $(BUILD)/pic/ffi.o \
            $(BUILD)/pic/trace.o

$(BUILD)/pic:
	mkdir -p $(BUILD)/pic

# Position-independent objects (separate tree so they never collide with the
# non-PIC objects the test/static-lib builds use).
$(BUILD)/pic/asmtest.o: src/asmtest.c include/asmtest.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/pic/ffi.o: src/ffi.c include/asmtest.h include/asmtest_emu.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Engine-neutral trace substrate (trace allocate/report/coverage + FFI handle
# accessors). No Unicorn/Capstone dependency; shared by the core lib, the
# emulator superset, and the optional native/hardware trace tiers.
$(BUILD)/pic/trace.o: src/trace.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/pic/emu.o: src/emu.c include/asmtest_emu.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -fPIC -c $< -o $@

# Coverage-guided fuzzing + mutation testing (Track E) in the emu shared lib, so
# bindings can reach it. No Unicorn include (calls emu_* from pic/emu.o).
$(BUILD)/pic/fuzz.o: src/fuzz.c include/asmtest.h include/asmtest_emu.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# Disassembly (Track C) for the emu shared lib (the superset). Gated on Capstone
# exactly like the test-binary disasm.o (degrades to offsets when absent);
# libasmtest_emu links it in, so disas_available() is true out of the box.
$(BUILD)/pic/disasm.o: src/disasm.c include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -fPIC -c $< -o $@

$(BUILD)/pic/assemble.o: src/assemble.c include/asmtest_assemble.h \
                         include/asmtest_emu.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(KEYSTONE_CFLAGS) -fPIC -c $< -o $@

ifeq ($(ASM_SYNTAX),nasm)
$(BUILD)/pic/capture.o: src/capture.asm include/asm_nasm.inc | $(BUILD)/pic
	$(NASM) $(NASMFLAGS) $< -o $@
$(BUILD)/pic/%.o: examples/%.asm include/asm_nasm.inc | $(BUILD)/pic
	$(NASM) $(NASMFLAGS) $< -o $@
else
$(BUILD)/pic/capture.o: src/capture.s include/asm.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(ASFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/%.o: examples/%.s include/asm.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(ASFLAGS) -fPIC -c $< -o $@
endif

# Core shared lib: real versioned file + soname/dev symlinks beside it.
shared: $(call shlib_dev,libasmtest)
$(call shlib_real,libasmtest): $(PIC_OBJS)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest) $^ -o $@
$(call shlib_dev,libasmtest): $(call shlib_real,libasmtest)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest)
	ln -sf $(notdir $(call shlib_compat,libasmtest)) $@

# Emulator shared lib: the SUPERSET — emulator (emu.o, -lunicorn) plus BOTH
# optional native tiers, the Keystone in-line assembler (assemble.o, -lkeystone)
# and the Capstone disassembler (disasm.o, -lcapstone). Every binding that links
# -lasmtest_emu or dlopens libasmtest_emu therefore gets all three tiers with no
# flag — asm_available() and disas_available() are true out of the box. ONE lib
# for everything optional, so binding asm/disas does not spawn a per-dependency
# lib matrix (emu, emu+asm, emu+disasm, …). Needs libunicorn + libkeystone +
# libcapstone at build time.
shared-emu: $(call shlib_dev,libasmtest_emu)
$(call shlib_real,libasmtest_emu): $(PIC_OBJS) $(BUILD)/pic/emu.o \
                                   $(BUILD)/pic/fuzz.o \
                                   $(BUILD)/pic/assemble.o \
                                   $(BUILD)/pic/disasm.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_emu) $^ \
	      $(UNICORN_LIBS) $(KEYSTONE_LIBS) $(CAPSTONE_LIBS) -o $@
$(call shlib_dev,libasmtest_emu): $(call shlib_real,libasmtest_emu)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_emu)
	ln -sf $(notdir $(call shlib_compat,libasmtest_emu)) $@

# Machine-readable layout manifest: a small program compiled against the real
# headers prints sizeof/offsetof for the host arch (see scripts/gen-manifest.c).
manifest: asmtest_abi.json
asmtest_abi.json: $(BUILD)/gen-manifest
	./$< > $@
	@echo "manifest: wrote $@ ($(UNAME_S))"
$(BUILD)/gen-manifest: scripts/gen-manifest.c include/asmtest.h \
                       include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

# Win64 variant of the manifest: same generator, built with -DASMTEST_ABI_WIN64
# so regs_t is the Microsoft x64 layout (the native Win64 tier). Runs on the host
# (it only prints offsetof/sizeof), emitting the layout bindings would mirror.
manifest-win64: asmtest_abi_win64.json
asmtest_abi_win64.json: $(BUILD)/gen-manifest-win64
	./$< > $@
	@echo "manifest-win64: wrote $@"
$(BUILD)/gen-manifest-win64: scripts/gen-manifest.c include/asmtest.h \
                             include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_ABI_WIN64 $< -o $@

# Generate a local pkg-config file (baked with the current PREFIX/VERSION).
pc: asmtest.pc
asmtest.pc: asmtest.pc.in
	$(pc_subst) $< > $@

# Single-header amalgamation (C surface only; see the file's banner).
amalgamate: asmtest_single.h
asmtest_single.h: scripts/amalgamate.sh include/asmtest.h src/asmtest.c
	sh scripts/amalgamate.sh > $@

# Install headers, the static lib, and a pkg-config file. The .pc is generated
# here (not via the asmtest.pc target) so it always reflects the PREFIX in use.
install: lib
	mkdir -p $(incdir) $(libdir) $(pcdir)
	cp include/asmtest.h include/asmtest_emu.h include/asmtest_trace.h \
	   include/asm.h include/asm_nasm.inc $(incdir)/
	cp $(BUILD)/libasmtest.a $(libdir)/
	$(pc_subst) asmtest.pc.in > $(pcdir)/asmtest.pc
	@echo "installed asmtest $(ASMTEST_VERSION) to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(incdir)/asmtest.h $(incdir)/asmtest_emu.h \
	      $(incdir)/asmtest_trace.h $(incdir)/asm.h $(incdir)/asm_nasm.inc
	-rmdir $(incdir) 2>/dev/null || true
	rm -f $(libdir)/libasmtest.a $(pcdir)/asmtest.pc
	rm -f $(libdir)/$(notdir $(call shlib_real,libasmtest)) \
	      $(libdir)/$(call shlib_soname,libasmtest) \
	      $(libdir)/$(notdir $(call shlib_dev,libasmtest))
	rm -f $(libdir)/$(notdir $(call shlib_real,libasmtest_emu)) \
	      $(libdir)/$(call shlib_soname,libasmtest_emu) \
	      $(libdir)/$(notdir $(call shlib_dev,libasmtest_emu)) \
	      $(pcdir)/asmtest-emu.pc $(incdir)/asmtest_abi.json

# Install the shared libs alongside libasmtest.a (Track 0). Kept separate from
# `make install` so the static-only install stays toolchain-light; run after it.
# Copies the real versioned file, recreates the soname + dev symlinks in
# $(libdir), and installs the JSON layout manifest next to the headers.
install-shared: shared manifest
	mkdir -p $(libdir) $(incdir)
	cp $(call shlib_real,libasmtest) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest)) \
	       $(libdir)/$(call shlib_soname,libasmtest)
	ln -sf $(call shlib_soname,libasmtest) $(libdir)/$(notdir $(call shlib_dev,libasmtest))
	cp asmtest_abi.json $(incdir)/
	@echo "installed shared libasmtest $(ASMTEST_VERSION) to $(libdir)"

# Install the emulator shared lib + its pkg-config. libasmtest_emu is the superset
# (emu + Keystone assembler + Capstone disassembler), so this needs libunicorn +
# libkeystone + libcapstone present — the C++/link bindings find it via the
# installed asmtest-emu.pc and get all tiers with no flag.
install-shared-emu: shared-emu
	mkdir -p $(libdir) $(pcdir)
	cp $(call shlib_real,libasmtest_emu) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest_emu)) \
	       $(libdir)/$(call shlib_soname,libasmtest_emu)
	ln -sf $(call shlib_soname,libasmtest_emu) \
	       $(libdir)/$(notdir $(call shlib_dev,libasmtest_emu))
	$(pc_subst) asmtest-emu.pc.in > $(pcdir)/asmtest-emu.pc
	@echo "installed shared libasmtest_emu $(ASMTEST_VERSION) to $(libdir)"

# --- Optional dependency bootstrap -----------------------------------------
# `make deps` installs the OPTIONAL toolchain (nasm, pkg-config, libunicorn,
# libkeystone, clang-tidy) via the system package manager — the core build needs
# none of it. scripts/install-deps.sh detects apt-get/dnf/yum/pacman/zypper/apk/
# brew. Pass DEPS_ARGS to select a subset, e.g. `make deps DEPS_ARGS=--emu`,
# `make deps DEPS_ARGS=--asm`, or preview with `make deps DEPS_ARGS=--dry-run`.
DEPS_ARGS ?=
deps:
	sh scripts/install-deps.sh $(DEPS_ARGS)

# Large, self-contained target groups split out by concern. Make reads each in
# place, so they share every variable/knob defined above; this is purely an
# organizational split (the expanded build is identical to one flat Makefile).
include mk/docker.mk        # Linux CI lanes in Docker
include mk/win64.mk         # native Win64 (cross-compile + Wine)
# --- Quality tooling targets (Track D) -------------------------------------
# Build + run the example suites and the self-tests under ASan + UBSan. The
# framework catches SIGSEGV/SIGBUS itself (crash containment), so tell ASan not
# to grab those signals; UBSan halts on the first violation. detect_leaks is
# left at its platform default (LSan on Linux; unsupported on macOS).
ASAN_RUN_OPTIONS ?= handle_segv=0:handle_sigbus=0:handle_sigfpe=0:abort_on_error=0
UBSAN_RUN_OPTIONS ?= halt_on_error=1:print_stacktrace=1
sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=$(ASAN_RUN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_RUN_OPTIONS) \
	    $(MAKE) SAN=1 test
	ASAN_OPTIONS=$(ASAN_RUN_OPTIONS) UBSAN_OPTIONS=$(UBSAN_RUN_OPTIONS) \
	    $(MAKE) SAN=1 check

# Coverage of the runner (src/asmtest.c). Forked children _exit() without
# flushing gcov, so drive the suites with --no-fork (one process, normal exit)
# across a few non-crashing invocations; .gcda accumulates across runs. Emits
# asmtest.c.gcov, surfaced as a CI artifact (informational, not a gate).
coverage:
	$(MAKE) clean
	$(MAKE) COV=1 $(BUILD)/tests_positive $(BUILD)/tests_negative \
	    $(BUILD)/test_fp $(BUILD)/test_simd $(BUILD)/test_refmatch \
	    $(BUILD)/test_bench
	-./$(BUILD)/tests_positive --no-fork >/dev/null 2>&1
	-./$(BUILD)/tests_positive --format=junit >/dev/null 2>&1
	-./$(BUILD)/tests_positive --shuffle --seed=1 >/dev/null 2>&1
	-./$(BUILD)/tests_negative --no-fork --filter='neg.eq' >/dev/null 2>&1
	-./$(BUILD)/tests_negative --no-fork --filter='neg.mem_eq' >/dev/null 2>&1
	-./$(BUILD)/tests_negative --format=junit --filter='neg.flag_set' >/dev/null 2>&1
	-./$(BUILD)/test_fp --no-fork >/dev/null 2>&1       # FP return + ULP paths
	-./$(BUILD)/test_simd --no-fork >/dev/null 2>&1     # vector capture/assert
	-./$(BUILD)/test_refmatch --no-fork >/dev/null 2>&1 # differential engine
	-./$(BUILD)/test_bench --bench >/dev/null 2>&1      # benchmark mode
	gcov -o $(BUILD) src/asmtest.c

# Static analysis over the runner with clang-tidy (checks curated in
# .clang-tidy). Informational by default — findings are warnings, not errors —
# so the job reports a baseline without gating; promote to gating later.
CLANG_TIDY ?= clang-tidy
tidy:
	$(CLANG_TIDY) src/asmtest.c -- $(CFLAGS)

# Formatting with clang-format (style in .clang-format). `fmt` rewrites in place;
# `fmt-check` reports drift and exits nonzero (so it CAN gate) but the CI job runs
# it informationally for now — the hand-tuned tree is not bulk-reformatted (see
# .clang-format). Scope: the C translation units + headers (the asm and C++/binding
# sources keep their own conventions and are left out). Run `make fmt` on new code.
CLANG_FORMAT ?= clang-format
FMT_SOURCES  := $(wildcard src/*.c src/*.h include/*.h tests/*.c \
                 tests/win64/*.c tests/win64/*.h bindings/conformance/*.c \
                 examples/*.c scripts/gen-manifest.c)
fmt:
	$(CLANG_FORMAT) -i $(FMT_SOURCES)

fmt-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FMT_SOURCES)

# --- Valgrind the routine under test (Track E) -----------------------------
# Run the example suites under Valgrind's memcheck to catch bugs in the ROUTINE
# UNDER TEST (bad loads/stores, uninitialized reads, leaks) — distinct from the
# `sanitize` target, which instruments the framework's own C. Tests run
# --no-fork so memcheck follows a single process to a clean exit; a real error
# fails the build via --error-exitcode. Linux/x86-64 (Valgrind isn't available
# on current macOS/arm64). The guard-page allocator (asmtest_guarded_alloc) is
# the complementary, always-on way to catch overruns without Valgrind.
VALGRIND      ?= valgrind
VALGRIND_OPTS ?= --leak-check=full --errors-for-leak-kinds=definite \
                 --error-exitcode=1 --quiet
.PHONY: valgrind
valgrind: $(SUITES) $(BUILD)/tests_positive
	@set -e; for t in $(SUITES) $(BUILD)/tests_positive; do \
	  echo "== valgrind $$t =="; \
	  $(VALGRIND) $(VALGRIND_OPTS) ./$$t --no-fork >/dev/null; \
	done
	@echo "valgrind: clean"

# Expected to exit nonzero; the leading '-' keeps make from erroring out.
$(BUILD)/test_failure_demo: $(FRAMEWORK_OBJS) $(BUILD)/flags.o $(BUILD)/fp.o \
                            $(BUILD)/refmatch.o $(BUILD)/test_failure_demo.o
	$(CC) $(CFLAGS) $^ -o $@

demo-fail: $(BUILD)/test_failure_demo
	-./$(BUILD)/test_failure_demo

# Phase 8 robustness demo: a hang and a crash are contained and reported, while
# the run continues. A short timeout catches the infinite loop quickly. Exits
# nonzero (two tests fail by design), so the leading '-' keeps make happy.
.PHONY: demo-robust
$(BUILD)/test_robust: $(FRAMEWORK_OBJS) $(BUILD)/robust.o $(BUILD)/test_robust.o
	$(CC) $(CFLAGS) $^ -o $@

demo-robust: $(BUILD)/test_robust
	-./$(BUILD)/test_robust --timeout=2

# Phase 9 benchmark demo: time the BENCH cases (cycles/call, min/median over
# repeated rounds). Auto-calibrates the inner repeat count per benchmark.
.PHONY: bench
$(BUILD)/test_bench: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/bench.o \
                     $(BUILD)/test_bench.o
	$(CC) $(CFLAGS) $^ -o $@

bench: $(BUILD)/test_bench
	./$(BUILD)/test_bench --bench

# --- Optional emulator tier (Phase 4; requires libunicorn) -----------------
# `make emu-test` runs the Unicorn-backed suite. The emulated guest is x86-64
# and the routine bytes are copied from the built routines, so build this on an
# x86-64 host (default GAS backend).
UNICORN_CFLAGS ?= $(shell pkg-config --cflags unicorn 2>/dev/null)
UNICORN_LIBS   ?= $(shell pkg-config --libs unicorn 2>/dev/null || echo -lunicorn)

# Optional disassembler (Capstone) for emulator diagnostics — the counterpart to
# the Keystone assembler. Unlike Keystone it ships as a distro/brew package, so
# it is AUTO-DETECTED here: when pkg-config finds it, disasm.o is built with
# -DASMTEST_HAVE_CAPSTONE and linked against it; when absent the same disasm.o
# compiles Capstone-free and the diagnostics degrade to bare byte offsets. It is
# kept out of emu.o / the shared libs (PIC_OBJS), so only a binary that wants
# disassembly pays the dependency — the core build stays Capstone-free.
CAPSTONE_CFLAGS ?= $(shell pkg-config --cflags capstone 2>/dev/null)
CAPSTONE_LIBS   ?= $(shell pkg-config --libs capstone 2>/dev/null)
ifeq ($(shell pkg-config --exists capstone 2>/dev/null && echo 1),1)
CAPSTONE_DEF := -DASMTEST_HAVE_CAPSTONE
endif

# Engine-neutral trace substrate (see src/trace.c). No Unicorn/Capstone
# dependency; linked by every binary that links emu.o or ffi.o.
$(BUILD)/trace.o: src/trace.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/emu.o: src/emu.c include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -c $< -o $@

# Disassembly helpers (emu_disas + the *_disasm reports). No Unicorn dependency;
# includes only asmtest_emu.h + (optionally) Capstone.
$(BUILD)/disasm.o: src/disasm.c include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@

# Coverage-guided fuzzing + mutation testing (Track E). Drives the emulator
# (emu_call/_traced) with the framework's RNG; no extra dependency, its own TU.
$(BUILD)/fuzz.o: src/fuzz.c include/asmtest.h include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The opaque-handle FFI accessor layer (regs/emu-result/trace handles + readers).
# Pulled into the conformance reference so it drives the exact binding-ABI surface
# a foreign binding uses. No Unicorn dependency, but built with its include path
# so the emu struct decls resolve identically to the shared-lib build.
$(BUILD)/ffi.o: src/ffi.c include/asmtest.h include/asmtest_emu.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -c $< -o $@

$(BUILD)/test_emu: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/mem.o \
                   $(BUILD)/flags.o $(BUILD)/branch.o $(BUILD)/emu.o \
                   $(BUILD)/trace.o $(BUILD)/disasm.o $(BUILD)/fuzz.o \
                   $(BUILD)/test_emu.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

.PHONY: emu-test
emu-test: $(BUILD)/test_emu
	./$(BUILD)/test_emu

# --- Optional in-line assembler (Phase: Keystone; requires libkeystone) -----
# `make asm-test` assembles routines from strings (asmtest_assemble) and runs
# them through the emulator tier (emu_call_asm), so it needs BOTH libkeystone
# and libunicorn. Released Keystone has no RISC-V backend; that guest reports a
# clean "unsupported" error rather than failing the build.
KEYSTONE_CFLAGS ?= $(shell pkg-config --cflags keystone 2>/dev/null)
KEYSTONE_LIBS   ?= $(shell pkg-config --libs keystone 2>/dev/null || echo -lkeystone)

$(BUILD)/assemble.o: src/assemble.c include/asmtest_assemble.h \
                     include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) $(KEYSTONE_CFLAGS) -c $< -o $@

$(BUILD)/test_asm: $(FRAMEWORK_OBJS) $(BUILD)/emu.o $(BUILD)/trace.o \
                   $(BUILD)/assemble.o $(BUILD)/test_asm.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) -o $@

.PHONY: asm-test
asm-test: $(BUILD)/test_asm
	./$(BUILD)/test_asm

# Emulator "unusual use case" suite (Track F): the virtual CPU as a security
# sandbox (precise over-read/over-write fault localization) and a cross-ISA
# equivalence checker (the same algorithm run on x86-64, AArch64, RISC-V, and
# ARM32 guests). GAS backend only, like emu-test; requires libunicorn.
$(BUILD)/test_emu_usecases: $(FRAMEWORK_OBJS) $(BUILD)/emucases.o \
                            $(BUILD)/emu.o $(BUILD)/trace.o \
                            $(BUILD)/test_emu_usecases.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) -o $@

usecases-emu: $(BUILD)/test_emu_usecases
	./$(BUILD)/test_emu_usecases

include mk/native-trace.mk  # DynamoRIO + hardware native-trace tiers
include mk/bindings.mk       # conformance corpus + language bindings + packaging
include mk/docs.mk           # Sphinx / Read the Docs

clean: docs-clean
	rm -rf $(BUILD)
	rm -f asmtest.pc asmtest-emu.pc asmtest_single.h asmtest_abi.json asmtest_abi_win64.json
	rm -f bindings/conformance/corpus.json
	rm -rf bindings/python/asmtest/_libs bindings/ruby/native bindings/lua/native \
	       bindings/node/native bindings/java/src/main/resources/native \
	       bindings/dotnet/runtimes
	rm -f bindings/ruby/*.gem
	# Coverage debris the gcov/llvm-cov tooling leaves in the tree (root + src).
	rm -f *.gcov *.gcno *.gcda src/*.gcov src/*.gcno src/*.gcda
