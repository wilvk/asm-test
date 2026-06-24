# asm-test

[![CI](https://github.com/wilvk/asm-test/actions/workflows/ci.yml/badge.svg)](https://github.com/wilvk/asm-test/actions/workflows/ci.yml)

A **C-hosted unit-testing framework for assembly language**. Write assembly
routines, call them from C test cases through the real ABI, and assert on the
results. Tests are auto-discovered and reported TAP-style.

Currently at **Phase 8** (runner robustness & CLI). See
[DESIGN.md](DESIGN.md) for the full plan and roadmap (Phases 9–11).

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
  into the in-process model). See `make demo-robust`.
- **Runner CLI:** `--filter=GLOB` (over suite, name, or `suite.name`), `--list`,
  `--shuffle`/`--seed` for order independence, `--timeout=SEC`, and
  `--format=junit` for CI ingestion alongside the default colored TAP.
- **Portable across x86-64 and AArch64, Linux and macOS:** the same sources
  build on ELF and Mach-O via the `ASM_FUNC` macros in `include/asm.h`; each
  routine and the capture trampoline carry both an x86-64 and an AArch64 body
  selected by `#if`. CI runs the suites on all four target combinations.
- **Two assembler backends:** GAS (default) or NASM (`make ASM_SYNTAX=nasm`,
  Intel syntax, x86-64 only) — the Intel-syntax sources live alongside the GAS
  ones and are also exercised in CI.
- **Emulator tier** (optional, `make emu-test`, needs libunicorn): run a routine
  inside a virtual CPU (x86-64 or AArch64 guest) to preload arbitrary
  registers/memory, single-step mid-routine, read back the **full** register
  file, and catch invalid memory accesses as precise faults — beyond what
  ABI-boundary inspection can see. ARM64 routines emulate even on an x86-64 host.

## Quick start

```sh
make test                   # build and run the example suites
make demo-fail              # see how a failing assertion is reported
make demo-robust            # see a hang and a crash contained & reported
make ASM_SYNTAX=nasm test   # same suites via the NASM backend (x86-64)
make clean
```

Each suite binary takes a small CLI:

```sh
./build/test_arith --list                 # list the tests, don't run them
./build/test_arith --filter='*overflow*'  # run a glob-matched subset
./build/test_arith --shuffle --seed=123   # run in a reproducible random order
./build/test_arith --timeout=5            # per-test timeout (seconds; 0 = off)
./build/test_arith --no-fork              # in-process (no per-test isolation)
./build/test_arith --format=junit         # JUnit XML instead of TAP, for CI
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

## Requirements

x86-64 or AArch64, Linux or macOS, with `make` and a C compiler (`cc` — gcc or
clang), which also assembles the GAS-syntax `.s` sources. The optional NASM
backend additionally needs `nasm` (x86-64 only). See [DESIGN.md](DESIGN.md).
