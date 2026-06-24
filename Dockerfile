# Dockerfile — run asm-test's Linux CI jobs locally in a container.
#
# Covers the Linux half of the CI matrix: x86-64 natively, and aarch64 via
# emulation when built/run with `--platform linux/arm64` (Docker Desktop ships
# qemu/Rosetta; on Linux run `docker run --privileged tonistiigi/binfmt` once).
# The macOS jobs CANNOT run in a container — use a Mac or hosted CI for those.
#
# Build:  docker build -t asmtest-ci .
# Run:    docker run --rm asmtest-ci                 # -> make test && make check
#         docker run --rm asmtest-ci make emu-test   # any other goal
# Or drive it through the Makefile: `make docker-test`, `make docker-emu`, ...
#
# The base layer installs only `make` + a C compiler (build-essential); every
# optional tool (nasm, pkg-config, libunicorn, clang-tidy) comes from the
# cross-platform installer via `make deps`, so this image dogfoods that script.

ARG BASE=ubuntu:24.04
FROM ${BASE}

ENV DEBIAN_FRONTEND=noninteractive

# Base toolchain only. The C compiler also assembles the GAS .s sources and
# ships gcov, so this is all the core build (make test/check) needs.
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Install the OPTIONAL toolchain first, from just the Makefile + installer, so
# this (slow) layer stays cached when only sources change. `make deps` runs as
# root here and so skips sudo. DEPS_ARGS lets you trim it, e.g.
# `docker build --build-arg DEPS_ARGS=--emu .`.
ARG DEPS_ARGS=--all
COPY Makefile ./
COPY scripts/ ./scripts/
RUN make deps DEPS_ARGS=$DEPS_ARGS && rm -rf /var/lib/apt/lists/*

# Now the rest of the tree (build artifacts excluded via .dockerignore).
COPY . .

# Default goal mirrors the `test` CI job: example suites + framework self-tests.
CMD ["sh", "-c", "make test && make check"]
