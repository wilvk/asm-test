# asm-test

[![CI](https://github.com/wilvk/asm-test/actions/workflows/ci.yml/badge.svg)](https://github.com/wilvk/asm-test/actions/workflows/ci.yml)

A **C-hosted unit-testing framework for assembly language**. Write assembly
routines, call them from C test cases through the real ABI, and assert on the
results. Tests are auto-discovered and reported TAP-style.

Currently at **Phase 3**. See [DESIGN.md](DESIGN.md) for the full plan and
roadmap.

**Available now:**

- Auto-discovered `TEST(...)` cases, a runner with `main()`, per-suite
  `SETUP`/`TEARDOWN`, `SKIP(reason)`, TAP-style colored reporting.
- Value assertions: `ASSERT_TRUE/FALSE`, `ASSERT_EQ/NE/LT/LE/GT/GE`,
  `ASSERT_STREQ`, `ASSERT_MEM_EQ`.
- **Register/flags capture** via `ASM_CALLn(&regs, fn, args...)`, then
  `ASSERT_ABI_PRESERVED(&regs)` (callee-saved compliance) and
  `ASSERT_FLAG_SET/CLEAR(&regs, CF|PF|ZF|SF|OF)`.
- **Guard-page buffers** (`asmtest_guarded_alloc`) so a one-past-the-end write
  faults, plus crash handling that turns a fatal signal (SIGSEGV/SIGBUS/…) in a
  buggy routine into a reported failure instead of killing the runner.
- **Portable across x86-64 and AArch64, Linux and macOS:** the same sources
  build on ELF and Mach-O via the `ASM_FUNC` macros in `include/asm.h`; each
  routine and the capture trampoline carry both an x86-64 and an AArch64 body
  selected by `#if`. CI runs the suites on all four target combinations.
- **Two assembler backends:** GAS (default) or NASM (`make ASM_SYNTAX=nasm`,
  Intel syntax, x86-64 only) — the Intel-syntax sources live alongside the GAS
  ones and are also exercised in CI.

## Quick start

```sh
make test                   # build and run the example suites
make demo-fail              # see how a failing assertion is reported
make ASM_SYNTAX=nasm test   # same suites via the NASM backend (x86-64)
make clean
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
