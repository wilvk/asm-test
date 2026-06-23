# asm-test

A **C-hosted unit-testing framework for assembly language**. Write assembly
routines, call them from C test cases through the real ABI, and assert on the
results. Tests are auto-discovered and reported TAP-style.

Currently at **Phase 1**. See [DESIGN.md](DESIGN.md) for the full plan and
roadmap.

**Available now:** auto-discovered `TEST(...)` cases, a runner with `main()`,
per-suite `SETUP`/`TEARDOWN`, `SKIP(reason)`, TAP-style colored reporting, and
assertions: `ASSERT_TRUE/FALSE`, `ASSERT_EQ/NE/LT/LE/GT/GE`, `ASSERT_STREQ`,
`ASSERT_MEM_EQ`.

## Quick start

```sh
make test        # build and run the example suites
make demo-fail   # see how a failing assertion is reported
make clean
```

## Writing a test

Assembly routine (`examples/add.s`, System V AMD64 ABI, GAS syntax):

```asm
    .globl _add_signed
_add_signed:
    movq %rdi, %rax
    addq %rsi, %rax
    ret
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

x86-64 macOS with `clang` (used to assemble GAS-syntax `.s` and compile C).
Other assemblers/architectures are planned — see [DESIGN.md](DESIGN.md).
