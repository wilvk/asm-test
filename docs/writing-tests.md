# Writing tests

A test suite is two files: an assembly source with the routines under test, and
a C source with the test cases. This page covers how they fit together.

## The two halves

### The assembly routine

Routines follow the platform calling convention (System V AMD64 on x86-64,
AAPCS64 on AArch64) and are wrapped in the `asm.h` macros so one source builds on
both ELF and Mach-O:

```asm
#include "asm.h"

ASM_FUNC add_signed
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    ret
#endif
ASM_ENDFUNC add_signed
```

`ASM_FUNC name` / `ASM_ENDFUNC name` emit the right symbol decoration (the
leading underscore on Mach-O), `.globl`, alignment, and size directives. Because
`;` is a comment on AArch64, the macros are `.macro`-based rather than relying on
statement separators. Write architecture-specific bodies behind
`#if defined(__x86_64__)` / `#elif defined(__aarch64__)`.

:::{note}
The same routine can also be written in Intel syntax for the
[NASM backend](portability.md#nasm-backend). The convention is `foo.s` (GAS) and
`foo.asm` (NASM) side by side; CI exercises both.
:::

### The C test file

```c
#include "asmtest.h"

extern long add_signed(long a, long b);

TEST(arith, adds_two_positives) {
    ASSERT_EQ(add_signed(2, 3), 5);
}
```

Declare each routine `extern` with its C prototype and call it directly, or
through one of the [capture macros](abi-capture.md) when you want to inspect
registers and flags.

## `TEST` — define and auto-register

```c
TEST(suite, name) { /* body */ }
```

`TEST(suite, name)` expands to a function plus a `constructor` that links the
case into a global registry as the binary starts. At startup the provided
`main()` walks every registered case, so **there is no list to maintain** —
adding a `TEST(...)` is all it takes. A failed assertion aborts only *that* test
(via `longjmp` back into the runner); the rest of the suite continues.

## Fixtures: `SETUP` and `TEARDOWN`

Per-suite fixtures run before and after **each** test in that suite:

```c
static emu_t *E;

SETUP(emu)    { E = emu_open(); }
TEARDOWN(emu) { emu_close(E); E = NULL; }

TEST(emu, runs_routine) {
    /* E is freshly opened here; closed automatically afterward. */
}
```

Use them to allocate and release shared resources, or to build a buffer the
tests operate on.

## Skipping a test

Call `SKIP(reason)` to mark the current test skipped (it counts as skipped, not
failed):

```c
TEST(arith, avx512_path) {
    if (!host_has_avx512())
        SKIP("no AVX-512 on this host");
    /* … */
}
```

## How a run is structured

1. `main()` (provided) parses the [CLI](runner.md), then enumerates the
   registry.
2. Each selected test runs in a `fork()`ed child with an `alarm()` timeout, so a
   crash or infinite loop in the routine under test becomes a reported failure
   instead of taking the runner down. (`--no-fork` runs in-process.)
3. Results stream out as colored TAP (or JUnit XML with `--format=junit`).
4. The process exits nonzero if any test failed.

See [The test runner](runner.md) for filtering, ordering, parallelism, and
timeouts.

## File and build conventions

| File | Role |
|---|---|
| `examples/foo.s` | Routines under test (GAS syntax) |
| `examples/foo.asm` | Same routines in Intel syntax (NASM backend, optional) |
| `examples/test_foo.c` | The `TEST(...)` cases that call them |

The Makefile globs `examples/test_*.c`, links each against the matching routine
object, and produces `build/test_foo`. Drop in a new pair and `make test` builds
and runs it — no Makefile edit required. To consume the framework from a
separate project instead, see [Integration](integration.md).
