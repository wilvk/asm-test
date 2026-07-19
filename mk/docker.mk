# docker.mk — Docker lanes that reproduce the Linux half of the CI matrix in a container.
#
# Included by ../Makefile (split out by concern for maintainability). All
# variables/knobs (CSTD, WERROR, ASM_SYNTAX, BUILD, ...) come from the parent
# Makefile, which reads this file in place; edit targets here, knobs there.

# --- Run the Linux CI jobs locally via Docker ------------------------------
# Covers the Linux half of the matrix; the macOS jobs can't run in a container.
#   make docker-test       build + run the example suites and self-tests
#   make docker-nasm       the NASM backend (x86-64 only)
#   make docker-emu        the emulator tier (libunicorn)
#   make docker-asm        the in-line assembler tier (libkeystone + libunicorn)
#   make docker-valgrind   memcheck the routines under test
#   make docker-sanitize   ASan + UBSan
#   make docker-analyze    clang-tidy
#   make docker-fmt-check  clang-format drift (informational)
#   make docker-coverage   gcov of the runner
#   make docker-ci         the whole x86-64 Linux matrix end to end
#   make docker-shell      interactive shell in the CI image
# Emulate the aarch64 runner with DOCKER_PLATFORM=linux/arm64; on arm64 CI runs
# the test, emu, asm, and package-libs jobs (NASM is x86-64 only), so use
# docker-test/docker-emu/docker-asm there rather than docker-ci.
DOCKER          ?= docker
DOCKER_IMAGE    ?= asmtest-ci
DOCKER_BASE     ?= ubuntu:24.04
DOCKER_PLATFORM ?=
# Which optional-toolchain set the image bakes in (Dockerfile's DEPS_ARGS). The
# default (--all) is unchanged; the riscv64 lane overrides it to --pkgconfig so
# the Keystone LLVM source build never runs under qemu-user emulation (hours).
DOCKER_DEPS_ARGS ?= --all
_docker_plat := $(if $(DOCKER_PLATFORM),--platform $(DOCKER_PLATFORM),)
_docker_run  := $(DOCKER) run --rm $(_docker_plat) $(DOCKER_IMAGE)

# riscv64 lane (riscv-native-tier). ubuntu:24.04 is multi-arch incl.
# linux/riscv64; Docker Desktop ships emulators, a Linux host runs
# `make binfmt-riscv64` once. DOCKER_DEPS_ARGS=--pkgconfig: the core suites need
# no optional engine, and Keystone-under-qemu would take hours. BINFMT_IMAGE is
# pinned by tag@sha256 (the qemu-v10.2.3 multi-arch index digest; audit anchor in
# scripts/third-party-digests.txt), so Docker verifies it at pull time.
BINFMT_IMAGE ?= tonistiigi/binfmt:qemu-v10.2.3@sha256:400a4873b838d1b89194d982c45e5fb3cda4593fbfd7e08a02e76b03b21166f0

.PHONY: docker-build docker-test docker-nasm docker-emu docker-asm \
        docker-valgrind docker-sanitize docker-analyze docker-fmt-check \
        docker-fmt docker-coverage docker-ci docker-shell docker-clean

docker-build:
	$(DOCKER) build $(_docker_plat) --build-arg BASE=$(DOCKER_BASE) \
	  --build-arg DEPS_ARGS=$(DOCKER_DEPS_ARGS) -t $(DOCKER_IMAGE) .

docker-test: docker-build
	$(_docker_run) sh -c 'make test && make check'

# Enable the pinned qemu-user riscv64 emulator once per Linux host (Docker
# Desktop ships it already, so this is a no-op there). Idempotent.
.PHONY: binfmt-riscv64 docker-riscv64
binfmt-riscv64:
	$(DOCKER) run --privileged --rm $(BINFMT_IMAGE) --install riscv64

# Build the linux/riscv64 image (env only — no optional engines) and run the
# core example suites + framework self-tests in it under qemu-user binfmt on any
# Docker host. The distinct asmtest-ci-riscv64 tag keeps the host-arch asmtest-ci
# image intact. First run is slow (qemu TCG); expect green with the rv64 skip set.
docker-riscv64:
	$(MAKE) docker-build DOCKER_PLATFORM=linux/riscv64 \
	  DOCKER_IMAGE=asmtest-ci-riscv64 DOCKER_DEPS_ARGS=--pkgconfig
	$(DOCKER) run --rm --platform linux/riscv64 asmtest-ci-riscv64 \
	  sh -c 'uname -m && make test && make check'

docker-nasm: docker-build
	$(_docker_run) sh -c 'make ASM_SYNTAX=nasm test && make ASM_SYNTAX=nasm check'

docker-emu: docker-build
	$(_docker_run) make emu-test

docker-asm: docker-build
	$(_docker_run) make asm-test

# libFuzzer/AFL external-engine coverage shim lane (libfuzzer-afl-shim, Track E).
# FROM asmtest-bindings-base (libunicorn + libcapstone already present); the
# Dockerfile.fuzz layer adds clang + libclang-rt + afl++ — all apt-installable,
# so per CLAUDE.md they are added HERE where the work runs, not gated away. Runs
# `make fuzz-shim-test`, which builds both engines' harnesses and FAILS unless
# each one finds its planted crash — a real test, never a self-skip. The AFL env
# vars keep afl-fuzz from aborting on the container's cpufreq/core-pattern
# settings. Linux x86-64 (the guest is x86-64 run under Unicorn).
.PHONY: docker-fuzz
docker-fuzz: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.fuzz \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-fuzz .
	$(DOCKER) run --rm $(_docker_plat) \
	  -e AFL_SKIP_CPUFREQ=1 -e AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
	  asmtest-fuzz make fuzz-shim-test

# --- docker-sve-sweep: pre-hardware SVE validation under qemu binfmt --------
# Runs the simd suite in an arm64 container at several SVE vector lengths by
# steering qemu-user's CPU through the QEMU_CPU env var (read by the binfmt
# interpreter): sve-max-vq=N caps VL at N*16 bytes and
# sve-default-vector-length=-1 selects the max, so the sweep covers VQ 1
# (128-bit), 3 (384-bit — non-power-of-two), 8 (1024-bit) and 16 (2048-bit).
# Verified 2026-07-19 on an x86-64 Zen 5 Docker host (qemu-user TCG binfmt):
# default VL=64B; vq=1 -> 16B, vq=3 -> 48B, vq=8 -> 128B, vq=16 +
# sve-default-vector-length=-1 -> 256B (the architectural max). On a NATIVE
# arm64 host there is no qemu to steer: QEMU_CPU is ignored and the test itself
# self-skips without SVE silicon (Apple silicon has none) — that skip line is
# this lane's honest output there, not a failure. TCG timings are meaningless;
# never point bench targets at this lane. No new pinned dependency: the host's
# binfmt qemu is unpinnable from inside the repo (no QEMU version is pinned
# anywhere in the tree). The simd suite links no optional engine, so the image
# is built with DEPS_ARGS=--pkgconfig — like docker-riscv64, it keeps its TCG
# image light rather than compiling Unicorn/Keystone under emulation.
DOCKER_SVE_IMAGE ?= asmtest-ci-arm64
DOCKER_SVE_VQS   ?= 1 3 8 16
.PHONY: docker-sve-sweep
docker-sve-sweep:
	$(DOCKER) build --platform linux/arm64 --build-arg BASE=$(DOCKER_BASE) \
	  --build-arg DEPS_ARGS=--pkgconfig -t $(DOCKER_SVE_IMAGE) .
	set -e; for vq in $(DOCKER_SVE_VQS); do \
	  echo "== docker-sve-sweep: QEMU_CPU=max,sve-max-vq=$$vq =="; \
	  $(DOCKER) run --rm --platform linux/arm64 \
	    -e QEMU_CPU=max,sve-max-vq=$$vq,sve-default-vector-length=-1 \
	    $(DOCKER_SVE_IMAGE) \
	    sh -c 'make build/test_simd >/dev/null && ./build/test_simd'; \
	done

# Data-flow live-attach lane (live-attach-dataflow-plan.md, Increment 1). A dedicated Capstone +
# Unicorn image (the base CI image builds Capstone from source only under --emu/--asm, not --all)
# running `make dataflow-test` with seccomp=unconfined so the PTRACE_SEIZE-based attach_pid
# producer — attach to an already-running FOREIGN pid, single-step a scoped region, DETACH so it
# SURVIVES — actually EXECUTES in-container (the default docker seccomp profile blocks the ptrace
# path, on which it self-skips DF_PTRACE_ETRACE rather than fail). The victim is the ptrace suite's
# own forked child (yama-safe) and the live L2 slices are cross-checked against the emulator L0.
.PHONY: docker-dataflow-attach
docker-dataflow-attach:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.dataflow-attach \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-dataflow-attach .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  asmtest-dataflow-attach

# F5 data-flow PT-replay lane (dataflow-pt-replay-tier). Reuses the SAME libipt+Unicorn image as
# docker-dataflow-attach (Dockerfile.dataflow-attach now carries libipt-dev), and runs
# `make dataflow-pt-test`: the SYNTHETIC decode->rebase->materialize->replay bridge (T2), the
# purity/replayability gates (T3), the replay-core byte-identity (T1) and the def-use/slice
# EQUIVALENCE against the emulator oracle (T5) — all exercised in-container with NO PT hardware
# (asmtest_pt_encode_fixture is libipt's own userspace encoder). The LIVE foreign-pid half (T4)
# self-skips cleanly here: no intel_pt PMU in a container on an AMD host, AND the sibling
# intel-pt-attach-foreign-pid capture symbol is not in the tree yet. seccomp=unconfined lets the
# code-image recorder's soft-dirty scan run, exactly as the attach lane needs it for ptrace.
.PHONY: docker-dataflow-pt
docker-dataflow-pt:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.dataflow-attach \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-dataflow-attach .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  asmtest-dataflow-attach make WERROR=1 dataflow-pt-test

docker-valgrind: docker-build
	$(_docker_run) make valgrind

docker-sanitize: docker-build
	$(_docker_run) make sanitize

docker-analyze: docker-build
	$(_docker_run) make tidy

docker-fmt-check: docker-build
	$(_docker_run) make fmt-check

# `fmt` must rewrite the HOST tree (the other lanes run on the baked-in /src
# copy, whose edits are discarded with the container), so this one bind-mounts
# the checkout over /src and runs as the invoking user to keep file ownership.
docker-fmt: docker-build
	$(DOCKER) run --rm $(_docker_plat) -v "$(CURDIR)":/src -w /src \
	  -u $(shell id -u):$(shell id -g) $(DOCKER_IMAGE) make fmt

docker-coverage: docker-build
	$(_docker_run) make coverage

# Reclaim build artifacts a previous docker/root run left owned by root:root —
# a recurring footgun where `make clean` (plain rm) and rebuilds then fail with
# EACCES. Chowns the whole checkout back to the invoking user via a throwaway
# root container (no image build needed). Run this if `make clean`/`make` starts
# failing on permission errors after using a `docker-*` lane.
fix-perms:
	$(DOCKER) run --rm $(_docker_plat) -v "$(CURDIR)":/src -w /src $(DOCKER_BASE) \
	  chown -R $(shell id -u):$(shell id -g) .

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
# K1 (repo-review 2026-07-04): how the shared bindings-base image is built. Its
# Keystone (trimmed LLVM) + Capstone source-build layer is the expensive part of
# every docker lane, so CI overrides this with a buildx invocation carrying
# `--cache-from/--cache-to type=gha` (see the ci.yml docker jobs) to reuse that
# layer across jobs and pushes. The default stays a plain `docker build`, so
# local behavior is unchanged; a cold cache is a no-op and the CI cache-to uses
# ignore-error=true, so an unavailable cache backend can never fail the build.
DOCKER_BASE_BUILD ?= $(DOCKER) build
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
# linux-tools-generic ships libperf-jvmti.so (HotSpot's jitdump encoder for the
# java-jitdump lane). It is a userspace JVMTI agent, so a kernel-version-mismatched
# package still works in the container; the other java lanes simply ignore it.
DOCKER_APT_java   := openjdk-25-jdk-headless linux-tools-generic
DOCKER_APT_dotnet := dotnet-sdk-8.0
DOCKER_APT_ruby   := ruby
DOCKER_APT_lua    := luajit
DOCKER_APT_go     := golang-go

ZIG_VERSION ?= 0.13.0
DOCKER_SETUP_node := npm install -g koffi
# Integrity pin (B5 residual): the zig tarball was the one third-party fetch with no
# digest check. Pinned per-arch; anchors recorded in scripts/third-party-digests.txt
# (gated by scripts/check-thirdparty-versions.sh). Bumping ZIG_VERSION without
# updating these digests fails the image build loudly — never ships unpinned.
ZIG_SHA256_x86_64  := d45312e61ebcc48032b77bc4cf7fd6915c11fa16e4aad116b66c9468211230ea
ZIG_SHA256_aarch64 := 041ac42323837eb5624068acd8b00cd5777dac4cf91179e8dad7a7e90dd0c556
DOCKER_SETUP_zig  := arch="$$(uname -m)"; \
  case "$$arch" in \
    x86_64)  zig_sha256=$(ZIG_SHA256_x86_64) ;; \
    aarch64) zig_sha256=$(ZIG_SHA256_aarch64) ;; \
    *) echo "no pinned zig digest for arch $$arch" >&2; exit 1 ;; \
  esac; \
  curl -fsSL "https://ziglang.org/download/$(ZIG_VERSION)/zig-linux-$$arch-$(ZIG_VERSION).tar.xz" -o /tmp/zig.tar.xz; \
  echo "$$zig_sha256  /tmp/zig.tar.xz" | sha256sum -c -; \
  mkdir -p /opt/zig; tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1; \
  rm /tmp/zig.tar.xz; ln -s /opt/zig/zig /usr/local/bin/zig; zig version

DOCKER_RUNENV_node   := -e NODE_PATH=/usr/local/lib/node_modules:/usr/lib/node_modules
DOCKER_RUNENV_go     := -e GOTOOLCHAIN=local -e GOFLAGS=-mod=mod -e GOPROXY=off
DOCKER_RUNENV_dotnet := -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1

.PHONY: docker-bindings-base docker-bindings docker-bindings-clean docker-clean-room \
        $(addprefix docker-,$(BINDING_LANGS)) \
        $(addprefix docker-build-,$(BINDING_LANGS)) \
        $(addprefix docker-clean-,$(BINDING_LANGS))

docker-bindings-base:
	$(DOCKER_BASE_BUILD) $(_docker_plat) -f Dockerfile.bindings-base \
	  --build-arg BASE=$(DOCKER_BASE) -t $(DOCKER_BINDINGS_BASE) .

# Generate, per language: `docker-build-<lang>` (build the image on the base),
# `docker-<lang>` (build, then run the default CMD = `make <lang>-test`), and
# `docker-clean-<lang>` (build, then run the cross-binding clean-room INSTALL test —
# scrubbed env, fresh install into a throwaway prefix, resolved-path assertion — which
# self-skips to this image's one binding). Splitting build from run lets the clean-room
# lane reuse the built image without also running the dev test.
define docker_lang_rule
docker-build-$(1): docker-bindings-base
	$$(DOCKER) build $$(_docker_plat) -f bindings/Dockerfile.lang \
	  --build-arg BASE_IMAGE=$$(DOCKER_BINDINGS_BASE) \
	  --build-arg APT_PKGS='$$(DOCKER_APT_$(1))' \
	  --build-arg SETUP='$$(DOCKER_SETUP_$(1))' \
	  --build-arg TARGET=$(1) -t asmtest-$(1) .
docker-$(1): docker-build-$(1)
	$$(DOCKER) run --rm $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) asmtest-$(1)
docker-clean-$(1): docker-build-$(1)
	$$(DOCKER) run --rm $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) asmtest-$(1) make clean-room-test CLEANROOM_ONLY=$(1)
endef
$(foreach L,$(BINDING_LANGS),$(eval $(call docker_lang_rule,$(L))))

# The dlopen bindings whose isolated image bundles both the language toolchain AND a
# self-contained native payload (@loader_path/$$ORIGIN-vendored deps), so the clean-room
# install test genuinely runs in-container. CLEANROOM_ONLY=<lang> makes the run FAIL if
# that binding self-skips (a missing toolchain would otherwise pass vacuously). python is
# excluded — self-containing its wheel needs auditwheel/build/venv that its lean test image
# omits, and the release.yml python job already runs the clean-room asserts on the repaired
# wheel. The link bindings (cpp/rust/go/zig) ship source (no bundled payload) — out of scope.
CLEANROOM_LANGS := ruby node java dotnet lua
docker-clean-room: $(addprefix docker-clean-,$(CLEANROOM_LANGS))

docker-bindings: $(addprefix docker-,$(BINDING_LANGS)) docker-win64

# Native-trace lane: a container with DynamoRIO installed that builds the tier and
# runs BOTH the C smoke harness and the Python language-wrapper suite. Separate
# from the binding images (it carries DynamoRIO, ~hundreds of MB, that the others
# don't need). Linux x86-64 only. (The Intel PT / AMD LBR hardware backends need bare
# metal; the single-step hardware backend, by contrast, IS containerizable — see the
# docker-hwtrace lane below.) Override DR_VERSION to bump the pinned DynamoRIO.
DR_VERSION ?= 11.91.20630
.PHONY: docker-drtrace
docker-drtrace:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.drtrace \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-drtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-drtrace

# SDE future/absent-ISA lane (pin-sde-future-isa-lane): an x86-64 image with a
# pinned, digest-gated Intel SDE 10.8.0 + APX-capable GAS (binutils 2.46.1) + NASM
# 3.02 + libunicorn that runs the suite under `sde64 -future`. SDE emulates ISA the
# host CPU lacks (APX/AVX10.2/AMX/AVX-512-on-AVX2) for the WHOLE process, so this
# lane needs NO future-ISA silicon — its entire reason to exist. Override
# SDE_VERSION to bump (a bump also needs the SDE/binutils/NASM URLs + digests in
# Dockerfile.sde / scripts/third-party-digests.txt). Linux x86-64 only; the sde-*
# targets self-skip off x86-64.
SDE_VERSION ?= 10.8.0
.PHONY: docker-sde
docker-sde:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.sde \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg SDE_VERSION=$(SDE_VERSION) \
	  -t asmtest-sde .
	$(DOCKER) run --rm $(_docker_plat) asmtest-sde

# Pin trace lane (pin-xed-trace-tier PIN-2 + pin-probe-mode-capture PIN-3): a container
# with a pinned, digest-gated Intel Pin kit (and DynamoRIO, for the three-way
# offset-parity arm) that builds the pintools + fixtures and runs `make pintool-test`
# (XED-decoded offset trace, Pin ≡ DR ≡ single-step) then `make pin-probe-test`
# (probe-mode arg/return capture diffed against the independent ptrace stepper). Pin
# decodes with XED, so PIN-2 can DBI-trace Intel APX today where the DynamoRIO decoder
# aborts. The probe lane's ptrace reference producer needs SYS_PTRACE, so the run adds
# --cap-add=SYS_PTRACE (as the taint-attach lanes do). Linux x86-64 only (Pin's pinned
# kit is gcc-linux; the pintool-* / pin-probe-* targets self-skip on arm64/macOS). Pin
# is proprietary freeware, fetched + digest-verified at build time and never bundled —
# test-lane only. Override PIN_VERSION to bump.
PIN_VERSION ?= 4.2-99776-g21d818fa2
.PHONY: docker-pintool
docker-pintool:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.pintool \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg PIN_VERSION=$(PIN_VERSION) \
	  --build-arg DR_VERSION=$(DR_VERSION) -t asmtest-pintool .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-pintool

# Throwaway extension-load probe lane (taint tier, Increment 2). A light image
# (DynamoRIO only, no Capstone/Unicorn) that builds drclient/probe_extensions.c
# and runs it under drrun, asserting the BSD-clean extension stack
# (drmgr/drreg/drx) loads under DR's private loader with a non-zero instrumented
# instruction count — the empirical yes/no gating the whole Phase-5 re-platform.
# See docs/internal/analysis/dr-extension-load-probe-findings.md.
.PHONY: docker-drext-probe
docker-drext-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.drext-probe \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-drext-probe .
	$(DOCKER) run --rm $(_docker_plat) asmtest-drext-probe

# In-band TAINT tier lane (taint tier, Increment 4 + Increment 5 native launch). Like
# docker-drtrace it installs DynamoRIO + Capstone + libunicorn, then runs `make
# dr-taint-native-test` (the in-process oracle diffs + negative controls) AND `make
# dr-taint-launch-test` (the launch-under-DR native workload + POSIX-shm + out-of-process
# validator). See docs/internal/plans/dynamorio-taint-tier-plan.md, Increments 4-5.
.PHONY: docker-taint-native
docker-taint-native:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-native \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-taint-native .
	$(DOCKER) run --rm $(_docker_plat) asmtest-taint-native

# DR ATTACH tier lane (dynamorio-attach-tier-plan.md, Increment 1, first slice): COOPERATIVE
# attach + detach on an already-running NATIVE process via the proven dr_app_* API. Like
# docker-taint-native it installs DynamoRIO + Capstone + libunicorn, then runs `make
# dr-taint-attach-coop-test`: examples/taint_attach_coop starts as a PLAIN native process (NOT
# under drrun), runs a fixture natively, self-attaches DR + the UNCHANGED taint client mid-run,
# captures a scoped window into POSIX-shm, detaches, runs native again, and a separate
# validator oracle-diffs the captured window out of process. No experimental API, no
# SYS_PTRACE — a plain `docker run` (the external foreign-PID injector landed separately as
# docker-taint-attach-probe below, which does need the cap). Also carries Increment 3's
# marker-less interactive nudge arm/disarm. CI-gated by the `taint-attach` job in
# .github/workflows/ci.yml. See docs/internal/archive/plans/dynamorio-attach-tier-plan.md.
.PHONY: docker-taint-attach
docker-taint-attach:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-attach \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-taint-attach .
	$(DOCKER) run --rm $(_docker_plat) asmtest-taint-attach

# DR ATTACH tier EXTERNAL-attach lane (Increments 2, 4 and 5). A light DR-only image that starts a
# plain native victim and injects DR into the RUNNING process via `drrun -attach <pid>`. Its CMD
# runs all four lanes: Increment 2's GO/NO-GO probe (a minimal counting client — the verdict was GO,
# recorded in dr-attach-probe-findings.md), Increment 4's marker-less external taint capture, and
# Increment 5's detach + K=3 attach/capture/detach cycling with the shadow-leaf leak assertion. The
# ptrace-seize needs CAP_SYS_PTRACE, so — UNLIKE the coop lane — the `docker run` adds
# `--cap-add=SYS_PTRACE` (mirroring the hwtrace cap lanes). Began as Increment 2's research probe,
# but landed capability was added to the SAME image, so it is now CI-gated by the `taint-attach`
# job in .github/workflows/ci.yml (a hosted runner grants the cap), not a manual diagnostic.
.PHONY: docker-taint-attach-probe
docker-taint-attach-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-attach-probe \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-taint-attach-probe .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-taint-attach-probe

# DR ATTACH tier Increment 6: MANAGED-attach empirical probe (research-gated spike). Like the
# native probe above but the victim is a running .NET process: it injects DR + the counting client
# into `dotnet` via `drrun -attach <pid>` and prints GO/NO-GO for whether the managed process
# survives takeover + detach without swallowing a .NET signal or crashing (the kill criterion).
# Needs the .NET SDK (in the image) + CAP_SYS_PTRACE (added here). A no-go is a valid research
# finding (recorded in dr-managed-attach-probe-findings.md) — a manual diagnostic, not in the gate.
.PHONY: docker-taint-managed-attach-probe
docker-taint-managed-attach-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-managed-attach-probe \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-taint-managed-attach-probe .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-taint-managed-attach-probe

# TAINT tier dotnet coexistence lane (taint tier, Increment 5). Installs DynamoRIO + the
# .NET SDK (no Capstone/Unicorn) and runs `make dr-taint-dotnet-test`:
# `drrun -c <taint client> -- dotnet taint_hello.dll` — the first in-tree test of DR's
# code cache coexisting with .NET's tiered JIT (the plan's risk concentration).
.PHONY: docker-taint-dotnet
docker-taint-dotnet:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.taint-dotnet \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-taint-dotnet .
	$(DOCKER) run --rm $(_docker_plat) asmtest-taint-dotnet

# GC-move-range extraction go/no-go PROBE (taint tier, Increment 7 / Phase-4). Installs
# DynamoRIO + the .NET SDK + git and runs `make dr-gcprofiler-probe`: a minimal in-process CLR
# profiler (ICorProfilerCallback4::MovedReferences2) runs under `drrun -c <taint client> --
# dotnet <compacting-GC workload>`, proving a profiler coexists with DynamoRIO on Linux and
# delivers the {old,new,len} move ranges the shadow remap needs. See
# docs/internal/analysis/gc-move-range-extraction-findings.md.
.PHONY: docker-gcprofiler-probe
docker-gcprofiler-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.gcprofiler-probe \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-gcprofiler-probe .
	$(DOCKER) run --rm $(_docker_plat) asmtest-gcprofiler-probe

# F4 ATTACH-MODE profiler go/no-go PROBE (live-attach-dataflow-followup-plan.md F4). Unlike the
# gcprofiler probe this needs NO DynamoRIO and no SYS_PTRACE — it is the out-of-band ptrace tier's
# question (dotnet SDK + C++ toolchain + git only): can a CLR profiler be ATTACHED to an
# already-running dotnet over its diagnostics port (CORECLR_ENABLE_PROFILING is startup-read, so the
# live-attach tier cannot use the DR tier's env-var wiring) and still receive
# ICorProfilerCallback4::MovedReferences2 GC-move ranges? Runs `make attachprof-probe`. See
# docs/internal/analysis/f4-attach-profiler-probe-findings.md.
.PHONY: docker-attachprof-probe
docker-attachprof-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.attachprof-probe \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-attachprof-probe .
	$(DOCKER) run --rm $(_docker_plat) asmtest-attachprof-probe

# F4 GC-FENCE FREEZE MEASUREMENT probe (live-attach-dataflow-followup-plan.md F4). The attach probe
# above proved the FEED; this one measures the assumption F4's STAMPING rests on: that the GC fence
# suspends the EE so completely that a ptrace-single-stepped managed thread retires ZERO
# instructions across it, so asmtest_gcmove_t.step (an index into insn_off[]) can be read at DRAIN
# time. An attach-mode profiler samples the tracer's live step counter at GarbageCollectionStarted
# (S0) and GarbageCollectionFinished (S1); S1-S0 is the verdict, and fence-time /proc state says
# whether the thread is blocked or spinning. No DynamoRIO (out-of-band ptrace tier), but — UNLIKE
# docker-attachprof-probe — it DOES need `--cap-add=SYS_PTRACE`, because the stepper ptrace-attaches
# to a SIBLING process (mirroring docker-taint-attach-probe). Runs `make gcfence-probe`. See
# docs/internal/analysis/f4-gc-fence-freeze-probe-findings.md.
.PHONY: docker-gcfence-probe
docker-gcfence-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.gcfence-probe \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-gcfence-probe .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-gcfence-probe

# F4 INCREMENTS 1+2 — the live GC-move canonicalization LANE (live-attach-dataflow-followup-plan.md
# F4). NOT a probe: the two probes above settled the questions (the feed attaches to a running dotnet;
# the stamp must be the profiler-sampled S0, because the freeze assumption measured FALSE), and this
# is the WIRING plus its validation. It joins the proven feed to the landed pure transform
# (asmtest_gcmove_canonicalize) on a live attach, and proves the join with a NEGATIVE CONTROL — the
# same capture is def-use-built WITHOUT canonicalization (the store->load edge across the compaction
# must be MISSING) and WITH it (it must APPEAR).
#
# Increment 2 adds the phases that close increment 1's open limitation — a window's GCs all share one
# S0, so two of them collapsed into one batch and under-forwarded a twice-moved object. `--selftest`
# (pure, no dotnet) proves the composition against the SHIPPING transform on randomized N-GC feeds;
# then one phase per GCCANON_PHASES value runs a fresh victim with that many compacting GCs
# choreographed into ONE call-out window, reproducing the collapse as a FAILING case (the edge is
# still missing) before the chained feed restores it.
#
# No DynamoRIO (out-of-band ptrace tier), but — like docker-gcfence-probe and UNLIKE
# docker-attachprof-probe — it DOES need `--cap-add=SYS_PTRACE`, because the tracer ptrace-attaches
# to a SIBLING process. Runs `make gccanon-attach`.
.PHONY: docker-gccanon-attach
docker-gccanon-attach:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.gccanon-attach \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-gccanon-attach .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-gccanon-attach

# MANAGED-ATTACH SAFEPOINT plan Increment-1 suspend-primitive probe. Same image shape as the
# gcprofiler probe (DynamoRIO + .NET SDK + git for the CoreCLR profiler headers), no SYS_PTRACE
# (launch, not attach): drives ICorProfilerInfo10::SuspendRuntime/ResumeRuntime cycles natively and
# under `drrun -c <taint client>`, the go/no-go for whether the Option-2 managed-attach path's
# suspension primitive survives DR coexistence. See dynamorio-managed-attach-safepoint-plan.md.
.PHONY: docker-suspendprof-probe
docker-suspendprof-probe:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.suspendprof-probe \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg DR_VERSION=$(DR_VERSION) \
	  -t asmtest-suspendprof-probe .
	$(DOCKER) run --rm --cap-add=SYS_PTRACE $(_docker_plat) asmtest-suspendprof-probe

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

# --- Hardware-trace (single-step) lane in Docker ---------------------------
# The single-step backend needs NO DynamoRIO, NO perf_event, NO privilege and NO
# extra Linux capability (it drives EFLAGS.TF -> SIGTRAP, baseline x86-64
# userspace). So unlike docker-drtrace these run under a PLAIN `docker run` — no
# --privileged/--cap-add/--security-opt. That portability is the whole point: the
# hardware-trace tier is containerizable and CI-able here for the first time.
#   make docker-hwtrace            C smoke + Python wrapper, in a plain container
#   make docker-hwtrace-bindings   every language wrapper, in a plain container
#   make docker-hwtrace-<lang>     just one (e.g. docker-hwtrace-rust)
#   make docker-hwtrace-attach-demo trace a SEPARATE process asm-test did not start (by PID)
#   make docker-hwtrace-syscall-log log a SEPARATE process's syscalls + data (a minimal strace)
#   make docker-hwtrace-jit        trace a live Node.js V8 JIT method out of band
#   make docker-hwtrace-jit-dotnet trace a live .NET CoreCLR JIT method out of band
#   make docker-hwtrace-jit-java   trace a live OpenJDK HotSpot JIT method out of band
#   make docker-hwtrace-jit-dotnet-bcl trace a live .NET Console.WriteLine (BCL) out of band
#   make docker-hwtrace-jit-java-bcl trace a live OpenJDK Math.floorDiv (JDK lib) out of band
#   make docker-hwtrace-jit-jitdump recover a real V8 jitdump method's bytes (binary path)
#   make docker-hwtrace-jit-java-jitdump recover a real HotSpot jitdump method's bytes
#   make docker-hwtrace-jit-dotnet-jitdump recover a real CoreCLR jitdump method's bytes
HWTRACE_DOCKER_LANGS := cpp rust go node java dotnet ruby lua zig

.PHONY: docker-hwtrace docker-hwtrace-attach-demo docker-hwtrace-syscall-log docker-hwtrace-amd docker-hwtrace-msr docker-hwtrace-ibs docker-hwtrace-privileged docker-hwtrace-codeimage docker-hwtrace-dotnet-amd docker-hwtrace-bindings \
        docker-hwtrace-jit docker-hwtrace-jit-dotnet docker-hwtrace-jit-java \
        docker-hwtrace-jit-java-jitdump docker-hwtrace-jit-jitdump \
        docker-hwtrace-jit-dotnet-jitdump \
        docker-hwtrace-jit-dotnet-bcl docker-hwtrace-jit-java-bcl \
        $(addprefix docker-hwtrace-,$(HWTRACE_DOCKER_LANGS))

docker-hwtrace: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-hwtrace

# "Trace a process asm-test did NOT start", end to end in a PLAIN container (no
# privilege, no --cap-add): attach_trace attaches to a SEPARATE attach_victim
# process BY PID and single-steps one call of its hot function out of band. The
# victim opts in via PR_SET_PTRACER_ANY, so it works under a default `docker run`
# even though Yama ptrace_scope (a non-namespaced host setting) is commonly 1.
docker-hwtrace-attach-demo: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-hwtrace make hwtrace-attach-demo

# The DATA-logging sibling: attach to a SEPARATE process and log its syscalls WITH
# the buffers/paths crossing the kernel boundary (a minimal strace on the ptrace
# seam). Plain container; the victim opts in via PR_SET_PTRACER_ANY.
docker-hwtrace-syscall-log: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-hwtrace make hwtrace-syscall-log

# Real managed-runtime trace: trace a live JIT method out of band. Each reuses a
# per-language image (runtime + Capstone + source) and runs the argv-driven `jit_trace`
# harness, which attaches to a runtime child it spawns — a plain `docker run`, no
# privilege (ptrace of one's own child needs none). Self-skips cleanly if the runtime
# does not cooperate (re-tiered/moved code, ptrace denied).
docker-hwtrace-jit: docker-node
	$(DOCKER) run --rm $(_docker_plat) asmtest-node make hwtrace-jit-node

docker-hwtrace-jit-dotnet: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) asmtest-dotnet make hwtrace-jit-dotnet

# The scoped-tracing demos (examples/dotnet, all projects), in the .NET image.
.PHONY: docker-hwtrace-dotnet-example
docker-hwtrace-dotnet-example: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) asmtest-dotnet make hwtrace-dotnet-example

# Interactive dev shell in the asmtest-dotnet container for building + running the
# dotnet examples. The working tree is LIVE-mounted at /src (edit examples/bindings on
# the host, rebuild + run inside), while build/ is a container-local NAMED VOLUME — a
# Capstone-enabled build isolated from the host's build/ (which is built without
# Capstone, so the region example's rendering would otherwise break). The shared lib is
# built on entry and the DllImport-resolver env vars are set, so `dotnet run` works at
# once. `docker volume rm asmtest-dotnet-build` for a clean rebuild.
.PHONY: dev-dotnet
dev-dotnet: docker-build-dotnet
	@echo ''
	@echo 'dev-dotnet: interactive shell in asmtest-dotnet (working tree live at /src).'
	@echo 'Building shared-hwtrace on entry; then run e.g.:'
	@echo '  dotnet run --project examples/dotnet/methods/methods.csproj'
	@echo '  dotnet run --project examples/dotnet/wholewindow/wholewindow.csproj'
	@echo '  dotnet run --project examples/dotnet/region/region.csproj'
	@echo '  make hwtrace-dotnet-example   # all demos   |   make hwtrace-dotnet-test'
	@echo ''
	$(DOCKER) run --rm -it $(_docker_plat) $(DOCKER_RUNENV_dotnet) \
	  -v "$(CURDIR)":/src -v asmtest-dotnet-build:/src/build -w /src \
	  -e ASMTEST_HWTRACE_LIB=/src/build/libasmtest_hwtrace.so \
	  -e LD_LIBRARY_PATH=/src/build \
	  asmtest-dotnet bash -c 'make shared-hwtrace; exec bash'

docker-hwtrace-jit-dotnet-bcl: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) asmtest-dotnet make hwtrace-jit-dotnet-bcl

# Call-descent demo lanes (Phase 8): descend into the runtime's sibling JIT methods at L2
# (`-descend`) or everything at L3 (`-descend-all`, guarded + expected to self-skip).
.PHONY: docker-hwtrace-jit-dotnet-descend docker-hwtrace-jit-dotnet-descend-all \
        docker-hwtrace-jit-dotnet-bcl-descend docker-hwtrace-jit-dotnet-bcl-descend-all \
        docker-hwtrace-jit-java-descend docker-hwtrace-jit-java-descend-all
docker-hwtrace-jit-dotnet-descend docker-hwtrace-jit-dotnet-descend-all \
docker-hwtrace-jit-dotnet-bcl-descend docker-hwtrace-jit-dotnet-bcl-descend-all: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) asmtest-dotnet make $(patsubst docker-%,%,$@)
docker-hwtrace-jit-java-descend docker-hwtrace-jit-java-descend-all: docker-java
	$(DOCKER) run --rm $(_docker_plat) asmtest-java make $(patsubst docker-%,%,$@)

docker-hwtrace-jit-dotnet-jitdump: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) asmtest-dotnet make hwtrace-jit-dotnet-jitdump

docker-hwtrace-jit-java: docker-java
	$(DOCKER) run --rm $(_docker_plat) asmtest-java make hwtrace-jit-java

docker-hwtrace-jit-java-bcl: docker-java
	$(DOCKER) run --rm $(_docker_plat) asmtest-java make hwtrace-jit-java-bcl

docker-hwtrace-jit-java-jitdump: docker-java
	$(DOCKER) run --rm $(_docker_plat) asmtest-java make hwtrace-jit-java-jitdump

docker-hwtrace-jit-jitdump: docker-node
	$(DOCKER) run --rm $(_docker_plat) asmtest-node make hwtrace-jit-jitdump

# AMD LBR live lane: same image, but with perf access so the AMD branch-stack
# backend actually runs instead of self-skipping. Unlike the single-step lane this
# is NOT a plain container — perf_event_open needs the default seccomp profile
# relaxed (it blocks the syscall) and CAP_PERFMON; it still needs an AMD Zen 3+/4/5
# host, but NOT a lowered perf_event_paranoid: CAP_PERFMON bypasses the sysctl
# entirely (MEASURED 2026-07-17 at paranoid=4 — unprivileged EACCES, with the cap
# fd=3; do not lower a host sysctl for this lane). On any other host the AMD test
# self-skips, so this lane is for a self-hosted AMD runner. (PT remains
# bare-metal-Intel only.)
docker-hwtrace-amd: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  --cap-add=PERFMON asmtest-hwtrace make hwtrace-test

# MSR-direct AMD LBR lane (src/msr_lbr.c): the same hwtrace image, run --privileged so the
# per-CPU MSR device nodes (/dev/cpu/N/msr — a directory --device cannot expose, and the
# thread may be scheduled on any core) are readable and asmtest_amd_msr_trace can read the
# LbrExtV2 FROM/TO MSRs directly for a zero-PMU-interrupt Tier-A capture. Needs the host
# `msr` kernel module loaded and an AMD amd_lbr_v2 host; the MSR test self-skips everywhere
# else (all other lanes lack /dev/cpu access), so this is a self-hosted AMD lane.
#
# Second consumer: W3 in-process BTF branch-granular single-step (src/ss_btf.c,
# asmtest_ss_btf_trace) rides the exact same /dev/cpu/N/msr access this lane already
# grants — no new capability, no new rule. Unlike the LBR lane above it needs no AMD
# amd_lbr_v2 substrate at all (DEBUGCTL.BTF is baseline AMD64/Intel); its own gate is
# bare-metal (a hypervisor masking DEBUGCTL.BTF self-skips, decided by the T2 functional
# probe, not a build guard) + the same CAP_SYS_ADMIN + msr module this lane provides.
docker-hwtrace-msr: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  --privileged asmtest-hwtrace make hwtrace-test

# Statistical AMD IBS-Op edge lane (src/ibs_backend.c). Same hwtrace image; runs
# `make ibs-test` (the pure decoder checks always run; the live out-of-band capture
# runs on an AMD IBS host, self-skips elsewhere). Unlike the LBR lane this needs NO
# capability: the kernel `swfilt` bit makes user-only IBS sampling open unprivileged
# at perf_event_paranoid=2 — but Docker's default seccomp still blocks perf_event_open,
# so seccomp must be unconfined. Needs an AMD host with the ibs_op PMU (Zen 2+) and a
# kernel exposing swfilt (~6.2+); the live test self-skips on any other host.
docker-hwtrace-ibs: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  asmtest-hwtrace make ibs-test

# First-class privileged hardware-capture lane: the same hwtrace image, run with
# CAP_PERFMON under Docker's DEFAULT seccomp profile — which gates perf_event_open
# on exactly that capability, so unlike the -amd/-ibs lanes above this needs NO
# seccomp=unconfined (and no --privileged, no custom profile). CAP_PERFMON also
# bypasses kernel.perf_event_paranoid — including the Debian/Ubuntu "=4, no
# unprivileged perf at all" setting — so on an AMD Zen 3+/4/5 host the exact AMD
# LBR tier (LbrExtV2 live capture, sample_window survey) AND the live IBS lanes
# (out-of-band capture + whole-process survey) actually run instead of
# self-skipping. This is the one lane for a self-hosted AMD runner; everywhere
# else the same tests self-skip and it degrades to the plain single-step run.
# (Intel PT stays bare-metal; the MSR-direct test still needs docker-hwtrace-msr.)
docker-hwtrace-privileged: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) --cap-add=PERFMON \
	  asmtest-hwtrace make hwtrace-test ibs-test

# Optional eBPF code-emission detector lane. Builds an image WITH the eBPF toolchain
# (clang + libbpf-dev + bpftool — which the plain hwtrace image omits), compiles the CO-RE
# program + skeleton, and runs `make codeimage-test`: the userspace soft-dirty recorder
# (always) plus the eBPF emission test. The detector needs CAP_BPF (load) + CAP_PERFMON
# (attach tracepoints), and Docker's default seccomp profile blocks bpf(2)/perf_event_open(2),
# so this runs with specific caps + seccomp=unconfined (NOT --privileged); SYS_PTRACE is
# added so the same image can also drive the recorder end to end. Needs a BTF-enabled
# kernel (CONFIG_DEBUG_INFO_BTF); the eBPF test self-skips otherwise.
docker-hwtrace-codeimage: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace-codeimage \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace-codeimage .
	$(DOCKER) run --rm $(_docker_plat) \
	  --cap-add=BPF --cap-add=PERFMON --cap-add=SYS_PTRACE \
	  --security-opt seccomp=unconfined --ulimit memlock=-1:-1 \
	  -v /sys/kernel/tracing:/sys/kernel/tracing:ro \
	  asmtest-hwtrace-codeimage

# E6 — the MANAGED-checkpoint tiling lane: the ONE image carrying both the .NET SDK and the
# eBPF toolchain, so a JIT'd managed method entry can actually be breakpointed and its frozen
# LBR island merged into WindowHot.Addresses. The native branchtile test proves the producer on
# C entries; only this lane can prove the MANAGED-checkpoint claim E6 is actually about, which
# is why it exists despite the plan's "optional operational plumbing" note (written for the
# sampled AMD examples, which their native siblings cover).
#
# Same privilege shape as docker-hwtrace-codeimage: CAP_BPF (load the program) + CAP_PERFMON
# (breakpoint + branch-stack perf events) + seccomp=unconfined (Docker's default profile blocks
# bpf(2)/perf_event_open(2)) + memlock unlimited (BPF maps). NOT --privileged. CAP_PERFMON also
# bypasses kernel.perf_event_paranoid, including the Debian/Ubuntu "=4, no unprivileged perf"
# default, so on an AMD Zen 4/5 host this runs LIVE. On any other host the demo self-skips
# (exit 0) — a self-hosted-Zen lane, like its docker-hwtrace-amd sibling.
#
# ASMTEST_TILE_REQUIRE=1 makes a self-skip FATAL here (the CLEANROOM_ONLY=<lang> pattern:
# fail rather than let a lane pass vacuously). This lane exists for exactly one purpose —
# to prove tiling at a MANAGED checkpoint — so if it cannot, that is the news, not a pass.
# Set it only here; every other lane keeps the ordinary self-skip. On a non-Zen / non-CAP_BPF
# host, run the image without this variable to get the graceful skip.
docker-hwtrace-dotnet-amd: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace-dotnet-amd \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace-dotnet-amd .
	$(DOCKER) run --rm $(_docker_plat) \
	  --cap-add=BPF --cap-add=PERFMON \
	  --security-opt seccomp=unconfined --ulimit memlock=-1:-1 \
	  -e ASMTEST_TILE_REQUIRE=1 \
	  asmtest-hwtrace-dotnet-amd

# Reuse each already-built per-language image (asmtest-<lang>: full source +
# toolchain + Capstone) and just run its hwtrace test target — no extra image
# layer needed (no DynamoRIO). Plain `docker run`.
define docker_hwtrace_lang_rule
docker-hwtrace-$(1): docker-$(1)
	$$(DOCKER) run --rm $$(_docker_plat) $$(DOCKER_RUNENV_$(1)) asmtest-$(1) \
	  make hwtrace-$(1)-test
endef
$(foreach L,$(HWTRACE_DOCKER_LANGS),$(eval $(call docker_hwtrace_lang_rule,$(L))))

docker-hwtrace-bindings: $(addprefix docker-hwtrace-,$(HWTRACE_DOCKER_LANGS))

# Forward-runtime drift check: the dotnet hwtrace self-test on .NET 9, in the same
# asmtest-dotnet image (net9 installed user-local at run time; self-skips offline).
.PHONY: docker-hwtrace-dotnet9
docker-hwtrace-dotnet9: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) $(DOCKER_RUNENV_dotnet) asmtest-dotnet \
	  make hwtrace-dotnet9-test

# Slow-host crash-avoidance stress: the tiering worker UNPINNED (no
# hwtrace_dotnet_env), lazy-arm Invokes adjacent to worker respawns. The container
# shares the host's (CI runner's) scheduling noise — the environment that killed the
# old stepped-DynamicInvoke path — so running it on a loaded runner is the real
# validation; on a fast dev box it just passes quickly.
.PHONY: docker-hwtrace-dotnet-stress
docker-hwtrace-dotnet-stress: docker-dotnet
	$(DOCKER) run --rm $(_docker_plat) $(DOCKER_RUNENV_dotnet) asmtest-dotnet \
	  make hwtrace-dotnet-stress

# libasmtest_emu is the superset and Dockerfile.bindings-base now carries Keystone
# + Capstone, so each per-language image exercises the in-line-assembler AND
# disassembler path under `make <lang>-test` — there is no separate asm image.

# sshpass client image for the Docker-OSX lane — see Dockerfile.sshpass. Keeps
# the lane's ssh/scp calls off a host sshpass install (which would need sudo).
.PHONY: docker-sshpass
docker-sshpass:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.sshpass \
	  --build-arg BASE=$(DOCKER_BASE) -t asmtest-sshpass .
	$(DOCKER) run --rm asmtest-sshpass sshpass -V | head -1

# --- Track D: Docker-OSX x86 macOS clean room (macOS clean-room plan) --------
# On-demand x86-64 macOS clean room on a bare-metal Linux host with KVM — the
# vanilla-Intel-userland counterpart of the tart lane (Track C / osx-vm-test).
# Boots sickcodes/Docker-OSX headless (QEMU + OpenCore + a macOS recovery
# image), SSHes in on localhost:50922, and runs the same Track-A clean-room
# install test (scripts/docker-osx-bindings.sh). Hard-errors without /dev/kvm —
# hosted CI runners and most laptops don't have it, by design.
# WRITTEN PER docs/internal/plans/macos-clean-test-plan.md, NOT YET VALIDATED — authored
# without a KVM-capable bare-metal host; see docs/clean-room-testing.md.
# Upstream note (verified 2026-07-17): sickcodes/docker-osx tags other than
# :latest/:master were deleted from Docker Hub in 2024 — :ventura et al. 404.
# :latest's first boot is an interactive macOS installer, so headless runs need
# a prebuilt disk via DOCKER_OSX_DISK (see scripts/docker-osx-bindings.sh).
DOCKER_OSX_IMAGE ?= sickcodes/docker-osx:latest
.PHONY: docker-osx-bindings
docker-osx-bindings: docker-sshpass
	@[ -e /dev/kvm ] || { \
	  echo "docker-osx-bindings: /dev/kvm absent — needs a bare-metal Linux host with KVM (hosted CI/laptops: unsupported by design; see docs/clean-room-testing.md)"; \
	  exit 1; }
	DOCKER_OSX_IMAGE="$(DOCKER_OSX_IMAGE)" ASMTEST_REPO_ROOT="$(CURDIR)" \
	  sh scripts/docker-osx-bindings.sh

docker-bindings-clean:
	-$(DOCKER) image rm $(addprefix asmtest-,$(BINDING_LANGS)) asmtest-win64 \
	  $(DOCKER_BINDINGS_BASE)

# --- System-package verification lanes (distribution-packaging.md T8-T13) ------
# Each lane builds the C core's OS package (Homebrew/Debian/AUR/vcpkg/conan) and
# runs its native lint + install + the shared pkg-config consumer smoke, mirroring
# the docker-drtrace shape (a Dockerfile.syspkg-<mgr> built with pinned build-args,
# then `docker run --rm`). The packages are MIT-only (the static core links nothing
# third-party; the GPL engines live only in the dlopen binding payloads).
#
# Hermetic by design: the manifests pin the v1.1.0 release tarball ASSET, which is
# published by T3 (a maintainer/credential action) and does not exist in this tree
# yet — so the lanes consume the reproducible `make package-source` tarball staged
# into the build context here (build/ is kept out of images by .dockerignore, so
# `syspkg-stage` copies it into the git-ignored packaging/.staging/). Only each
# spec's real registry/tag publish or upstream PR (step 4) is credential-gated.
SYSPKG_STAGE := packaging/.staging
.PHONY: syspkg-stage
syspkg-stage: package-source
	mkdir -p $(SYSPKG_STAGE)
	cp $(DIST)/asm-test-$(ASMTEST_VERSION).tar.gz $(DIST)/SHA256SUMS $(SYSPKG_STAGE)/

# T8 — Homebrew formula: build-from-source + brew test/audit/style on Linux.
# Override BREW_VERSION to bump the pinned homebrew/brew image.
BREW_VERSION ?= 4.6.20
.PHONY: docker-syspkg-brew
docker-syspkg-brew: syspkg-stage
	$(DOCKER) build $(_docker_plat) -f Dockerfile.syspkg-brew \
	  --build-arg BREW_VERSION=$(BREW_VERSION) --build-arg VER=$(ASMTEST_VERSION) \
	  -t asmtest-syspkg-brew .
	$(DOCKER) run --rm $(_docker_plat) asmtest-syspkg-brew

# T9 — Debian libasmtest-dev: dpkg-buildpackage + lintian + install + consumer.
# Native (3.0) build straight from the tree, so no staged tarball is needed.
# Override DEB_BASE to bump the pinned Debian base image.
DEB_BASE ?= debian:bookworm-slim
.PHONY: docker-syspkg-deb
docker-syspkg-deb:
	$(DOCKER) build $(_docker_plat) -f Dockerfile.syspkg-deb \
	  --build-arg DEB_BASE=$(DEB_BASE) -t asmtest-syspkg-deb .
	$(DOCKER) run --rm $(_docker_plat) asmtest-syspkg-deb

# T10 — AUR PKGBUILD: makepkg build+check+package + namcap + .SRCINFO diff +
# pacman -U + consumer, on x86_64 (the official archlinux image has no aarch64).
# Override ARCH_TAG to bump the pinned dated archlinux base-devel image.
ARCH_TAG ?= base-devel-20260712.0.555161
.PHONY: docker-syspkg-aur
docker-syspkg-aur: syspkg-stage
	$(DOCKER) build $(_docker_plat) -f Dockerfile.syspkg-aur \
	  --build-arg ARCH_TAG=$(ARCH_TAG) --build-arg VER=$(ASMTEST_VERSION) \
	  -t asmtest-syspkg-aur .
	$(DOCKER) run --rm $(_docker_plat) asmtest-syspkg-aur

# T11 — vcpkg overlay port: vcpkg install asmtest + a CMake/pkg-config consumer.
# Override VCPKG_REF to bump the pinned microsoft/vcpkg release tag.
VCPKG_REF ?= 2026.06.24
.PHONY: docker-syspkg-vcpkg
docker-syspkg-vcpkg: syspkg-stage
	$(DOCKER) build $(_docker_plat) -f Dockerfile.syspkg-vcpkg \
	  --build-arg BASE=$(DOCKER_BASE) --build-arg VCPKG_REF=$(VCPKG_REF) \
	  --build-arg VER=$(ASMTEST_VERSION) -t asmtest-syspkg-vcpkg .
	$(DOCKER) run --rm $(_docker_plat) asmtest-syspkg-vcpkg

# T12 — Conan 2 recipe: conan create builds the recipe + runs its test_package.
# Override CONAN_VERSION to bump the pinned conan release.
CONAN_VERSION ?= 2.30.0
.PHONY: docker-syspkg-conan
docker-syspkg-conan: syspkg-stage
	$(DOCKER) build $(_docker_plat) -f Dockerfile.syspkg-conan \
	  --build-arg CONAN_VERSION=$(CONAN_VERSION) --build-arg VER=$(ASMTEST_VERSION) \
	  -t asmtest-syspkg-conan .
	$(DOCKER) run --rm $(_docker_plat) asmtest-syspkg-conan

# T13 — aggregate: prove all five system packagings in one command (serial). The
# `syspkg` CI job in .github/workflows/ci.yml runs the five lanes as a matrix.
.PHONY: docker-syspkg
docker-syspkg: docker-syspkg-brew docker-syspkg-deb docker-syspkg-aur \
               docker-syspkg-vcpkg docker-syspkg-conan

