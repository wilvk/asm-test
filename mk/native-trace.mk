# native-trace.mk — Native runtime-trace tiers: DynamoRIO (in-process) and hardware (Intel PT / CoreSight).
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

# CORPUS_LIB is the conformance fixture lib the per-binding hwtrace test lanes below
# link (via each crate's build.rs / cgo). It is fully defined in mk/bindings.mk, but
# that file is included AFTER this one, so a bare `$(CORPUS_LIB)` in a prerequisite
# here would expand to EMPTY (Make expands prerequisites when the rule is read) — the
# lane would then never build the fixture and cargo/cgo would fail to link
# -lasmtest_corpus. Define it here too (identical `:=`; bindings.mk redefines it
# harmlessly) so the `hwtrace-rust-test` / `hwtrace-go-test` corpus prereq resolves.
ifeq ($(UNAME_S),Darwin)
CORPUS_LIB := $(BUILD)/libasmtest_corpus.dylib
else
CORPUS_LIB := $(BUILD)/libasmtest_corpus.so
endif

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

# Flag-identity tripwire for drtrace_app.o. The object is compiled with
# $(DRAPP_KS_DEF) (Keystone on/off), but that flag is NOT among the rule's
# prerequisites, so flipping DRAPP_KEYSTONE between sub-makes in the SAME build tree
# would silently reuse a stale object. That is not hypothetical: the per-binding
# lanes below invoke `$(MAKE) shared-drtrace ... DRAPP_KEYSTONE=0` because a
# Keystone-enabled drapp .so has unresolved emu_* symbols and won't dlopen — so a
# reused Keystone-on object breaks exactly those lanes (CI only escapes it by running
# each in a clean tree). Record the full compile-flag string in a sentinel that
# changes only when the flags do (Keystone, and by extension SAN/COV via CFLAGS), and
# make the objects depend on it so they rebuild precisely when a knob flips.
DRAPP_FLAGS := $(strip $(CFLAGS) $(DRAPP_KS_DEF) $(KEYSTONE_CFLAGS))
$(BUILD)/.drapp-flags: FORCE | $(BUILD)
	@printf '%s\n' '$(DRAPP_FLAGS)' | cmp -s - $@ || printf '%s\n' '$(DRAPP_FLAGS)' > $@
.PHONY: FORCE
FORCE:

# App-side library object (lifecycle + markers + W^X exec memory). No DR headers
# or link dependency (it dlopen()s libdynamorio and declares dr_app_* via dlsym),
# so it always compiles regardless of whether DynamoRIO is installed.
$(BUILD)/drtrace_app.o: src/drtrace_app.c include/asmtest_drtrace.h \
                        include/asmtest_trace.h include/asmtest_assemble.h \
                        $(BUILD)/.drapp-flags | $(BUILD)
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
                            include/asmtest_trace.h include/asmtest_assemble.h \
                            $(BUILD)/.drapp-flags | $(BUILD)/pic
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

# LbrExtV2 speculation flags (AMD Phase 4): struct perf_branch_entry gained the
# `spec:2` bitfield (PERF_BR_SPEC_WRONG_PATH) in Linux 6.1. Older
# <linux/perf_event.h> lack it, so probe the member (a `-fsyntax-only` semantic
# check, no link) and only then build amd_backend.c -DASMTEST_HAVE_PERF_BR_SPEC.
# Without the define the wrong-path filter compiles out — a no-op, exactly as on
# Zen 3 BRS / non-Linux where there are no spec bits. Mirrors the LIBIPT_DEF probe.
# The header is injected with -include rather than a literal `#include` line: an
# unescaped `#` inside $(shell ...) is a comment to GNU make < 4.3 (macOS ships
# 3.81), truncating the line, while `\#` breaks make >= 4.3 the other way.
PERF_BR_SPEC_DEF := $(shell printf 'int f(struct perf_branch_entry *e){return (int)e->spec;}\n' | $(CC) -fsyntax-only -include linux/perf_event.h -xc - >/dev/null 2>&1 && echo -DASMTEST_HAVE_PERF_BR_SPEC)

# Optional eBPF JIT code-emission detector (libbpf CO-RE) — the sideband accelerator for
# the code-image recorder (src/codeimage.c): it detects mprotect/mmap PROT_EXEC +
# memfd_create for a target PID and raises events into a bpf_ringbuf the recorder drains.
# The .bpf.o + libbpf skeleton are built ONLY when clang + bpftool + libbpf are ALL present
# (the `make docker-hwtrace-codeimage` lane). On a bare host without them CODEIMAGE_SKEL is
# empty, so codeimage.o has no skeleton prerequisite, clang/bpftool are never invoked, the
# TU compiles WITHOUT -DASMTEST_HAVE_LIBBPF, and asmtest_codeimage_bpf_available() returns 0
# (self-skip) — exactly how an empty LIBIPT_DEF makes pt_backend.o compile decoder-free.
CLANG         ?= clang
BPFTOOL       ?= bpftool
LIBBPF_CFLAGS ?= $(shell pkg-config --cflags libbpf 2>/dev/null)
LIBBPF_LIBS   ?= $(shell pkg-config --libs libbpf 2>/dev/null)
ifeq ($(shell pkg-config --exists libbpf 2>/dev/null && command -v $(CLANG) >/dev/null 2>&1 && command -v $(BPFTOOL) >/dev/null 2>&1 && echo 1),1)
LIBBPF_DEF     := -DASMTEST_HAVE_LIBBPF
CODEIMAGE_SKEL := $(BUILD)/codeimage.skel.h
BRANCHSNAP_SKEL := $(BUILD)/branchsnap.skel.h
CODEIMAGE_INC  := -I$(BUILD)
LINK_LIBBPF    := $(LIBBPF_LIBS)
endif

# CO-RE build chain (only reached when CODEIMAGE_SKEL is non-empty). vmlinux.h comes from
# the running kernel's BTF (the container shares the host kernel at `docker run` time);
# fall back to the checked-in minimal header. clang is BPF's only front end; -g emits the
# BTF CO-RE relocations need, -O2 is required by the verifier.
$(BUILD)/vmlinux.h: | $(BUILD)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@ 2>/dev/null \
	  || cp bpf/vmlinux_min.h $@
$(BUILD)/codeimage.bpf.o: bpf/codeimage.bpf.c bpf/codeimage_event.h $(BUILD)/vmlinux.h | $(BUILD)
	$(CLANG) -target bpf -D__TARGET_ARCH_x86 -g -O2 -Wall -I$(BUILD) -Ibpf -c $< -o $@
$(BUILD)/codeimage.skel.h: $(BUILD)/codeimage.bpf.o | $(BUILD)
	$(BPFTOOL) gen skeleton $< > $@

# AMD-P0 deterministic LBR snapshot: the branchsnap BPF program + its skeleton, built on
# the SAME BPF-toolchain gate as codeimage (BRANCHSNAP_SKEL is set only inside the ifeq
# above, so without the toolchain branchsnap.o has no skeleton prereq and compiles its stub).
$(BUILD)/branchsnap.bpf.o: bpf/branchsnap.bpf.c bpf/branchsnap_event.h $(BUILD)/vmlinux.h | $(BUILD)
	$(CLANG) -target bpf -D__TARGET_ARCH_x86 -g -O2 -Wall -I$(BUILD) -Ibpf -c $< -o $@
$(BUILD)/branchsnap.skel.h: $(BUILD)/branchsnap.bpf.o | $(BUILD)
	$(BPFTOOL) gen skeleton $< > $@


$(BUILD)/hwtrace.o: src/hwtrace.c include/asmtest_hwtrace.h \
                    include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/pt_backend.o: src/pt_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(LIBIPT_DEF) $(LIBIPT_CFLAGS) -c $< -o $@
$(BUILD)/cs_backend.o: src/cs_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(OPENCSD_DEF) $(OPENCSD_CFLAGS) -c $< -o $@
# AMD branch-record reconstruction reuses the Capstone layer (disasm.o) via
# asmtest_disas for instruction lengths — no new decoder lib, just Capstone.
# $(PERF_BR_SPEC_DEF) enables the LbrExtV2 wrong-path filter where the header has it.
$(BUILD)/amd_backend.o: src/amd_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) $(PERF_BR_SPEC_DEF) -c $< -o $@
# Single-step (EFLAGS.TF / SIGTRAP) backend: no external library at all, just the
# same Capstone length-decoder (disasm.o) for block normalization. Runs on ANY
# x86-64 Linux OR macOS host with no PMU/perf/privilege — the universal hardware-tier
# path (both deliver the #DB single-step trap as an in-process SIGTRAP).
$(BUILD)/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# AMD MSR-direct LBR snapshot (src/msr_lbr.c): reads the LbrExtV2 FROM/TO MSRs via
# /dev/cpu/N/msr; no external library, decodes through amd_backend.o's asmtest_amd_decode.
$(BUILD)/msr_lbr.o: src/msr_lbr.c include/asmtest_hwtrace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Cross-tier orchestrator (asmtest_trace_auto.h): no external library; calls
# asmtest_hwtrace_available() directly and dlopen-probes libasmtest_drapp (-ldl).
$(BUILD)/trace_auto.o: src/trace_auto.c include/asmtest_trace_auto.h \
                       include/asmtest_hwtrace.h include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Out-of-process ptrace single-step backend (W2): no external library, just the
# same Capstone length-decoder (disasm.o) for block normalization.
$(BUILD)/ptrace_backend.o: src/ptrace_backend.c include/asmtest_ptrace.h \
                          include/asmtest_trace.h include/asmtest_codeimage.h \
                          include/asmtest_descent_internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Call-descent handle (asmtest_descent_t): edges + nested frames read through scalar
# accessors. No external library (plain malloc pools); sibling of src/trace.c.
$(BUILD)/descent.o: src/descent.c include/asmtest_ptrace.h \
                    include/asmtest_descent_internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
# Time-aware code-image recorder (asmtest_codeimage.h): a userspace soft-dirty timeline
# (the foreign-JIT byte source) + an OPTIONAL eBPF emission detector. The userspace path
# needs no external library; the eBPF half is compiled in only when built
# -DASMTEST_HAVE_LIBBPF (the LIBBPF_* probe below, which the bare host leaves empty so the
# detector self-skips). $(LIBBPF_DEF)/$(LIBBPF_CFLAGS)/$(CODEIMAGE_INC) are empty unless
# that probe fires (see Phase C).
$(BUILD)/codeimage.o: src/codeimage.c include/asmtest_codeimage.h $(CODEIMAGE_SKEL) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) $(CODEIMAGE_INC) -c $< -o $@
# AMD-P0 deterministic LBR snapshot (src/branchsnap.c). Same libbpf gate as codeimage:
# with the toolchain it compiles the real bpf_get_branch_snapshot loader (needs the
# skeleton + -Ibpf for branchsnap_event.h); without it, the self-skipping #else stub.
$(BUILD)/branchsnap.o: src/branchsnap.c include/asmtest_hwtrace.h $(BRANCHSNAP_SKEL) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) -I$(BUILD) -Ibpf -c $< -o $@
# §D3 concealed ptrace-stealth stepper: the shared stepping body + bundled-binary
# discovery (dladdr-sibling, mirroring drtrace_app.c's dr_bundled_lib). Compiled
# into BOTH libasmtest_hwtrace (the in-process forked-child fallback path) and the
# standalone asmtest-stealth-helper binary below (the bundled path).
$(BUILD)/stealth_helper.o: src/stealth_helper.c src/stealth_helper.h \
                           include/asmtest_hwtrace.h include/asmtest_ptrace.h \
                           include/asmtest_trace.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/stealth_helper_main.o: src/stealth_helper_main.c src/stealth_helper.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

HWTRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o $(BUILD)/cs_backend.o \
                $(BUILD)/amd_backend.o $(BUILD)/ss_backend.o \
                $(BUILD)/trace_auto.o $(BUILD)/ptrace_backend.o \
                $(BUILD)/descent.o $(BUILD)/stealth_helper.o \
                $(BUILD)/codeimage.o $(BUILD)/branchsnap.o \
                $(BUILD)/msr_lbr.o \
                $(BUILD)/disasm.o $(BUILD)/trace.o

# The test builds a synthetic wrong-path perf_branch_entry (test_amd_spec_filter), so
# it needs $(PERF_BR_SPEC_DEF) too — it guards the .spec field the same way amd_backend.c does.
$(BUILD)/test_hwtrace.o: CFLAGS += $(PERF_BR_SPEC_DEF)
$(BUILD)/test_hwtrace: $(HWTRACE_OBJS) $(BUILD)/test_hwtrace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

# AMD deterministic LBR-snapshot test: drives the public asmtest_amd_snapshot_trace()
# (in branchsnap.o, part of HWTRACE_OBJS above). Self-skips (exit 0) without the BPF
# toolchain / privilege / AMD substrate. Defined AFTER HWTRACE_OBJS so the prerequisite
# is non-empty (Make expands prerequisites when the rule is read).
$(BUILD)/test_branchsnap: $(HWTRACE_OBJS) $(BUILD)/test_branchsnap.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@
.PHONY: branchsnap-test
branchsnap-test: $(BUILD)/test_branchsnap
	./$(BUILD)/test_branchsnap

# §D3 bundled stepper: a real separate process the managed packages ship (vs. the
# in-process forked child). Links only the ptrace stepper + Capstone length-decoder
# (NOT the PT/CoreSight/AMD decoders), so a bundled copy carries no libipt/OpenCSD
# runtime dependency. Found at run time via the dladdr-sibling lookup in
# stealth_helper.c (or the ASMTEST_STEALTH_HELPER path override).
STEALTH_HELPER_OBJS := $(BUILD)/stealth_helper_main.o $(BUILD)/stealth_helper.o \
    $(BUILD)/ptrace_backend.o $(BUILD)/descent.o $(BUILD)/codeimage.o \
    $(BUILD)/disasm.o $(BUILD)/trace.o
$(BUILD)/asmtest-stealth-helper: $(STEALTH_HELPER_OBJS)
	$(CC) $(CFLAGS) $^ $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

.PHONY: stealth-helper
stealth-helper: $(BUILD)/asmtest-stealth-helper

# hwtrace-test builds the bundled helper alongside the C harness so the §D3 test's
# bundled sub-case (which finds asmtest-stealth-helper as test_hwtrace's sibling in
# $(BUILD)/) exercises the real exec'd-binary path, not only the forked child.
.PHONY: hwtrace-test
hwtrace-test: $(BUILD)/test_hwtrace $(BUILD)/asmtest-stealth-helper
	@echo "== hwtrace-test =="
	./$(BUILD)/test_hwtrace

# Code-image recorder self-test (same-address-different-bytes temporal proof; runs live on
# any Linux with soft-dirty — no privilege). When built with libbpf it additionally
# exercises the eBPF emission detector, which self-skips without CAP_BPF.
$(BUILD)/test_codeimage: $(HWTRACE_OBJS) $(BUILD)/test_codeimage.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

.PHONY: codeimage-test
codeimage-test: $(BUILD)/test_codeimage
	@echo "== codeimage-test =="
	./$(BUILD)/test_codeimage

# Real managed-runtime trace: attach to a LIVE JIT runtime and trace a genuine
# JIT-compiled method out of band (resolve from the runtime's perf-map -> attach ->
# run_to -> single-step). One argv-driven harness drives all three runtimes (V8, CoreCLR,
# HotSpot); each self-skips (never hangs/flakes) when the runtime is absent, ptrace is
# denied, or the JIT re-tiered the code. Driven in plain containers by
# `make docker-hwtrace-jit{,-dotnet,-java}`.
$(BUILD)/jit_trace: $(HWTRACE_OBJS) $(BUILD)/jit_trace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

# §D3 whole-window capture over the cross-process JIT-address channel: a self-contained
# demo (no managed runtime) proving the stepper records methods it learns only through the
# shared channel after forking. Runs on any ptrace-capable x86-64 Linux; self-skips where
# ptrace is denied. `make windowed-trace` (and the docker-hwtrace lane below runs it).
$(BUILD)/windowed_trace: $(HWTRACE_OBJS) $(BUILD)/windowed_trace.o
	$(CC) $(CFLAGS) $^ $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@

.PHONY: windowed-trace
windowed-trace: $(BUILD)/windowed_trace
	./$(BUILD)/windowed_trace

.PHONY: hwtrace-jit hwtrace-jit-node hwtrace-jit-dotnet hwtrace-jit-java \
        hwtrace-jit-java-jitdump hwtrace-jit-jitdump hwtrace-jit-dotnet-jitdump \
        hwtrace-jit-dotnet-bcl hwtrace-jit-java-bcl
hwtrace-jit: hwtrace-jit-node # back-compat alias for the default (Node.js) lane

# The perf JVMTI agent (libperf-jvmti.so, from linux-tools) — HotSpot's de-facto jitdump
# encoder. Userspace, so a kernel-version-mismatched copy still runs; resolve by glob.
PERF_JVMTI := $(firstword $(wildcard /usr/lib/linux-tools*/*/libperf-jvmti.so \
                                     /usr/lib/linux-tools/*/libperf-jvmti.so \
                                     /usr/lib/*/libperf-jvmti.so))

# Node.js (V8): trace the optimized `asmtjit` body (needs node + Capstone).
hwtrace-jit-node: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-node (real Node.js V8 JIT method) =="
	./$(BUILD)/jit_trace node

# Binary jitdump path (asmtest_jitdump_find) against a real V8 jit-<pid>.dump
# (node --perf-prof): recover a method's recorded bytes and validate them vs the perf-map
# address and the live code. Needs node + Capstone.
hwtrace-jit-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-jitdump (real V8 jitdump byte recovery) =="
	./$(BUILD)/jit_trace jitdump

# .NET (CoreCLR): build the bare console app (offline, from the SDK packs) and trace its
# `Program::Add` body. DOTNET_TieredCompilation=0 (set by the harness) gives a stable
# single-compilation address. Needs the dotnet SDK + Capstone.
hwtrace-jit-dotnet: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet (real .NET CoreCLR JIT method) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	./$(BUILD)/jit_trace dotnet $(BUILD)/jit_dotnet/jit_dotnet.dll

# .NET (CoreCLR) FRAMEWORK method: trace System.Console::WriteLine — BCL code that ships as
# ReadyToRun precompiled native, so the JIT never emits it by default. The harness sets
# DOTNET_ReadyToRun=0 to force the whole BCL to JIT on demand, then single-steps WriteLine
# like any user method. Reuses the jit_dotnet app (invoked with the "bcl" arg). Needs the
# dotnet SDK + Capstone.
hwtrace-jit-dotnet-bcl: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-bcl (real .NET CoreCLR BCL method: Console.WriteLine) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	./$(BUILD)/jit_trace dotnet-bcl $(BUILD)/jit_dotnet/jit_dotnet.dll

# Binary jitdump path against a THIRD producer: .NET CoreCLR. Unlike HotSpot, CoreCLR writes
# a real /tmp/jit-<pid>.dump NATIVELY (no agent) under DOTNET_PerfMapEnabled=1 (set by the
# harness), naming the method identically in the perf-map and the jitdump — so it reuses the
# same trace_jitdump path as V8. Recovers `Program::Add`'s bytes and validates them vs the
# perf-map and the live code. The dll path is absolute (the harness chdirs to /tmp). Needs
# the dotnet SDK + Capstone.
hwtrace-jit-dotnet-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-jitdump (real .NET CoreCLR jitdump byte recovery) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	./$(BUILD)/jit_trace dotnet-jitdump $(abspath $(BUILD)/jit_dotnet/jit_dotnet.dll)

# OpenJDK (HotSpot): compile the one-method hot loop and trace its `Hot.asmtjit` C2 body.
# -XX:-TieredCompilation + CompileCommand dontinline (set by the harness) give a stable,
# standalone nmethod; the harness drives `jcmd <pid> Compiler.perfmap` to materialize the
# perf-map on a live process (HotSpot does not stream one). Needs the JDK (javac + jcmd) +
# Capstone.
hwtrace-jit-java: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java (real OpenJDK HotSpot JIT method) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	./$(BUILD)/jit_trace java $(BUILD)/jit_java

# --- Call-descent demo lanes (Phase 8 of docs/internal/archive/plans/call-descent-plan.md) ---------------
# `<lane>-descend` runs the same live trace at descent level 2 (DESCEND_KNOWN): the tracer
# descends INTO the runtime's own sibling JIT methods the traced method calls (the .NET
# `dotnet-bcl-descend` lane descends Console.WriteLine's `get_Out`) while still stepping OVER
# libc/PLT. `<lane>-descend-all` runs at level 3 (DESCEND_ALL) with a conservative instruction
# budget + the backend watchdog and is EXPECTED to self-skip (truncate) when it trips a guard
# — it asserts the guards fire, never that L3 is transparent. All watchdog-bounded and
# self-skipping like the plain lanes. Needs the runtime SDK + Capstone.
.PHONY: hwtrace-jit-dotnet-descend hwtrace-jit-dotnet-descend-all \
        hwtrace-jit-dotnet-bcl-descend hwtrace-jit-dotnet-bcl-descend-all \
        hwtrace-jit-java-descend hwtrace-jit-java-descend-all
hwtrace-jit-dotnet-descend hwtrace-jit-dotnet-descend-all: hwtrace-jit-dotnet-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-$* (descend into CoreCLR sibling JIT methods) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	./$(BUILD)/jit_trace dotnet-$* $(BUILD)/jit_dotnet/jit_dotnet.dll
hwtrace-jit-dotnet-bcl-descend hwtrace-jit-dotnet-bcl-descend-all: hwtrace-jit-dotnet-bcl-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-dotnet-bcl-$* (descend Console.WriteLine -> get_Out) =="
	DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  dotnet build examples/dotnet/jit_dotnet -c Release -o $(BUILD)/jit_dotnet >/dev/null
	./$(BUILD)/jit_trace dotnet-bcl-$* $(BUILD)/jit_dotnet/jit_dotnet.dll
hwtrace-jit-java-descend hwtrace-jit-java-descend-all: hwtrace-jit-java-%: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-$* (descend into HotSpot sibling JIT methods) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	./$(BUILD)/jit_trace java-$* $(BUILD)/jit_java

# OpenJDK (HotSpot) LIBRARY method: trace java.lang.Math::floorDiv. Unlike .NET, HotSpot JITs
# JDK methods on demand BY DEFAULT — no precompilation-disable flag — so this needs only the
# same dontinline nudge as the user-method lane (added by the harness) plus the jcmd perf-map
# refresh. Reuses Hot (invoked with the "bcl" arg). Needs the JDK (javac + jcmd) + Capstone.
hwtrace-jit-java-bcl: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-bcl (real OpenJDK HotSpot JDK method: Math.floorDiv) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	./$(BUILD)/jit_trace java-bcl $(BUILD)/jit_java

# Binary jitdump path against a SECOND producer: OpenJDK HotSpot via the perf JVMTI agent
# (libperf-jvmti.so). The agent records each C2 method's bytes to a real jitdump; the lane
# recovers asmtjit's bytes with asmtest_jitdump_find and validates them against the live
# code and HotSpot's own jcmd perf-map. Needs the JDK, Capstone, and libperf-jvmti.so
# (self-skips if the agent is absent). Drives jcmd, so it also needs jcmd on PATH.
hwtrace-jit-java-jitdump: $(BUILD)/jit_trace
	@echo "== hwtrace-jit-java-jitdump (real HotSpot jitdump byte recovery) =="
	@mkdir -p $(BUILD)/jit_java
	javac -d $(BUILD)/jit_java examples/jit_java/Hot.java
	@rm -rf /tmp/.debug/jit 2>/dev/null || true
	./$(BUILD)/jit_trace java-jitdump $(BUILD)/jit_java "$(PERF_JVMTI)"

# Python language-wrapper test for the native-trace tier (asmtest.drtrace). Builds
# the app lib + DR client, then runs the pytest suite with the lib paths wired up.
# Self-skips when DynamoRIO is absent. Runs on a dev box (DYNAMORIO_HOME=...) and
# is the body of the `make docker-drtrace` container lane.
.PHONY: drtrace-python-test
drtrace-python-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	@echo "== drtrace-python-test =="
	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>"
else
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

# Shared native-trace artifacts, built ONCE before the per-language fan-out (B7).
# Every drtrace-*-test lane used to run its own `$(MAKE) shared-drtrace
# drtrace-client DRAPP_KEYSTONE=0` (a recursive sub-make — needed because the
# DRAPP_KEYSTONE=0 override is per-lane, not global). Under `make -j` two lanes
# then built the same build/pic/*.o + libasmtest_drapp + drclient into one shared
# tree concurrently → torn writes / cmake collisions. Making it a single shared
# prerequisite of every lane makes GNU make build it exactly once and every lane
# wait on it. Guarded by DR_AVAILABLE so it stays a no-op (and the lanes SKIP) off
# a DynamoRIO host.
#
# The base prep builds ONLY the DR essentials (libasmtest_drapp + the DR client):
# eight lanes and the CI `drtrace` job dlopen just libasmtest_drapp, and that job
# installs no emulator dep, so pulling shared-emu in here would fail on the missing
# libunicorn/unicorn.h. rust/go additionally load libasmtest_emu and link the
# conformance corpus fixture, so they depend on drtrace-shared-prep-emu, which layers
# shared-emu + $(CORPUS_LIB) on top. Ordered AFTER the base prep, so the shared
# pic/*.o are already built once before the emu sub-make runs — no -j torn writes.
.PHONY: drtrace-shared-prep
drtrace-shared-prep:
ifdef DR_AVAILABLE
	@$(MAKE) shared-drtrace drtrace-client DRAPP_KEYSTONE=0
endif

.PHONY: drtrace-shared-prep-emu
drtrace-shared-prep-emu: drtrace-shared-prep
ifdef DR_AVAILABLE
	@$(MAKE) shared-emu $(CORPUS_LIB) DRAPP_KEYSTONE=0
endif

drtrace-cpp-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,cpp)
else
	@echo "== drtrace-cpp-test =="
	$(CXX) -std=c++17 -Iinclude bindings/cpp/test_drtrace.cpp -ldl -o $(BUILD)/test_drtrace_cpp
	$(drtrace_env) ./$(BUILD)/test_drtrace_cpp
endif

drtrace-rust-test: drtrace-shared-prep-emu
ifndef DR_AVAILABLE
	$(call drtrace_skip,rust)
else
	@echo "== drtrace-rust-test =="
	@# Run as an EXAMPLE binary (main thread), NOT `cargo test` (which runs the
	@# test on a worker thread, making the process multi-threaded when dr_app_start
	@# takes over — that crashes; a single-threaded main lets DR take over cleanly).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(drtrace_env) \
	  $(CARGO) run --quiet --example drtrace
endif

drtrace-go-test: drtrace-shared-prep-emu
ifndef DR_AVAILABLE
	$(call drtrace_skip,go)
else
	@echo "== drtrace-go-test =="
	cd bindings/go && CGO_LDFLAGS="-L$(abspath $(BUILD))" CGO_CFLAGS="-I$(abspath include)" \
	  GOTOOLCHAIN=local GOFLAGS=-mod=mod GOPROXY=off \
	  ASMTEST_LIB=$(abspath $(call shlib_dev,libasmtest_emu)) $(drtrace_env) \
	  $(GO) test -run TestDrtrace ./...
endif

drtrace-node-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,node)
else
	@echo "== drtrace-node-test =="
	$(call drtrace_run,cd bindings/node && $(drtrace_env) $(NODE) test_drtrace.js)
endif

drtrace-java-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,java)
else
	@echo "== drtrace-java-test =="
	mkdir -p $(BUILD)/java-drtrace
	$(JAVAC) --release 22 -d $(BUILD)/java-drtrace \
	  bindings/java/DrTrace.java bindings/java/DrTraceTest.java
	$(call drtrace_run,$(drtrace_env) $(JAVA) --enable-native-access=ALL-UNNAMED -cp $(BUILD)/java-drtrace DrTraceTest)
endif

drtrace-dotnet-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,dotnet)
else
	@echo "== drtrace-dotnet-test =="
	$(call drtrace_run,$(drtrace_env) $(DOTNET) run --project bindings/dotnet/drtrace/drtrace.csproj)
endif

drtrace-ruby-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,ruby)
else
	@echo "== drtrace-ruby-test =="
	cd bindings/ruby && $(drtrace_env) $(RUBY) test_drtrace.rb
endif

drtrace-lua-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,lua)
else
	@echo "== drtrace-lua-test =="
	cd bindings/lua && $(drtrace_env) $(LUAJIT) test_drtrace.lua
endif

drtrace-zig-test: drtrace-shared-prep
ifndef DR_AVAILABLE
	$(call drtrace_skip,zig)
else
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

# .NET lanes only: pin CoreCLR's tiered-compilation background worker resident
# (0x36EE80 ms = 1 h; DOTNET_* DWORDs parse as hex). The suites arm in-process
# EFLAGS.TF windows on managed threads; if the worker idle-exits (default ~4 s)
# and is respawned INSIDE such a window, glibc's pthread_create blocks all
# signals around clone() and the very next #DB is a SIGTRAP delivered while
# blocked — the kernel force-resets it to SIG_DFL and kills the process (exit
# 133). A fast host never steps a window long enough to hit it; GitHub-hosted
# runners died deterministically (dmesg: RIP inside pthread_create, EFLAGS.TF
# set). See docs/guides/tracing/hardware-tracing.md ("per-thread footgun").
hwtrace_dotnet_env = DOTNET_TC_BackgroundWorkerTimeoutMs=36EE80

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
hwtrace-rust-test: shared-hwtrace shared-emu $(CORPUS_LIB)
	@echo "== hwtrace-rust-test =="
	@# --test-threads=1: the single-step backend is process-global (single active
	@# region, a global SIGTRAP handler, per-thread EFLAGS.TF), so the two live
	@# single-step tests must run serially — concurrent stepping crashes (SIGTRAP).
	cd bindings/rust && ASMTEST_LIB_DIR=$(abspath $(BUILD)) $(hwtrace_env) \
	  $(CARGO) test --test hwtrace -- --nocapture --test-threads=1

hwtrace-go-test: shared-hwtrace shared-emu $(CORPUS_LIB)
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
	$(JAVAC) --release 22 -d $(BUILD)/java-hwtrace \
	  bindings/java/HwTrace.java bindings/java/HwTraceTest.java
	$(hwtrace_env) $(JAVA) --enable-native-access=ALL-UNNAMED \
	  -cp $(BUILD)/java-hwtrace HwTraceTest

hwtrace-dotnet-test: shared-hwtrace
	@echo "== hwtrace-dotnet-test =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj

# Slow-host crash-avoidance stress (managed-singlestep-lazy-arm-plan "Sharpening 1"):
# the ONE lane that must run with the tiering worker UNPINNED — no $(hwtrace_dotnet_env)
# — so CoreCLR's worker idle-exits and is respawned via pthread_create adjacent to the
# lazy-arm windows. Under the old stepped-DynamicInvoke Invoke this died (exit 133) on
# loaded runners; under lazy-arm the process must survive with every capture intact.
# Meaningful on a slow/loaded host (CI); on a fast dev box it simply passes quickly.
.PHONY: hwtrace-dotnet-stress
hwtrace-dotnet-stress: shared-hwtrace
	@echo "== hwtrace-dotnet-stress (tiering worker UNPINNED) =="
	$(hwtrace_env) ASMTEST_METHOD_STRESS=60 $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj

# Forward-runtime drift check: the same self-test on .NET 9. The images carry only
# dotnet-sdk-8.0, so install net9 user-local via the official dotnet-install script
# into a scratch dir, flip the TFM in a scratch COPY of the project (the tree is
# never modified), and run. Guards the diagnostics-IPC (DOTNET_IPC_V1) and
# MethodLoadVerbose surfaces against runtime drift — the two places a new runtime
# would silently regress the rundown/labelling paths. Network-dependent (dot.net):
# self-skips, never fails, when the toolchain cannot be fetched.
.PHONY: hwtrace-dotnet9-test
hwtrace-dotnet9-test: shared-hwtrace
	@echo "== hwtrace-dotnet9-test (forward-runtime drift check) =="
	@set -e; d9=$$(mktemp -d); trap 'rm -rf "$$d9"' EXIT; \
	if ! curl -fsSL --max-time 60 https://dot.net/v1/dotnet-install.sh -o $$d9/di.sh; then \
	  echo "# SKIP net9: dotnet-install.sh unreachable"; exit 0; fi; \
	if ! bash $$d9/di.sh --channel 9.0 --install-dir $$d9/dotnet >$$d9/install.log 2>&1; then \
	  echo "# SKIP net9: install failed"; tail -3 $$d9/install.log; exit 0; fi; \
	cp -r bindings/dotnet $$d9/dn && rm -rf $$d9/dn/*/obj $$d9/dn/*/bin; \
	sed -i 's#<TargetFramework>net8.0</TargetFramework>#<TargetFramework>net9.0</TargetFramework>#' \
	  $$d9/dn/hwtrace/hwtrace.csproj; \
	cd $$d9/dn/hwtrace && $(hwtrace_env) $(hwtrace_dotnet_env) \
	  DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
	  $$d9/dotnet/dotnet run

# Runnable demos of the scoped-trace facility (§Z0/§Z1) on the single-step WEAK tier:
# one project per report — see examples/dotnet/README.md for what each shows.
.PHONY: hwtrace-dotnet-example
hwtrace-dotnet-example: shared-hwtrace
	@echo "== hwtrace-dotnet-example (wholewindow) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/wholewindow/wholewindow.csproj
	@echo "== hwtrace-dotnet-example (region) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/region/region.csproj
	@echo "== hwtrace-dotnet-example (methods) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/methods/methods.csproj
	@echo "== hwtrace-dotnet-example (rundown) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/rundown/rundown.csproj
	@echo "== hwtrace-dotnet-example (localscope) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope/localscope.csproj
	@echo "== hwtrace-dotnet-example (localscope_oop) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope_oop/localscope_oop.csproj
	@echo "== hwtrace-dotnet-example (localscope_oop_managed) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/localscope_oop_managed/localscope_oop_managed.csproj
	@echo "== hwtrace-dotnet-example (windowhybrid) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/windowhybrid/windowhybrid.csproj
	@echo "== hwtrace-dotnet-example (amdhot) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amdhot/amdhot.csproj
	@echo "== hwtrace-dotnet-example (amdlbr) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amdlbr/amdlbr.csproj
	@echo "== hwtrace-dotnet-example (assemblies) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/assemblies/assemblies.csproj
	@echo "== hwtrace-dotnet-example (annotated) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/annotated/annotated.csproj
	@echo "== hwtrace-dotnet-example (tiers) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/tiers/tiers.csproj
	@echo "== hwtrace-dotnet-example (hotspots) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/hotspots/hotspots.csproj
	@echo "== hwtrace-dotnet-example (coverage) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/coverage/coverage.csproj
	@echo "== hwtrace-dotnet-example (callgraph) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/callgraph/callgraph.csproj
	@echo "== hwtrace-dotnet-example (ptrace_native) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/ptrace_native/ptrace_native.csproj
	@echo "== hwtrace-dotnet-example (blockstep) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/blockstep/blockstep.csproj
	@echo "== hwtrace-dotnet-example (ptrace_dotnet) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/ptrace_dotnet/ptrace_dotnet.csproj
	@echo "== hwtrace-dotnet-example (flatprofile) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/flatprofile/flatprofile.csproj
	@echo "== hwtrace-dotnet-example (amplification) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amplification/amplification.csproj
	@echo "== hwtrace-dotnet-example (runtimegaps) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/runtimegaps/runtimegaps.csproj
	@echo "== hwtrace-dotnet-example (footprint) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/footprint/footprint.csproj
	@echo "== hwtrace-dotnet-example (runtimebuckets) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/runtimebuckets/runtimebuckets.csproj
	@echo "== hwtrace-dotnet-example (instructionmix) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/instructionmix/instructionmix.csproj
	@echo "== hwtrace-dotnet-example (perfannotate) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/perfannotate/perfannotate.csproj
	@echo "== hwtrace-dotnet-example (loops) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/loops/loops.csproj
	@echo "== hwtrace-dotnet-example (descent) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descent/descent.csproj
	@echo "== hwtrace-dotnet-example (descent_dotnet) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descent_dotnet/descent_dotnet.csproj
	@echo "== hwtrace-dotnet-example (single-method) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/single-method/single-method.csproj
	@echo "== hwtrace-dotnet-example (perf-triage-drill) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/perf-triage-drill/perf-triage-drill.csproj
	@echo "== hwtrace-dotnet-example (concurrent-isolation) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/concurrent-isolation/concurrent-isolation.csproj
	@echo "== hwtrace-dotnet-example (async-stitch) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/async-stitch/async-stitch.csproj
	@echo "== hwtrace-dotnet-example (trace-diff) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/trace-diff/trace-diff.csproj
	@echo "== hwtrace-dotnet-example (coverage-guided-fuzz) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/coverage-guided-fuzz/coverage-guided-fuzz.csproj
	@echo "== hwtrace-dotnet-example (trace-cost-overhead) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/trace-cost-overhead/trace-cost-overhead.csproj
	@echo "== hwtrace-dotnet-example (descend-all) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/descend-all/descend-all.csproj
	@echo "== hwtrace-dotnet-example (crashproof-showdown) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/crashproof-showdown/crashproof-showdown.csproj
	@echo "== hwtrace-dotnet-example (crashproof-survey) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/crashproof-survey/crashproof-survey.csproj
	@echo "== hwtrace-dotnet-example (tier-ladder) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/tier-ladder/tier-ladder.csproj
	@echo "== hwtrace-dotnet-example (amd-period-sweep) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amd-period-sweep/amd-period-sweep.csproj
	@echo "== hwtrace-dotnet-example (amd-snapshot) =="
	$(hwtrace_env) $(hwtrace_dotnet_env) $(DOTNET) run --project examples/dotnet/amd-snapshot/amd-snapshot.csproj
	@echo "== hwtrace-dotnet-example (codeimage) =="
	$(hwtrace_env) $(DOTNET) run --project examples/dotnet/codeimage/codeimage.csproj

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
	$(CC) $(CFLAGS) $(PERF_BR_SPEC_DEF) -fPIC -c $< -o $@
$(BUILD)/pic/ss_backend.o: src/ss_backend.c include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/trace_auto.o: src/trace_auto.c include/asmtest_trace_auto.h \
                           include/asmtest_hwtrace.h include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/ptrace_backend.o: src/ptrace_backend.c include/asmtest_ptrace.h \
                               include/asmtest_trace.h include/asmtest_codeimage.h \
                               include/asmtest_descent_internal.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/descent.o: src/descent.c include/asmtest_ptrace.h \
                        include/asmtest_descent_internal.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/codeimage.o: src/codeimage.c include/asmtest_codeimage.h \
                          $(CODEIMAGE_SKEL) | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) $(CODEIMAGE_INC) -fPIC -c $< -o $@
$(BUILD)/pic/branchsnap.o: src/branchsnap.c include/asmtest_hwtrace.h \
                           $(BRANCHSNAP_SKEL) | $(BUILD)/pic
	$(CC) $(CFLAGS) $(LIBBPF_DEF) $(LIBBPF_CFLAGS) -I$(BUILD) -Ibpf -fPIC -c $< -o $@
$(BUILD)/pic/stealth_helper.o: src/stealth_helper.c src/stealth_helper.h \
                               include/asmtest_hwtrace.h include/asmtest_ptrace.h \
                               include/asmtest_trace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@
$(BUILD)/pic/msr_lbr.o: src/msr_lbr.c include/asmtest_hwtrace.h | $(BUILD)/pic
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

# K5: rebuild the hardware/native-trace object tree on a build-knob flip
# (SAN=1/COV=1/CSTD), like the core tree. drtrace_app.o already tracks its own
# .drapp-flags (Keystone + CFLAGS); the rest gain the shared .build-flags
# sentinel here, so `make shared-hwtrace && make SAN=1 shared-hwtrace` no longer
# relinks stale, uninstrumented objects. codeimage.o is included (its bytecode
# skeleton is regenerated separately, but the host object still tracks CFLAGS).
NATIVE_TRACE_OBJS := $(BUILD)/hwtrace.o $(BUILD)/pt_backend.o \
    $(BUILD)/cs_backend.o $(BUILD)/amd_backend.o $(BUILD)/ss_backend.o \
    $(BUILD)/trace_auto.o $(BUILD)/ptrace_backend.o $(BUILD)/descent.o \
    $(BUILD)/stealth_helper.o $(BUILD)/codeimage.o $(BUILD)/branchsnap.o \
    $(BUILD)/msr_lbr.o
$(NATIVE_TRACE_OBJS) $(patsubst $(BUILD)/%,$(BUILD)/pic/%,$(NATIVE_TRACE_OBJS)): \
    $(BUILD)/.build-flags

shared-hwtrace: $(call shlib_dev,libasmtest_hwtrace)
$(call shlib_real,libasmtest_hwtrace): $(BUILD)/pic/hwtrace.o \
                                       $(BUILD)/pic/pt_backend.o \
                                       $(BUILD)/pic/cs_backend.o \
                                       $(BUILD)/pic/amd_backend.o \
                                       $(BUILD)/pic/ss_backend.o \
                                       $(BUILD)/pic/trace_auto.o \
                                       $(BUILD)/pic/ptrace_backend.o \
                                       $(BUILD)/pic/descent.o \
                                       $(BUILD)/pic/stealth_helper.o \
                                       $(BUILD)/pic/codeimage.o \
                                       $(BUILD)/pic/branchsnap.o \
                                       $(BUILD)/pic/msr_lbr.o \
                                       $(BUILD)/pic/disasm.o \
                                       $(BUILD)/pic/trace.o
	$(CC) $(CFLAGS) $(call shlib_ldflags,libasmtest_hwtrace) $^ \
	      $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(CAPSTONE_LIBS) $(LINK_LIBBPF) -ldl -o $@
$(call shlib_dev,libasmtest_hwtrace): $(call shlib_real,libasmtest_hwtrace)
	ln -sf $(notdir $<) $(call shlib_compat,libasmtest_hwtrace)
	ln -sf $(notdir $(call shlib_compat,libasmtest_hwtrace)) $@

# K6: install the native-trace shared libs + their headers, so an installed-prefix
# consumer can compile against the hardware-trace / DynamoRIO tiers (the guide's
# `#include "asmtest_drtrace.h"` and packaging.md's "installs the libs"). `make
# install` installs the headers; these add the shared lib + soname/dev symlinks
# (mirroring install-shared-emu). No pkg-config template ships for these tiers —
# a consumer links by name (-lasmtest_hwtrace / -lasmtest_drapp).
.PHONY: install-shared-hwtrace install-shared-drtrace install-stealth-helper
# §D3 bundled stepper: install the standalone helper NEXT TO libasmtest_hwtrace so
# the dladdr-sibling discovery in stealth_helper.c finds it with no env/opts — the
# same "sibling of the payload" placement the managed packages use. It is an
# executable in $(libdir) by design (that is where the sibling lookup resolves).
install-stealth-helper: stealth-helper
	mkdir -p $(libdir)
	cp $(BUILD)/asmtest-stealth-helper $(libdir)/
	@echo "installed asmtest-stealth-helper to $(libdir)"

install-shared-hwtrace: shared-hwtrace
	mkdir -p $(libdir) $(incdir)
	cp $(call shlib_real,libasmtest_hwtrace) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest_hwtrace)) \
	       $(libdir)/$(call shlib_soname,libasmtest_hwtrace)
	ln -sf $(call shlib_soname,libasmtest_hwtrace) \
	       $(libdir)/$(notdir $(call shlib_dev,libasmtest_hwtrace))
	cp include/asmtest_hwtrace.h include/asmtest_ptrace.h \
	   include/asmtest_codeimage.h include/asmtest_trace_auto.h \
	   include/asmtest_trace.h $(incdir)/
	@echo "installed shared libasmtest_hwtrace $(ASMTEST_VERSION) to $(libdir)"

install-shared-drtrace: shared-drtrace
	mkdir -p $(libdir) $(incdir)
	cp $(call shlib_real,libasmtest_drapp) $(libdir)/
	ln -sf $(notdir $(call shlib_real,libasmtest_drapp)) \
	       $(libdir)/$(call shlib_soname,libasmtest_drapp)
	ln -sf $(call shlib_soname,libasmtest_drapp) \
	       $(libdir)/$(notdir $(call shlib_dev,libasmtest_drapp))
	cp include/asmtest_drtrace.h include/asmtest_trace.h $(incdir)/
	@echo "installed shared libasmtest_drapp $(ASMTEST_VERSION) to $(libdir)"

