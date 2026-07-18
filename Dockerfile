# Dockerfile — run asm-test's Linux CI jobs locally in a container.
#
# Covers the Linux half of the CI matrix: x86-64 natively, and aarch64 or
# riscv64 via emulation when built/run with `--platform linux/arm64` or
# `--platform linux/riscv64` (Docker Desktop ships qemu/Rosetta; on Linux run
# `docker run --privileged tonistiigi/binfmt` once — `make binfmt-riscv64` for
# the rv64 lane). The macOS jobs CANNOT run in a container — use a Mac or hosted
# CI for those.
#
# Build:  docker build -t asmtest-ci .
# Run:    docker run --rm asmtest-ci                 # -> make test && make check
#         docker run --rm asmtest-ci make emu-test   # any other goal
# Or drive it through the Makefile: `make docker-test`, `make docker-emu`, ...
#
# The base layer installs only `make` + a C compiler (build-essential); every
# optional tool (nasm, pkg-config, libunicorn, libcapstone, clang-tidy) comes
# from the cross-platform installer via `make deps`, so this image dogfoods that
# script.

ARG BASE=ubuntu:24.04
FROM ${BASE}

ENV DEBIAN_FRONTEND=noninteractive

# Base toolchain. The C compiler also assembles the GAS .s sources and ships
# gcov; cmake + git are here for the Keystone source build (no distro package).
# libxml2-utils supplies xmllint, which `make check` uses to validate the JUnit
# XML it emits (CI runners install it too, ci.yml); without it every docker lane
# — including docker-riscv64 — silently printed expect.sh's "SKIP junit XML
# validation" line. Per the CLAUDE.md dependency rule that skip is an
# installable, not a gate.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libxml2-utils \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Install the OPTIONAL toolchain first, from just the Makefile (+ its mk/
# includes) + installer, so this (slow) layer stays cached when only sources
# change. The mk/ includes are needed because the Makefile reads them at parse
# time, so any `make` here (even `make deps`) requires them present. `make deps`
# runs as
# root here and so skips sudo. DEPS_ARGS lets you trim it, e.g.
# `docker build --build-arg DEPS_ARGS=--emu .`. Keystone has no apt package, so
# when asm is in scope (--asm/--all) it is built from source (cached layer).
ARG DEPS_ARGS=--all
COPY Makefile ./
COPY mk/ ./mk/
COPY scripts/ ./scripts/
RUN make deps DEPS_ARGS=$DEPS_ARGS && rm -rf /var/lib/apt/lists/*
RUN case "$DEPS_ARGS" in *--asm*|*--all*) sh scripts/build-keystone.sh ;; esac

# Now the rest of the tree (build artifacts excluded via .dockerignore).
COPY . .

# Default goal mirrors the `test` CI job: example suites + framework self-tests.
CMD ["sh", "-c", "make test && make check"]
