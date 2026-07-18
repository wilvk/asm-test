# win64.mk — Native Win64 tier: cross-compile to a Windows PE and run it under Wine.
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

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

# code-review-plausible-triage T6: the --no-fork VEH crash handler
# (rt_veh_cb, src/platform_win32.c) is scoped to the armed test thread, not
# process-global -- a fault on any OTHER thread must take the OS's normal
# unhandled-exception path instead of being redirected onto the test
# thread's recovery stack (which lives on that thread's own stack). PE/Wine
# only, mirrors win64-seh-test's build.
.PHONY: win64-veh-scope-test
win64-veh-scope-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  src/platform_win32.c tests/win64/test_veh_scope_win64.c \
	  -o $(WIN64_BUILD)/test_veh_scope_win64.exe
	@echo "# scenario 'main': same-thread fault still caught (regression guard)"
	$(WINE) $(WIN64_BUILD)/test_veh_scope_win64.exe main
	@echo "# scenario 'foreign': a non-test-thread fault must not hijack the test thread"
	@rc=0; timeout 30 $(WINE) $(WIN64_BUILD)/test_veh_scope_win64.exe foreign \
	  > $(WIN64_BUILD)/veh_scope.out 2>&1 || rc=$$?; \
	cat $(WIN64_BUILD)/veh_scope.out; \
	if [ "$$rc" = 0 ] || [ "$$rc" = 124 ] || \
	   grep -q "MAIN-HIJACKED" $(WIN64_BUILD)/veh_scope.out || \
	   grep -q "SURVIVED-FOREIGN-FAULT" $(WIN64_BUILD)/veh_scope.out; then \
	  echo "FAIL: foreign-thread fault was not contained by the OS (rc=$$rc)"; \
	  exit 1; \
	fi; \
	echo "win64-veh-scope-test: foreign-thread fault correctly took the OS's unhandled-exception path (rc=$$rc)"

# Phase 5 slice (single-step plan): the VEH single-step front-end — EFLAGS.TF
# armed around a library-owned call, EXCEPTION_SINGLE_STEP to a vectored
# handler, the exact in-region instruction stream recorded (the Windows twin of
# the Linux in-process stepper; same expected offset streams). PE/Wine only
# (<windows.h>); self-skips where single-step exceptions are not delivered.
.PHONY: win64-ss-test
win64-ss-test: | $(WIN64_BUILD)
	$(WIN64_CC) -O2 -Wall -Iinclude -Isrc -DASMTEST_ABI_WIN64 \
	  src/ss_win64.c tests/win64/test_ss_win64.c \
	  -o $(WIN64_BUILD)/test_ss_win64.exe
	$(WINE) $(WIN64_BUILD)/test_ss_win64.exe

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
# runner-port slices (guard pages, isolation, pool, --filter, SEH, VEH thread
# scoping) + the integrated runner itself.
.PHONY: win64-check
win64-check: win64-smoke win64-test win64-guard-test win64-isolate-test \
             win64-pool-test win64-filter-test win64-seh-test \
             win64-veh-scope-test win64-ss-test win64-runner-test

# Windows mirror of `make hwtrace-attach-demo`: attach to a SEPARATE process the
# framework did NOT start (attach_victim_win) and single-step one call of its hot
# function out of process, via the Win32 Debug API (DebugActiveProcess + int3 at the
# region + EFLAGS.TF single-step). Capstone-free, so it reports ordered offsets like
# the Linux no-Capstone path. BUILDS with mingw; runs on real Windows, best-effort
# under Wine (cross-process debug-event delivery is a Wine feature, not guaranteed).
.PHONY: win64-attach-demo
win64-attach-demo: | $(WIN64_BUILD)
	$(WIN64_CC) -O0 -g -Wall examples/win/attach_victim_win.c \
	  -o $(WIN64_BUILD)/attach_victim_win.exe
	$(WIN64_CC) -O2 -Wall examples/win/attach_trace_win.c \
	  -o $(WIN64_BUILD)/attach_trace_win.exe
	@echo "== win64-attach-demo (trace a separate, already-running Windows process) =="
	WIN64_BUILD=$(WIN64_BUILD) WINE='$(WINE)' sh examples/win/attach_demo_win.sh

# Windows mirror of `make hwtrace-syscall-log`: attach to a SEPARATE process and log
# its ntdll calls WITH the data crossing the kernel boundary (write buffers, create
# paths) by breakpointing the ntdll Nt* stubs — Windows has no PTRACE_SYSCALL, so the
# stubs ARE the stable interception layer. BUILDS with mingw; runs on real Windows,
# best-effort under Wine.
.PHONY: win64-syscall-log
win64-syscall-log: | $(WIN64_BUILD)
	$(WIN64_CC) -O0 -g -Wall examples/win/syscall_victim_win.c \
	  -o $(WIN64_BUILD)/syscall_victim_win.exe
	$(WIN64_CC) -O2 -Wall examples/win/syscall_log_win.c \
	  -o $(WIN64_BUILD)/syscall_log_win.exe
	@echo "== win64-syscall-log (log a separate Windows process's ntdll calls + data) =="
	WIN64_BUILD=$(WIN64_BUILD) WINE='$(WINE)' sh examples/win/syscall_demo_win.sh

# Build the win64 image on the cached bindings base, then run its CMD.
# x86-64 only: under linux/arm64 emulation an x86-64 PE will not run via Wine.
.PHONY: docker-win64 docker-win64-attach-demo docker-win64-syscall-log
docker-win64: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.win64 \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-win64 .
	$(DOCKER) run --rm $(_docker_plat) asmtest-win64

# The two out-of-process Windows demos in the asmtest-win64 image (mingw + Wine).
docker-win64-attach-demo: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.win64 \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-win64 .
	$(DOCKER) run --rm $(_docker_plat) asmtest-win64 make win64-attach-demo

docker-win64-syscall-log: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.win64 \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-win64 .
	$(DOCKER) run --rm $(_docker_plat) asmtest-win64 make win64-syscall-log

