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
_docker_plat := $(if $(DOCKER_PLATFORM),--platform $(DOCKER_PLATFORM),)
_docker_run  := $(DOCKER) run --rm $(_docker_plat) $(DOCKER_IMAGE)

.PHONY: docker-build docker-test docker-nasm docker-emu docker-asm \
        docker-valgrind docker-sanitize docker-analyze docker-fmt-check \
        docker-coverage docker-ci docker-shell docker-clean

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

docker-fmt-check: docker-build
	$(_docker_run) make fmt-check

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
DOCKER_SETUP_zig  := arch="$$(uname -m)"; \
  curl -fsSL "https://ziglang.org/download/$(ZIG_VERSION)/zig-linux-$$arch-$(ZIG_VERSION).tar.xz" -o /tmp/zig.tar.xz; \
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
	$(DOCKER) build $(_docker_plat) -f Dockerfile.bindings-base \
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
#   make docker-hwtrace-jit        trace a live Node.js V8 JIT method out of band
#   make docker-hwtrace-jit-dotnet trace a live .NET CoreCLR JIT method out of band
#   make docker-hwtrace-jit-java   trace a live OpenJDK HotSpot JIT method out of band
#   make docker-hwtrace-jit-dotnet-bcl trace a live .NET Console.WriteLine (BCL) out of band
#   make docker-hwtrace-jit-java-bcl trace a live OpenJDK Math.floorDiv (JDK lib) out of band
#   make docker-hwtrace-jit-jitdump recover a real V8 jitdump method's bytes (binary path)
#   make docker-hwtrace-jit-java-jitdump recover a real HotSpot jitdump method's bytes
#   make docker-hwtrace-jit-dotnet-jitdump recover a real CoreCLR jitdump method's bytes
HWTRACE_DOCKER_LANGS := cpp rust go node java dotnet ruby lua zig

.PHONY: docker-hwtrace docker-hwtrace-amd docker-hwtrace-codeimage docker-hwtrace-bindings \
        docker-hwtrace-jit docker-hwtrace-jit-dotnet docker-hwtrace-jit-java \
        docker-hwtrace-jit-java-jitdump docker-hwtrace-jit-jitdump \
        docker-hwtrace-jit-dotnet-jitdump \
        docker-hwtrace-jit-dotnet-bcl docker-hwtrace-jit-java-bcl \
        $(addprefix docker-hwtrace-,$(HWTRACE_DOCKER_LANGS))

docker-hwtrace: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) asmtest-hwtrace

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
# host with perf_event_paranoid lowered. On any other host the AMD test self-skips,
# so this lane is for a self-hosted AMD runner. (PT remains bare-metal-Intel only.)
docker-hwtrace-amd: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.hwtrace \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-hwtrace .
	$(DOCKER) run --rm $(_docker_plat) --security-opt seccomp=unconfined \
	  --cap-add=PERFMON asmtest-hwtrace make hwtrace-test

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

# libasmtest_emu is the superset and Dockerfile.bindings-base now carries Keystone
# + Capstone, so each per-language image exercises the in-line-assembler AND
# disassembler path under `make <lang>-test` — there is no separate asm image.

# --- Track D: Docker-OSX x86 macOS clean room (macOS clean-room plan) --------
# On-demand x86-64 macOS clean room on a bare-metal Linux host with KVM — the
# vanilla-Intel-userland counterpart of the tart lane (Track C / osx-vm-test).
# Boots sickcodes/Docker-OSX headless (QEMU + OpenCore + a macOS recovery
# image), SSHes in on localhost:50922, and runs the same Track-A clean-room
# install test (scripts/docker-osx-bindings.sh). Hard-errors without /dev/kvm —
# hosted CI runners and most laptops don't have it, by design.
# WRITTEN PER docs/plans/macos-clean-test-plan.md, NOT YET VALIDATED — authored
# without a KVM-capable bare-metal host; see docs/clean-room-testing.md.
DOCKER_OSX_IMAGE ?= sickcodes/docker-osx:ventura
.PHONY: docker-osx-bindings
docker-osx-bindings:
	@[ -e /dev/kvm ] || { \
	  echo "docker-osx-bindings: /dev/kvm absent — needs a bare-metal Linux host with KVM (hosted CI/laptops: unsupported by design; see docs/clean-room-testing.md)"; \
	  exit 1; }
	DOCKER_OSX_IMAGE="$(DOCKER_OSX_IMAGE)" ASMTEST_REPO_ROOT="$(CURDIR)" \
	  sh scripts/docker-osx-bindings.sh

docker-bindings-clean:
	-$(DOCKER) image rm $(addprefix asmtest-,$(BINDING_LANGS)) asmtest-win64 \
	  $(DOCKER_BINDINGS_BASE)

