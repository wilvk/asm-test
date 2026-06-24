# asm-test build.
#
#   make test                    build and run the example suites (green)
#   make demo-fail               build and run the intentional-failure demo
#   make bench                   build and run the Phase 9 benchmark demo
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
                  $(BUILD)/test_fpover $(BUILD)/test_refmatch

.PHONY: all test check demo-fail clean
.PHONY: lib install uninstall amalgamate pc
.PHONY: sanitize coverage tidy
.PHONY: deps
all: test

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
incdir := $(DESTDIR)$(PREFIX)/include/asmtest
libdir := $(DESTDIR)$(PREFIX)/lib
pcdir  := $(DESTDIR)$(PREFIX)/lib/pkgconfig
pc_subst := sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(ASMTEST_VERSION)|g'

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

test: $(SUITES)
	@set -e; for t in $(SUITES); do echo "== $$t =="; ./$$t; done

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

# --- Optional dependency bootstrap -----------------------------------------
# `make deps` installs the OPTIONAL toolchain (nasm, pkg-config, libunicorn,
# clang-tidy) via the system package manager — the core build needs none of it.
# scripts/install-deps.sh detects apt-get/dnf/yum/pacman/zypper/apk/brew. Pass
# DEPS_ARGS to select a subset, e.g. `make deps DEPS_ARGS=--emu` or preview with
# `make deps DEPS_ARGS=--dry-run`.
DEPS_ARGS ?=
deps:
	sh scripts/install-deps.sh $(DEPS_ARGS)

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

clean:
	rm -rf $(BUILD)
	rm -f asmtest.pc asmtest_single.h
