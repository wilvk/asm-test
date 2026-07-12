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
$(BUILD)/%.o: cli/%.c cli/asmspy.h include/asmtest_ptrace.h \
              include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
	$(CC) $(CFLAGS) -I$(BUILD) -pthread -c $< -o $@

$(BUILD)/asmspy_engine.o: $(BUILD)/asmspy_syscall_names.inc

ASMSPY_OBJS := $(BUILD)/asmspy.o $(BUILD)/asmspy_proc.o $(BUILD)/asmspy_engine.o

# -lstdc++ supplies __cxa_demangle (Itanium C++ ABI demangler) for the ELF symbol
# resolver (cli/asmspy_proc.c); asmspy is otherwise pure C.
$(BUILD)/asmspy: $(HWTRACE_OBJS) $(ASMSPY_OBJS)
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

# int3_victim executes its own int3 breakpoints under a SIGTRAP handler, so the
# smoke can prove asmspy re-injects an app-delivered SIGTRAP (si_code split)
# instead of swallowing it — and survives (CONT-, not SINGLESTEP-, re-inject).
$(BUILD)/int3_victim: $(BUILD)/int3_victim.o
	$(CC) $(CFLAGS) $^ -o $@

# test_logview — headless unit test for the TUI scrollback viewport math
# (cli/asmspy_logview.h); no ncurses, so it runs anywhere the smoke does.
$(BUILD)/test_logview: cli/test_logview.c cli/asmspy_logview.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -o $@ cli/test_logview.c

.PHONY: cli-smoke
cli-smoke: $(BUILD)/asmspy $(BUILD)/attach_victim $(BUILD)/syscall_victim \
           $(BUILD)/spy_victim $(BUILD)/threads_victim $(BUILD)/cpp_victim \
           $(BUILD)/jit_victim $(BUILD)/int3_victim $(BUILD)/tid_victim \
           $(BUILD)/test_logview
	@echo "== cli-smoke =="
	BUILD=$(BUILD) sh cli/cli_smoke.sh

# Build the CLI image (bindings base + libipt-dev + libncurses-dev) and run the
# headless smoke. Interactive use: `docker run --rm -it asmtest-cli bash` then
# `./build/asmspy`.
.PHONY: docker-cli
docker-cli: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.cli \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-cli .
	$(DOCKER) run --rm $(_docker_plat) asmtest-cli
