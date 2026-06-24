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
                  $(BUILD)/test_callback

.PHONY: all test check demo-fail clean
.PHONY: lib install uninstall amalgamate pc
.PHONY: shared shared-emu manifest install-shared install-shared-emu
.PHONY: sanitize coverage tidy
.PHONY: deps usecases usecases-emu
all: test

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
ASMTEST_VERSION := 1.0.0
ASMTEST_VER_MAJOR := $(word 1,$(subst ., ,$(ASMTEST_VERSION)))
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
PIC_OBJS := $(BUILD)/pic/asmtest.o $(BUILD)/pic/capture.o

$(BUILD)/pic:
	mkdir -p $(BUILD)/pic

# Position-independent objects (separate tree so they never collide with the
# non-PIC objects the test/static-lib builds use).
$(BUILD)/pic/asmtest.o: src/asmtest.c include/asmtest.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/pic/emu.o: src/emu.c include/asmtest_emu.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -fPIC -c $< -o $@

ifeq ($(ASM_SYNTAX),nasm)
$(BUILD)/pic/capture.o: src/capture.asm include/asm_nasm.inc | $(BUILD)/pic
	$(NASM) $(NASMFLAGS) $< -o $@
else
$(BUILD)/pic/capture.o: src/capture.s include/asm.h | $(BUILD)/pic
	$(CC) $(CFLAGS) $(ASFLAGS) -fPIC -c $< -o $@
endif

# Core shared lib: real versioned file + soname/dev symlinks beside it.
shared: $(call shlib_dev,libasmtest)
$(call shlib_real,libasmtest): $(PIC_OBJS)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest) $^ -o $@
$(call shlib_dev,libasmtest): $(call shlib_real,libasmtest)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest)
	ln -sf $(notdir $(call shlib_compat,libasmtest)) $@

# Emulator shared lib: kept separate so the core binding never pulls in Unicorn.
shared-emu: $(call shlib_dev,libasmtest_emu)
$(call shlib_real,libasmtest_emu): $(PIC_OBJS) $(BUILD)/pic/emu.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_emu) $^ $(UNICORN_LIBS) -o $@
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
	cp include/asmtest.h include/asmtest_emu.h include/asm.h \
	   include/asm_nasm.inc $(incdir)/
	cp $(BUILD)/libasmtest.a $(libdir)/
	$(pc_subst) asmtest.pc.in > $(pcdir)/asmtest.pc
	@echo "installed asmtest $(ASMTEST_VERSION) to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(incdir)/asmtest.h $(incdir)/asmtest_emu.h $(incdir)/asm.h \
	      $(incdir)/asm_nasm.inc
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

# Install the emulator shared lib + its pkg-config (needs libunicorn present).
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
# clang-tidy) via the system package manager — the core build needs none of it.
# scripts/install-deps.sh detects apt-get/dnf/yum/pacman/zypper/apk/brew. Pass
# DEPS_ARGS to select a subset, e.g. `make deps DEPS_ARGS=--emu` or preview with
# `make deps DEPS_ARGS=--dry-run`.
DEPS_ARGS ?=
deps:
	sh scripts/install-deps.sh $(DEPS_ARGS)

# --- Run the Linux CI jobs locally via Docker ------------------------------
# Covers the Linux half of the matrix; the macOS jobs can't run in a container.
#   make docker-test       build + run the example suites and self-tests
#   make docker-nasm       the NASM backend (x86-64 only)
#   make docker-emu        the emulator tier (libunicorn)
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

.PHONY: docker-build docker-test docker-nasm docker-emu docker-valgrind \
        docker-sanitize docker-analyze docker-coverage docker-ci docker-shell \
        docker-clean

docker-build:
	$(DOCKER) build $(_docker_plat) --build-arg BASE=$(DOCKER_BASE) -t $(DOCKER_IMAGE) .

docker-test: docker-build
	$(_docker_run) sh -c 'make test && make check'

docker-nasm: docker-build
	$(_docker_run) sh -c 'make ASM_SYNTAX=nasm test && make ASM_SYNTAX=nasm check'

docker-emu: docker-build
	$(_docker_run) make emu-test

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
	  make clean && make valgrind; \
	  make clean && make sanitize; \
	  make tidy'

docker-shell: docker-build
	$(DOCKER) run --rm -it $(_docker_plat) $(DOCKER_IMAGE) sh

docker-clean:
	-$(DOCKER) image rm $(DOCKER_IMAGE)

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

$(BUILD)/emu.o: src/emu.c include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) $(UNICORN_CFLAGS) -c $< -o $@

$(BUILD)/test_emu: $(FRAMEWORK_OBJS) $(BUILD)/add.o $(BUILD)/mem.o \
                   $(BUILD)/flags.o $(BUILD)/branch.o $(BUILD)/emu.o \
                   $(BUILD)/test_emu.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) -o $@

.PHONY: emu-test
emu-test: $(BUILD)/test_emu
	./$(BUILD)/test_emu

# Emulator "unusual use case" suite (Track F): the virtual CPU as a security
# sandbox (precise over-read/over-write fault localization) and a cross-ISA
# equivalence checker (the same algorithm run on x86-64, AArch64, RISC-V, and
# ARM32 guests). GAS backend only, like emu-test; requires libunicorn.
$(BUILD)/test_emu_usecases: $(FRAMEWORK_OBJS) $(BUILD)/emucases.o \
                            $(BUILD)/emu.o $(BUILD)/test_emu_usecases.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) -o $@

usecases-emu: $(BUILD)/test_emu_usecases
	./$(BUILD)/test_emu_usecases

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
