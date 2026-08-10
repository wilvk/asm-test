# CI & Docker

asm-test is tested by a GitHub Actions matrix, and the Linux half of that matrix
can be reproduced locally in a container so you can debug a CI failure on your own
machine. (Looking to run **your own** asm-test suites in CI instead? See
[Using asm-test in your CI](../guides/ci-integration.md) — a GitHub Action, a
GitLab template, and the raw recipe.)

## What CI covers

The pipeline is 49 jobs. The core `test` job runs the suites across all four
OS/architecture combinations (`ubuntu-latest`, `ubuntu-24.04-arm`,
`macos-latest`, and the nightly `macos-15-intel` x86-64 leg — the `macos-13`
image was retired 2025-12-08), and dedicated jobs cover the rest of the surface:

- **Backends & tiers** — `nasm` (Intel-syntax backend), `emu` (Unicorn emulator),
  `asm` (in-line Keystone assembler), `drtrace` (DynamoRIO, plus a
  `drtrace-macos` leg), `sde` (the Intel SDE future-ISA lane), `hwtrace`
  (hardware-trace decode + self-skip gating, plus `hwtrace-arm64` and
  `hwtrace-privileged` variants), `hwtrace-bindings` (every language
  wrapper's single-step tracer, plus an arm64 leg), and `codeimage` (the eBPF
  emission detector).
- **Architecture ports** — `test-riscv64` runs the core example suites + framework
  self-tests in a `linux/riscv64` container under QEMU binfmt (there is no hosted
  riscv64 runner, so it is a Docker-under-QEMU job rather than a matrix row),
  covering the native RISC-V (rv64 / RV64GC / LP64D) host tier; a
  `test-macos-x86-rosetta` leg keeps the x86-64 build honest under Rosetta 2.
- **Tracing, data-flow & taint** — `cli` (the `asmspy` build, its unit tests, the
  byte-exact `.asmtrace` golden gate, and the end-to-end smoke), `dataflow` +
  `dataflow-bindings` (the operand-value/def-use tier and its language wrappers),
  `fuzz` (the libFuzzer/AFL++ shim, which must *find* its planted crashes),
  `drext-probe`, and the taint family (`taint`, `taint-oracle`, `taint-gcmove`,
  `taint-attach`, `gccanon-attach`).
- **Desktop GUI** — `desktop` builds the ImGui/GL app and runs its replay/render
  test suite, including the golden byte gate, under software GL.
- **Language bindings** — the 10-language `bindings` matrix, plus `clean-room`
  ([fresh-install resolution](../clean-room-testing.md)), `bindings-parity`, and
  `go-vet`.
- **Windows** — `win64` (mingw cross-compile + Wine) and `windows` (native).
- **Quality** — `valgrind`, `sanitize` (ASan + UBSan), `analyze` (clang-tidy),
  `format` (clang-format drift), `coverage` (gcov), and `docs` (this
  documentation, Sphinx with warnings-as-errors).
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
three today:

- `hwtrace-privileged-zen` runs `make docker-hwtrace-privileged` on a registered
  AMD Zen 4/5 runner and fails hard if any AMD-exact live path (LbrExtV2, live
  IBS) self-skips.
- `hwtrace-pt-baremetal` runs the same broad tier plus
  `make docker-hwtrace-pt-live` on a **bare-metal** Intel PT box. That second
  target sets `ASMTEST_REQUIRE_PT=1`, which turns the PT tier's availability
  self-skip into a hard failure — so a host whose `intel_pt` PMU is missing or
  hidden by a hypervisor goes red rather than quietly green.
- `hwtrace-coresight-board` is written but deliberately **dark**: the CoreSight
  decode tree does not exist yet, so its variable stays `0` and turning it on is
  the acceptance step of that work, not a thing to do early.

Every job is guarded by an `HW_RUNNER_*` repository variable that is `0`/absent
by default (and by an actor guard restricting it to the repo owner), so with no
runner registered the workflow still lands green with each job skipped. See the
[self-hosted runner runbook](https://github.com/wilvk/asm-test/blob/main/docs/internal/ci/runners.md).

The framework's own self-tests run via `make check`: `tests/positive.c` and
`tests/negative.c` driven black-box by the `tests/expect.sh` harness, the
`tests/glob_parity.c` / `tests/grow_overflow.c` differential oracles, and the
compile-only consumer gate over `tests/portability/`. (Writing tests *with* the
framework is a different topic — see
[Writing tests](../getting-started/writing-tests.md).)

`--format=junit` ([runner](../guides/runner.md#output-formats)) emits JUnit XML for CI
systems that ingest structured test results.

## Running the Linux CI locally with Docker

The **Linux** jobs reproduce in a container (the macOS jobs can't — use a Mac or
hosted CI for those). The [`Dockerfile`] installs a minimal toolchain
(`build-essential`, plus `cmake` and `git` for the pinned Keystone/Capstone
source builds), then pulls the optional tiers through `make deps
DEPS_ARGS=--all`, and the `docker-*` targets build the image and run each job:

```sh
make docker-test        # example suites + framework self-tests (the `test` job)
make docker-nasm        # NASM backend (x86-64 only)
make docker-emu         # emulator tier (libunicorn)
make docker-valgrind    # memcheck the routines under test
make docker-sanitize    # ASan + UBSan
make docker-analyze     # clang-tidy
make docker-coverage    # gcov of the runner
make docker-ci          # the core x86-64 legs end to end (test, check, nasm,
                        # emu, asm, valgrind, sanitize, tidy)
make docker-riscv64     # native rv64 host tier under QEMU (the `test-riscv64` job)
make docker-shell       # interactive shell in the CI image
```

The subsystem jobs run in their own images (`Dockerfile.cli`,
`Dockerfile.desktop`, `Dockerfile.fuzz`, …) rather than the CI image above, and
each has a lane of its own:

```sh
make docker-cli              # asmspy build + unit tests + golden gate + smoke (the `cli` job)
make docker-desktop          # desktop GUI build + replay/render tests (the `desktop` job)
make docker-fuzz             # libFuzzer + AFL++ shim (the `fuzz` job)
make docker-sde              # Intel SDE future-ISA lane (the `sde` job)
make docker-dataflow-attach  # data-flow tier (the `dataflow` job; PT variant: docker-dataflow-pt)
make docker-taint-native     # taint tiers (also: docker-taint-oracle, docker-taint-attach, …)
make docker-hwtrace          # hardware-trace tier (also: -privileged, -codeimage, -amd, …)
make docker-win64            # mingw cross-compile + Wine (the `win64` job)
make docker-clean-room       # fresh-install bindings resolution (the `clean-room` job)
make docker-docs             # this documentation, warnings-as-errors (the `docs` job)
make docker-sve-sweep        # SVE vector-length sweep under qemu-user (no CI job; see ci.yml's arm64 grep)
```

Run `make help` for the complete lane list — there are ~90 `docker-*` targets.

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
| `make usecases` | the documented use-case walkthroughs |
| `make cli-smoke` | `asmspy` unit tests, the golden `.asmtrace` gate, and the end-to-end smoke |
| `make desktop-test` / `make desktop-ui-test` | the desktop GUI's replay/render and interaction suites |
| `make asmtrace-golden-check` | byte-exact `.asmtrace` golden corpus gate (regenerate only in the `docker-cli` image) |
| `make win64-check` | the Win64 tier's test aggregate (needs mingw + Wine) |
| `make dataflow-test` | the data-flow (operand value) tier suites |
| `make fuzz-shim-test` | the fuzzing shim — both engines must find their planted crashes |
| `make bench-check` | the deterministic golden emulator instruction counts |
| `make check-bindings-parity` | all ten bindings expose the same API surface |

See [Installation](../getting-started/installation.md#installing-the-optional-tools) for installing
the tools these targets need.

[`Dockerfile`]: https://github.com/wilvk/asm-test/blob/main/Dockerfile
