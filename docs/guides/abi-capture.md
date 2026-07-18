# ABI capture & registers

This is the framework's differentiator. Rather than only checking a return value,
asm-test calls your routine through a small assembly **capture trampoline** that
seeds the callee-saved registers with sentinels, marshals the arguments into the
ABI argument registers, performs the real `call`, and snapshots **every
general-purpose register plus the flags** into a `regs_t`. You then assert on
that snapshot.

> **Diagram:** [Capture trampoline](../reference/diagrams.md#capture-trampoline)

## Capturing a call

The `ASM_CALLn` macros call a routine with `n` integer arguments and fill a
`regs_t`:

```c
#include "asmtest.h"

extern long add_signed(long a, long b);

TEST(arith, capture_state) {
    regs_t r;
    ASM_CALL2(&r, add_signed, 2, 3);
    ASSERT_EQ(r.ret, 5);             // return value (rax / x0)
    ASSERT_ABI_PRESERVED(&r);        // callee-saved registers restored
    ASSERT_FLAG_CLEAR(&r, CF);       // carry clear
}
```

| Macro | Arguments |
|---|---|
| `ASM_CALL0(&r, fn)` … `ASM_CALL6(&r, fn, a, …, f)` | 0–6 integer args (the register slots) |
| `ASM_CALLN(&r, fn, …)` | **Any** number of integer args; the overflow (7th+ on x86-64, 9th+ on AArch64) spills to the stack per the ABI |

`ASM_CALLN` is the general form — use it whenever a routine takes more arguments
than fit in registers.

## The register snapshot

`regs_t` is architecture-specific. The fields you read most are `ret` (the return
value) and `flags`; the callee-saved registers back `ASSERT_ABI_PRESERVED`.

**x86-64 (System V):**

| Field | Register | Meaning |
|---|---|---|
| `ret` | `rax` | return value |
| `rdx` | `rdx` | second return register |
| `rbx`, `rbp`, `r12`–`r15` | callee-saved | checked by `ASSERT_ABI_PRESERVED` |
| `flags` | `RFLAGS` | CF/PF/ZF/SF/OF |
| `fret` | `xmm0` | FP return (after an FP call) |
| `vec[16]` | `xmm0`–`xmm15` | vector file (after a vector call) |

**AArch64 (AAPCS64):**

| Field | Register | Meaning |
|---|---|---|
| `ret` | `x0` | return value |
| `x19`–`x28`, `x29` | callee-saved | checked by `ASSERT_ABI_PRESERVED` |
| `flags` | `NZCV` | condition flags |
| `fret` | `d0` | FP return (after an FP call) |
| `vec[32]` | `v0`–`v31` | vector file (after a vector call) |

**RISC-V rv64 (LP64D):**

| Field | Register | Meaning |
|---|---|---|
| `ret` | `a0` | return value |
| `a1` | `a1` | second return register |
| `s0`–`s11` | callee-saved | checked by `ASSERT_ABI_PRESERVED` |
| `flags` | — | always `0` (no flags register; `ASMTEST_NO_FLAGS`) |
| `fret` | `fa0` | FP return (after an FP call) |
| `vec[32]` | `fs0`–`fs11` slots | FP callee-saved snapshot (after an `_fp`/`_fp_n` call) |

rv64 keeps the **second return register** `a1` (as x86-64 keeps `rdx`), which
AArch64 does not capture — so a 9–16-byte integer-pair return is fully
observable there. RV64GC has no 128-bit vector file, so `vec[]` instead holds the
FP callee-saved snapshot the `_fp`/`_fp_n` trampolines write (see below).

`regs_t` keeps the same shape across ABIs where it can: `ret` and `flags` are
present everywhere so tests stay portable, while the named callee-saved fields
(and a few others) differ by calling convention. The field offsets are fixed and
`_Static_assert`-guarded so the bindings can mirror them.

> **Diagram:** [Register snapshot layouts across ABIs](../reference/diagrams.md#register-snapshot-layouts-across-abis)

:::{note}
Capture is defined as the **post-return** register state plus sentinel-based
callee-saved checks — the `call` itself necessarily touches some state. For an
arbitrary *mid-routine* snapshot of the full register file, use the
[emulator tier](emulator.md).
:::

## ABI preservation

`ASSERT_ABI_PRESERVED(&r)` verifies the routine restored the callee-saved
*general-purpose* registers (System V: `rbx`, `rbp`, `r12`–`r15`; AArch64:
`x19`–`x28`). The trampoline seeds those registers with distinctive sentinels
(`0x1111…`, `0x2222…`, …) before the call, so a routine that clobbers `rbx`
without restoring it is caught even if it happened to leave a plausible-looking
value. It does **not** check the stack pointer (no `sp`/`rsp` is captured); a
stack-pointer imbalance instead surfaces as a crash from the forked runner.

Callee-saved *vector/FP* registers are checked separately by
`ASSERT_ABI_PRESERVED_VEC(&r)`: Win64 `xmm6`–`xmm15`, or AArch64 `d8`–`d15` (only
the low 64 bits are callee-saved per AAPCS64 6.1.2), after a `_vec`/`_vec_n`
capture. System V x86-64 has no callee-saved vector registers, so the macro is
not defined there. On **rv64** there is no vector file, so the check covers the
FP callee-saved set `fs0`–`fs11` and is valid after an **`_fp` or `_fp_n`**
capture (the `_vec` path self-skips); a routine that clobbers `d8`/`xmm6`/`fs2`
without restoring it is caught.

## Flags

CPU flags come back in `r.flags`. Assert individual bits:

```c
ASSERT_FLAG_SET(&r, CF);     // carry set
ASSERT_FLAG_CLEAR(&r, ZF);   // zero clear
```

On x86-64 the masks are `CF`, `PF`, `ZF`, `SF`, `OF`. On AArch64 the condition
flags (`NZCV`) map onto the same assertions.

**RISC-V rv64 has no condition-flags register** (comparisons fold into the branch
instructions), so `r.flags` is always `0`, no flag-mask macros are defined
(`ASMTEST_NO_FLAGS` is set), and `ASSERT_FLAG_SET(&r, CF)` is a **compile error**
there by design — surfacing the ISA fact rather than silently reading a
nonexistent bit. Route an overflow signal through a value-returning
`__builtin_add_overflow`-style check instead.

:::{note}
**rv64 FP-argument overflow (`ASM_FCALLN` / `asm_call_capture_fp_n`).** When more
than eight `double`s are passed, rv64 places doubles 9 and 10 in the **integer**
registers `a6`/`a7` as raw bit patterns — the psABI "FP args after the FP
registers are exhausted use the integer convention" rule, since this path fills
`a0`–`a5` with the integer-arg slots first — and doubles 11+ on the stack. That
placement is psABI-exact for a callee whose prototype has ≥6 leading integer
parameters, and is the framework's documented convention for the hand-written
`.s` routines it tests (`examples/fpover.s`); a **C-compiled** callee with >8 FP
args and <6 integer parameters would expect those doubles in different registers
— a recorded limitation of this capture path on rv64.
:::

## Exact register values

`ASSERT_REG_EQ(&r, field, value)` compares a named `regs_t` field as unsigned:

```c
regs_t r;
ASM_CALL2(&r, add_signed, 2, 3);
ASSERT_REG_EQ(&r, ret, 5);
ASSERT_REG_EQ(&r, rdx, 0);
```

## Struct returns

When a routine returns a struct, large ones come back through the hidden result
pointer (`rdi` on x86-64, `x8` on AArch64, `a0` on rv64 — the implicit first
argument, the x86-ish shape) and small ones in the return registers. `ASM_SRET`
handles both:

```c
struct point { long x, y, z, w; };   // large: returned via hidden pointer
extern struct point make_point(long, long, long, long);

TEST(structs, returns_large_struct) {
    regs_t r;
    struct point out;
    ASM_SRET(&r, make_point, &out, 1, 2, 3, 4);
    ASSERT_EQ(out.x, 1);
    ASSERT_EQ(out.w, 4);
}
```

A small struct (≤16 bytes) lands in the captured `r.ret`/`r.rdx` (`r.ret`/`r.a1`
on rv64; `vec[0]`/`vec[1]` for an all-FP small struct).

## Struct-by-value parameters

Small structs pass as their "eightbytes" through the integer/FP register paths —
pass them with the ordinary `ASM_CALL2` / `ASM_FCALL*` macros. Large structs go
through `asm_call_capture_bigstruct` (an inline stack copy on x86-64, by-pointer
on AArch64 and rv64). A small `struct{long; double}` uses the hardware-FP
convention on rv64/LP64D — `a0` + `fa0` (the x86-ish `ASM_MIXCALL` shape), not
AArch64's two-GP-register shape. See the [API reference](../reference/api-reference.md#capture-functions) for the
underlying function signatures.

## Underlying functions

The macros wrap C entry points you can call directly when you need full control:

```c
void asm_call_capture(regs_t *out, void *fn, const long *args);          // 6 regs
void asm_call_capture_args(regs_t *out, void *fn, const long *args, int nargs);
void asm_call_capture_sret(regs_t *out, void *fn, void *result,
                           const long *args, int nargs);
void asm_call_capture_bigstruct(regs_t *out, void *fn, const long *iargs, ...);
```

Continue to [Floating-point & SIMD](floating-point-simd.md) for the FP and vector
call paths.
