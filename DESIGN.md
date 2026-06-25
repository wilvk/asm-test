# asm-test — Design Plan

A **C-hosted unit-testing framework for assembly language**. You write assembly
routines (the code under test) and test cases in C; the framework links them into
a single binary, runs every test through the real ABI, and asserts on **return
values, register state, CPU flags, and memory** — with test discovery, colored
reporting, and a nonzero exit on failure.

Target host (initial): **x86-64 macOS**, System V AMD64 ABI, GAS syntax via Apple
`clang`/`as`. AArch64 (Apple Silicon) is a later phase.

---

## 1. Prior art

| Project | Approach | Asserts on | Gap |
|---|---|---|---|
| [briansteffens/asmtest](https://github.com/briansteffens/asmtest) | Python templating; assemble→link→run a binary per case | Exit status + stdout only | No register/memory inspection; you must print results yourself |
| [asmUnit (Code Cop)](http://blog.code-cop.org/2015/08/how-to-unit-test-assembly.html) | Pure NASM assertion macros, in-asm | Registers via `cmp` macros | "Little real assembly left"; one assert per method; manual test list |
| [MIPSUnit](https://scholarworks.gvsu.edu/fsdg/545/) | JUnit/RSpec wrappers (academic) | MIPS sim state | MIPS-only, not C, not x86 |
| cc65 + sim65 | C `main` calls asm, runs in simulator | stdout/exit | 6502-only |

**Open niche:** a C-hosted framework that calls assembly through the real ABI and
asserts on registers/flags/memory with proper discovery and reporting. That is the
goal of this project.

---

## 2. Concept

- Author writes assembly routines following the System V AMD64 ABI.
- Author writes test cases in C using framework macros.
- Framework assembles + compiles + links C and asm into one test binary.
- Running the binary executes all tests and reports results (TAP-style, colored),
  exiting nonzero if any fail.

```c
#include "asmtest.h"

extern long add_signed(long a, long b);   // routine under test (in add.s)

TEST(arith, adds_two_numbers) {
    ASSERT_EQ(add_signed(2, 3), 5);
}

TEST(arith, preserves_callee_saved) {
    regs_t r;
    asm_call2_capture(&r, (void*)add_signed, 2, 3);
    ASSERT_EQ(r.rax, 5);
    ASSERT_ABI_PRESERVED(&r);     // rbx, rbp, r12–r15 restored
    ASSERT_FLAG_CLEAR(&r, CF);
}
```

---

## 3. Repository layout

```
asm-test/
├── include/asmtest.h        # public API: TEST, ASSERT_*, fixtures, regs_t
├── src/asmtest.c            # runner, registry, reporting, failure model
├── src/capture.s            # register/flags capture trampoline (x86-64 SysV)
├── examples/                # sample routines (.s) + their tests (.c)
├── tests/                   # the framework's own self-tests
├── Makefile                 # assemble + compile + link + run
└── DESIGN.md
```

---

## 4. Core components

### 4.1 Test discovery & runner (C)
- `TEST(suite, name) { ... }` expands to a function plus a registration record.
- Registration via a dedicated linker section (preferred) or
  `__attribute__((constructor))` pushing into a global registry.
- `main()` (provided by the framework) walks the registry, runs each test,
  tracks pass/fail/skip, prints a summary, and sets the exit code.

### 4.2 Failure model
- Assertion macros capture file/line/expression + actual vs expected, then
  `longjmp` back into the runner. A failed assert aborts *that* test only; the
  suite continues.
- `SETUP(suite)` / `TEARDOWN(suite)` run before/after each test in a suite.

### 4.3 Calling assembly from C
- Routines declared `extern` with C prototypes; macOS `_` symbol prefix handled
  in the asm source / linkage.
- Simple path: direct call, assert on the return value.

### 4.4 Register & flag inspection (the differentiator)
- `src/capture.s` provides trampolines (`asm_call0_capture` … `asm_callN_capture`)
  that: seed callee-saved registers with sentinels → marshal args into the ABI
  argument registers → `call` the routine → snapshot all GP registers + RFLAGS
  into a `regs_t` struct.
- Enables:
  - `ASSERT_ABI_PRESERVED(&r)` — verifies rbx, rbp, r12–r15 (and rsp) restored.
  - `ASSERT_FLAG_SET(&r, CF)` / `ASSERT_FLAG_CLEAR(&r, ZF)` etc.
  - Return-register checks (`r.rax`, `r.rdx`).

### 4.5 Memory assertions
- `ASSERT_MEM_EQ(ptr, expect, len)` with a hexdump diff on failure.
- Buffer helpers with canary bytes and/or `mmap` guard pages to catch overruns
  and out-of-bounds writes by the routine under test.

### 4.6 Build layer
- Makefile assembles `.s` (default: `clang`/`as`, GAS syntax), compiles C
  tests, links one binary per suite.
- Assembler is a config variable, so a NASM (`-f macho64`) backend is opt-in.

---

## 5. Public API sketch

```c
// Definition / registration
TEST(suite, name)            // define + auto-register a test
SETUP(suite)  / TEARDOWN(suite)
SKIP(reason)                 // mark current test skipped

// Value assertions
ASSERT_TRUE(x) / ASSERT_FALSE(x)
ASSERT_EQ(a, b) / ASSERT_NE(a, b)
ASSERT_LT(a, b) / ASSERT_LE / ASSERT_GT / ASSERT_GE
ASSERT_STREQ(a, b)

// Memory
ASSERT_MEM_EQ(ptr, expect, len)

// Register / flags (via captured regs_t)
ASSERT_ABI_PRESERVED(&r)
ASSERT_FLAG_SET(&r, CF|PF|ZF|SF|OF|...)
ASSERT_FLAG_CLEAR(&r, ...)

// Capture trampolines
void asm_call0_capture(regs_t *out, void *fn);
void asm_call1_capture(regs_t *out, void *fn, long a);
void asm_call2_capture(regs_t *out, void *fn, long a, long b);
// ... up to the 6 SysV integer arg registers
```

---

## 6. Phasing

- **Phase 0 — Skeleton (proof of life): _done._** repo layout, Makefile, runner
  `main`, `TEST` + registration, `ASSERT_EQ` on return values, one example asm
  routine plus a passing and a failing test.
- **Phase 1 — Assertion library: _done._** full `ASSERT_*` set, setjmp failure
  model, colored TAP output, pass/fail/skip summary, exit codes, setup/teardown.
- **Phase 2 — Introspection: _done._** `capture.s` trampoline, ABI-preservation
  + RFLAGS assertions, guard pages, and fatal-signal-to-failure handling.
- **Phase 3 — Portability & CI: _done._** Cross-platform across x86-64 and
  AArch64 on Linux and macOS: shared sources with `ASM_FUNC` (`.macro`-based,
  since `;` is a comment on AArch64) abstracting ELF/Mach-O symbol decoration,
  and per-arch routine/trampoline bodies selected by `#if`. A GitHub Actions
  matrix runs the suites on all four combinations (ubuntu-latest,
  ubuntu-24.04-arm, macos-latest, macos-13). The opt-in **NASM backend**
  (`ASM_SYNTAX=nasm`) ships Intel-syntax counterparts of the sources (x86-64
  only) with its own `asm_nasm.inc`, and has its own CI job.
- **Phase 4 — Emulator tier: _done (x86-64 and AArch64 guests)._** A
  [Unicorn Engine](https://www.unicorn-engine.org/) wrapper (`src/emu.c`,
  `include/asmtest_emu.h`, built with `-lunicorn`) runs a routine inside a
  virtual CPU, preloads registers/memory, runs it to `ret` or single steps N
  instructions, reads back the full register file, and reports invalid accesses
  as precise faults — free of ABI constraints. The x86-64 path copies bytes from
  a built routine; the AArch64 path (`emu_arm64_*`) takes raw machine code, so
  ARM64 routines emulate even on an x86-64 host. Has its own CI job.

- **In-line assembler tier: _done._** A [Keystone Engine](https://www.keystone-engine.org/)
  wrapper (`src/assemble.c`, `include/asmtest_assemble.h`, built with
  `-lkeystone`) turns an assembly *string* into machine code for the emulator's
  guests, then `emu_*_call_asm` runs it via the Phase 4 engine — assembling at the
  emulator's load base so PC-relative and branch targets resolve. It is the
  assembler counterpart to the Unicorn disassembler: the emulator already consumes
  raw `(code, code_len)` buffers, so this is a text→bytes front end. Optional and
  pkg-config gated, and deliberately kept out of `libasmtest_emu` so the
  Unicorn-only bindings are unaffected; `make asm-test` and a CI `asm` job
  (Keystone has no distro package, so it is source-built via
  `scripts/build-keystone.sh`). The five dlopen bindings expose it as an optional
  `CallAsm` (running against a separate `libasmtest_emu_asm`, self-skipping
  otherwise), exercised by the native `bindings-asm` CI job. See
  [the implementation plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/inline-asm-keystone-plan.md).

The phases below are **planned**, ordered to deepen the framework's core
promise — calling assembly through the real ABI and inspecting the result —
before broadening reach. The current testable surface is narrow: a routine's
whole signature must be ≤6 integer arguments (all `long`) with an integer
return. Phases 5–7 widen *what can be tested*; 8–9 mature *the runner*; 10–11
add reach.

- **Phase 5 — Floating-point & SIMD: _done._** `asm_call_capture_fp` marshals
  `double` args into the FP argument registers (`xmm0–7` / `d0–7`) and captures
  the scalar FP return into `regs_t.fret`, with `ASSERT_FP_EQ` /
  `ASSERT_FP_NEAR(&r, expected, ulps)`. `asm_call_capture_vec` marshals 8 full
  128-bit vector args and captures the entire vector file (`xmm0–15` / `v0–31`)
  into `regs_t.vec[]` (return = `vec[0]`), with `ASSERT_VEC_EQ` plus raw lane
  assertions `ASSERT_DEQ/DNEAR` (double) and `ASSERT_FEQ/FNEAR` (float). Driven
  by `ASM_FCALLn` / `ASM_VCALLn`; callee-saved integers stay checked across both.
- **Phase 6 — Full ABI call model: _done._** `asm_call_capture_args` /
  `ASM_CALLN` pass any number of integer arguments (registers then stack, via a
  variable-size frame). `asm_call_capture_sret` / `ASM_SRET` handle large struct
  returns through the hidden result pointer (rdi / x8); small returns land in the
  captured rax:rdx (x0:x1). Struct-by-value parameters: small structs pass as
  their eightbytes through the integer/FP register paths, and large ones via
  `asm_call_capture_bigstruct` (x86-64 copies inline onto the stack, AArch64
  passes a pointer, dispatched in C). Mixed integer/float register args work
  through `asm_call_capture_fp`. Float/vector *register-overflow* args (more than
  8 FP/vector args) now spill to the stack too, via `asm_call_capture_fp_n` /
  `asm_call_capture_vec_n` (`ASM_FCALLN` / `ASM_VCALLN`), which marshal the first
  8 into registers and the rest onto the stack per the ABI. All on both arches
  and both assemblers.
- **Phase 7 — Differential / property testing: _done._** A test supplies both
  the routine and a C reference model, and the framework fuzzes random inputs
  and asserts equivalence: `ASSERT_MATCHES_REF{1,2,3}(fn, ref, gen, n)` (one per
  integer arity — C cannot dispatch on a function pointer's arity). A seedable
  splitmix64 RNG (`asmtest_rng_t`, `asmtest_rng_long/range`) drives per-test
  generators (`int gen(asmtest_rng_t*, long*, int)`) that produce each input
  tuple; the asm routine is called through the real ABI (`asm_call_capture_args`)
  and compared against the C model. On the first mismatch the offending input,
  both results, and the seed are reported; the seed is fixed by default (so
  failures reproduce) and overridable via the `ASMTEST_SEED` env var (so CI can
  vary it). Turns fixed-vector assertions into specification conformance over
  thousands of random inputs; pairs naturally with the emulator tier, where a
  looping or faulting routine cannot take down the harness.
- **Phase 8 — Runner robustness & CLI: _done._** Each test runs in a `fork()`ed
  child by default with an `alarm()` timeout, so an infinite loop or heap
  corruption in the routine under test becomes a reported timeout/crash failure
  instead of hanging or poisoning later tests (the native path's answer to the
  emulator's `max_insns`). The child ships its outcome (PASS/FAIL/SKIP + message
  + location) back over a pipe; if it dies before reporting — a SIGABRT-class
  corruption the in-process signal handler can't catch, a hard kill — the parent
  synthesizes the result from the `wait()` status (`killed by signal …` /
  `timed out … (killed)`). `--no-fork` keeps the in-process model (which still
  catches SIGSEGV/SIGBUS/… and times out hangs via the same alarm, but a
  SIGABRT-class crash takes the runner down — the reason fork is the default).
  `main()` takes argv: `--filter` (glob over suite, name, or `suite.name`),
  `--list`, `--shuffle` with optional `--seed` (Fisher–Yates over the splitmix64
  RNG; the seed is printed for reproducibility), `--timeout=SEC` (also
  `ASMTEST_TIMEOUT`), and `--format=junit` (suite-grouped XML for CI ingestion)
  alongside the default colored TAP. Demoed by `make demo-robust`.
- **Phase 9 — Benchmark mode: _done._** A `BENCH(suite, name) { ... }` form whose
  body is one measured iteration; the runner auto-calibrates an inner repeat
  count (doubling until a round spans enough of the counter to be resolvable,
  capped) and times several rounds, reporting **min/median/mean cycles per call**
  via `rdtsc` (x86-64) / `cntvct_el0` (AArch64), read by the inline
  `asmtest_cycle_counter()`. Benchmarks register in their own list and run only
  under `--bench` (so `make test` is unaffected), honoring `--filter` and
  `--list`; `--bench-reps=N` pins the repeat count for reproducibility. Each runs
  in-process under the same signal/timeout guard as a test, so a crashing or hung
  routine is reported as an error rather than taking the process down.
  `BENCH_USE(x)` funnels a pure-C result through a volatile sink so it is not
  optimized away (calls into the routine under test need no help). Demoed by
  `make bench`. On AArch64 the counter is the virtual timer, whose tick is
  coarser than a core cycle (reported as `ticks`).
- **Phase 10 — Emulator coverage & tracing: _done._** Building on the Phase 4
  emulator (both guests already hook `UC_HOOK_MEM_INVALID`), `emu_call_traced` /
  `emu_arm64_call_traced` take an opt-in `emu_trace_t` and add a `UC_HOOK_CODE`
  instruction trace (each executed instruction's byte offset from the routine
  entry, in order) and `UC_HOOK_BLOCK` basic-block coverage (the *distinct*
  block-start offsets, de-duplicated) into caller-owned buffers — either may be
  NULL to skip that dimension. Tracing *appends*, so re-running with the same
  struct unions coverage across inputs: the example `classify` routine has three
  return paths no single input walks, and the test asserts the union of blocks
  across `{-5, 0, +7}` exceeds any one input's — "did the tests exercise every
  branch?" answered inside the emulator. `blocks_total` counts every block entry
  (a loop re-enters its body block each pass) while `blocks_len` stays the
  distinct count; `insns_total` counts past a short buffer and sets `truncated`.
  `emu_call` / `emu_arm64_call` remain as the no-trace wrappers. Covered by
  `make emu-test`.
- **Phase 11 — Breadth: _done._** Additional calling conventions and targets,
  all carried by the emulator tier so they run on the existing CI hosts.
  - _RISC-V (RV64) emulator guest._ A third Unicorn guest (`emu_riscv_*`,
    `UC_ARCH_RISCV` / `UC_MODE_RISCV64`) alongside x86-64 and AArch64, built to
    the same shape as the AArch64 guest: it takes raw RV64 machine code,
    preloads integer args into `a0..a7` (`x10..x17`), seeds `ra` with the
    sentinel return address and `sp` with the stack, runs to the routine's
    `ret`, and reads back the full `x0..x31` + `pc` file (RISC-V has no flags
    register — comparisons fold into its branches). It shares the fault,
    instruction-trace, and basic-block-coverage hooks, so RISC-V routines
    emulate — and report coverage — on an x86-64 host. One backend quirk handled
    in `emu_riscv_open`: Unicorn's RISC-V core *fetches* the instruction at the
    `until` address before stopping, so the sentinel `EMU_RET_MAGIC` is mapped
    as a read/exec landing page (emulation still stops before executing it).
  - _ARM32 (A32) emulator guest._ A fourth guest (`emu_arm_*`, `UC_ARCH_ARM` /
    `UC_MODE_ARM`) on the same template: integer args in `r0..r3` per AAPCS,
    return in `r0`, sentinel `lr`, returning via `bx lr`; it reads back the full
    32-bit `r0..r15` file plus `cpsr`. Unlike RISC-V, its `bx lr` stops cleanly
    at the sentinel with no landing page needed.
  - _Windows x64 ("Win64") calling convention._ Rather than a native trampoline
    (which would need a Windows host), Win64 rides the existing x86-64 emulator
    engine via `emu_call_win64` / `_traced`: integer args go in `rcx, rdx, r8,
    r9` then on the stack *above 32 bytes of caller-reserved shadow space* (the
    5th arg lands at `[rsp+40]`, not `[rsp+8]`), the return is in `rax`, and
    `rsi`/`rdi` move into the nonvolatile set. So a Win64 routine is testable on
    a System V host; a contrast test runs identical bytes (`mov rax, rcx`) under
    both conventions and shows the result diverge.
  - All covered by `make emu-test` (the `emu_riscv`, `emu_arm`, and `emu_win64`
    suites).

**Near-term correctness fixes (independent of the phasing above): _done._**

- _Done._ Added unsigned 64-bit comparisons `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE`
  (compared and reported as unsigned hex) for addresses and register values; the
  signed `ASSERT_EQ` family remains for signed integers.
- _Done._ `ASSERT_MEM_EQ` now reports the first differing byte plus a hexdump
  window of expected vs actual.
- _Done._ Added `ASSERT_REG_EQ(&r, field, val)` (unsigned field compare).
- _Done._ Added `asmtest_guarded_alloc_under` / `_free_under` with a leading
  guard page so underruns (`buf[-1]`) fault, alongside the trailing-guard alloc.

---

## 7. Key decisions (revisit as needed)

- **Assembler:** GAS via Apple `clang`/`as` is the default; NASM (also
  installed) is opt-in via `ASM_SYNTAX=nasm`. Standardizing on NASM instead
  means flipping the Makefile default.
- **Architecture:** x86-64 first (host); AArch64 in Phase 3.
- **Introspection strategy:** ABI-based register capture first; emulator-based
  deep introspection only after Phases 0–2 prove the value.

---

## 8. Risks / open questions

- macOS Mach-O specifics: `_` symbol prefix, PIE/position-independent code, and
  how aggressively to constrain routine-under-test linkage.
- Register-snapshot fidelity: the `call` itself touches state, so capture is
  defined as *post-return* GP regs + RFLAGS, plus sentinel-based callee-saved
  checks — not an arbitrary mid-routine snapshot (that needs the Phase 4 emulator).
- Whether per-suite binaries or one combined binary gives the better
  isolation/reporting trade-off.

---

## Sources
- asmtest — <https://github.com/briansteffens/asmtest>
- How to Unit-Test Assembly — <http://blog.code-cop.org/2015/08/how-to-unit-test-assembly.html>
- MIPSUnit — <https://scholarworks.gvsu.edu/fsdg/545/>
- nesdev: assembler with unit testing — <https://forums.nesdev.org/viewtopic.php?t=23598>
