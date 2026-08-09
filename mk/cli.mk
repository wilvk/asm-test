# cli.mk — asmspy, an ncurses front-end over the out-of-process tracer (cli/).
#
# Links the hwtrace tier objects (run_to / trace_attached_ex + descent + disasm)
# plus ncursesw + pthread. asmspy carries its own /proc lister and ELF .symtab/
# .dynsym function resolver (cli/asmspy_proc.c) because the library has none.
# Modeled on the examples/jit_trace link line (mk/native-trace.mk).

# ncursesw dev files ship in libncurses-dev on Ubuntu (Dockerfile.cli adds it).
NCURSES_LIBS ?= $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

# asmspy runs on Linux x86-64 AND AArch64: its register/single-step/detach reads
# are lifted behind the arch shim (cli/asmspy_arch.h — the landed asmspy-plan.md
# Theme F row). Every OTHER machine (32-bit ARM, riscv, ...) still has no register
# body, so the compile would dump raw errors (`no member named 'rip'`) instead of
# gating. An architecture is a REAL gate (CLAUDE.md): unlike the CLI_MISSING branch
# below there is nothing to apt-install, so it is checked FIRST — falling through
# would tell a riscv user to install libncurses-dev, which cannot fix it. And
# `make docker-cli` on an arm64 HOST is now the SUPPORTED path, not a skip:
# _docker_plat follows the host unless DOCKER_PLATFORM is set, so the in-container
# make sees aarch64 and builds asmspy natively.
CLI_ARCH := $(shell uname -m)

# The machines asmspy has a register/step/detach body for (cli/asmspy_arch.h).
# uname -m reports aarch64 on Linux; some toolchains say arm64 — accept both.
CLI_ARCH_SUPPORTED := x86_64 aarch64 arm64

# asmspy needs Capstone (disassembly, via the hwtrace tier) AND ncursesw (the TUI).
# Both are present in the asmtest-cli / asmtest-hwtrace images; a bare host often
# lacks them, so detect that and point at the container instead of dumping a raw
# "ncurses.h: No such file" compiler error (this mirrors the other optional tiers,
# which self-skip when their toolchain is absent).
CLI_MISSING :=
ifneq ($(shell pkg-config --exists ncursesw 2>/dev/null && echo ok),ok)
CLI_MISSING += libncurses-dev
endif
ifneq ($(shell pkg-config --exists capstone 2>/dev/null && echo ok),ok)
CLI_MISSING += Capstone
endif

# The syscall-name table asmspy decodes against is generated from the COMPILING
# host's own <sys/syscall.h> (names only; the numbers come from __NR_ at compile
# time), so it tracks whatever kernel headers are installed instead of drifting.
# cli/asmspy_syscall_name.h #includes it — asmspy_engine.c's --log decoder AND
# asmspy_proc.c's attach-free process snapshot include THAT header (not this
# .inc directly), hence the -I$(BUILD) below wherever either TU compiles.
$(BUILD)/asmspy_syscall_names.inc: cli/gen-syscall-names.sh | $(BUILD)
	CC='$(CC)' sh $< >$@

# cli/ sources compile like examples/, but with -pthread (a dedicated tracer
# thread owns the ptrace loop while the ncurses UI thread stays responsive).
# NOTE the fixed header list: every cli/ object depends on ALL of these, so a
# header missing from it is a stale object nothing rebuilds. That bit
# asmspy_ptracesample.h, whose tunables (ASMSPY_PS_HZ, _CONFIRM_MS,
# _HIT_BUDGET, the two inline decisions) live in the header while the code
# lives in the .c — and because the header IS listed as a prerequisite of
# build/test_ptracesample below, the TEST relinked and looked fresh while the
# sampler under test was still built from the old constants. A green run over
# code that no longer exists.
$(BUILD)/%.o: cli/%.c cli/libasmspy.h cli/asmspy.h cli/asmspy_graphsort.h \
              cli/asmspy_dataview.h cli/asmspy_treefilter.h \
              cli/asmspy_autoregion.h cli/asmspy_arch.h \
              cli/asmspy_ptracesample.h \
              cli/asmspy_syscall_name.h cli/asmspy_tidsort.h \
              include/asmtest_ptrace.h \
              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -c $< -o $@

$(BUILD)/asmspy_engine.o: $(BUILD)/asmspy_syscall_names.inc
$(BUILD)/asmspy_proc.o: $(BUILD)/asmspy_syscall_names.inc

# THE PERF-FREE PICKER'S EXHAUSTIVENESS GATE, and it is a correctness mechanism
# rather than a style flag.
#
# What to do with a stopped tracee is a TOTAL function of (whose address space
# it is in) x (why it stopped) — asmspy_ps_decide in cli/asmspy_ptracesample.c,
# written as nested switches with no `default:`. Four rounds of fixes to that
# file each closed the defects they were given and opened exactly one more of
# the same class: a task the conditions did not describe fell out of the bottom
# of an `if` chain and was handed a verb (PTRACE_CONT with a signal, DETACH,
# POKETEXT) anyway. The last one delivered our OWN int3's SIGTRAP to a process
# with no handler.
#
#   -Werror=switch       a new enumerator with no case is a build failure
#   -Werror=switch-enum  ... and stays one even if someone adds a `default:`,
#                        which would otherwise silence -Wswitch entirely
#
# Scoped to this object because it is where the tables live; WERROR=1 is only
# set in CI, and this must fail on a developer's box too. DEMONSTRATED, not
# asserted: adding a fourth asmspy_ps_kind_t breaks the build at every site that
# has not been revisited (asmspy_ps_decide, ps_kind_may_poke, ps_kind_may_sample).
PS_SWITCH_GATE := -Werror=switch -Werror=switch-enum
$(BUILD)/asmspy_ptracesample.o: CFLAGS += $(PS_SWITCH_GATE)
$(BUILD)/pic/asmspy_ptracesample.o: CFLAGS += $(PS_SWITCH_GATE)
# ... and fold the gate into the build-knob sentinel (Makefile:312), because a
# target-specific CFLAGS addition is invisible to it: GNU make compares mtimes,
# not recipe text, so weakening or removing the gate would otherwise leave the
# object built WITH it sitting there looking fresh — a build that still passes
# because it is the old build. This file is included at Makefile:1003, after
# BUILD_FLAGS is defined, so the append lands.
BUILD_FLAGS += $(PS_SWITCH_GATE)

# asmspy.o is the FRONT END (main, the headless subcommands, the ncurses TUI);
# the engine it drives is libasmspy below, not a loose object here.
ASMSPY_OBJS := $(BUILD)/asmspy.o $(BUILD)/asmtrace_ndjson.o

# --dataflow (Increment 6) links the scoped-ptrace L0 VALUE producer
# (dataflow_ptrace.o) plus its pure L0 sink / L1 def-use / L2 slicer (dataflow.o)
# and Capstone operand enumerator (dataflow_operands.o) — the same object set the
# producer's own test links. The producer ships no public header (asmspy_engine.c
# re-declares its entry point); off Linux x86-64 / without Capstone these compile
# to ENOSYS stubs and the subcommand self-skips. Rules live in mk/dataflow.mk.
ASMSPY_DATAFLOW_OBJS := $(BUILD)/dataflow_ptrace.o $(BUILD)/dataflow.o \
                        $(BUILD)/dataflow_operands.o

# --- libasmspy: the tracer engine as a linkable library (07 T0) --------------
# The engine (asmspy_engine.o) + the /proc/ELF/JIT resolver (asmspy_proc.o) used
# to be loose objects compiled straight into the binary with no public header,
# which is why anything else that wanted them — `--serve`, a future binding —
# would have had to re-declare or re-implement them. They are now a library with
# ONE public header (cli/libasmspy.h), mirroring how every other tier already
# ships (libasmtest_dataflow / _emu / _hwtrace): a static .a the CLI and its
# tests link, plus a shared .so.
#
# This is PACKAGING, not a boundary change. The desktop GUI still never links it
# (D9) — it reaches the engines only through the `asmspy --serve` subprocess.
ASMSPY_LIB_OBJS := $(BUILD)/asmspy_engine.o $(BUILD)/asmspy_proc.o \
                   $(BUILD)/asmspy_ptracesample.o
ASMSPY_LIB      := $(BUILD)/libasmspy.a

$(ASMSPY_LIB): $(ASMSPY_LIB_OBJS)
	$(AR) rcs $@ $^

# What a libasmspy consumer must ALSO link: the framework tier objects the engine
# calls into (the hwtrace attach/step seam + disasm, and the data-flow producer
# behind --dataflow). Named once here so the CLI, test_libasmspy and any future
# consumer cannot drift into different sets. NOT part of the .a — an archive of
# another tier's objects would duplicate them in every binary that links both.
ASMSPY_LIB_DEPS := $(HWTRACE_OBJS) $(ASMSPY_DATAFLOW_OBJS)
# -lstdc++ supplies __cxa_demangle (Itanium C++ ABI demangler) for the ELF symbol
# resolver (cli/asmspy_proc.c); the library is otherwise pure C — note NO ncurses,
# which is the front end's dependency, not the engine's.
ASMSPY_LIB_LIBS := $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) \
                   $(LINK_LIBBPF) -ldl -lstdc++

# The .a goes LAST: plain .o are linked unconditionally, so the archive is what
# gets searched for the engine symbols asmspy.o references, and it in turn finds
# its own tier symbols in the objects already on the line.
$(BUILD)/asmspy: $(ASMSPY_LIB_DEPS) $(ASMSPY_OBJS) $(ASMSPY_LIB)
	$(CC) $(CFLAGS) -pthread $^ $(ASMSPY_LIB_LIBS) $(NCURSES_LIBS) -o $@

# The shared build. PIC objects live beside every other tier's in $(BUILD)/pic/;
# the .so bundles the hwtrace + data-flow tiers it calls into (as
# libasmtest_dataflow bundles pic/codeimage.o) so it resolves standalone.
$(BUILD)/pic/asmspy_engine.o: cli/asmspy_engine.c cli/libasmspy.h \
                              cli/asmspy_arch.h cli/asmspy_syscall_name.h \
                              $(BUILD)/asmspy_syscall_names.inc \
                              $(BUILD)/.build-flags | $(BUILD)/pic
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -fPIC -c $< -o $@
$(BUILD)/pic/asmspy_proc.o: cli/asmspy_proc.c cli/libasmspy.h \
                            cli/asmspy_syscall_name.h cli/asmspy_tidsort.h \
                            $(BUILD)/asmspy_syscall_names.inc \
                            $(BUILD)/.build-flags | $(BUILD)/pic
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -fPIC -c $< -o $@

$(BUILD)/pic/asmspy_ptracesample.o: cli/asmspy_ptracesample.c \
                                    cli/asmspy_ptracesample.h cli/libasmspy.h \
                                    cli/asmspy_arch.h cli/asmspy_autoregion.h \
                                    $(BUILD)/.build-flags | $(BUILD)/pic
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -fPIC -c $< -o $@

ASMSPY_SHLIB_OBJS := $(BUILD)/pic/asmspy_engine.o $(BUILD)/pic/asmspy_proc.o \
    $(BUILD)/pic/asmspy_ptracesample.o \
    $(patsubst $(BUILD)/%,$(BUILD)/pic/%,$(NATIVE_TRACE_OBJS)) \
    $(BUILD)/pic/disasm.o $(BUILD)/pic/trace.o \
    $(BUILD)/pic/dataflow.o $(BUILD)/pic/dataflow_operands.o \
    $(BUILD)/pic/dataflow_ptrace.o

.PHONY: shared-asmspy
# Linux + x86-64/AArch64 only, for exactly the reasons `cli` is: the engine
# carries ~473 ptrace / user_regs references and has no body for another OS or
# architecture. That is a REAL gate (CLAUDE.md — nothing to install), so it self
# -skips with the measured reason rather than dumping compiler errors.
ifneq ($(UNAME_S),Linux)
shared-asmspy:
	@echo "# SKIP shared-asmspy: libasmspy is a Linux-only out-of-process tracer engine (ptrace / process_vm_readv / /proc); this host is $(UNAME_S)."
	@echo "#   Nothing to install — this is an OS gate, not a missing dependency."
else ifeq ($(filter $(CLI_ARCH),$(CLI_ARCH_SUPPORTED)),)
shared-asmspy:
	@echo "# SKIP shared-asmspy: libasmspy supports Linux x86-64 and AArch64; this host is $(CLI_ARCH)."
	@echo "#   Its register/single-step/detach reads (cli/asmspy_arch.h) have no body here."
else
shared-asmspy: $(call shlib_dev,libasmspy)
endif
$(call shlib_real,libasmspy): $(ASMSPY_SHLIB_OBJS)
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmspy) -pthread $^ \
	  $(ASMSPY_LIB_LIBS) -o $@
$(call shlib_dev,libasmspy): $(call shlib_real,libasmspy)
	ln -sf $(notdir $<) $(call shlib_compat,libasmspy)
	ln -sf $(notdir $(call shlib_compat,libasmspy)) $@

# asmspy is a Linux-only out-of-process tracer: its reads use ptrace(2),
# process_vm_readv(2), personality(2), /proc, <linux/futex.h>, <sys/user.h> and the
# glibc extension pthread_timedjoin_np — none of which exist on macOS/BSD, and its
# per-file victims include <sys/prctl.h>. On macOS the single-step tracer is the
# SEPARATE Mach-exception tier (src/mach_backend.c, `make mach-stepper-test`); asmspy
# has no Mach body. Like the arch gate this is a REAL gate (nothing to apt-install),
# and it is checked FIRST: on macOS `uname -m` is a SUPPORTED arch (x86_64), so
# without an OS gate the build would fall through and HARD-FAIL at process_vm_readv /
# pthread_timedjoin_np / <elf.h> instead of skipping transparently (per-file include guards
# cannot help — asmspy_engine.c alone carries ~473 Linux-only ptrace/user_regs refs).
.PHONY: cli
ifneq ($(UNAME_S),Linux)
cli:
	@echo "# SKIP cli: asmspy is a Linux-only out-of-process tracer (ptrace / process_vm_readv / personality / /proc); this host is $(UNAME_S)."
	@echo "#   Nothing to install — this is an OS gate, not a missing dependency. On macOS"
	@echo "#   the single-step tracer is the Mach-exception tier: make mach-stepper-test."
else ifeq ($(filter $(CLI_ARCH),$(CLI_ARCH_SUPPORTED)),)
cli:
	@echo "# SKIP cli: asmspy supports Linux x86-64 and AArch64; this host is $(CLI_ARCH)."
	@echo "#   Its register/single-step/detach reads (cli/asmspy_arch.h) have no body"
	@echo "#   for this architecture. Nothing to install — this is an architecture"
	@echo "#   gate, not a missing dependency."
else ifeq ($(strip $(CLI_MISSING)),)
cli: $(BUILD)/asmspy
	@echo "built $(BUILD)/asmspy — run it with no args for the TUI, or --help"
else
cli:
	@echo "asmspy is not buildable here — missing:$(CLI_MISSING)"
	@echo ""
	@echo "  Recommended — build + run it in a container (no host deps):"
	@echo "      make docker-cli"
	@echo ""
	@echo "  Or install the toolchain and retry (Debian/Ubuntu):"
	@echo "      sudo apt-get install -y libncurses-dev   # the TUI"
	@echo "      make deps                                # Capstone (+ emu deps)"
	@false
endif

# Headless smoke: spawn the example victims and drive asmspy's non-interactive
# subcommands against them (list / syms / trace / log). Proves the engine +
# resolver end to end without an interactive terminal (the TUI shares the same
# engine). Reuses examples/attach_victim (has hotfn) + examples/syscall_victim
# (does file I/O); both opt in via PR_SET_PTRACER_ANY so attach works in a plain
# container. `make docker-cli` runs this in the asmtest-cli image.
# a non-leaf victim (work -> helper) so the smoke also exercises the call-graph.
$(BUILD)/spy_victim: $(BUILD)/spy_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# threads_victim is multi-threaded (thread-follow smoke), so it links -pthread.
$(BUILD)/threads_victim: $(BUILD)/threads_victim.o
	$(CC) $(CFLAGS) -pthread $^ -o $@

# tid_victim runs two threads in DISTINCT functions (alpha_work/beta_work) for
# the --tid per-thread filter smoke; multi-threaded, so -pthread.
$(BUILD)/tid_victim: $(BUILD)/tid_victim.o
	$(CC) $(CFLAGS) -pthread $^ -o $@

# cpp_victim is C++ (its hot function demo::hot_loop(int) keeps a MANGLED ELF
# symbol) so the smoke can prove asmspy demangles it. Built with $(CXX)/$(CXXFLAGS)
# (from mk/bindings.mk) — a one-shot compile+link, no shared cli/ pattern rule.
$(BUILD)/cpp_victim: cli/cpp_victim.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

# jit_victim mmaps an anonymous executable region and self-registers it in
# /tmp/perf-<pid>.map (a JIT stand-in), so the smoke can prove asmspy resolves
# managed/JIT frames from the perf map. Compiles via the cli/ .o pattern rule.
$(BUILD)/jit_victim: $(BUILD)/jit_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# jitdump_victim publishes the same anonymous hot loop via the BINARY perf
# jitdump format instead (jit-<pid>.dump + the perf-style discovery mmap, NO
# text perf-map), so the smoke can prove the jitdump reader end to end.
$(BUILD)/jitdump_victim: $(BUILD)/jitdump_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# int3_victim executes its own int3 breakpoints under a SIGTRAP handler, so the
# smoke can prove asmspy re-injects an app-delivered SIGTRAP (si_code split)
# instead of swallowing it — and survives (CONT-, not SINGLESTEP-, re-inject).
$(BUILD)/int3_victim: $(BUILD)/int3_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# sample_victim spins a hot loop (hot_spin) instead of sleeping, so asmspy's
# --sample (AMD IBS-Op, out of band) has retired taken branches to sample and the
# smoke can assert the function is named. Self-skips off IBS like the ibs tier.
$(BUILD)/sample_victim: $(BUILD)/sample_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# auto_victim backs the --dataflow --auto smoke, and its SHAPE is the test:
# grind_forever() is entered ONCE and never returns (the residency winner — what a
# PC histogram picks, and an entry breakpoint there can never fire again), while
# entered_often() is called from its inner loop (the only pick the producer can
# actually catch). The two rules disagree, so "--auto picked entered_often" cannot
# pass by accident. quiet_helper() is never called: the negative control. Spins for
# real because IBS-Op samples retired ops — attach_victim's 5Hz hotfn yields ZERO
# samples in a 400ms window (measured). Compiles via the cli/ .o pattern rule.
$(BUILD)/auto_victim: $(BUILD)/auto_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# evex_victim backs the ymm16-31 smoke: its hot routines write the UPPER SIXTEEN
# vector registers, which the live ptrace producer used to decline. EVEX is an x86
# encoding, so this is an ARCHITECTURE sentinel like CLI_I386_VICTIM above —
# aarch64 has no ymm16 to read and the smoke says so rather than failing. The
# AVX-512 question is NOT decided here: the victim probes __builtin_cpu_supports
# at runtime and self-skips, so an x86-64 box without avx512f still BUILDS it (and
# so does a cross-built image), which keeps the CPU gate in one place. The
# target("avx512f,avx512vl") attribute is per-function, so no -m flag is needed and
# the file's own guard runs on plain baseline code. Compiles via the cli/ .o
# pattern rule.
ifeq ($(CLI_ARCH),x86_64)
CLI_EVEX_VICTIM := $(BUILD)/evex_victim $(BUILD)/test_hi16
else
CLI_EVEX_VICTIM :=
endif

$(BUILD)/evex_victim: $(BUILD)/evex_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# test_hi16 pins src/xstate_hi16.h's decision about WHETHER the upper sixteen are
# readable — the one input that matters most (component enumerated by the part but
# never enabled by the OS) cannot be produced on a running host, so it is tested as
# a pure function instead of via a capture. Header-only, so -Isrc and nothing to
# link; same shape as test_sha256 against cli/asmtrace_sha256.h.
$(BUILD)/test_hi16: cli/test_hi16.c src/xstate_hi16.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ cli/test_hi16.c

# sigload_victim backs the PERF-FREE picker's signal-fidelity check, and it is
# the only victim here that is busy AND signal-driven. auto_victim cannot do its
# job: MEASURED, an unconditional PTRACE_CONT(sig=0) in the residency sampler
# destroyed 89% of a 100 Hz ITIMER_REAL target's SIGALRMs and collapsed its
# throughput ~99%, while costing a signal-FREE spinner about 1% — i.e. the most
# destructive thing that sampler can do to a process it does not own is
# completely invisible against every other victim in this tree. Compiles via the
# cli/ .o pattern rule.
$(BUILD)/sigload_victim: $(BUILD)/sigload_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# forkhot_victim FORKS from inside its hot loop, and the child re-enters the
# same hot function. That is the only shape in this tree that can reach the
# perf-free picker's copy-on-write hazard: a fork inside an armed window hands
# the child a private copy of the planted int3, and the child -- a different
# process, with no tracer -- dies of SIGTRAP the first time it executes that
# entry. clone_victim covers the THREAD path (same address space,
# PTRACE_O_TRACECLONE); fork_victim forks once, two seconds in, then sleeps, so
# it is never hot enough to be armed. Compiles via the cli/ .o pattern rule.
$(BUILD)/forkhot_victim: $(BUILD)/forkhot_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# hotthreads_victim: several threads hammering ONE entry, so the perf-free
# picker's phase 3 gets CONCURRENT arrivals. Every other victim that reaches an
# armed window is single-threaded, so the step-and-rearm race -- a second thread
# trapping while the first is mid-step, whose stop is consumed by a pump that is
# not collecting arrivals -- had no coverage at all. Multi-threaded, so -pthread.
$(BUILD)/hotthreads_victim: $(BUILD)/hotthreads_victim.o
	$(CC) $(CFLAGS) -pthread $^ -o $@

# scenes_victim backs the documented 3D-scene screenshots
# (docs/guides/desktop-gui-scenes.md) and its SHAPE is the requirement: SSE lane
# writes for the LanePrism scene, a constantly-entered routine for the Invocation
# scene, threads across libc/libm for the ModuleRibbon, a strided heap walk for
# the terrain's data spans, and a --seed that changes DATA ONLY so two runs share
# a code_sha and can diverge. Needs -lpthread and -lm for the module spine.
#
# An ARCHITECTURE sentinel like CLI_EVEX_VICTIM above, and for the same reason:
# the file includes <emmintrin.h> unguarded because the SSE lane writes ARE the
# LanePrism scene's subject, not an implementation detail of it — desktop/ pins
# blend_tile's mix as measured constants (test_standalone.cpp:736, standalone.h:295
# "movdqa x5 and movd x1 of its 11"). A NEON rewrite would keep the name and
# change the thing being depicted, so aarch64 does not build it at all. Nothing is
# lost in the smoke: cli-smoke has only ever BUILT this victim, never run it (the
# runner is scripts/capture-shot-recordings.sh, an x86 doc-generation path).
ifeq ($(CLI_ARCH),x86_64)
CLI_SCENES_VICTIM := $(BUILD)/scenes_victim
else
CLI_SCENES_VICTIM :=
endif

$(BUILD)/scenes_victim: $(BUILD)/scenes_victim.o
	$(CC) $(CFLAGS) $^ -lpthread -lm -o $@

# 61 T7c: the serve-session recording test and the target it traces. The target
# is ALSO the crossings fixture's program (its syscalls span three
# SyscallClass families), so the two never drift apart.
$(BUILD)/serve_record_target: examples/serve_record_target.c
	$(CC) $(CFLAGS) -O0 -g $< -o $@

$(BUILD)/test_serve_record: cli/test_serve_record.c
	$(CC) $(CFLAGS) $< -o $@

# quiet_hot_victim backs the 39 T4 continuous-through-a-quiet-window smoke: hotfn
# goes HOT (dense entries), then QUIET (~1.5s never entered), then hot again, so a
# --continuous capture pinned to it must SURVIVE the quiet stretch and capture the
# later burst. A uniform-period victim cannot produce that reliably. Compiles via
# the cli/ .o pattern rule.
$(BUILD)/quiet_hot_victim: $(BUILD)/quiet_hot_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# watch_victim's WORKER thread (not the leader) stores a known magic into a known
# 8-byte global, so the --watch (hardware data-watchpoint) smoke can prove asmspy
# arms the watchpoint PER-THREAD (a leader-only arm would trap none of the writes)
# and captures the written value + faulting PC. Multi-threaded, so -pthread.
$(BUILD)/watch_victim: $(BUILD)/watch_victim.o
	$(CC) $(CFLAGS) -pthread $^ -o $@

# longjmp_victim: main setjmps, calls 3 deep, and longjmps straight back, then
# calls after_jump() from main at depth 0. longjmp discards those 3 frames with
# NO `ret` retiring, so a push-on-call/pop-on-ret depth COUNTER never comes back
# down and renders after_jump 3 levels too deep — forever. Backs the --tree
# return-address-stack smoke (Theme C).
$(BUILD)/longjmp_victim: $(BUILD)/longjmp_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# sigcall_victim: main spins on an INDIRECT call while a SIGUSR1 handler waits.
# The tracer forces a signal into the one-instruction window between arming the
# pending call and the call retiring (ASMSPY_TEST_SIGRACE), where the engine used
# to attribute the handler's entry to the call site. Backs the indirect-call
# attribution smoke (Theme C). Compiles via the cli/ .o pattern rule.
$(BUILD)/sigcall_victim: $(BUILD)/sigcall_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# argdecode_victim makes a fixed set of syscalls with KNOWN arguments (creating
# and non-creating opens, mmap/mprotect flag words, writev iovecs, a sigset, a
# signal number, an arity-0 getpid, a timespec) so the syscall arg-decoding smoke
# can assert the RENDERED TEXT exactly rather than "something plausible
# appeared". Backs Theme E. Compiles via the cli/ .o pattern rule.
$(BUILD)/argdecode_victim: $(BUILD)/argdecode_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# exit_victim is the one victim that EXITS on its own (~2s of nanosleep+getpid,
# then return 0), so the negative-`n` "run until exit" smoke can prove
# `--log`/`--stream <pid> -1` returns rc 0 when the target leaves. Every other
# victim loops forever, which is why that row was never testable. Backs Theme D.
# Compiles via the cli/ .o pattern rule.
$(BUILD)/exit_victim: $(BUILD)/exit_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# sock_victim opens a LISTENing TCP socket, a connected TCP pair (both ends in
# one process, so the expected endpoints are derivable from the port it prints)
# and an AF_UNIX socket bound to a path — for the fd->endpoint smoke, which
# proves asmspy renders the endpoint behind "socket:[inode]" rather than the
# inode. Compiles via the cli/ .o pattern rule.
$(BUILD)/sock_victim: $(BUILD)/sock_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# i386_victim is a REAL 32-bit tracee (-m32) for the EI_CLASS refusal smoke
# (asmspy-plan Theme F3). Dockerfile.cli installs gcc-multilib for exactly this,
# so `make docker-cli` — the lane this feature is verified in — always builds and
# runs it. A 32-bit process is not hardware or a credential, so per CLAUDE.md it
# is a dependency to add, not a gate to skip.
#
# The parse-time probe below exists ONLY for toolchains outside that lane (CI
# runs `make cli-smoke` on a bare GitHub runner, whose apt line lives in
# .github/workflows/ci.yml and would need `gcc-multilib` added to it). A recipe
# line that self-skips would not work here anyway: `command -v ... || exit 0` in
# a recipe exits only that line's shell and make runs the next one regardless —
# hence a parse-time conditional feeding a sentinel the smoke reads.
CLI_M32_PROBE := $(shell t=$$(mktemp -d 2>/dev/null) &&   printf 'int main(void){return 0;}' > $$t/m32.c &&   $(CC) -m32 $$t/m32.c -o $$t/m32 >/dev/null 2>&1 && echo yes; rm -rf $$t)
ifeq ($(CLI_M32_PROBE),yes)
CLI_I386_VICTIM := $(BUILD)/i386_victim
else
CLI_I386_VICTIM :=
endif

$(BUILD)/i386_victim: cli/i386_victim.c | $(BUILD)
	$(CC) $(CFLAGS) -m32 $< -o $@

# clone_victim spawns threads DURING the trace (every other threaded victim
# starts its workers before the attach, so they arrive via seize_threads' /proc
# scan instead). spawned_fn runs only on those post-attach clones. Backs the
# post-attach clone-following smoke (Theme D) and the thr_get OOM survival smoke
# (Theme C, via ASMSPY_TEST_THR_OOM). Multi-threaded, so -pthread.
$(BUILD)/clone_victim: $(BUILD)/clone_victim.o
	$(CC) $(CFLAGS) -pthread $^ -o $@

# exec_victim / exec_stage2 — the exec-stop re-resolution pair (Theme B).
# exec_victim runs preexec_fn, then execv()s exec_stage2, which runs postexec_fn.
# The two functions live in DIFFERENT binaries, so naming postexec_fn proves
# asmspy re-read the symtab at the exec-stop (the attach-time table is
# exec_victim's and describes a different image at a different load bias).
$(BUILD)/exec_victim: $(BUILD)/exec_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# exec_stage2 is FREESTANDING on purpose (-nostdlib): it is single-stepped from
# the exec-stop onwards, so anything it runs before postexec_fn is charged to the
# smoke's step budget. It used to be an ordinary -static glibc program, and
# MEASURED, 134,848 of its 136,072 steps were startup — 118,941 of those (88%)
# glibc registering the unwind tables (classify_object_over_fdes /
# read_encoded_value_with_base) of a libc this test never calls. That left a
# 1.47x margin on a count that moves with the environment (136,629 -> 140,893
# from ifunc dispatch alone), which is what turned CI red. -nostdlib drops the
# loader AND libc: postexec_fn is now ~20 steps past the exec.
#
# EXPLICIT flags, not $(CFLAGS), and that is the point of this rule: a
# freestanding TU must not inherit SAN=1 / COV=1 / the distro's default
# -fstack-protector-strong, all of which reference libc symbols that -nostdlib
# will not link. This is the one object here that cannot use the shared flags.
$(BUILD)/exec_stage2: cli/exec_stage2.c | $(BUILD)
	$(CC) -std=gnu11 -Wall -Wextra -O0 -g -static -nostdlib -nostartfiles \
	  -fno-stack-protector -fno-asynchronous-unwind-tables $< -o $@

# fork_victim forks; parent and child run DISTINCT functions and each open a
# DIFFERENT file at the SAME fd number (3, opened after the fork). Backs the
# --follow (strace -f parity) smoke: following the child, and resolving its fds
# through ITS OWN fd table rather than the parent's. Optional argv[1] = a binary
# the child execs, which also puts it in an image of its own.
$(BUILD)/fork_victim: $(BUILD)/fork_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# debuglink_victim carries a function (debuglink_only_fn) that lands in .symtab
# but NOT .dynsym, so the smoke can strip a copy, prove asmspy resolves nothing,
# then attach the debug info back as a separate .gnu_debuglink / build-id file and
# prove the symbol returns. Compiles via the cli/ .o pattern rule.
$(BUILD)/debuglink_victim: $(BUILD)/debuglink_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# test_logview — headless unit test for the TUI scrollback viewport math
# (cli/asmspy_logview.h); no ncurses, so it runs anywhere the smoke does.
$(BUILD)/test_logview: cli/test_logview.c cli/asmspy_logview.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_logview.c

# test_view — headless unit test for the data-flow view's pure render/analysis
# logic (cli/asmspy_dataview.h: value annotation, def-use in/out split, L2 slice
# highlight/dim), the Increment-7 TUI-mode-9 payoff the ncurses window can't be
# driven to exercise in CI. Links the PURE L0/L1/L2 sink object (dataflow.o —
# no Capstone, no ptrace) for asmtest_valtrace_* / _defuse_* / _slice_*.
$(BUILD)/test_view: cli/test_view.c cli/asmspy_dataview.h $(BUILD)/dataflow.o \
                    include/asmtest_valtrace.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_view.c $(BUILD)/dataflow.o

# test_asmtrace — headless unit test for the .asmtrace NDJSON writer
# (cli/asmtrace_ndjson.h) plus the reader-level fidelity checks: the schema doc's
# own embedded example is extracted and parsed here, so the written contract
# (docs/internal/gui/asmtrace-schema.md) cannot drift from the writer silently.
# Links ONLY the writer object — no ptrace, no ncurses, no Capstone — so it runs
# on every host the smoke does.
$(BUILD)/test_asmtrace: cli/test_asmtrace.c cli/asmtrace_ndjson.h \
                        $(BUILD)/asmtrace_ndjson.o | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_asmtrace.c $(BUILD)/asmtrace_ndjson.o

# test_graphsort — headless unit test for the call-graph sort comparator
# (cli/asmspy_graphsort.h, shared by --graph --sort=... and TUI mode 4).
$(BUILD)/test_graphsort: cli/test_graphsort.c cli/asmspy_graphsort.h \
                         cli/asmspy.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_graphsort.c

# test_ghash — headless unit test for the graph's open-addressed table index
# (cli/asmspy_ghash.h), with FORCED collisions. The engine's own graphs are too
# small to collide, so only this test can catch a probe loop that trusts the
# hash and skips the key compare (measured: that mutant is byte-identical in
# the smoke).
$(BUILD)/test_ghash: cli/test_ghash.c cli/asmspy_ghash.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_ghash.c

# test_sha256 — pins cli/asmtrace_sha256.h (the `code` header's routine-identity
# digest) against the published FIPS-180-4 vectors, so a transcription typo in
# the algorithm cannot pass as a wrong-but-stable hash the golden corpus blesses.
# Header-only + pure C, so it runs on every host the smoke does.
$(BUILD)/test_sha256: cli/test_sha256.c cli/asmtrace_sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_sha256.c

# test_treefilter — headless unit test for the call-tree output filter
# (cli/asmspy_treefilter.h: --tree --depth/--focus/--module). Replays scripted
# call/ret streams through the filter, so the focus open/close and depth re-base
# sequences are asserted directly instead of only via whatever shapes a live
# single-stepped victim happens to run.
$(BUILD)/test_treefilter: cli/test_treefilter.c cli/asmspy_treefilter.h \
                          | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_treefilter.c

# test_autoregion — headless unit test for the --dataflow --auto region picker
# (cli/asmspy_autoregion.h: rank the hottest ENTRY edge). This test carries the
# real burden for that feature: the sampler feeding the picker is AMD IBS-Op
# HARDWARE and self-skips everywhere else (including every GitHub runner), so a
# rule verified only end-to-end would be verified almost nowhere. The ranking is
# pure over hand-built edges, so it runs on ANY host — the live lane
# (docker-cli-ibs) is then only responsible for the wiring.
$(BUILD)/test_autoregion: cli/test_autoregion.c cli/asmspy_autoregion.h \
                          cli/asmspy.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_autoregion.c

# test_ptracesample — the PERF-FREE region picker (cli/asmspy_ptracesample.c).
# Unlike test_autoregion, which covers the pure RANKING, this one has to be
# live: the thing under test is what ptrace and /proc can observe about a
# running process, and every one of its checks pins a defect a prototype
# actually shipped (residency picks the un-re-enterable function; an
# unconditional PTRACE_CONT(sig=0) eats the target's signals; a thread cloned
# after the seize reaches the shared int3 untraced and dies). It spawns its own
# three victims.
#
# Links the sampler TU + the resolver TU (asmspy_proc.o, for asmspy_symtab_*)
# + disasm.o (asmtest_disas_call_target, phase 2). NOT the engine and NOT
# ncurses — the picker is meant to stand on its own, and this link line is what
# keeps it that way. -lstdc++ supplies asmspy_proc.o's __cxa_demangle.
$(BUILD)/test_ptracesample: cli/test_ptracesample.c \
                            $(BUILD)/asmspy_ptracesample.o \
                            $(BUILD)/asmspy_proc.o $(BUILD)/disasm.o \
                            $(BUILD)/trace.o \
                            cli/asmspy_ptracesample.h cli/libasmspy.h \
                            | $(BUILD) $(BUILD)/asmspy_syscall_names.inc
	$(CC) $(CFLAGS) -Icli -I$(BUILD) -pthread cli/test_ptracesample.c \
	  $(BUILD)/asmspy_ptracesample.o $(BUILD)/asmspy_proc.o \
	  $(BUILD)/disasm.o $(BUILD)/trace.o $(CAPSTONE_LIBS) -lstdc++ -o $@

# test_arch — headless unit test for the register/step/watch arch seam
# (cli/asmspy_arch.h): the per-arch register accessors (pc/sp/ret/lr/syscall-nr)
# and the AArch64 NT_ARM_HW_WATCH DBGWCR/DBGWVR/BAS control-word encoder. Both are
# pure (no ptrace, no hardware), so this runs green on EVERY host — and pins the
# AArch64 watchpoint encoding even on x86-64, where no AArch64 watchpoint can ever
# fire (the "pure module carries the burden" discipline test_autoregion uses).
$(BUILD)/test_arch: cli/test_arch.c cli/asmspy_arch.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_arch.c

# test_vmmap — the /proc/<pid>/maps parse + rank + JSON body encoder
# (cli/asmspy_vmmap.h). Carries the real burden for the vmmap kind: emission
# needs a live target and a serve session, but the parse and the CAP DISCIPLINE
# are pure over a FILE*, so they run on any host with no /proc and no victim —
# and "rank the full table, then cap" is the rule whose violation silently drops
# libc in favour of a 1 GiB anonymous reservation. The smoke lane is then only
# responsible for the wiring. Same discipline as test_autoregion above.
$(BUILD)/test_vmmap: cli/test_vmmap.c cli/asmspy_vmmap.h cli/asmtrace_ndjson.h \
                     $(BUILD)/asmtrace_ndjson.o | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_vmmap.c $(BUILD)/asmtrace_ndjson.o

# test_symtab — unit test for the symbol REVERSE lookup (asmspy_symtab_at), the
# one function every view that names an address goes through. Pins the edges
# where a resolver lies quietly instead of failing: one byte past a function,
# the gap between two functions, a zero-SIZE symbol, an address below the first.
# Links the resolver TU directly, like test_jitdump.
$(BUILD)/test_symtab: cli/test_symtab.c $(BUILD)/asmspy_proc.o cli/asmspy.h \
                      | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_symtab.c $(BUILD)/asmspy_proc.o \
	  -lstdc++ -o $@

# test_procinfo — the attach-free process snapshot (asmspy_procinfo). Its key
# assertion is NEGATIVE: the snapshot must succeed against a target we hold no
# ptrace permission for (our own parent, under Yama ptrace_scope=1), which a
# gatherer that quietly attached could not do. Links the resolver TU directly,
# like test_symtab.
$(BUILD)/test_procinfo: cli/test_procinfo.c $(BUILD)/asmspy_proc.o cli/asmspy.h \
                        cli/asmspy_tidsort.h \
                        | $(BUILD) $(BUILD)/asmspy_syscall_names.inc
	$(CC) $(CFLAGS) -Icli -I$(BUILD) -pthread cli/test_procinfo.c \
	  $(BUILD)/asmspy_proc.o -lstdc++ -o $@

# test_libasmspy — the standalone proof that libasmspy is a LIBRARY (07 T0).
# Its LINK LINE is the test: libasmspy.a + the tier objects the engine calls
# into, and NOT $(BUILD)/asmspy.o, and NOT $(NCURSES_LIBS). A hidden dependency
# on the CLI front end, or a TUI dependency leaking into the engine, fails here
# and nowhere else. Its SOURCE includes only cli/libasmspy.h, so a declaration
# left behind in asmspy.h fails to compile here for the same reason.
$(BUILD)/test_libasmspy: cli/test_libasmspy.c cli/libasmspy.h \
                         $(ASMSPY_LIB) $(ASMSPY_LIB_DEPS) | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_libasmspy.c $(ASMSPY_LIB_DEPS) \
	  $(ASMSPY_LIB) $(ASMSPY_LIB_LIBS) -o $@

# test_jitdump — unit test for the binary jitdump reader + the two-tier JIT
# resolve chain. Links the resolver TU (asmspy_proc.o) directly; -lstdc++
# supplies its __cxa_demangle just like the asmspy link line.
$(BUILD)/test_jitdump: cli/test_jitdump.c $(BUILD)/asmspy_proc.o \
                       cli/asmspy.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_jitdump.c $(BUILD)/asmspy_proc.o \
	  -lstdc++ -o $@

# ---------------------------------------------------------------------------
# asmtrace_record — the Author-mode conformance-corpus recorder + the golden
# corpus targets (docs/internal/archive/gui/01-asmtrace-format.md T6/T7).
#
# It runs corpus routines under the DETERMINISTIC emulator L0 value producer and
# writes one .asmtrace per routine, so the committed corpus is a byte-comparable
# fixture every reader/exporter/viewer test replays. It links the writer TU that
# asmspy links — one owner of field order — plus the pure L0/L1 sink, the
# Capstone operand enumerator, the emulator producer, and the corpus routine
# objects (the test_dataflow_emu link shape, mk/dataflow.mk).
$(BUILD)/corpus_routines.o: bindings/conformance/corpus_routines.c \
                            $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/asmtrace_record.o: tools/asmtrace_record.c cli/asmtrace_ndjson.h \
                            include/asmtest_valtrace.h include/asmtest_emu.h \
                            $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -Icli -c $< -o $@

# The routine objects carry the corpus's assembly (add.o: add_signed;
# flags.o: set_carry/clear_carry/sum_via_rbx/clobbers_rbx; args.o: sum3) and
# the rest satisfy corpus_routines.c's other externs.
ASMTRACE_ROUTINE_OBJS := $(BUILD)/add.o $(BUILD)/args.o $(BUILD)/flags.o \
                         $(BUILD)/fp.o $(BUILD)/simd.o $(BUILD)/structs.o \
                         $(BUILD)/fault.o $(BUILD)/corpus_routines.o

$(BUILD)/asmtrace_record: $(BUILD)/asmtrace_record.o \
                          $(BUILD)/asmtrace_ndjson.o $(BUILD)/dataflow.o \
                          $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
                          $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o \
                          $(ASMTRACE_ROUTINE_OBJS)
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

# The golden corpus is x86-64-only (the corpus routines are HOST-ARCH assembly,
# the same gate the conformance emulator leg carries) and needs libunicorn for
# the producer. libunicorn/Capstone are installable and present in the lane
# image, so their absence points at the container rather than self-skipping the
# feature; the architecture is a REAL gate (CLAUDE.md) and is recorded as one.
ASMTRACE_GOLDEN_DIR := tests/golden-asmtrace
ASMTRACE_GOLDEN_OK := $(and $(filter x86_64,$(CLI_ARCH)),$(DF_HAVE_UNICORN))

.PHONY: asmtrace-golden asmtrace-golden-check
ifneq ($(ASMTRACE_GOLDEN_OK),)
asmtrace-golden: $(BUILD)/asmtrace_record
	@mkdir -p $(ASMTRACE_GOLDEN_DIR)
	$(BUILD)/asmtrace_record $(ASMTRACE_GOLDEN_DIR)
	@echo "regenerated $(ASMTRACE_GOLDEN_DIR)/*.asmtrace — commit them from the"
	@echo "docker-cli image only (host Capstone 4.x renders different disasm text)"

# Regenerate into a temp dir and diff: a field-order or value change fails here
# as a byte diff instead of passing quietly. Compares only the flat generated
# files — low-fidelity/ is hand-authored and never regenerated.
asmtrace-golden-check: $(BUILD)/asmtrace_record
	@tmp=$$(mktemp -d) && $(BUILD)/asmtrace_record $$tmp >/dev/null && \
	  for f in $$tmp/*.asmtrace; do \
	    b=$$(basename $$f); \
	    cmp -s $$f $(ASMTRACE_GOLDEN_DIR)/$$b || { \
	      echo "asmtrace-golden-check: $$b differs from the committed golden"; \
	      diff $(ASMTRACE_GOLDEN_DIR)/$$b $$f | head -20; \
	      rm -rf $$tmp; exit 1; }; \
	  done; \
	  n=$$(ls $$tmp/*.asmtrace | wc -l); rm -rf $$tmp; \
	  echo "asmtrace-golden-check: $$n recordings byte-identical to the corpus"
else
asmtrace-golden asmtrace-golden-check:
	@echo "# SKIP $@: the golden .asmtrace corpus is generated from HOST-ARCH corpus"
	@echo "#   assembly under the emulator L0 producer — it needs an x86-64 host"
	@echo "#   (this host is $(CLI_ARCH))$(if $(DF_HAVE_UNICORN),, and libunicorn, which docker-cli has)."
	@echo "#   The committed corpus is authoritative; regenerate it with make docker-cli."
endif

# test_regstate_parity (26 T5.2) — the load-bearing cross-producer check: one
# deterministic routine through BOTH the live single-step ptrace producer (regfile
# ring armed) and the trusted emulator ring, asserting their per-step register files
# agree on the operand-visible computation registers (rax/rdi/rsi), differing only
# in the base-dependent rip/rsp (real ASLR vs the emulator's fixed EMU_CODE_BASE).
# Links both producers — the asmtrace_record emulator objects PLUS the ptrace
# producer (dataflow_ptrace.o + codeimage.o) — and Unicorn/Capstone. Same
# x86_64 + libunicorn gate as the golden (ASMTRACE_GOLDEN_OK); a run-time ptrace
# refusal (seccomp) self-skips transparently.
$(BUILD)/test_regstate_parity: cli/test_regstate_parity.c \
                          $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                          $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
                          $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_ptrace.o \
                          $(BUILD)/codeimage.o $(BUILD)/emu.o $(BUILD)/trace.o \
                          $(BUILD)/disasm.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Icli -pthread $^ \
	  $(UNICORN_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -o $@

ifneq ($(ASMTRACE_GOLDEN_OK),)
CLI_REGSTATE_PARITY := $(BUILD)/test_regstate_parity
else
CLI_REGSTATE_PARITY :=
endif

# test_mem_parity (29 R2 T3) — the cross-producer check for the `mem` address
# stream: one load/store routine through BOTH the live single-step ptrace producer
# and the trusted emulator L0 producer, asserting their per-access `mem` streams
# agree on the base-independent structure (count + each access's step/size/rw)
# while the effective addresses differ (real ASLR'd stack vs the emulator's fixed
# DF_STACK_BASE). Same link set and x86_64 + libunicorn gate as the regstate parity
# above; a run-time ptrace refusal (seccomp) self-skips transparently.
$(BUILD)/test_mem_parity: cli/test_mem_parity.c \
                          $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                          $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
                          $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_ptrace.o \
                          $(BUILD)/codeimage.o $(BUILD)/emu.o $(BUILD)/trace.o \
                          $(BUILD)/disasm.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Icli -pthread $^ \
	  $(UNICORN_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -o $@

ifneq ($(ASMTRACE_GOLDEN_OK),)
CLI_MEM_PARITY := $(BUILD)/test_mem_parity
else
CLI_MEM_PARITY :=
endif

# test_reweave (30 R3 T1/T2) — the resume-from-state producer checks:
#   T1: the emu_t-HOSTED value producer (asmtest_dataflow_emu_run_hosted, on a
#       fresh emu_open handle) yields a valtrace BYTE-IDENTICAL to the standalone
#       asmtest_dataflow_emu_run for every corpus routine — the re-host is a pure
#       refactor with zero observable change (the strongest byte-identity proof).
#   T2: a checkpoint at step K then a resume reproduces the ORIGINAL run's tail
#       (K->end) byte-for-byte, and a resume after a register edit at K diverges
#       only where the edit reaches.
# Links the emulator producer + the HOSTED/resume TU (dataflow_resume.o) + emu.o
# (the snapshot/restore keystone) + a corpus routine object. Same x86_64 +
# libunicorn gate as the parity tests above.
$(BUILD)/test_reweave: cli/test_reweave.c \
                       $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                       $(BUILD)/dataflow_gcmove.o $(BUILD)/dataflow_method.o \
                       $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_resume.o \
                       $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Icli -pthread $^ \
	  $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

ifneq ($(ASMTRACE_GOLDEN_OK),)
CLI_REWEAVE := $(BUILD)/test_reweave
else
CLI_REWEAVE :=
endif

.PHONY: cli-smoke
ifneq ($(UNAME_S),Linux)
# Same OS gate as `cli` above: asmspy is a Linux-only ptrace/proc tracer and its
# victims include <sys/prctl.h>/<linux/futex.h>, so there is nothing to smoke off
# Linux — and the prerequisites below would hard-fail to compile before the smoke
# ever runs. A green skip is legitimate: the smoke measures asmspy, and there is no
# asmspy off Linux.
cli-smoke:
	@echo "# SKIP cli-smoke: asmspy is Linux-only (this host is $(UNAME_S)); nothing to measure."
else ifeq ($(filter $(CLI_ARCH),$(CLI_ARCH_SUPPORTED)),)
# Same architecture gate as `cli` above: without it, the smoke's prerequisites
# try to compile TUs that have no register body for this machine and dump the raw
# errors the gate exists to replace. A green skip here is legitimate — the smoke
# measures asmspy, and there is no asmspy to measure on this architecture
# (recorded, per CLAUDE.md's hardware-gate rule).
cli-smoke:
	@echo "# SKIP cli-smoke: asmspy supports Linux x86-64 and AArch64; this host is $(CLI_ARCH)"
else
cli-smoke: $(BUILD)/asmspy $(BUILD)/attach_victim $(BUILD)/syscall_victim \
           $(BUILD)/spy_victim $(BUILD)/threads_victim $(BUILD)/cpp_victim \
           $(BUILD)/jit_victim $(BUILD)/jitdump_victim $(BUILD)/int3_victim \
           $(BUILD)/tid_victim $(BUILD)/sample_victim $(BUILD)/watch_victim \
           $(BUILD)/auto_victim $(BUILD)/quiet_hot_victim $(CLI_SCENES_VICTIM) \
           $(BUILD)/sigload_victim $(BUILD)/forkhot_victim \
           $(BUILD)/hotthreads_victim \
           $(BUILD)/debuglink_victim $(BUILD)/test_arch $(BUILD)/test_logview \
           $(BUILD)/test_graphsort $(BUILD)/test_jitdump $(BUILD)/test_view \
           $(BUILD)/test_treefilter $(BUILD)/test_symtab $(BUILD)/test_autoregion \
           $(BUILD)/test_ptracesample $(BUILD)/test_procinfo \
           $(BUILD)/test_ghash $(BUILD)/test_sha256 $(BUILD)/test_asmtrace \
           $(BUILD)/test_vmmap \
           $(BUILD)/test_libasmspy \
           $(BUILD)/exec_victim $(BUILD)/exec_stage2 \
           $(BUILD)/fork_victim $(BUILD)/clone_victim \
           $(BUILD)/sock_victim $(BUILD)/longjmp_victim \
           $(BUILD)/sigcall_victim $(BUILD)/argdecode_victim \
           $(BUILD)/exit_victim $(CLI_I386_VICTIM) $(CLI_EVEX_VICTIM) \
           $(CLI_REGSTATE_PARITY) \
           $(CLI_MEM_PARITY) $(CLI_REWEAVE) \
           $(BUILD)/test_serve_record $(BUILD)/serve_record_target
	@echo "== cli-smoke =="
	@echo "   disassembler: Capstone $$(pkg-config --modversion capstone 2>/dev/null || echo '?')" \
	      "(5.x = pinned 5.0.1 source; 4.x = apt, some disasm silently degraded)"
	@echo "--- asmtrace-golden-check (the committed .asmtrace corpus is byte-stable) ---"
	@$(MAKE) --no-print-directory asmtrace-golden-check
	@echo "--- regstate parity (26 T5.2: live ptrace ring == emulator ring, modulo base) ---"
	@if [ -x "$(BUILD)/test_regstate_parity" ]; then $(BUILD)/test_regstate_parity; \
	 else echo "# SKIP regstate-parity: needs x86_64 + libunicorn (this host: $(CLI_ARCH))"; fi
	@echo "--- mem parity (29 R2 T3: live ptrace mem stream == emulator, modulo base) ---"
	@if [ -x "$(BUILD)/test_mem_parity" ]; then $(BUILD)/test_mem_parity; \
	 else echo "# SKIP mem-parity: needs x86_64 + libunicorn (this host: $(CLI_ARCH))"; fi
	@echo "--- reweave (30 R3: emu_t-hosted run == standalone; checkpoint/resume tail identity) ---"
	@if [ -x "$(BUILD)/test_reweave" ]; then $(BUILD)/test_reweave; \
	 else echo "# SKIP reweave: needs x86_64 + libunicorn (this host: $(CLI_ARCH))"; fi
	@echo "--- procinfo (the attach-free process snapshot) ---"
	@$(BUILD)/test_procinfo
	@echo "--- test_serve_record (61 T7c: a serve session is ONE recording) ---"
	$(BUILD)/test_serve_record
	BUILD=$(BUILD) ASMSPY_HAVE_M32='$(CLI_M32_PROBE)' sh cli/cli_smoke.sh
endif

# Build the CLI image (bindings base + libipt-dev + libncurses-dev) and run the
# headless smoke. Interactive use: `docker run --rm -it asmtest-cli bash` then
# `./build/asmspy`.
.PHONY: docker-cli
docker-cli: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.cli \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-cli .
	$(DOCKER) run --rm $(_docker_plat) asmtest-cli

# docker-cli-ibs — the SAME image and smoke, but with perf access so the --sample
# (AMD IBS-Op) block actually RUNS instead of self-skipping. Mirrors
# docker-hwtrace-amd (mk/docker.mk), and exists for the same measured reason.
#
# WHY THIS LANE EXISTS. `docker-cli` above is a PLAIN `docker run` — deliberately,
# because every ptrace engine needs no privilege (the victims opt in via
# PR_SET_PTRACER_ANY, so Yama ptrace_scope=1 is satisfied without CAP_SYS_PTRACE).
# But Docker's DEFAULT SECCOMP PROFILE BLOCKS perf_event_open, so --sample there
# ALWAYS self-skips, and cli_smoke.sh's `if grep -q '^# SKIP --sample'; then
# <accept>; else <every assertion> fi` takes the skip branch every time. Measured
# 2026-07-17 on a Zen 5 9950X: `make docker-cli` reports cli-smoke PASS while the
# sampler's assertions have NEVER run — a green gate over an untested view. Per
# CLAUDE.md, IBS *hardware* is a legitimate self-skip gate, but a capability flag
# on a run line is NOT: the hardware is present, only the flags were missing.
#
# Both flags are REQUIRED and were measured independently (paranoid=4 host):
#   plain                                    -> EPERM  (seccomp blocks the syscall)
#   --cap-add=PERFMON + seccomp=unconfined   -> fd=3   OK
# CAP_PERFMON BYPASSES perf_event_paranoid — the host sysctl does NOT need
# lowering (mk/docker.mk:536 claims otherwise; that claim is measured false).
# On a non-AMD host --sample still self-skips, transparently: that part IS hardware.
.PHONY: docker-cli-ibs
docker-cli-ibs: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.cli \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-cli .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  --cap-add=PERFMON asmtest-cli
