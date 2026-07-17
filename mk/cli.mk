# cli.mk — asmspy, an ncurses front-end over the out-of-process tracer (cli/).
#
# Links the hwtrace tier objects (run_to / trace_attached_ex + descent + disasm)
# plus ncursesw + pthread. asmspy carries its own /proc lister and ELF .symtab/
# .dynsym function resolver (cli/asmspy_proc.c) because the library has none.
# Modeled on the examples/jit_trace link line (mk/native-trace.mk).

# ncursesw dev files ship in libncurses-dev on Ubuntu (Dockerfile.cli adds it).
NCURSES_LIBS ?= $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncursesw)

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
              include/asmtest_ptrace.h \
              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -c $< -o $@

$(BUILD)/asmspy_engine.o: $(BUILD)/asmspy_syscall_names.inc

ASMSPY_OBJS := $(BUILD)/asmspy.o $(BUILD)/asmspy_proc.o $(BUILD)/asmspy_engine.o

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

.PHONY: cli
ifeq ($(strip $(CLI_MISSING)),)
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

# exec_stage2 is linked -static ON PURPOSE. After an execve the dynamic loader
# runs first, and relocating a PIE against a shared libc costs well over 20k
# instructions before main() is ever reached — so a single-step smoke would burn
# its whole budget inside ld.so and never see postexec_fn, whether or not the
# re-resolution works. Static linking removes ld.so from the post-exec path
# (_start -> __libc_start_main -> main), which puts postexec_fn within a few
# thousand steps. This is a TEST-RUNTIME concession, not a narrowing of the
# feature: the reload is keyed off the exec-stop and /proc/<pid>/maps, neither of
# which cares how the new image is linked. Built from the source directly — the
# cli/ .o pattern rule's object is shared with the dynamic link line.
$(BUILD)/exec_stage2: cli/exec_stage2.c | $(BUILD)
	$(CC) $(CFLAGS) -static $< -o $@

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

# test_graphsort — headless unit test for the call-graph sort comparator
# (cli/asmspy_graphsort.h, shared by --graph --sort=... and TUI mode 4).
$(BUILD)/test_graphsort: cli/test_graphsort.c cli/asmspy_graphsort.h \
                         cli/asmspy.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_graphsort.c

# test_treefilter — headless unit test for the call-tree output filter
# (cli/asmspy_treefilter.h: --tree --depth/--focus/--module). Replays scripted
# call/ret streams through the filter, so the focus open/close and depth re-base
# sequences are asserted directly instead of only via whatever shapes a live
# single-stepped victim happens to run.
$(BUILD)/test_treefilter: cli/test_treefilter.c cli/asmspy_treefilter.h \
                          | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_treefilter.c

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
cli-smoke: $(BUILD)/asmspy $(BUILD)/attach_victim $(BUILD)/syscall_victim \
           $(BUILD)/spy_victim $(BUILD)/threads_victim $(BUILD)/cpp_victim \
           $(BUILD)/jit_victim $(BUILD)/jitdump_victim $(BUILD)/int3_victim \
           $(BUILD)/tid_victim $(BUILD)/sample_victim $(BUILD)/watch_victim \
           $(BUILD)/debuglink_victim $(BUILD)/test_logview \
           $(BUILD)/test_graphsort $(BUILD)/test_jitdump $(BUILD)/test_view \
           $(BUILD)/test_treefilter $(BUILD)/test_symtab $(BUILD)/exec_victim $(BUILD)/exec_stage2 \
           $(BUILD)/fork_victim $(BUILD)/clone_victim \
           $(BUILD)/sock_victim $(BUILD)/longjmp_victim \
           $(BUILD)/sigcall_victim $(CLI_I386_VICTIM)
	@echo "== cli-smoke =="
	@echo "   disassembler: Capstone $$(pkg-config --modversion capstone 2>/dev/null || echo '?')" \
	      "(5.x = pinned 5.0.1 source; 4.x = apt, some disasm silently degraded)"
	BUILD=$(BUILD) ASMSPY_HAVE_M32='$(CLI_M32_PROBE)' sh cli/cli_smoke.sh

# Build the CLI image (bindings base + libipt-dev + libncurses-dev) and run the
# headless smoke. Interactive use: `docker run --rm -it asmtest-cli bash` then
# `./build/asmspy`.
.PHONY: docker-cli
docker-cli: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.cli \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-cli .
	$(DOCKER) run --rm $(_docker_plat) asmtest-cli
