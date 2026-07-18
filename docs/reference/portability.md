# Portability & assemblers

asm-test runs the **same sources** on x86-64, AArch64, and RISC-V (rv64), on
Linux and macOS, through either of two assembler backends. This page explains how
that works and how to choose a backend.

## Supported targets

| | Linux | macOS |
|---|---|---|
| **x86-64** | ✓ | ✓ |
| **AArch64** | ✓ | ✓ (Apple Silicon) |
| **RISC-V (rv64)** | ✓ | — (no RISC-V macOS) |

CI runs the suites on the four x86-64/AArch64 OS combinations (`ubuntu-latest`,
`ubuntu-24.04-arm`, `macos-latest`, `macos-15-intel`); the RISC-V (rv64) host
tier runs in a `linux/riscv64` container under QEMU binfmt (the `test-riscv64`
job — there is no hosted riscv64 runner). See the **RISC-V (rv64) host tier**
notes below for its two documented divergences. The packaged bindings'
install-tests are additionally **clean-room** hardened — no leaked dev tree,
Homebrew copy, or loader override can satisfy a load — see
[Clean-room testing](../clean-room-testing.md).

This page covers the core capture library; the out-of-process single-step
*tracing* tier (a separate subsystem — [native tracing](../guides/tracing/native-tracing.md))
follows the same Linux/macOS split: `ptrace`-based on Linux x86-64/AArch64, and
on **macOS x86-64** via Mach exception ports (`asmtest_mach.h`) instead, since
macOS `ptrace` cannot edit `RIP`/`RFLAGS` at all.

One source set reaches every target two ways: a **native** build for the host
architecture (through either assembler backend), and the **emulator** guests,
which run on any host:

> **Diagram:** [Portability across targets](diagrams.md#portability-across-targets)

## One source, two object formats

The `ASM_FUNC` / `ASM_ENDFUNC` macros in `include/asm.h` abstract the ELF vs
Mach-O differences — symbol decoration (the leading `_` on Mach-O), `.globl`,
alignment, and size directives — so a routine source builds unchanged on Linux
and macOS. Because `;` is a statement separator on x86 but a **comment** on
AArch64, the macros are `.macro`-based rather than relying on `;`.

Architecture-specific instruction bodies live behind preprocessor guards:

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
#elif defined(__riscv) && __riscv_xlen == 64
    add     a0, a0, a1
    ret
#endif
ASM_ENDFUNC add_signed
```

On RISC-V the ELF `@function` / `@progbits` directives that `include/asm.h`
already emits for x86-64 apply unchanged, so `ASM_FUNC` / `ASM_ENDFUNC` need no
new branch — only an `#elif defined(__riscv) && __riscv_xlen == 64` instruction
body per routine.

The C compiler (`cc` — gcc or clang) assembles the GAS-syntax `.s` sources
directly, so **no separate assembler is required** for the default backend.

## Assembler backends

### GAS (default)

The default backend uses GAS syntax via the system C compiler. It is the only
backend on AArch64 (the `.s` sources assemble through the compiler's built-in
assembler, so nothing extra is needed there).

### NASM backend

An opt-in NASM backend ships **Intel-syntax** counterparts of the sources
(`foo.asm` beside `foo.s`) with their own `asm_nasm.inc` include. It is
**x86-64 only** (NASM is x86-only by design). Select it per build:

```sh
make ASM_SYNTAX=nasm test
```

This needs `nasm` installed (`make deps` provides it). The NASM sources are
exercised by their own CI job, so both backends stay in sync.

:::{note}
**On AArch64, GAS is the only backend.** A `make ASM_SYNTAX=nasm` build there is
not supported — write the AArch64 body in the `.s` source under
`#if defined(__aarch64__)` and assemble it with the default backend.
:::

## RISC-V (rv64) host tier

The native capture tier supports RISC-V rv64 on the **RV64GC / LP64D** baseline
(the Linux-distro standard — hard-float doubles, no vector extension). Its
`regs_t` captures the return pair `a0`/`a1` (unlike AArch64, which captures only
`x0`), the integer callee-saved set `s0`–`s11`, and the FP callee-saved set
`fs0`–`fs11`; the trampolines seed and check all of them. Two divergences are
worth knowing, both by design:

- **No condition flags.** RISC-V has no architectural flags register —
  comparisons fold into the branch instructions. So `regs_t.flags` is always `0`,
  no `ASMTEST_CF`/`ZF`/`SF`/`OF` macros are defined (`ASMTEST_NO_FLAGS` is set
  instead), and `ASSERT_FLAG_SET(&r, CF)` is a **compile error** on rv64 rather
  than a silent always-false check. A routine whose signal is an overflow —
  `examples/checked.s`'s `checked_add` — returns the wrapping sum on rv64 and its
  flag assertions self-skip; the rv64 way to surface overflow is a
  value-returning `__builtin_add_overflow`-style check, not a condition code.
- **No 128-bit vector capture.** RV64GC has no vector registers (RVV would be a
  future arm — `asmtest_cpu_has_vec128()` is the designed hook, returning `0`
  today), so the `ASM_VCALL*` macros **self-skip** with a printed reason. The FP
  callee-saved check `ASSERT_ABI_PRESERVED_VEC` therefore rides the `_fp` /
  `_fp_n` capture on rv64 (which seed/capture `fs0`–`fs11`), not `_vec`.

The out-of-process tracing tiers (single-step/`ptrace`, DynamoRIO, hardware
trace) are **not** ported to rv64 — they stay x86-64/AArch64. RISC-V also remains
a first-class **emulator guest** (Unicorn/Keystone) on any host, independent of
this native tier.

## Register-snapshot fidelity

Native capture is defined as the **post-return** general-purpose registers plus
flags, with sentinel-based callee-saved checks — not an arbitrary mid-routine
snapshot, because the `call` itself touches state. When you need a mid-routine or
full-file view (or a non-host architecture), use the [emulator tier](../guides/emulator.md),
which has no such constraint and adds RISC-V and ARM32 guests on top.
