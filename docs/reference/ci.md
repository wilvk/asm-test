# CI & Docker

asm-test is tested by a GitHub Actions matrix, and the Linux half of that matrix
can be reproduced locally in a container so you can debug a CI failure on your own
machine. (Looking to run **your own** asm-test suites in CI instead? See
[Using asm-test in your CI](../guides/ci-integration.md) — a GitHub Action, a
GitLab template, and the raw recipe.)

## What CI covers

The pipeline is ~20 jobs. The core `test` job runs the suites across all four
OS/architecture combinations (`ubuntu-latest`, `ubuntu-24.04-arm`,
`macos-latest`, and the nightly `macos-15-intel` x86-64 leg — the `macos-13`
image was retired 2025-12-08), and dedicated jobs cover the rest of the surface:

- **Backends & tiers** — `nasm` (Intel-syntax backend), `emu` (Unicorn emulator),
  `asm` (in-line Keystone assembler), `drtrace` (DynamoRIO), `hwtrace`
  (hardware-trace decode + self-skip gating), `hwtrace-bindings` (every language
  wrapper's single-step tracer), and `codeimage` (the eBPF emission detector).
- **Architecture ports** — `test-riscv64` runs the core example suites + framework
  self-tests in a `linux/riscv64` container under QEMU binfmt (there is no hosted
  riscv64 runner, so it is a Docker-under-QEMU job rather than a matrix row),
  covering the native RISC-V (rv64 / RV64GC / LP64D) host tier.
- **Language bindings** — the 10-language `bindings` matrix, plus `clean-room`
  ([fresh-install resolution](../clean-room-testing.md)) and `bindings-parity`.
- **Windows** — `win64` (mingw cross-compile + Wine) and `windows` (native).
- **Quality** — `valgrind`, `sanitize` (ASan + UBSan), `analyze` (clang-tidy),
  `format` (clang-format drift), and `coverage` (gcov).
- **Benchmarks** — `benchmarks` runs the cross-system report (`make bench-report`)
  on each per-push OS × arch leg, gates the deterministic golden emu counts
  (`make bench-check`, host/OS-independent), and uploads a per-system JSON, plus a
  per-push `benchmarks-windows` (`windows-latest`) leg and a nightly
  `benchmarks-macos-x86` (`macos-15-intel`) leg so all five OS × arch systems
  produce a report; `benchmarks-compare` merges them into one cross-system matrix
  (performance + the feature & trace-completeness grid). On the nightly schedule
  `benchmarks-record` commits each leg's per-box record (`benchmarks/boxes/gh-**`)
  back to `main` as `github-actions[bot]`; golden files stay a reviewed human PR.
  See [Cross-system benchmarking](../guides/cross-system-benchmarking.md).
- **Packaging** — `package-libs` (+ its macOS and collect legs).

A separate **`hw` workflow** (`.github/workflows/hw.yml`) is *allowed to be absent*:
it never runs on `push`/`pull_request` (only `workflow_dispatch` + a nightly
schedule) and carries the jobs that need real silicon the hosted runners lack —
e.g. `hwtrace-privileged-zen`, which runs `make docker-hwtrace-privileged` on a
registered AMD Zen 4/5 self-hosted runner and fails hard if any AMD-exact live
path (LbrExtV2, live IBS) self-skips. Every job is guarded by an `HW_RUNNER_*`
repository variable that is `0`/absent by default, so with no runner registered
the workflow still lands green with each job skipped. See the
[self-hosted runner runbook](https://github.com/wilvk/asm-test/blob/main/docs/internal/ci/runners.md).

The framework's own [self-tests](../getting-started/writing-tests.md)
(`tests/positive.c`, `tests/negative.c`, the `tests/expect.sh` black-box harness)
run via `make check`.

`--format=junit` ([runner](../guides/runner.md#output-formats)) emits JUnit XML for CI
systems that ingest structured test results.

## Running the Linux CI locally with Docker

The **Linux** jobs reproduce in a container (the macOS jobs can't — use a Mac or
hosted CI for those). The [`Dockerfile`] installs only `make` plus a C compiler,
then pulls the optional toolchain through `make deps`, and the `docker-*` targets
build the image and run each job:

```sh
make docker-test        # example suites + framework self-tests (the `test` job)
make docker-nasm        # NASM backend (x86-64 only)
make docker-emu         # emulator tier (libunicorn)
make docker-valgrind    # memcheck the routines under test
make docker-sanitize    # ASan + UBSan
make docker-analyze     # clang-tidy
make docker-coverage    # gcov of the runner
make docker-ci          # the whole x86-64 Linux matrix, end to end
make docker-riscv64     # native rv64 host tier under QEMU (the `test-riscv64` job)
make docker-shell       # interactive shell in the CI image
```

### Emulating the arm64 runner

Pass `DOCKER_PLATFORM=linux/arm64` to emulate the `ubuntu-24.04-arm` runner.
Docker Desktop ships the emulation; on Linux run
`docker run --privileged tonistiigi/binfmt` once first. On arm64
(`ubuntu-24.04-arm`), CI runs the `test`, `emu`, and `asm` jobs, plus the
`payloads` (`package-libs`) leg that stages the native libs:

```sh
make docker-test DOCKER_PLATFORM=linux/arm64
make docker-emu  DOCKER_PLATFORM=linux/arm64
make docker-asm  DOCKER_PLATFORM=linux/arm64
```

### Running the riscv64 tier

`make docker-riscv64` builds a `linux/riscv64` image and runs the core example
suites + framework self-tests in it under QEMU binfmt — the native RISC-V (rv64)
host tier, mirroring the `test-riscv64` CI job. On a Linux host, enable the pinned
QEMU emulator once first (Docker Desktop already ships it):

```sh
make binfmt-riscv64     # register qemu-user riscv64 (pinned tonistiigi/binfmt)
make docker-riscv64     # build + run under qemu-user (first run is slow: TCG)
```

The lane deliberately builds with `DEPS_ARGS=--pkgconfig` (no Keystone/Unicorn —
those would take hours to build under emulation); the core suites need no
optional engine. The flag-only (`checked`, carry) and 128-bit-vector (`simd`,
`qadd`, `fpover` vector) cases self-skip on rv64 with a printed ISA reason.

### Other distributions

Override `DOCKER_BASE` to test another distro/release:

```sh
make docker-test DOCKER_BASE=ubuntu:22.04
```

## Local quality targets (no Docker)

The same checks run directly on a suitably equipped host:

| Target | Checks |
|---|---|
| `make test` | build and run every example suite |
| `make check` | the framework's own positive/negative self-tests |
| `make sanitize` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `make valgrind` | Valgrind memcheck over the routines under test |
| `make tidy` | clang-tidy static analysis |
| `make coverage` | gcov coverage of the runner |
| `make emu-test` | the [emulator](../guides/emulator.md) suites (needs libunicorn) |

See [Installation](../getting-started/installation.md#installing-the-optional-tools) for installing
the tools these targets need.

[`Dockerfile`]: https://github.com/wilvk/asm-test/blob/main/Dockerfile
