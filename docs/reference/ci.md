# CI & Docker

asm-test is tested by a GitHub Actions matrix, and the Linux half of that matrix
can be reproduced locally in a container so you can debug a CI failure on your own
machine.

## What CI covers

The pipeline runs the suites across all four OS/architecture combinations
(`ubuntu-latest`, `ubuntu-24.04-arm`, `macos-latest`, `macos-13`) and adds
dedicated jobs for the NASM backend, the emulator tier, Valgrind, the
sanitizers, clang-tidy, and gcov coverage. The framework's own
[self-tests](../getting-started/writing-tests.md) (`tests/positive.c`, `tests/negative.c`, the
`tests/expect.sh` black-box harness) run via `make check`.

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
