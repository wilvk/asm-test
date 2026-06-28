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
CFLAGS     := -Wall -Wextra -O0 -g -Iinclude
ASFLAGS    := -x assembler-with-cpp -Iinclude
BUILD      := build
ASM_SYNTAX ?= gas

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
.PHONY: sanitize coverage tidy
.PHONY: deps usecases usecases-emu
.PHONY: drtrace-test drtrace-client shared-drtrace hwtrace-test shared-hwtrace
all: test

# Self-documenting target list. `make` / `make all` still runs the test suites;
# `make help` prints the common targets grouped by area (a curated summary — see
# the section banners below for the full set and the knobs each accepts).
help:
	@echo 'asm-test — make targets (default: test). Knobs: ASM_SYNTAX=nasm, SAN=1, COV=1, PREFIX=...'
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
	@echo '  hwtrace-test    hardware trace: Intel PT / ARM CoreSight (bare metal)'
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

# --- Run the Linux CI jobs locally via Docker ------------------------------
# Covers the Linux half of the matrix; the macOS jobs can't run in a container.
#   make docker-test       build + run the example suites and self-tests
#   make docker-nasm       the NASM backend (x86-64 only)
#   make docker-emu        the emulator tier (libunicorn)
#   make docker-asm        the in-line assembler tier (libkeystone + libunicorn)
#   make docker-valgrind   memcheck the routines under test
#   make docker-sanitize   ASan + UBSan
#   make docker-analyze    clang-tidy
#   make docker-coverage   gcov of the runner
#   make docker-ci         the whole x86-64 Linux matrix end to end
#   make docker-shell      interactive shell in the CI image
# Emulate the aarch64 runner with DOCKER_PLATFORM=linux/arm64; on arm64 CI only
# runs the test + emu jobs (NASM is x86-64 only), so use docker-test/docker-emu
# there rather than docker-ci.
DOCKER          ?= docker
DOCKER_IMAGE    ?= asmtest-ci
DOCKER_BASE     ?= ubuntu:24.04
DOCKER_PLATFORM ?=
_docker_plat := $(if $(DOCKER_PLATFORM),--platform $(DOCKER_PLATFORM),)
_docker_run  := $(DOCKER) run --rm $(_docker_plat) $(DOCKER_IMAGE)

.PHONY: docker-build docker-test docker-nasm docker-emu docker-asm \
        docker-valgrind docker-sanitize docker-analyze docker-coverage \
        docker-ci docker-shell docker-clean

docker-build:
	$(DOCKER) build $(_docker_plat) --build-arg BASE=$(DOCKER_BASE) -t $(DOCKER_IMAGE) .

docker-test: docker-build
	$(_docker_run) sh -c 'make test && make check'

docker-nasm: docker-build
	$(_docker_run) sh -c 'make ASM_SYNTAX=nasm test && make ASM_SYNTAX=nasm check'

docker-emu: docker-build
	$(_docker_run) make emu-test

docker-asm: docker-build
	$(_docker_run) make asm-test

docker-valgrind: docker-build
	$(_docker_run) make valgrind

docker-sanitize: docker-build
	$(_docker_run) make sanitize

docker-analyze: docker-build
	$(_docker_run) make tidy

docker-coverage: docker-build
	$(_docker_run) make coverage

# Mirror the full Linux matrix in one container, cleaning between phases so the
# GAS/NASM backends and the sanitizer build don't share stale objects.
docker-ci: docker-build
	$(_docker_run) sh -c 'set -e; \
	  make test && make check; \
	  make clean && make ASM_SYNTAX=nasm test && make ASM_SYNTAX=nasm check; \
	  make clean && make emu-test; \
	  make clean && make asm-test; \
	  make clean && make valgrind; \
	  make clean && make sanitize; \
	  make tidy'

docker-shell: docker-build
	$(DOCKER) run --rm -it $(_docker_plat) $(DOCKER_IMAGE) sh

docker-clean:
	-$(DOCKER) image rm $(DOCKER_IMAGE)

# --- Language wrappers in Docker (Tracks P/R/X/Z/N/J/D/C) ------------------
# Each language is tested in its OWN image for isolation (one generic
# bindings/Dockerfile.lang, FROM a shared C+libunicorn base) — toolchains never
# mix. A docker-<lang> target builds the base (once, cached), then the small
# per-language image, then runs it (its CMD is `make <lang>-test`).
#   make docker-bindings   build + run every language's image
#   make docker-python / -cpp / -rust / -zig / -node / -java / -dotnet /
#        -ruby / -lua / -go    just that language
# Emulate aarch64 with DOCKER_PLATFORM=linux/arm64.
DOCKER_BINDINGS_BASE ?= asmtest-bindings-base
BINDING_LANGS := python cpp rust zig node java dotnet ruby lua go
# Bindings that ship a DynamoRIO native-trace wrapper test (Python has its own
# drtrace-python-test lane). Defined here so both the docker-drtrace-<lang> rules
# below and the drtrace-<lang>-test rules further down can see it.
DRTRACE_BINDING_LANGS := cpp rust go node java dotnet ruby lua zig

# Per-language knobs for the generic image (bindings/Dockerfile.lang):
#   DOCKER_APT_<lang>    extra distro packages (C++ is header-only -> none)
#   DOCKER_SETUP_<lang>  extra build-time shell (npm global, Zig tarball fetch)
#   DOCKER_RUNENV_<lang> runtime-only env, passed to `docker run` as -e flags
DOCKER_APT_python := python3 python3-pytest
DOCKER_APT_cpp    :=
DOCKER_APT_rust   := cargo rustc
DOCKER_APT_zig    :=
DOCKER_APT_node   := nodejs npm
DOCKER_APT_java   := openjdk-21-jdk-headless
DOCKER_APT_dotnet := dotnet-sdk-8.0
DOCKER_APT_ruby   := ruby
DOCKER_APT_lua    := luajit
DOCKER_APT_go     := golang-go

ZIG_VERSION ?= 0.13.0
DOCKER_SETUP_node := npm install -g koffi
DOCKER_SETUP_zig  := arch="$$(uname -m)"; \
  curl -fsSL "https://ziglang.org/download/$(ZIG_VERSION)/zig-linux-$$arch-$(ZIG_VERSION).tar.xz" -o /tmp/zig.tar.xz; \
  mkdir -p /opt/zig; tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1; \
  rm /tmp/zig.tar.xz; ln -s /opt/zig/zig /usr/local/bin/zig; zig version

DOCKER_RUNENV_node   := -e NODE_PATH=/usr/local/lib/node_modules:/usr/lib/node_modules
DOCKER_RUNENV_go     := -e GOTOOLCHAIN=local -e GOFLAGS=-mod=mod -e GOPROXY=off
DOCKER_RUNENV_dotnet := -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1

.PHONY: docker-bindings-base docker-bindings docker-bindings-clean \
        $(addprefix docker-,$(BINDING_LANGS))

docker-bindings-base:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.bindings-base \
	  --build-arg BASE=$(DOCKER_BASE) -t $(DOCKER_BINDINGS_BASE) .

# Generate `docker-<lang>`: build the per-language image on the base, then run it.
define docker_lang_rule
docker-$(1): docker-bindings-base
	$$(DOCKER) build $$(_docker_plat) -f bindings/Dockerfile.lang \
	  --build-arg BASE_IMAGE=$$(DOCKER_BINDINGS_BASE) \
	  --build-arg APT_PKGS='$$(DOCKER_APT_$(1))' \
	  --build-arg SETUP='$$(DOCKER_SETUP_$(1))' \
	  --build-arg TARGET=$(1) -t asmtest-$(1) .
	$$(DOCKER) run --rm $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) asmtest-$(1)
endef
$(foreach L,$(BINDING_LANGS),$(eval $(call docker_lang_rule,$(L))))

docker-bindings: $(addprefix docker-,$(BINDING_LANGS)) docker-win64

# Native-trace lane: a container with DynamoRIO installed that builds the tier and
# runs BOTH the C smoke harness and the Python language-wrapper suite. Separate
# from the binding images (it carries DynamoRIO, ~hundreds of MB, that the others
# don't need). Linux x86-64 only; the hardware-trace tier needs bare metal and is
# not containerizable. Override DR_VERSION to bump the pinned DynamoRIO.
DR_VERSION ?= 11.91.20630
.PHONY: docker-drtrace
docker-drtrace:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.drtrace \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-drtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-drtrace

# Per-language native-trace lane: layer DynamoRIO onto each already-built
# per-language image (asmtest-<lang>) and run that binding's drtrace wrapper test
# against a real in-process DynamoRIO — the cross-language counterpart of
# docker-drtrace's C+Python run.
#   make docker-drtrace-bindings   every language wrapper, in Docker
#   make docker-drtrace-<lang>     just one (e.g. docker-drtrace-rust)
.PHONY: docker-drtrace-bindings \
        $(addprefix docker-drtrace-,$(DRTRACE_BINDING_LANGS))

define docker_drtrace_lang_rule
docker-drtrace-$(1): docker-$(1)
	$$(DOCKER) build $$(_docker_plat) -f Dockerfile.drtrace-lang \
	  --build-arg BASE_IMAGE=asmtest-$(1) \
	  --build-arg DR_VERSION=$$(DR_VERSION) \
	  --build-arg TARGET=$(1) -t asmtest-drtrace-$(1) .
	$$(DOCKER) run --rm $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) asmtest-drtrace-$(1)
endef
$(foreach L,$(DRTRACE_BINDING_LANGS),$(eval $(call docker_drtrace_lang_rule,$(L))))

docker-drtrace-bindings: $(addprefix docker-drtrace-,$(DRTRACE_BINDING_LANGS))

# libasmtest_emu is the superset and Dockerfile.bindings-base now carries Keystone
# + Capstone, so each per-language image exercises the in-line-assembler AND
# disassembler path under `make <lang>-test` — there is no separate asm image.

docker-bindings-clean:
	-$(DOCKER) image rm $(addprefix asmtest-,$(BINDING_LANGS)) asmtest-win64 \
	  $(DOCKER_BINDINGS_BASE)

# --- Native Win64 tier (Win64 native tier plan) ----------------------------
# Cross-compile to a Windows PE and run it under Wine — no Windows host. The
# capture trampoline (Phase 1) will use the same `nasm -f win64` + mingw-w64
# link chain the smoke target below proves. Override the tools to run locally
# (non-Docker) where a Win64 cross-toolchain + Wine are installed.
#   make win64-smoke       host-side substrate smoke (PE -> Wine)
#   make win64-test        host-side capture-trampoline test (PE -> Wine)
#   make win64-msabi-test  fast native lane (no Wine; clang/gcc ms_abi, x86-64)
#   make win64-check       smoke + capture test (the image's CMD)
#   make docker-win64      all of the above inside the asmtest-win64 image (x86-64)
WIN64_CC        ?= x86_64-w64-mingw32-gcc
WIN64_NASM      ?= nasm
WIN64_NASMFLAGS ?= -f win64
WINE            ?= wine64
WIN64_BUILD     := $(BUILD)/win64

$(WIN64_BUILD):
	mkdir -p $@

# Phase 0 substrate smoke: a `nasm -f win64` Win64-ABI leaf + a mingw-w64 C
# driver -> PE -> run under Wine. Proves the toolchain chain end to end.
.PHONY: win64-smoke
win64-smoke: | $(WIN64_BUILD)
	$(WIN64_NASM) $(WIN64_NASMFLAGS) tests/win64/smoke_asm.asm -o $(WIN64_BUILD)/smoke_asm.obj
	$(WIN64_CC) -O2 -Wall tests/win64/smoke.c $(WIN64_BUILD)/smoke_asm.obj \
	  -o $(WIN64_BUILD)/smoke.exe
	$(WINE) $(WIN64_BUILD)/smoke.exe

# Phase 1 capture trampoline (PE/Wine lane): assemble the Win64 trampoline +
# routines with `nasm -f win64`, link the driver with mingw-w64, run under Wine.
# The driver consumes <asmtest.h>'s ASMTEST_ABI_WIN64 regs_t layout.
.PHONY: win64-test
win64-test: | $(WIN64_BUILD)
	$(WIN64_NASM) $(WIN64_NASMFLAGS) -Iinclude/ src/capture_win64.asm \
	  -o $(WIN64_BUILD)/capture_win64.obj
	$(WIN64_NASM) $(WIN64_NASMFLAGS) -Iinclude/ tests/win64/routines_win64.asm \
	  -o $(WIN64_BUILD)/routines_win64.obj
	$(WIN64_CC) -O2 -Wall -Iinclude -DASMTEST_ABI_WIN64 \
	  tests/win64/test_capture_win64.c \
	  $(WIN64_BUILD)/capture_win64.obj $(WIN64_BUILD)/routines_win64.obj \
	  -o $(WIN64_BUILD)/test_capture_win64.exe
	$(WINE) $(WIN64_BUILD)/test_capture_win64.exe

# Fast native lane (Win64 native tier plan, lane A): no Wine, no PE. The host is
# System V, so the trampoline is driven via clang/gcc __attribute__((ms_abi)).
# x86-64 only (ms_abi is an x86-64 attribute).
WIN64_MSABI_FMT := $(if $(filter Darwin,$(shell uname -s)),macho64,elf64)
.PHONY: win64-msabi-test
win64-msabi-test: | $(WIN64_BUILD)
	$(WIN64_NASM) -f $(WIN64_MSABI_FMT) -Iinclude/ src/capture_win64.asm \
	  -o $(WIN64_BUILD)/capture_win64.msabi.o
	$(WIN64_NASM) -f $(WIN64_MSABI_FMT) -Iinclude/ tests/win64/routines_win64.asm \
	  -o $(WIN64_BUILD)/routines_win64.msabi.o
	$(CC) -O2 -Wall -Iinclude -DASMTEST_ABI_WIN64 -c tests/win64/test_capture_win64.c \
	  -o $(WIN64_BUILD)/test_capture_win64.msabi.o
	$(CC) $(WIN64_BUILD)/test_capture_win64.msabi.o \
	  $(WIN64_BUILD)/capture_win64.msabi.o $(WIN64_BUILD)/routines_win64.msabi.o \
	  -o $(WIN64_BUILD)/test_capture_win64.msabi
	./$(WIN64_BUILD)/test_capture_win64.msabi

# Phase 4 slice (Win32 runner port): the guard-page allocator ported to
# VirtualAlloc/PAGE_NOACCESS (src/platform_win32.c), verified under Wine via
# VirtualQuery. PE/Wine only (it needs <windows.h>), so not in the ms_abi lane.
.PHONY: win64-guard-test
win64-guard-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -DASMTEST_ABI_WIN64 \
	  src/platform_win32.c tests/win64/test_guard_win64.c \
	  -o $(WIN64_BUILD)/test_guard_win64.exe
	$(WINE) $(WIN64_BUILD)/test_guard_win64.exe

# Phase 4 slice (Win32 runner port): isolated execution with crash containment +
# timeout (CreateProcess + WaitForSingleObject + TerminateProcess). PE/Wine only.
.PHONY: win64-isolate-test
win64-isolate-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  src/platform_win32.c tests/win64/test_isolate_win64.c \
	  -o $(WIN64_BUILD)/test_isolate_win64.exe
	$(WINE) $(WIN64_BUILD)/test_isolate_win64.exe

# Phase 4 slice (Win32 runner port): the parallel -jN pool over isolated children
# (WaitForMultipleObjects + per-task timeout). PE/Wine only.
.PHONY: win64-pool-test
win64-pool-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  src/platform_win32.c tests/win64/test_pool_win64.c \
	  -o $(WIN64_BUILD)/test_pool_win64.exe
	$(WINE) $(WIN64_BUILD)/test_pool_win64.exe

# Phase 4 slice (Win32 runner port): the portable --filter glob matcher (mingw
# has no fnmatch). Platform-neutral C; built with mingw and run under Wine here.
.PHONY: win64-filter-test
win64-filter-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Isrc \
	  src/glob_match.c tests/win64/test_glob.c \
	  -o $(WIN64_BUILD)/test_glob.exe
	$(WINE) $(WIN64_BUILD)/test_glob.exe

# Phase 4 slice (Win32 runner port): in-process crash-to-failure via a vectored
# exception handler + __builtin_longjmp (the --no-fork path). PE/Wine only.
.PHONY: win64-seh-test
win64-seh-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  src/platform_win32.c tests/win64/test_seh_win64.c \
	  -o $(WIN64_BUILD)/test_seh_win64.exe
	$(WINE) $(WIN64_BUILD)/test_seh_win64.exe

# Phase 4 capstone + Track B: the framework runner (src/asmtest.c) built for
# Win64 and run under Wine across ALL execution modes — per-test re-exec
# isolation (the default, Track B), the -jN pool, in-process --no-fork, and
# --bench. A real TEST() suite is discovered and run; a crashing and a hanging
# test are contained as reported failures while the run survives. Isolation
# contains the crash in a child process (caught there, or backstopped by the
# child's death) and the hang via the parent's deadline; --no-fork uses the
# in-process vectored handler + watchdog. Verifies the integrated runner end to
# end on every path.
.PHONY: win64-runner-test
win64-runner-test: | $(WIN64_BUILD)
	$(WIN64_NASM) $(WIN64_NASMFLAGS) -Iinclude/ src/capture_win64.asm \
	  -o $(WIN64_BUILD)/capture_win64.obj
	$(WIN64_NASM) $(WIN64_NASMFLAGS) -Iinclude/ tests/win64/routines_win64.asm \
	  -o $(WIN64_BUILD)/routines_win64.obj
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  -D__USE_MINGW_ANSI_STDIO=1 \
	  src/asmtest.c src/platform_win32.c src/glob_match.c \
	  tests/win64/suite_win64.c \
	  $(WIN64_BUILD)/capture_win64.obj $(WIN64_BUILD)/routines_win64.obj \
	  -o $(WIN64_BUILD)/suite_win64.exe
	@echo "# Track B: per-test re-exec isolation (default)"
	timeout 60 $(WINE) $(WIN64_BUILD)/suite_win64.exe --timeout 3 \
	  > $(WIN64_BUILD)/runner.out 2>&1 || true
	@cat $(WIN64_BUILD)/runner.out
	@grep -qE "^ok [0-9]+ - win64.ret_arg0" $(WIN64_BUILD)/runner.out
	@grep -qE "^ok [0-9]+ - win64.detects_clobber" $(WIN64_BUILD)/runner.out
	@grep -qE "^ok [0-9]+ - win64.fp_preserved" $(WIN64_BUILD)/runner.out
	@grep -qE "^not ok [0-9]+ - win64.crash_contained" $(WIN64_BUILD)/runner.out
	@grep -q "caught fatal exception" $(WIN64_BUILD)/runner.out
	@grep -qE "^not ok [0-9]+ - win64.hang_timed_out" $(WIN64_BUILD)/runner.out
	@grep -q "timed out" $(WIN64_BUILD)/runner.out
	@grep -qE "5 passed, 2 failed" $(WIN64_BUILD)/runner.out
	@echo "# Track B: -jN parallel pool over isolated children"
	timeout 60 $(WINE) $(WIN64_BUILD)/suite_win64.exe --timeout 3 -j3 \
	  > $(WIN64_BUILD)/runner_j.out 2>&1 || true
	@cat $(WIN64_BUILD)/runner_j.out
	@grep -q "caught fatal exception" $(WIN64_BUILD)/runner_j.out
	@grep -q "timed out" $(WIN64_BUILD)/runner_j.out
	@grep -qE "^ok [0-9]+ - win64.ret_arg0" $(WIN64_BUILD)/runner_j.out
	@grep -qE "5 passed, 2 failed" $(WIN64_BUILD)/runner_j.out
	@echo "# in-process --no-fork (vectored handler + watchdog)"
	timeout 60 $(WINE) $(WIN64_BUILD)/suite_win64.exe --timeout 3 --no-fork \
	  > $(WIN64_BUILD)/runner_nf.out 2>&1 || true
	@cat $(WIN64_BUILD)/runner_nf.out
	@grep -q "caught fatal exception" $(WIN64_BUILD)/runner_nf.out
	@grep -q "timed out" $(WIN64_BUILD)/runner_nf.out
	@grep -qE "5 passed, 2 failed" $(WIN64_BUILD)/runner_nf.out
	@echo "# Track B: --bench (rdtsc) on Win64"
	timeout 60 $(WINE) $(WIN64_BUILD)/suite_win64.exe --bench \
	  --filter='win64.ret_arg0' > $(WIN64_BUILD)/runner_bench.out 2>&1 || true
	@cat $(WIN64_BUILD)/runner_bench.out
	@grep -q "benchmarks" $(WIN64_BUILD)/runner_bench.out
	@grep -qE "win64.ret_arg0 +min=" $(WIN64_BUILD)/runner_bench.out
	@echo "win64 runner: isolation + -jN pool + --no-fork + --bench all verified"

# What the asmtest-win64 image runs: substrate smoke + capture + the Phase 4
# runner-port slices (guard pages, isolation, pool, --filter, SEH) + the
# integrated runner itself.
.PHONY: win64-check
win64-check: win64-smoke win64-test win64-guard-test win64-isolate-test \
             win64-pool-test win64-filter-test win64-seh-test win64-runner-test

# Build the win64 image on the cached bindings base, then run its CMD.
# x86-64 only: under linux/arm64 emulation an x86-64 PE will not run via Wine.
.PHONY: docker-win64
docker-win64: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.win64 \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-win64 .
	$(DOCKER) run --rm $(_docker_plat) asmtest-win64

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

HWTRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o $(BUILD)/cs_backend.o \
                $(BUILD)/trace.o

$(BUILD)/test_hwtrace: $(HWTRACE_OBJS) $(BUILD)/test_hwtrace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) -o $@

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
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(drtrace_env) \
	  $(CARGO) test --test drtrace -- --nocapture
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

# PIC variants + the standalone shared lib (never linked by core/superset).
$(BUILD)/pic/hwtrace.o: src/hwtrace.c include/asmtest_hwtrace.h \
                        include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/pt_backend.o: src/pt_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/cs_backend.o: src/cs_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(OPENCSD_DEF) $(OPENCSD_CFLAGS) -fPIC -c $< -o $@

shared-hwtrace: $(call shlib_dev,libasmtest_hwtrace)
$(call shlib_real,libasmtest_hwtrace): $(BUILD)/pic/hwtrace.o \
                                       $(BUILD)/pic/pt_backend.o \
                                       $(BUILD)/pic/cs_backend.o \
                                       $(BUILD)/pic/trace.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_hwtrace) $^ \
	      $(LIBIPT_LIBS) $(OPENCSD_LIBS) -o $@
$(call shlib_dev,libasmtest_hwtrace): $(call shlib_real,libasmtest_hwtrace)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_hwtrace)
	ln -sf $(notdir $(call shlib_compat,libasmtest_hwtrace)) $@

# --- Cross-language conformance corpus (Track 0.4) -------------------------
# The canonical-routine corpus + its C reference runner: the single source of
# truth every language binding must reproduce. Drives the routines through the
# binding-ABI entry points (asm_call_capture* + emu_call) and checks each result
# against the expected literal, then emits the portable table to corpus.json.
# Links the runtime built -DASMTEST_NO_MAIN (so its main() doesn't collide) plus
# the emulator; requires libunicorn, like `make emu-test`.
# Without main(), the runner-only static helpers (install_handlers, run_forked,
# the JUnit/TAP printers, the CLI parser, ...) are legitimately unused, so quiet
# that one warning for this build only.
$(BUILD)/asmtest_nomain.o: src/asmtest.c include/asmtest.h | $(BUILD)
	$(CC) $(CFLAGS) -DASMTEST_NO_MAIN -Wno-unused-function -c $< -o $@

$(BUILD)/conformance.o: bindings/conformance/conformance.c include/asmtest.h \
                        include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -c $< -o $@

$(BUILD)/conformance: $(BUILD)/conformance.o $(BUILD)/asmtest_nomain.o \
                      $(BUILD)/capture.o $(BUILD)/emu.o $(BUILD)/ffi.o \
                      $(BUILD)/trace.o $(BUILD)/add.o $(BUILD)/flags.o \
                      $(BUILD)/fp.o $(BUILD)/simd.o $(BUILD)/fault.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) -o $@

conformance: $(BUILD)/conformance
	./$(BUILD)/conformance
	./$(BUILD)/conformance --emit > bindings/conformance/corpus.json
	@echo "conformance: wrote bindings/conformance/corpus.json"

# In-line-assembler variant: the same reference runner built -DASMTEST_ENABLE_ASM
# and linked against the assembler (assemble.o, -lkeystone), so the optional asm
# tier (asm.add_signed / asm.att_3arg / asm.bad_source / asm.arm64_bytes) is
# compiled in and actually executes — the C-side anchor for the asm cases the
# bindings test. Does not re-emit corpus.json (the asm cases are emitted there
# unconditionally by the base `conformance` target).
$(BUILD)/conformance_asm.o: bindings/conformance/conformance.c include/asmtest.h \
                            include/asmtest_emu.h include/asmtest_assemble.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) $(KEYSTONE_CFLAGS) -DASMTEST_ENABLE_ASM \
	      -c $< -o $@

$(BUILD)/conformance_asm: $(BUILD)/conformance_asm.o $(BUILD)/asmtest_nomain.o \
                          $(BUILD)/capture.o $(BUILD)/emu.o $(BUILD)/ffi.o \
                          $(BUILD)/trace.o $(BUILD)/assemble.o $(BUILD)/add.o \
                          $(BUILD)/flags.o $(BUILD)/fp.o $(BUILD)/simd.o \
                          $(BUILD)/fault.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) -o $@

conformance-asm: $(BUILD)/conformance_asm
	./$(BUILD)/conformance_asm

# --- Python binding (Track P) ----------------------------------------------
# A pure-ctypes binding that loads the shared lib + manifest and replays the
# conformance corpus from Python. `make python-test` builds the shared libs, the
# manifest, the corpus, and a fixture lib exporting the canonical routines as
# symbols (the "code under test" a binding dlsym()s), then runs pytest. Requires
# python3 + pytest, and libunicorn (for the emulator cases), like `make emu-test`.
PYTEST ?= python3 -m pytest
ifeq ($(UNAME_S),Darwin)
CORPUS_LIB     := $(BUILD)/libasmtest_corpus.dylib
CORPUS_LDFLAGS := -dynamiclib
else
CORPUS_LIB     := $(BUILD)/libasmtest_corpus.so
CORPUS_LDFLAGS := -shared
endif
CORPUS_ROUTINE_OBJS := $(BUILD)/pic/add.o $(BUILD)/pic/flags.o \
                       $(BUILD)/pic/fp.o $(BUILD)/pic/simd.o \
                       $(BUILD)/pic/fault.o $(BUILD)/pic/corpus_routines.o

# name -> routine-address lookup, so bindings need no per-FFI symbol-address API.
$(BUILD)/pic/corpus_routines.o: bindings/conformance/corpus_routines.c | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(CORPUS_LIB): $(CORPUS_ROUTINE_OBJS)
	$(CC) $(CFLAGS) $(CORPUS_LDFLAGS) $^ -o $@

python-test: shared-emu manifest conformance $(CORPUS_LIB)
	cd bindings/python && \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) \
	  ASMTEST_MANIFEST=$(abspath asmtest_abi.json) \
	  ASMTEST_CORPUS_JSON=$(abspath bindings/conformance/corpus.json) \
	  ASMTEST_CORPUS_LIB=$(abspath $(CORPUS_LIB)) \
	  $(PYTEST) -q

# --- C++ binding (Track X) -------------------------------------------------
# The C headers are C++-consumable (extern "C" guards); bindings/cpp/asmtest.hpp
# adds RAII + typed conveniences. The example suite drives the framework from a
# C++ TU and links the same framework objects as the C suites. `make cpp-test`
# builds and runs it; requires a C++ compiler and libunicorn (emulator case).
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -g -Iinclude

# Full build: the bindings base now carries Keystone + Capstone (libasmtest_emu is
# the superset), so the C++ example enables emulator + in-line assembler + disas
# by default — matching the other bindings, where asm/disas are on out of the box.
$(BUILD)/test_cpp.o: bindings/cpp/test_cpp.cpp bindings/cpp/asmtest.hpp \
                     include/asmtest.h include/asmtest_emu.h \
                     include/asmtest_assemble.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(UNICORN_CFLAGS) $(KEYSTONE_CFLAGS) $(CAPSTONE_CFLAGS) \
	       $(CAPSTONE_DEF) -DASMTEST_ENABLE_EMU -DASMTEST_ENABLE_ASM \
	       -DASMTEST_ENABLE_DISAS -c $< -o $@

$(BUILD)/test_cpp: $(FRAMEWORK_OBJS) $(BUILD)/emu.o $(BUILD)/trace.o \
                   $(BUILD)/fuzz.o $(BUILD)/assemble.o $(BUILD)/disasm.o \
                   $(BUILD)/add.o $(BUILD)/flags.o $(BUILD)/fp.o \
                   $(BUILD)/simd.o $(BUILD)/fault.o $(BUILD)/test_cpp.o
	$(CXX) $(CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) $(CAPSTONE_LIBS) -o $@

cpp-test: $(BUILD)/test_cpp
	./$(BUILD)/test_cpp

# --- Rust binding (Track R) ------------------------------------------------
# A no-dependency crate (#[repr(C)] structs + extern "C" over the binding ABI)
# linked against the shared libs. `make rust-test` builds the shared libs + the
# routine fixture lib, then runs `cargo test`; requires cargo + libunicorn.
CARGO ?= cargo
rust-test: shared-emu $(CORPUS_LIB)
	cd bindings/rust && \
	  ASMTEST_LIB_DIR=$(abspath $(BUILD)) \
	  LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
	  DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH" \
	  $(CARGO) test

# --- Zig binding (Track Z) -------------------------------------------------
# Zig consumes the C headers directly via @cImport — no separate binding layer.
# `make zig-test` builds the shared libs + the routine fixture lib, then runs
# `zig build test`; requires zig + libunicorn. (build.zig targets Zig 0.13.x.)
ZIG ?= zig
zig-test: shared-emu $(CORPUS_LIB)
	cd bindings/zig && \
	  LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
	  DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH" \
	  $(ZIG) build test -Dincdir=$(abspath include) -Dlibdir=$(abspath $(BUILD))

# --- Community / managed-runtime bindings (Tracks N, J, D, C, G) ------------
# Each replays the conformance corpus through the opaque-handle FFI layer (no
# struct layout): asmtest_corpus_routine for addresses, asmtest_capture6/_fp2 +
# accessors for capture, asmtest_emu_call2 + accessors for the emulator. They
# need only the shared emulator lib + the routine fixture lib; their toolchains
# live in the Docker bindings image (use `make docker-ruby` / `-lua` / `-node` /
# `-java` / `-dotnet` / `-go`). Shared env points the loader at the build dir.
RUBY   ?= ruby
LUAJIT ?= luajit
NODE   ?= node
JAVAC  ?= javac
JAVA   ?= java
DOTNET ?= dotnet
GO     ?= go
bindings_env = ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) \
               ASMTEST_CORPUS_LIB=$(abspath $(CORPUS_LIB)) \
               LD_LIBRARY_PATH="$(abspath $(BUILD)):$$LD_LIBRARY_PATH" \
               DYLD_LIBRARY_PATH="$(abspath $(BUILD)):$$DYLD_LIBRARY_PATH"

# libasmtest_emu is the superset, so bindings_env resolves the assembler +
# disassembler too — asm_available()/disas_available() are true and every
# binding's `<lang>-test` exercises asm and disas (no separate asm env/target).
ruby-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(RUBY) bindings/ruby/conformance.rb

lua-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(LUAJIT) bindings/lua/conformance.lua

node-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(NODE) bindings/node/conformance.js

java-test: shared-emu $(CORPUS_LIB)
	mkdir -p $(BUILD)/java
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java \
	  bindings/java/Asmtest.java bindings/java/Conformance.java
	$(bindings_env) $(JAVA) --enable-preview --enable-native-access=ALL-UNNAMED \
	  -cp $(BUILD)/java Conformance

dotnet-test: shared-emu $(CORPUS_LIB)
	$(bindings_env) $(DOTNET) run --project bindings/dotnet/asmtest.csproj

# Go links the shared libs at build time via cgo; CGO_LDFLAGS carries the -L (so
# a custom BUILD works), and bindings_env's LD_LIBRARY_PATH/DYLD_LIBRARY_PATH
# resolves them at run time. GOTOOLCHAIN=local + GOPROXY=off keep it offline (no
# module deps). The emulator case uses the x86-64 guest, so run on an x86-64 host.
go-test: shared-emu $(CORPUS_LIB)
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  $(bindings_env) $(GO) test ./...

# --- Packaging scaffolding (publishable artifacts) -------------------------
# See docs/packaging.md. `make package-libs` stages the host's shared libs into
# build/dist/native/<plat>/; each `make <lang>-package` re-stages into that
# ecosystem's native-payload location and runs its packer, emitting under
# build/dist/<lang>/. Scaffolding only: a multi-platform release repeats the
# native staging per target OS/arch (or uses the ecosystem's prebuild tooling),
# and `make packages` needs every toolchain (prefer one language at a time, or
# each binding's Docker image).
PKG_DIST   := $(BUILD)/dist
PKG_PLAT   := $(shell uname -s | tr '[:upper:]' '[:lower:]')-$(shell uname -m)
DOTNET_RID ?= $(PKG_PLAT)
GEM      ?= gem
NPM      ?= npm
CARGO    ?= cargo
JAR      ?= jar
LUAROCKS ?= luarocks
PYBUILD  ?= python3 -m build
# The unversioned names the dlopen bindings look up (libasmtest_emu.dylib, ...).
pkg_emu_name  := $(notdir $(call shlib_dev,libasmtest_emu))
pkg_core_name := $(notdir $(call shlib_dev,libasmtest))

.PHONY: packages package-libs package-libs-verify native-payload-check \
        python-package rust-package zig-package cpp-package node-package \
        java-package dotnet-package ruby-package lua-package go-package

# The dlopen packers consume build/dist/native/ (staged by `make package-libs`
# locally, or the CI payloads matrix downloaded into it) rather than depending on
# package-libs directly — so a release job can package the collected
# multi-platform tree without rebuilding the host slot (which would need the full
# Unicorn + Keystone + Capstone toolchain). This guard fails fast if the tree is
# absent.
native-payload-check:
	@ls -d $(PKG_DIST)/native/*/ >/dev/null 2>&1 || { \
	  echo "native-payload-check: no payload in $(PKG_DIST)/native — run 'make package-libs' first"; exit 1; }

# package-libs stages the build host's native payload into build/dist/native/<plat>/.
# libasmtest_emu IS the full superset (emu + Keystone assembler + Capstone
# disassembler), so it is staged directly under that slot and scripts/package-native.sh
# then vendors its three native deps beside it (rpath -> $$ORIGIN/@loader_path) and
# assembles THIRD-PARTY-LICENSES, so a fresh install runs both optional tiers with no
# system libs. Needs libunicorn + libkeystone + libcapstone at build time (make deps
# DEPS_ARGS=--asm plus the build-{keystone,capstone}.sh source builds).
package-libs: shared shared-emu
	mkdir -p $(PKG_DIST)/native/$(PKG_PLAT)
	cp -f $(call shlib_real,libasmtest)     $(PKG_DIST)/native/$(PKG_PLAT)/$(pkg_core_name)
	cp -f $(call shlib_real,libasmtest_emu) $(PKG_DIST)/native/$(PKG_PLAT)/$(pkg_emu_name)
	sh scripts/package-native.sh $(PKG_DIST)/native/$(PKG_PLAT) $(pkg_emu_name)
	@echo "package-libs: staged $(PKG_PLAT) full payload in $(PKG_DIST)/native/$(PKG_PLAT)"

# dlopen bindings: bundle the prebuilt libasmtest_emu (the superset) + its vendored
# native deps + THIRD-PARTY-LICENSES in the package's payload. Python's wheel is
# per-platform (cibuildwheel builds one per tag in CI), so it bundles only the host
# slot, flat in asmtest/_libs/ (where _native.py looks); auditwheel/delocate vendor
# the deps at repair time, and the notices ride along.
python-package: package-libs manifest
	rm -rf bindings/python/asmtest/_libs bindings/python/build bindings/python/*.egg-info
	mkdir -p bindings/python/asmtest/_libs $(PKG_DIST)/python
	# Stage the RAW (system-linked) superset + core libs, NOT the pre-vendored
	# build/dist copies: the wheel's deps are vendored by auditwheel (Linux) /
	# delocate (macOS) at repair time, and those tools resolve the lib's
	# dependencies from the system — they choke on a lib already rewritten to
	# @loader_path. The THIRD-PARTY-LICENSES notices still ride along.
	cp -f $(call shlib_real,libasmtest_emu) bindings/python/asmtest/_libs/$(pkg_emu_name)
	cp -f $(call shlib_real,libasmtest)     bindings/python/asmtest/_libs/$(pkg_core_name)
	cp -f asmtest_abi.json bindings/python/asmtest/_libs/
	cp -Rf $(PKG_DIST)/native/$(PKG_PLAT)/THIRD-PARTY-LICENSES bindings/python/asmtest/_libs/ 2>/dev/null || true
	cd bindings/python && $(PYBUILD) --wheel --outdir $(abspath $(PKG_DIST))/python

# Each dlopen packer ships the reusable library module (asmtest.rb / asmtest.js /
# asmtest.lua / Asmtest.java / AsmTest.dll), NOT the conformance runner, and
# bundles one native slot per platform present in build/dist/native/ (staged by
# package-libs locally; the CI payloads matrix collects all four). emu_lib_slots
# copies the whole runtime payload from each <plat> slot — libasmtest_emu, the
# vendored Unicorn/Keystone/Capstone, and THIRD-PARTY-LICENSES — into a per-platform
# dir (the rpath patching staged at package-libs time keeps the deps resolvable).
define emu_lib_slots
	@for pd in $(PKG_DIST)/native/*/; do \
	  [ -d "$$pd" ] || continue; p=$$(basename "$$pd"); \
	  mkdir -p "$(1)/$$p"; \
	  cp -f "$$pd"lib*.so* "$(1)/$$p/" 2>/dev/null || true; \
	  cp -f "$$pd"lib*.dylib "$(1)/$$p/" 2>/dev/null || true; \
	  cp -Rf "$$pd"THIRD-PARTY-LICENSES "$(1)/$$p/" 2>/dev/null || true; \
	  echo "  bundled $$p (lib + vendored deps + licenses)"; \
	done
endef

ruby-package: native-payload-check
	rm -rf bindings/ruby/native && mkdir -p $(PKG_DIST)/ruby
	$(call emu_lib_slots,bindings/ruby/native)
	cd bindings/ruby && $(GEM) build asmtest.gemspec
	mv bindings/ruby/asmtest-$(ASMTEST_VERSION).gem $(PKG_DIST)/ruby/

node-package: native-payload-check
	rm -rf bindings/node/native && mkdir -p $(PKG_DIST)/node
	$(call emu_lib_slots,bindings/node/native)
	cd bindings/node && $(NPM) pack --pack-destination $(abspath $(PKG_DIST))/node

java-package: native-payload-check
	rm -rf bindings/java/src/main/resources/native
	mkdir -p $(BUILD)/java-pkg $(PKG_DIST)/java
	$(call emu_lib_slots,bindings/java/src/main/resources/native)
	$(JAVAC) --release 21 --enable-preview -d $(BUILD)/java-pkg bindings/java/Asmtest.java
	cp -r bindings/java/src/main/resources/native $(BUILD)/java-pkg/
	cd $(BUILD)/java-pkg && $(JAR) cf $(abspath $(PKG_DIST))/java/asmtest-$(ASMTEST_VERSION).jar .

# .NET: stage a runtimes/<rid>/native/ slot per platform (mapping the <os>-<arch>
# payload name to the .NET RID the loader resolves), then `dotnet pack` the
# classlib project (asmtest-lib.csproj) — it compiles the AsmTest.dll library from
# Asmtest.cs and packs it + those native assets into a real nupkg the consumer
# restores (the loader picks runtimes/<rid>/native/ at run time).
dotnet-package: native-payload-check
	rm -rf bindings/dotnet/runtimes
	mkdir -p $(PKG_DIST)/dotnet
	@for pd in $(PKG_DIST)/native/*/; do \
	  [ -d "$$pd" ] || continue; p=$$(basename "$$pd"); \
	  rid=$$(echo "$$p" | sed -e 's/^darwin-/osx-/' -e 's/-x86_64$$/-x64/' -e 's/-aarch64$$/-arm64/'); \
	  mkdir -p bindings/dotnet/runtimes/$$rid/native; \
	  cp -f "$$pd"lib*.so* bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  cp -f "$$pd"lib*.dylib bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  cp -Rf "$$pd"THIRD-PARTY-LICENSES bindings/dotnet/runtimes/$$rid/native/ 2>/dev/null || true; \
	  echo "  bundled $$p -> runtimes/$$rid/native (lib + vendored deps + licenses)"; \
	done
	$(DOTNET) pack bindings/dotnet/asmtest-lib.csproj -c Release \
	  -p:PackageOutputPath=$(abspath $(PKG_DIST))/dotnet
	@echo "dotnet-package: packed AsmTest nupkg (lib/net8.0/AsmTest.dll + runtimes/<rid>/native) in $(PKG_DIST)/dotnet"

lua-package: native-payload-check
	rm -rf bindings/lua/native && mkdir -p $(PKG_DIST)/lua
	$(call emu_lib_slots,bindings/lua/native)
	cp -f bindings/lua/asmtest-1.0.0-1.rockspec $(PKG_DIST)/lua/
	@echo "lua-package: staged rockspec + native in $(PKG_DIST)/lua (luarocks pack/upload to publish)"

# link bindings: source distributions (the consumer builds/installs libasmtest).
rust-package:
	mkdir -p $(PKG_DIST)/rust
	cd bindings/rust && CARGO_TARGET_DIR=$(abspath $(BUILD))/rust-pkg \
	  $(CARGO) package --no-verify --allow-dirty
	cp $(BUILD)/rust-pkg/package/*.crate $(PKG_DIST)/rust/

zig-package:
	mkdir -p $(PKG_DIST)/zig
	tar czf $(PKG_DIST)/zig/asmtest-zig-$(ASMTEST_VERSION).tar.gz \
	  -C bindings/zig build.zig build.zig.zon src README.md

cpp-package:
	mkdir -p $(PKG_DIST)/cpp
	tar czf $(PKG_DIST)/cpp/asmtest-cpp-$(ASMTEST_VERSION).tar.gz \
	  -C bindings/cpp asmtest.hpp CMakeLists.txt README.md

go-package:
	@echo "go-package: Go modules publish from the tagged repo (no artifact to build)."
	@echo "  module: github.com/wilvk/asm-test/bindings/go"
	@echo "  consumers set CGO_LDFLAGS to link libasmtest_emu (see docs/packaging.md)."

packages: package-libs python-package rust-package zig-package cpp-package node-package \
          java-package dotnet-package ruby-package lua-package go-package

# One-shot LOCAL packaging: `make <lang>-package-full` runs every step needed to
# emit a complete package on this host — it builds + stages the host's native
# payload (package-libs: the superset emu lib + vendored Unicorn/Keystone/Capstone
# + THIRD-PARTY-LICENSES) first, then runs the packer. The plain `<lang>-package`
# targets stay payload-consumers (gated by native-payload-check) so a CI release
# host can package a downloaded multi-platform `native-all` tree without the full
# toolchain; the `-full` aliases are the convenience entrypoint for a single-box
# local build. Needs libunicorn + libkeystone + libcapstone (make deps).
#
# dlopen packagers need the staged payload, so their -full alias prepends
# package-libs. python-package already depends on package-libs, so its alias just
# forwards. The link/source packagers (rust/zig/cpp/go) bundle no native payload,
# so their -full alias is the plain target — provided for a uniform interface.
.PHONY: python-package-full ruby-package-full node-package-full java-package-full \
        dotnet-package-full lua-package-full rust-package-full zig-package-full \
        cpp-package-full go-package-full

ruby-package-full:   package-libs ; $(MAKE) ruby-package
node-package-full:   package-libs ; $(MAKE) node-package
java-package-full:   package-libs ; $(MAKE) java-package
dotnet-package-full: package-libs ; $(MAKE) dotnet-package
lua-package-full:    package-libs ; $(MAKE) lua-package

python-package-full: python-package
rust-package-full:   rust-package
zig-package-full:    zig-package
cpp-package-full:    cpp-package
go-package-full:     go-package

# Verify a (possibly multi-platform) build/dist/native/ tree carries a complete
# fully-featured set: every <plat> subdir must hold the core lib, the libasmtest_emu
# superset the dlopen bindings load, the three vendored native deps
# (Unicorn/Keystone/Capstone) that make it self-contained, and a THIRD-PARTY-LICENSES
# dir. `make package-libs` stages only the build host's slot; the CI `payloads`
# matrix runs it on each OS/arch and the collect job merges the artifacts into one
# tree, then runs this target so a release never ships a payload missing a platform,
# a lib, a vendored dep, or its notices. File-existence only (it runs on the collect
# host, which is one platform — the per-slot symbol assertion happens at
# package-libs time via scripts/package-native.sh). Exits nonzero if any slot is
# incomplete.
package-libs-verify:
	@dir=$(PKG_DIST)/native; \
	test -d "$$dir" || { echo "package-libs-verify: no $$dir — run 'make package-libs' first"; exit 1; }; \
	plats=$$(cd "$$dir" && ls -d */ 2>/dev/null | tr -d /); \
	test -n "$$plats" || { echo "package-libs-verify: $$dir has no platform subdirs"; exit 1; }; \
	rc=0; n=0; \
	for p in $$plats; do \
	  n=$$((n+1)); pd="$$dir/$$p"; miss=""; \
	  core=$$(ls "$$pd"/libasmtest.so* "$$pd"/libasmtest.*.dylib "$$pd"/libasmtest.dylib 2>/dev/null | head -1); \
	  emu=$$(ls "$$pd"/libasmtest_emu.so* "$$pd"/libasmtest_emu.dylib 2>/dev/null | grep -v _emu_ | head -1); \
	  [ -n "$$core" ] || miss="$$miss core"; \
	  [ -n "$$emu" ] || miss="$$miss emu"; \
	  for d in unicorn keystone capstone; do \
	    ls "$$pd"/lib$$d.* >/dev/null 2>&1 || miss="$$miss $$d"; \
	  done; \
	  [ -d "$$pd/THIRD-PARTY-LICENSES" ] || miss="$$miss licenses"; \
	  if [ -z "$$miss" ]; then \
	    echo "  ok   $$p   (core, emu, unicorn, keystone, capstone, licenses)"; \
	  else \
	    echo "  MISS $$p  missing:$$miss"; rc=1; \
	  fi; \
	done; \
	echo "package-libs-verify: $$n platform(s) in $$dir"; \
	exit $$rc

# --- Documentation (Sphinx → Read the Docs) --------------------------------
# `make docs`           build the HTML docs into docs/_build/html
# `make docs-serve`     build, then serve them on http://localhost:$(DOCS_PORT)
# `make docs-linkcheck` verify external links resolve
# `make docs-clean`     remove the built docs
# Mirrors the Read the Docs build (config in docs/conf.py + .readthedocs.yaml);
# SPHINXOPTS defaults to -W to match its fail_on_warning. Install the optional
# doc toolchain with `pip install -r docs/requirements.txt`.
SPHINXBUILD ?= sphinx-build
SPHINXOPTS  ?= -W --keep-going
DOCS_SRC    := docs
DOCS_OUT    := docs/_build
DOCS_PORT   ?= 8000

.PHONY: docs docs-serve docs-linkcheck docs-clean
docs:
	$(SPHINXBUILD) -b html $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/html
	@echo "docs: built $(DOCS_OUT)/html/index.html"

docs-serve: docs
	@echo "serving docs on http://localhost:$(DOCS_PORT)/ (Ctrl-C to stop)"
	cd $(DOCS_OUT)/html && python3 -m http.server $(DOCS_PORT)

docs-linkcheck:
	$(SPHINXBUILD) -b linkcheck $(SPHINXOPTS) $(DOCS_SRC) $(DOCS_OUT)/linkcheck

docs-clean:
	rm -rf $(DOCS_OUT)

clean: docs-clean
	rm -rf $(BUILD)
	rm -f asmtest.pc asmtest-emu.pc asmtest_single.h asmtest_abi.json
	rm -f bindings/conformance/corpus.json
	rm -rf bindings/python/asmtest/_libs bindings/ruby/native bindings/lua/native \
	       bindings/node/native bindings/java/src/main/resources/native \
	       bindings/dotnet/runtimes
	rm -f bindings/ruby/*.gem
