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
- Makefile assembles `.s` (default: `clang`/`as`, GAS syntax — `nasm` is not
  installed on the host), compiles C tests, links one binary per suite.
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
- **Phase 4 — Emulator tier: _done (x86-64 guest)._** A
  [Unicorn Engine](https://www.unicorn-engine.org/) wrapper (`src/emu.c`,
  `include/asmtest_emu.h`, built with `-lunicorn`) copies a routine into a
  virtual x86-64 CPU, preloads registers/memory, runs it to `ret` or single
  steps N instructions, reads back the full register file, and reports invalid
  accesses as precise faults — all free of ABI constraints and host arch
  (Unicorn emulates x86-64 regardless of host). Has its own CI job. AArch64-guest
  emulation is a natural follow-up (Unicorn supports `UC_ARCH_ARM64`).

---

## 7. Key decisions (revisit as needed)

- **Assembler:** GAS via Apple `clang`/`as` (installed). NASM opt-in; if we
  standardize on NASM instead, `brew install nasm` and flip the Makefile default.
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
