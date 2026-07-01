# asm-test

[![CI](https://github.com/wilvk/asm-test/actions/workflows/ci.yml/badge.svg)](https://github.com/wilvk/asm-test/actions/workflows/ci.yml)

A **C-hosted unit-testing framework for assembly language**. Write assembly
routines, call them from C test cases through the real ABI, and assert on the
results. Tests are auto-discovered and reported TAP-style.

Every capability documented below is implemented and exercised in CI. The core —
capture, the assertion library, differential testing, the runner, and the Unicorn
emulator — runs on any supported host. The native-trace tiers layer on top:
DynamoRIO in-process tracing needs Linux x86-64 (and a DynamoRIO install), and
the hardware-assisted backends (Intel PT, AMD LBR, ARM CoreSight) plus the eBPF
code-image detector compile everywhere but **self-skip** where their hardware,
permissions, or (for CoreSight) a live trace decoder are unavailable. See
[DESIGN.md](DESIGN.md) for the full design and roadmap.

**Available now:**

- Auto-discovered `TEST(...)` cases, a runner with `main()`, per-suite
  `SETUP`/`TEARDOWN`, `SKIP(reason)`, TAP-style colored reporting.
- Value assertions: `ASSERT_TRUE/FALSE`, `ASSERT_EQ/NE/LT/LE/GT/GE`, unsigned
  `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE`, `ASSERT_STREQ`, `ASSERT_MEM_EQ` (hexdump diff).
- **Register/flags capture** via `ASM_CALLn(&regs, fn, args...)`, then
  `ASSERT_ABI_PRESERVED(&regs)` (callee-saved compliance),
  `ASSERT_FLAG_SET/CLEAR(&regs, CF|PF|ZF|SF|OF)`, and `ASSERT_REG_EQ`.
- **Arbitrary integer arity** via `ASM_CALLN(&regs, fn, ...)`: passes the
  overflow (7th+ on x86-64, 9th+ on AArch64) on the stack per the ABI.
- **Struct return** via `ASM_SRET(&regs, fn, &out, ...)`: large structs come
  back through the hidden result pointer (`rdi`/`x8`); small ones land in the
  captured `regs.ret`/`regs.rdx`.
- **Struct-by-value parameters**: small structs pass as their eightbytes via the
  integer/FP paths; large ones via `asm_call_capture_bigstruct` (inline stack
  copy on x86-64, by-pointer on AArch64).
- **Floating-point** via `ASM_FCALLn(&regs, fn, doubles...)`: marshals `double`
  args into the FP registers and captures the FP return (`regs.fret`), with
  `ASSERT_FP_EQ` and `ASSERT_FP_NEAR(&regs, expected, ulps)`. `ASM_FCALLN`
  passes any number of doubles, spilling the 9th+ onto the stack per the ABI.
- **SIMD** via `ASM_VCALLn(&regs, fn, vec128_t...)`: marshals 128-bit vector
  args and captures the whole vector file (`regs.vec[]`, return in `vec[0]`),
  with `ASSERT_VEC_EQ` plus lane assertions `ASSERT_DEQ/DNEAR` / `ASSERT_FEQ/FNEAR`.
  `ASM_VCALLN` passes any number of vectors, spilling the 9th+ onto the stack.
- **Differential / property testing** via
  `ASSERT_MATCHES_REF{1,2,3}(fn, ref, gen, n)`: supply a C reference model and a
  generator, and the framework fuzzes `n` random inputs (from a seedable
  splitmix64 RNG) calling the routine through the real ABI, then reports the
  first input where it disagrees with the model. The seed is fixed by default so
  failures reproduce, and overridable with `ASMTEST_SEED` so CI can vary it.
- **Guard-page buffers** (`asmtest_guarded_alloc`) so a one-past-the-end write
  faults, plus crash handling that turns a fatal signal (SIGSEGV/SIGBUS/…) in a
  buggy routine into a reported failure instead of killing the runner.
- **Per-test isolation & timeout:** each test runs in a forked child with an
  `alarm()` timeout, so an infinite loop or a SIGABRT-class corruption becomes a
  reported timeout/crash failure and the run continues (`--no-fork` opts back
  into the in-process model). See `make demo-robust`. `-jN` runs up to N tests
  concurrently (extending the fork model); output stays in registration order.
- **Runner CLI:** `--filter=GLOB` (over suite, name, or `suite.name`), `--list`,
  `--shuffle`/`--seed` for order independence, `--timeout=SEC`, and
  `--format=junit` for CI ingestion alongside the default colored TAP.
- **Benchmark mode** via `BENCH(suite, name) { ... }`: the body is one measured
  call; the runner auto-calibrates a repeat count and reports min/median/mean
  cycles per call (`rdtsc` / `cntvct_el0`, via `asmtest_cycle_counter()`). Run
  with `--bench` (honors `--filter`/`--list`; `--bench-reps=N` pins the count).
  `BENCH_USE(x)` keeps a pure-C result from being optimized away. See `make bench`.
- **Portable across x86-64 and AArch64, Linux and macOS:** the same sources
  build on ELF and Mach-O via the `ASM_FUNC` macros in `include/asm.h`; each
  routine and the capture trampoline carry both an x86-64 and an AArch64 body
  selected by `#if`. CI runs the suites on all four target combinations.
- **Two assembler backends:** GAS (default) or NASM (`make ASM_SYNTAX=nasm`,
  Intel syntax, x86-64 only) — the Intel-syntax sources live alongside the GAS
  ones and are also exercised in CI. **On AArch64, GAS is the only backend**
  (NASM is x86-only by design); the `.s` sources assemble there through the C
  compiler's built-in assembler, so no extra tool is needed.
- **Emulator tier** (optional, `make emu-test`, needs libunicorn): run a routine
  inside a virtual CPU (x86-64, AArch64, RISC-V/RV64, or ARM32 guest) to preload
  arbitrary registers/memory, single-step mid-routine, read back the **full**
  register file, and catch invalid memory accesses as precise faults — beyond
  what ABI-boundary inspection can see. The AArch64, RISC-V, and ARM32 guests
  emulate even on an x86-64 host (the guest is independent of the host).
  `emu_call_traced` also records an **instruction trace and basic-block
  coverage**; accumulating coverage across inputs answers "did the tests
  exercise every branch?". The x86-64 engine also runs the **Windows x64 ABI**
  via `emu_call_win64` (args in `rcx/rdx/r8/r9`, 32-byte shadow space, `rsi`/`rdi`
  callee-saved) — so Win64 routines are testable on a System V host. The x86-64
  guest also marshals `double` args (`emu_call_fp`) and 128-bit vector args
  (`emu_call_vec`) and captures the XMM file (the AArch64 guest does the same for
  NEON; the RISC-V and ARM32 guests marshal scalar FP), with emulator assertions
  (`ASSERT_NO_FAULT`, `ASSERT_FAULT_AT`, `ASSERT_EMU_REG_EQ`, `ASSERT_EMU_FP_EQ`,
  `ASSERT_EMU_VEC_EQ`) and coverage reporting (`emu_trace_report`,
  `emu_coverage_uncovered`, an lcov export, and `ASSERT_BLOCK_COVERED`).

## Quick start

```sh
make test                   # build and run the example suites
make demo-fail              # see how a failing assertion is reported
make demo-robust            # see a hang and a crash contained & reported
make bench                  # time the BENCH cases (cycles/call)
make ASM_SYNTAX=nasm test   # same suites via the NASM backend (x86-64)
make asm-test               # run routines from in-line assembly strings (Keystone)
make clean
```

Each suite binary takes a small CLI:

```sh
./build/test_arith --list                 # list the tests, don't run them
./build/test_arith --filter='*overflow*'  # run a glob-matched subset
./build/test_arith --shuffle --seed=123   # run in a reproducible random order
./build/test_arith --timeout=5            # per-test timeout (seconds; 0 = off)
./build/test_arith --no-fork              # in-process (no per-test isolation)
./build/test_arith -j4                    # run up to 4 tests at once (ordered)
./build/test_arith --format=junit         # JUnit XML instead of TAP, for CI
./build/test_bench --bench                # time BENCH cases instead of testing
```

## Writing a test

Assembly routine (`examples/add.s`, System V AMD64 ABI, GAS syntax). `ASM_FUNC`
handles the ELF/Mach-O symbol differences so it builds on Linux and macOS:

```asm
#include "asm.h"

ASM_FUNC(add_signed)
    movq %rdi, %rax
    addq %rsi, %rax
    ret
ASM_ENDFUNC(add_signed)
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
exits nonzero if any fail.

### Routines that call back into C

A routine can take a function pointer and call back into C — a qsort-style
comparator, or a map/filter over an array. `examples/callback.s` (with
`examples/test_callback.c`) shows `sum_map(arr, n, fn)` and `count_if(arr, n,
pred)` invoking an ordinary C callback per element; the test just passes C
function pointers and asserts on the result. The routine keeps its live state
(pointer, counter, callback, accumulator) in callee-saved registers and keeps
the stack 16-byte aligned at each call — exactly the ABI discipline these
helpers are built to exercise.

### Pass assembly as a string (in-line)

The usual path assembles `.s`/`.asm` files ahead of time and links them as
symbols. The optional **in-line assembler tier** (Keystone) lets you instead
hand the emulator a routine as an *assembly string* and run it — handy for
generated snippets, quick experiments, or table-driven cases:

```c
#include "asmtest.h"
#include "asmtest_assemble.h"

TEST(inline_asm, adds_two) {
    emu_t *e = emu_open();
    emu_result_t r;
    long args[] = {40, 2};
    /* assemble at the emulator's load base, then run it */
    ASSERT_TRUE(emu_call_asm(e, "mov rax, rdi; add rax, rsi; ret", args, 2, 0, &r));
    ASSERT_FALSE(r.faulted);
    ASSERT_EQ(r.regs.rax, 42);
    emu_close(e);
}
```

`emu_call_asm` (x86-64, Intel syntax by default) has siblings for the other
guests — `emu_arm64_call_asm`, `emu_arm_call_asm`, and `emu_riscv_call_asm`
(RISC-V where the linked Keystone supports it). For just the bytes, call
`asmtest_assemble(arch, syntax, source, addr, &out)` and free with
`asmtest_asm_free`; a bad string is reported as data (`out.ok == false`, message
in `out.err`), never a crash. Build and run with `make asm-test` (needs
`libkeystone` alongside `libunicorn`). Keystone has no Linux distro package, so
`make deps DEPS_ARGS=--asm` installs `libunicorn` and points at
[scripts/build-keystone.sh](scripts/build-keystone.sh) for a pinned source build
(macOS gets it from Homebrew). Capstone (the disassembler) is built the same way
from a pinned release via [scripts/build-capstone.sh](scripts/build-capstone.sh),
so neither optional tier depends on a distro/brew package. See
[docs/plans/inline-asm-keystone-plan.md](docs/plans/inline-asm-keystone-plan.md).

The **published language packages bundle both optional tiers** (the superset
`libasmtest_emu` plus vendored Unicorn/Keystone/Capstone), so a fresh install
runs in-line assembly and disassembly with no system libraries. Because they
convey the GPL-2.0 engines as binaries, those packages are **effectively GPL-2.0**
(asm-test's own source stays MIT — see [LICENSE](LICENSE) and
[docs/plans/fully-featured-packages-plan.md](docs/plans/fully-featured-packages-plan.md)).

## Debugging a routine under test

Two complementary ways to catch bugs in the **routine** (not just assert on its
result):

- **Guard-page buffers** — `asmtest_guarded_alloc(n)` returns `n` writable bytes
  backed by an inaccessible page, so a one-past-the-end write faults *exactly* at
  the overrun (the framework turns the fault into a reported failure);
  `asmtest_guarded_alloc_under(n)` puts the guard *before* the buffer to catch
  underruns (`buf[-1]`). Always on, no extra tooling.
- **Valgrind / memcheck** — `make valgrind` runs the example suites under
  Valgrind's memcheck (`--no-fork`, so it follows one process to a clean exit)
  to catch bad loads/stores, uninitialized reads, and leaks in the routine. A
  real error fails the build. Linux/x86-64 (Valgrind isn't available on current
  macOS/arm64); `make deps DEPS_ARGS=--valgrind` installs it. This is distinct
  from `make sanitize` (ASan + UBSan), which instruments the *framework's* C.

## Using asm-test in your project

Two ways to consume the framework. The **static library** is the primary path.

### 1. Static library + pkg-config (recommended)

```sh
make install                 # to /usr/local by default
make install PREFIX=$HOME/.local
make DESTDIR=/tmp/stage install   # staged install (packaging)
```

This installs the headers (under `…/include/asmtest/`), `libasmtest.a`, and a
`asmtest.pc` pkg-config file. A consumer then builds against it:

```sh
cc $(pkg-config --cflags asmtest) -c my_tests.c -o my_tests.o
cc my_tests.o my_routine.o $(pkg-config --libs asmtest) -o my_tests
```

where `my_routine.o` is your assembly routine under test, assembled with the
same `#include "asm.h"` shim (also installed). `make uninstall` reverses it.

### 2. Single-header amalgamation

```sh
make amalgamate              # writes asmtest_single.h
```

Include `asmtest_single.h` for the API; in **exactly one** translation unit
`#define ASMTEST_IMPLEMENTATION` before including it to emit the runtime.

> **Note:** the register/flags capture trampoline is assembly (`capture.s`) and
> cannot live in a C header, so even with the single header you must assemble +
> link that trampoline (or just link `libasmtest.a`). The amalgamation covers
> the C surface only; the optional emulator tier is not included.

The installed header exposes `ASMTEST_VERSION` (`"1.0.0"`) and
`ASMTEST_VERSION_NUM` for compile-time checks. See [CHANGELOG.md](CHANGELOG.md).

## Requirements

x86-64 or AArch64, Linux or macOS, with `make` and a C compiler (`cc` — gcc or
clang), which also assembles the GAS-syntax `.s` sources. The optional NASM
backend additionally needs `nasm` (x86-64 only). Installing the pkg-config file
and consuming it needs `pkg-config`. See [DESIGN.md](DESIGN.md).

The core build needs nothing beyond `make` + a C compiler. The **optional**
tools (`nasm`, `pkg-config`, `libunicorn`, `clang-tidy`, `valgrind`) can be
installed cross-platform with:

```sh
make deps                       # full dev setup, via the system package manager
make deps DEPS_ARGS=--emu       # just what `make emu-test` needs
make deps DEPS_ARGS=--dry-run   # preview the commands without running them
```

[scripts/install-deps.sh](scripts/install-deps.sh) detects apt-get / dnf / yum /
pacman / zypper / apk / brew (and uses `sudo` on Linux when not root).

## Running the CI locally with Docker

The **Linux** half of the CI matrix can be reproduced in a container (the macOS
jobs can't — use a Mac or hosted CI for those). The [Dockerfile](Dockerfile)
installs only `make` + a C compiler, then pulls the optional toolchain through
`make deps`, and the `docker-*` targets build it and run each job:

```sh
make docker-test        # example suites + framework self-tests (the `test` job)
make docker-nasm        # NASM backend (x86-64 only)
make docker-emu         # emulator tier (libunicorn)
make docker-asm         # in-line assembler tier (libkeystone + libunicorn)
make docker-valgrind    # memcheck the routines under test
make docker-sanitize    # ASan + UBSan
make docker-analyze     # clang-tidy
make docker-coverage    # gcov of the runner
make docker-ci          # the whole x86-64 Linux matrix end to end
make docker-shell       # interactive shell in the CI image
```

To emulate the `ubuntu-24.04-arm` runner, pass `DOCKER_PLATFORM=linux/arm64`
(Docker Desktop ships the emulation; on Linux run
`docker run --privileged tonistiigi/binfmt` once first). On arm64, CI only runs
the `test` and `emu` jobs:

```sh
make docker-test DOCKER_PLATFORM=linux/arm64
make docker-emu  DOCKER_PLATFORM=linux/arm64
```

Override `DOCKER_BASE` to test another distro/release (e.g.
`make docker-test DOCKER_BASE=ubuntu:22.04`).
