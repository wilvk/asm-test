# asm-test

**A C-hosted unit-testing framework for assembly language.**

Write assembly routines, call them from C test cases *through the real ABI*, and
assert on the results — return values, **CPU registers**, **flags**, and
**memory**. Tests are auto-discovered, run in isolation, and reported TAP-style
with a nonzero exit on failure.

```c
#include "asmtest.h"

extern long add_signed(long a, long b);   // routine under test (in add.s)

TEST(arith, adds_two_numbers) {
    ASSERT_EQ(add_signed(2, 3), 5);
}

TEST(arith, preserves_callee_saved_and_clears_carry) {
    regs_t r;
    ASM_CALL2(&r, add_signed, 2, 3);
    ASSERT_EQ(r.ret, 5);
    ASSERT_ABI_PRESERVED(&r);     // rbx, rbp, r12–r15 restored
    ASSERT_FLAG_CLEAR(&r, CF);
}
```

The framework provides `main()`, discovers every `TEST(...)`, runs each one in a
forked child with a timeout, and prints a colored summary.

## How it fits together

You write the assembly routines and the C tests; the Makefile assembles,
compiles, and links them into one binary per suite; the framework's runner
discovers the tests and drives each routine through the **real calling
convention** — either natively (a capture trampoline on the real CPU) or, opting
in, inside a virtual CPU (the [emulator tier](guides/emulator.md)) — then asserts on the
result and reports.

> **Diagram:** [Framework build and run pipeline](reference/diagrams.md#framework-build-and-run-pipeline)

## Why asm-test

Existing tools either run a whole binary per case and only see *exit status and
stdout*, or assert from inside the assembly itself. asm-test fills the open
niche: a **C-hosted** framework that calls assembly through the **real calling
convention** and inspects registers, flags, and memory afterward — with proper
discovery and reporting. See [Design & background](project/design.md) for the prior-art
comparison and roadmap.

## What you get

- **Auto-discovered `TEST(...)`** cases, a provided runner, per-suite
  `SETUP`/`TEARDOWN`, `SKIP(reason)`, and colored TAP output.
- A full **assertion library** — signed/unsigned integer comparisons, strings,
  memory (with a hexdump diff), floating-point (ULP-aware), and SIMD lanes.
- **Register, flag, and ABI-preservation capture** through a real call, plus the
  full System V call model: arbitrary arity, struct returns, struct-by-value,
  floating-point, and 128-bit vectors.
- **Differential / property testing** against a C reference model over fuzzed
  inputs, with reproducible seeds.
- A robust **runner**: per-test `fork()` isolation, timeouts, crash/hang
  containment, filtering, shuffling, parallelism, and JUnit output.
- **Benchmark mode** reporting cycles per call.
- An optional **emulator tier** (Unicorn) that runs a routine inside a virtual
  CPU — x86-64, AArch64, RISC-V, ARM32, and the Windows x64 ABI — to read the
  *full* register file, catch precise faults, and record
  [execution traces and coverage](guides/tracing/traces.md).
- **Portability** across x86-64 and AArch64, Linux and macOS, with GAS and NASM
  assembler backends.

## Where to start

- New here? Read [Installation](getting-started/installation.md) then the
  [Quick start](getting-started/quickstart.md).
- Writing your first suite? See [Writing tests](getting-started/writing-tests.md) and the
  [Assertion reference](guides/assertions.md).
- Want to see it in action by use case? See [Examples](getting-started/examples.md) — correctness
  & ABI capture, differential testing, benchmarking, and crash/hang containment.
- Want the full capability list and what's available across architectures, OSes,
  and languages? See [Features & support matrix](reference/features.md).
- Consuming the framework from another project? See
  [Using asm-test in your project](reference/integration.md).
- Driving it from another language? See [Language bindings](bindings/index.md) for the
  shared overview, then the per-language page (Python, .NET, Go, Rust, C++, Zig,
  Node, Java, Ruby, Lua).

```{toctree}
:maxdepth: 2
:caption: Getting started
:hidden:

getting-started/installation
getting-started/quickstart
getting-started/writing-tests
getting-started/examples
```

```{toctree}
:maxdepth: 2
:caption: Guides
:hidden:

guides/assertions
guides/abi-capture
guides/floating-point-simd
guides/property-testing
guides/runner
guides/benchmarks
guides/emulator
guides/tracing/index
guides/disassembly
guides/win64
```

```{toctree}
:maxdepth: 2
:caption: Reference
:hidden:

reference/features
reference/portability
reference/integration
reference/packaging
reference/releasing
reference/ci
reference/api-reference
reference/troubleshooting
reference/diagrams
```

```{toctree}
:maxdepth: 2
:caption: Language bindings
:hidden:

bindings/index
```

```{toctree}
:maxdepth: 1
:caption: Project
:hidden:

project/design
project/glossary
project/changelog
```
