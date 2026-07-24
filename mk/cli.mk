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
# asmspy_engine.c #includes it, hence the -I$(BUILD) below.
$(BUILD)/asmspy_syscall_names.inc: cli/gen-syscall-names.sh | $(BUILD)
	CC='$(CC)' sh $< >$@

# cli/ sources compile like examples/, but with -pthread (a dedicated tracer
# thread owns the ptrace loop while the ncurses UI thread stays responsive).
$(BUILD)/%.o: cli/%.c cli/asmspy.h cli/asmspy_graphsort.h \
              cli/asmspy_dataview.h cli/asmspy_treefilter.h \
              cli/asmspy_autoregion.h cli/asmspy_arch.h \
              include/asmtest_ptrace.h \
              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -c $< -o $@

$(BUILD)/asmspy_engine.o: $(BUILD)/asmspy_syscall_names.inc

ASMSPY_OBJS := $(BUILD)/asmspy.o $(BUILD)/asmspy_proc.o $(BUILD)/asmspy_engine.o \
               $(BUILD)/asmtrace_ndjson.o

# --dataflow (Increment 6) links the scoped-ptrace L0 VALUE producer
# (dataflow_ptrace.o) plus its pure L0 sink / L1 def-use / L2 slicer (dataflow.o)
# and Capstone operand enumerator (dataflow_operands.o) — the same object set the
# producer's own test links. The producer ships no public header (asmspy_engine.c
# re-declares its entry point); off Linux x86-64 / without Capstone these compile
# to ENOSYS stubs and the subcommand self-skips. Rules live in mk/dataflow.mk.
ASMSPY_DATAFLOW_OBJS := $(BUILD)/dataflow_ptrace.o $(BUILD)/dataflow.o \
                        $(BUILD)/dataflow_operands.o

# -lstdc++ supplies __cxa_demangle (Itanium C++ ABI demangler) for the ELF symbol
# resolver (cli/asmspy_proc.c); asmspy is otherwise pure C.
$(BUILD)/asmspy: $(HWTRACE_OBJS) $(ASMSPY_OBJS) $(ASMSPY_DATAFLOW_OBJS)
	$(CC) $(CFLAGS) -pthread $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) \
	  $(LINK_LIBBPF) -ldl $(NCURSES_LIBS) -lstdc++ -o $@

# asmspy is a Linux-only out-of-process tracer: its reads use ptrace(2),
# process_vm_readv(2), personality(2), /proc, <linux/futex.h>, <sys/user.h> and the
# glibc extension pthread_timedjoin_np — none of which exist on macOS/BSD, and its
# per-file victims include <sys/prctl.h>. On macOS the single-step tracer is the
# SEPARATE Mach-exception tier (src/mach_backend.c, `make mach-stepper-test`); asmspy
# has no Mach body. Like the arch gate this is a REAL gate (nothing to apt-install),
# and it is checked FIRST: on macOS `uname -m` is a SUPPORTED arch (x86_64), so
# without an OS gate the build would fall through and HARD-FAIL at process_vm_readv /
# pthread_timedjoin_np / <elf.h> instead of skipping honestly (per-file include guards
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
# (cli/asmtrace_ndjson.h) plus the reader-level honesty checks: the schema doc's
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

# test_arch — headless unit test for the register/step/watch arch seam
# (cli/asmspy_arch.h): the per-arch register accessors (pc/sp/ret/lr/syscall-nr)
# and the AArch64 NT_ARM_HW_WATCH DBGWCR/DBGWVR/BAS control-word encoder. Both are
# pure (no ptrace, no hardware), so this runs green on EVERY host — and pins the
# AArch64 watchpoint encoding even on x86-64, where no AArch64 watchpoint can ever
# fire (the "pure module carries the burden" discipline test_autoregion uses).
$(BUILD)/test_arch: cli/test_arch.c cli/asmspy_arch.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_arch.c

# test_symtab — unit test for the symbol REVERSE lookup (asmspy_symtab_at), the
# one function every view that names an address goes through. Pins the edges
# where a resolver lies quietly instead of failing: one byte past a function,
# the gap between two functions, a zero-SIZE symbol, an address below the first.
# Links the resolver TU directly, like test_jitdump.
$(BUILD)/test_symtab: cli/test_symtab.c $(BUILD)/asmspy_proc.o cli/asmspy.h \
                      | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_symtab.c $(BUILD)/asmspy_proc.o \
	  -lstdc++ -o $@

# test_jitdump — unit test for the binary jitdump reader + the two-tier JIT
# resolve chain. Links the resolver TU (asmspy_proc.o) directly; -lstdc++
# supplies its __cxa_demangle just like the asmspy link line.
$(BUILD)/test_jitdump: cli/test_jitdump.c $(BUILD)/asmspy_proc.o \
                       cli/asmspy.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_jitdump.c $(BUILD)/asmspy_proc.o \
	  -lstdc++ -o $@

.PHONY: cli-smoke
ifneq ($(UNAME_S),Linux)
# Same OS gate as `cli` above: asmspy is a Linux-only ptrace/proc tracer and its
# victims include <sys/prctl.h>/<linux/futex.h>, so there is nothing to smoke off
# Linux — and the prerequisites below would hard-fail to compile before the smoke
# ever runs. A green skip is honest: the smoke measures asmspy, and there is no
# asmspy off Linux.
cli-smoke:
	@echo "# SKIP cli-smoke: asmspy is Linux-only (this host is $(UNAME_S)); nothing to measure."
else ifeq ($(filter $(CLI_ARCH),$(CLI_ARCH_SUPPORTED)),)
# Same architecture gate as `cli` above: without it, the smoke's prerequisites
# try to compile TUs that have no register body for this machine and dump the raw
# errors the gate exists to replace. A green skip here is honest — the smoke
# measures asmspy, and there is no asmspy to measure on this architecture
# (recorded, per CLAUDE.md's hardware-gate rule).
cli-smoke:
	@echo "# SKIP cli-smoke: asmspy supports Linux x86-64 and AArch64; this host is $(CLI_ARCH)"
else
cli-smoke: $(BUILD)/asmspy $(BUILD)/attach_victim $(BUILD)/syscall_victim \
           $(BUILD)/spy_victim $(BUILD)/threads_victim $(BUILD)/cpp_victim \
           $(BUILD)/jit_victim $(BUILD)/jitdump_victim $(BUILD)/int3_victim \
           $(BUILD)/tid_victim $(BUILD)/sample_victim $(BUILD)/watch_victim \
           $(BUILD)/auto_victim \
           $(BUILD)/debuglink_victim $(BUILD)/test_arch $(BUILD)/test_logview \
           $(BUILD)/test_graphsort $(BUILD)/test_jitdump $(BUILD)/test_view \
           $(BUILD)/test_treefilter $(BUILD)/test_symtab $(BUILD)/test_autoregion \
           $(BUILD)/test_ghash $(BUILD)/test_asmtrace \
           $(BUILD)/exec_victim $(BUILD)/exec_stage2 \
           $(BUILD)/fork_victim $(BUILD)/clone_victim \
           $(BUILD)/sock_victim $(BUILD)/longjmp_victim \
           $(BUILD)/sigcall_victim $(BUILD)/argdecode_victim \
           $(BUILD)/exit_victim $(CLI_I386_VICTIM)
	@echo "== cli-smoke =="
	@echo "   disassembler: Capstone $$(pkg-config --modversion capstone 2>/dev/null || echo '?')" \
	      "(5.x = pinned 5.0.1 source; 4.x = apt, some disasm silently degraded)"
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
# On a non-AMD host --sample still self-skips, honestly: that part IS hardware.
.PHONY: docker-cli-ibs
docker-cli-ibs: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.cli \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-cli .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  --cap-add=PERFMON asmtest-cli
