# Quick start

This walks you from a clean checkout to your own passing (and failing) test in a
few minutes. It assumes you have a C compiler and `make` — see
[Installation](installation.md) if not.

## 1. Run the bundled suites

```sh
make test
```

You should see colored, TAP-style output and a passing summary. Each suite is a
self-contained binary under `build/`.

## 2. Write an assembly routine

Create `examples/square.s`. The `ASM_FUNC` / `ASM_ENDFUNC` macros (from
`asm.h`) handle the ELF vs Mach-O symbol differences, so the same source builds
on Linux and macOS:

```asm
#include "asm.h"

/* long square(long n);  n * n  — System V AMD64 / AArch64 */
ASM_FUNC square
#if defined(__x86_64__)
    movq    %rdi, %rax
    imulq   %rdi, %rax
    ret
#elif defined(__aarch64__)
    mul     x0, x0, x0
    ret
#endif
ASM_ENDFUNC square
```

## 3. Write the C test

Create `examples/test_square.c`. Declare the routine `extern`, then assert on
it. The framework supplies `main()`:

```c
#include "asmtest.h"

extern long square(long n);

TEST(square, small_values) {
    ASSERT_EQ(square(2), 4);
    ASSERT_EQ(square(9), 81);
}

TEST(square, is_nonnegative) {
    ASSERT_GE(square(-7), 0);
}
```

By convention a suite is the pair `foo.s` + `test_foo.c`, and the Makefile
**auto-discovers** it: every `examples/test_*.c` is linked against the
same-named routine object through a pattern rule, so dropping the two files in
is all it takes — no Makefile edit. (If your routine file's name doesn't match
the test's — say `test_square.c` driving routines in `mathutils.s` — add one
explicit link rule alongside the existing legacy ones in the Makefile.)

## 4. Run it

```sh
make test                          # builds and runs every suite, including yours
./build/test_square                # run just this suite
./build/test_square --list         # list its tests without running
./build/test_square --filter='*nonneg*'
```

## 5. See a failure

Change an expected value (say `square(2)` should be `5`) and re-run. The report
points at the file, line, expression, and the actual-vs-expected values, and the
process exits nonzero. Two bundled demos show the reporting without editing
anything:

```sh
make demo-fail      # how a failing assertion is reported
make demo-robust    # how a hang and a crash are contained and reported
```

## Where next

- [Writing tests](writing-tests.md) — discovery, fixtures, `SKIP`, and the
  `asm.h` shim in depth.
- [Assertions](../guides/assertions.md) — the full comparison, string, and memory set.
- [ABI capture & registers](../guides/abi-capture.md) — inspect registers, flags, and
  callee-saved preservation through a real call.
- [The test runner](../guides/runner.md) — filtering, shuffling, timeouts, parallelism,
  and CI output formats.
