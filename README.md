# asm-test

[![CI](https://github.com/wilvk/asm-test/actions/workflows/ci.yml/badge.svg)](https://github.com/wilvk/asm-test/actions/workflows/ci.yml)

A **C-hosted unit-testing framework for assembly language**. Write assembly
routines, call them from C test cases through the real ABI, and assert on the
results — return values, **CPU registers**, **flags**, and **memory**. Tests are
auto-discovered and reported TAP-style.

Every capability documented here is implemented and exercised in CI. The core —
capture, the assertion library, differential testing, the runner, and the Unicorn
emulator — runs on any supported host. The native-trace tiers layer on top:
DynamoRIO in-process tracing needs Linux x86-64 (and a DynamoRIO install), and
the hardware-assisted backends (Intel PT, AMD LBR, ARM CoreSight) plus the eBPF
code-image detector compile everywhere but **self-skip** where their hardware,
permissions, or (for CoreSight) a live trace decoder are unavailable. See
[DESIGN.md](DESIGN.md) for the full design and roadmap.

## Highlights

- **Auto-discovered `TEST(...)` cases** with a provided runner: per-test `fork()`
  isolation, timeouts, crash/hang containment, filtering, shuffling, sharding,
  parallelism (`-jN`), and TAP or JUnit output. Per-suite `SETUP`/`TEARDOWN`,
  `SKIP(reason)`.
- A full **assertion library** — signed/unsigned comparisons, strings, memory
  (hexdump diff), floating-point (ULP-aware), and SIMD lanes.
- **Register, flag, and ABI-preservation capture** through a real call
  (`ASM_CALLn`, `ASSERT_ABI_PRESERVED`, `ASSERT_FLAG_SET/CLEAR`), plus the full
  System V call model: arbitrary arity, struct return and by-value, FP
  (`ASM_FCALLn`), and 128/256/512-bit vectors (`ASM_VCALLn`).
- **Differential / property testing** against a C reference model over fuzzed
  inputs (`ASSERT_MATCHES_REFn`), with reproducible, overridable seeds.
- **Benchmark mode** — auto-calibrated cycles per call, text or JSON output.
- **Guard-page buffers** and crash handling that turn overruns and fatal signals
  into reported failures instead of killing the run.
- An optional **emulator tier** (Unicorn): x86-64, AArch64, RISC-V, and ARM32
  guests on any host — full register file, precise faults, instruction traces,
  block coverage, and the Windows x64 ABI on a System V host.
- Optional **in-line assembly** (Keystone: run routines from strings) and
  **disassembly in diagnostics** (Capstone).
- **Native runtime tracing**: in-process DynamoRIO, hardware trace (Intel PT,
  AMD LBR, single-step, an ARM CoreSight scaffold), and out-of-process `ptrace`.
- A **native Win64 tier** (cross-compile + Wine, or the `ms_abi` lane) and
  **ten language bindings** (Python, .NET, Go, Rust, C++, Zig, Node, Java, Ruby,
  Lua).
- **Portable** across x86-64 and AArch64, Linux and macOS, with GAS and NASM
  assembler backends.

The complete capability list, per-platform, lives in
[Features & support matrix](docs/reference/features.md).

## Documentation

Full documentation lives in [docs/](docs/index.md) (also built on Read the Docs).
Where to start:

- **Getting started** — [Quick start](docs/getting-started/quickstart.md) ·
  [Installation](docs/getting-started/installation.md) ·
  [Writing tests](docs/getting-started/writing-tests.md) ·
  [Examples](docs/getting-started/examples.md)
- **Guides** — [ABI capture](docs/guides/abi-capture.md) ·
  [Assertions](docs/guides/assertions.md) ·
  [Floating-point & SIMD](docs/guides/floating-point-simd.md) ·
  [The runner](docs/guides/runner.md) · [Benchmarks](docs/guides/benchmarks.md) ·
  [Property testing](docs/guides/property-testing.md) ·
  [Emulator](docs/guides/emulator.md) ·
  [Disassembly](docs/guides/disassembly.md) ·
  [Windows x64 tier](docs/guides/win64.md) ·
  [CI integration](docs/guides/ci-integration.md) ·
  [Teaching with asm-test](docs/guides/classroom.md)
- **Language bindings** — [overview](docs/bindings/index.md) for C++, Rust, Zig,
  Go, Node, Python, Ruby, Lua, Java, and .NET.
- **Tracing tiers** — [the tracing hub](docs/guides/tracing/index.md):
  [emulator traces](docs/guides/tracing/traces.md), in-process
  [DynamoRIO](docs/guides/tracing/native-tracing.md), and the
  [hardware backends](docs/guides/tracing/hardware-tracing.md) (Intel PT, AMD
  LBR, ARM CoreSight, single-step).
- **Reference** — [API reference](docs/reference/api-reference.md) ·
  [Integration](docs/reference/integration.md) ·
  [vs. alternatives](docs/reference/comparison.md) ·
  [Packaging](docs/reference/packaging.md) ·
  [CI & Docker](docs/reference/ci.md) ·
  [Troubleshooting & FAQ](docs/reference/troubleshooting.md)

## Quick start

```sh
make test                   # build and run the example suites
make demo-fail              # see how a failing assertion is reported
make demo-robust            # see a hang and a crash contained & reported
make bench                  # time the BENCH cases (cycles/call)
make ASM_SYNTAX=nasm test   # same suites via the NASM backend (x86-64)
make asm-test               # run routines from in-line assembly strings (Keystone)
```

Assembly routine (`examples/add.s`, System V AMD64 ABI, GAS syntax). `ASM_FUNC`
handles the ELF/Mach-O symbol differences so it builds on Linux and macOS:

```asm
#include "asm.h"

ASM_FUNC add_signed
    movq %rdi, %rax
    addq %rsi, %rax
    ret
ASM_ENDFUNC add_signed
```

C test (`examples/test_arith.c`):

```c
#include "asmtest.h"

extern long add_signed(long a, long b);

TEST(arith, adds_two_positives) {
    ASSERT_EQ(add_signed(2, 3), 5);
}
```

The framework provides `main()`, discovers every `TEST(...)`, runs them, and
exits nonzero if any fail. Suites are auto-discovered too: drop an
`examples/foo.s` + `examples/test_foo.c` pair in and `make test` builds and runs
it. Each suite binary takes a small CLI (`--list`, `--filter=GLOB`, `--shuffle`,
`--timeout=SEC`, `-jN`, `--format=junit`, …) — see
[the runner](docs/guides/runner.md).

Continue with the [Quick start](docs/getting-started/quickstart.md) to build a
suite of your own, and [Examples](docs/getting-started/examples.md) for the
use-case tour (correctness & ABI, differential testing, benchmarking, crash
containment, SIMD, cross-ISA).

## Using asm-test in your project

The static library + pkg-config is the primary path; a single-header
amalgamation (`make amalgamate`) is the lightweight alternative:

```sh
make install                 # headers + libasmtest.a + asmtest.pc (PREFIX=/usr/local)
cc $(pkg-config --cflags asmtest) -x assembler-with-cpp -c my_routine.s -o my_routine.o
cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
```

See [Using asm-test in your project](docs/reference/integration.md) for the
details (staged installs, the `asmtest-emu` module, version macros, caveats) and
[Using asm-test in your CI](docs/guides/ci-integration.md) for the GitHub Action
and GitLab template.

## Language packages & licensing

`make <lang>-package` builds a self-contained package per binding — the superset
native lib plus vendored Unicorn/Keystone/Capstone, so a fresh install runs the
emulator, in-line assembly, and disassembly with no system libraries. The
packages build and install-smoke in the release pipeline but are **not published
to public registries yet** — today you consume the bindings from a checkout (see
[the bindings overview](docs/bindings/index.md)). Because they convey the
GPL-2.0 engines as binaries, the packages are **effectively GPL-2.0 as
distributed**; asm-test's own source stays MIT (see [LICENSE](LICENSE) and
[Packaging](docs/reference/packaging.md)).

## Requirements

x86-64 or AArch64, Linux or macOS, with `make` and a C compiler (`cc` — gcc or
clang), which also assembles the GAS-syntax `.s` sources. The core build needs
nothing else. The **optional** tools (`nasm`, `pkg-config`, `libunicorn`,
`libkeystone`, `libcapstone`, `clang-tidy`, `valgrind`) install cross-platform
with:

```sh
make deps                       # full dev setup, via the system package manager
make deps DEPS_ARGS=--emu       # what `make emu-test` needs (unicorn + capstone)
make deps DEPS_ARGS=--dry-run   # preview the commands without running them
```

See [Installation](docs/getting-started/installation.md) for the full
requirements table.

## Running the CI locally with Docker

The **Linux** half of the CI matrix reproduces in a container — `make
docker-test`, `docker-emu`, `docker-asm`, `docker-sanitize`, `docker-ci` (the
whole x86-64 matrix), `docker-shell`, and more; pass
`DOCKER_PLATFORM=linux/arm64` to emulate the arm64 runner. See
[CI & Docker](docs/reference/ci.md) for the full job-to-target mapping, and
`make help` for the complete target list.
