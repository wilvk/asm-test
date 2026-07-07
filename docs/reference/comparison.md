# asm-test vs. alternatives

There is no like-for-like competitor — no other maintained framework calls
assembly through the real ABI and asserts on registers, flags, and memory with
test discovery and CI reporting. What people actually use instead is one of
four *workflows*. This page compares against those honestly: what each does
well, what you give up, and when it is the better choice.

> Kept current against the tools' documentation as of 2026-07. If a claim here
> has drifted, please open an issue — this page is maintained, unlike the
> historical prior-art survey in [DESIGN.md](../project/design.md).

## The short version

| Workflow | Asserts on | Isolation | Where it beats asm-test |
|---|---|---|---|
| **C unit framework + `.s` files** (cmocka, Criterion, Unity, gtest) | return values via a C prototype | Criterion forks; cmocka/Unity do not | you already use one everywhere and only care about return values |
| **Raw Unicorn scripting** (Python/C) | anything — full guest state, hand-rolled | total (emulated) | one-off exploration of foreign/hostile code; no toolchain needed |
| **qemu + gdb** (gdbstub, cross-ISA) | anything — interactive or GDB-scripted | total (separate process/VM) | debugging one failure interactively; full-system/firmware code |
| **asmUnit-style in-asm macros** | registers via `cmp` macros | none | zero-dependency environments where C cannot be hosted |
| **asm-test** | registers, flags, FP/vector files, memory, ABI compliance, traces | fork-per-test + guard pages + emulator/tracing tiers | everything below |

## vs. a C unit framework with assembly linked in

The most common setup: keep using cmocka / Criterion / Unity / googletest,
add `.s` files to the build, declare a C prototype, and assert on the return
value.

What that workflow cannot see is exactly what assembly bugs are made of:

- **Only the return value is observable.** A routine that clobbers a
  callee-saved register, leaves flags wrong, or scribbles past a buffer still
  *returns* the right value. asm-test's capture macros record the full
  register file (`ASM_CALL*` → `regs_t`), so `ASSERT_ABI_PRESERVED`,
  `ASSERT_FLAG_SET`, `ASSERT_REG_EQ`, and the FP/vector-file assertions test
  the contract, not just the result.
- **The C compiler is in the way.** Calling through a prototype lets the
  compiler reorder, inline, and allocate registers around your call; the
  framework's trampoline calls through the real ABI with a known register
  image on entry.
- **No memory-safety instrumentation for hand-written code.** ASan does not
  instrument your `.s`; asm-test's guard-page allocators
  (`asmtest_guarded_alloc`) turn a one-byte overrun into a caught fault, and
  the emulator tier catches reads/writes that never fault natively.
- **No differential testing.** `ASSERT_MATCHES_REF*` / `ASSERT_MATCHES_FREF*`
  fuzz a routine against a C model (with input shrinking); peer C frameworks
  have nothing equivalent built in.

When it is still the right choice: the assembly surface is one or two leaf
functions, you only care about results, and adding a second framework is not
worth it. (You can also embed asm-test *into* that setup — the runner emits
TAP/JUnit, so a `make`-invoked asm-test binary slots into an existing CI
matrix; see [Integration](integration.md).)

## vs. raw Unicorn scripting

Driving [Unicorn](https://www.unicorn-engine.org/) directly from Python or C
gives total control: map memory, set registers, emulate, inspect. asm-test's
own emulator tier is built on it, so this is a comparison with our own
substrate, scripted by hand.

What hand-rolling costs you:

- **Everything is manual.** Register setup per ABI, stack layout, argument
  marshaling, fault classification, instruction caps, coverage — each script
  reimplements them, per architecture. asm-test's `emu_call*` does the SysV /
  AAPCS64 / Win64 marshaling and returns a typed result struct.
- **No test semantics.** No discovery, no assertion diagnostics, no TAP/JUnit,
  no CI story — a mismatch is a Python traceback, not a report.
- **No native cross-check.** The same asm-test suite can run a routine both
  natively (real silicon, real ABI) and emulated, and the trace tiers assert
  the two agree block-for-block. A standalone script tests only the emulation.
- Watchpoints, register invariants, snapshot/restore, coverage-guided fuzzing,
  and mutation testing exist in the tier already ([Emulator](../guides/emulator.md)).

When it is still the right choice: exploratory analysis of code you do not
build — shellcode, firmware blobs, malware — where there is no toolchain and
no repeatable suite to keep.

## vs. qemu + gdb

`qemu-user -g` (or `-s` for system mode) plus a cross-gdb is the classic way
to run and inspect foreign-ISA assembly.

- **It is a debugger, not a test harness.** GDB scripting can be bent into
  assertions, but there is no discovery, isolation, reporting, or CI-friendly
  exit discipline; failures are interactive artifacts.
- **Slow and serial** — one qemu+gdb session per scenario, versus thousands of
  in-process emulator calls or forked native tests per second.
- **qemu-user cannot host a ptrace tracer/tracee pair**, so single-step-based
  flows self-skip under it (this repo's own arm64 CI documents that limit) —
  the thing it is best at is *being* the debugger, not being debugged around.

When it is still the right choice: interactively diagnosing one concrete
failure, or full-system/firmware work where code needs a kernel, MMU state,
or devices — asm-test tests *routines*, not systems.

## vs. in-asm assertion macros (asmUnit style)

Pure-assembly `cmp`-macro harnesses (see the [prior art in
DESIGN.md](../project/design.md)) need no C host at all. The cost: assertions
are limited to what `cmp` can express, there is one assert per "method", no
containment (a segfault kills the run), and no reporting beyond a character
stream. If your environment can host C at all, the C-hosted approach
dominates; if it cannot (bare metal, exotic bootstrap), asmUnit-style macros
remain the only option.

## What asm-test does not try to be

For calibration, things the alternatives sometimes cover that this framework
deliberately does not:

- **Not a debugger** — failures report state; stepping through them
  interactively is gdb's job (`--no-fork` exists precisely to hand a crash to
  a debugger).
- **Not full-system** — no MMU/kernel/device emulation; routines, not OSes.
- **Not a fuzzer platform** — the coverage-guided fuzz/mutation tier targets
  single routines with integer inputs, not AFL-scale corpus management.
- **Wall-clock benchmarking is cycle-count only** — `BENCH` reports cycles
  (with dispersion), not a full statistical harness like Criterion-the-Rust
  crate or hyperfine.

## Feature matrix

| Capability | asm-test | C fw + `.s` | raw Unicorn | qemu+gdb |
|---|---|---|---|---|
| Register/flag/FP/vector assertions | ✅ | ❌ return value only | hand-rolled | GDB script |
| ABI-compliance check (callee-saved, ±`d8–d15`) | ✅ | ❌ | hand-rolled | manual |
| Crash/hang containment per test | ✅ fork + alarm | Criterion only | ✅ emulated | session dies |
| Guard-page buffer safety | ✅ | ❌ | via mappings | ❌ |
| Differential testing vs C model (+ shrinking) | ✅ | ❌ | hand-rolled | ❌ |
| Cross-ISA guests (AArch64/ARM32/RISC-V on x86 host) | ✅ emulator tier | ❌ | ✅ | ✅ |
| Native + emulated cross-check of the same routine | ✅ | ❌ | ❌ | ❌ |
| Instruction/branch tracing tiers (PT, LBR, single-step, DynamoRIO) | ✅ | ❌ | per-insn hooks | ❌ |
| TAP/JUnit + CI discipline | ✅ | ✅ | ❌ | ❌ |
| 10 language bindings | ✅ | n/a | Python/others | n/a |
| No toolchain needed (test foreign blobs) | emulator tier | ❌ | ✅ | ✅ |

If something here reads as unfair to a tool you know well, that is a bug in
this page — file an issue.
