# ABI capture & registers

This is the framework's differentiator. Rather than only checking a return value,
asm-test calls your routine through a small assembly **capture trampoline** that
seeds the callee-saved registers with sentinels, marshals the arguments into the
ABI argument registers, performs the real `call`, and snapshots **every
general-purpose register plus the flags** into a `regs_t`. You then assert on
that snapshot.

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

:::{note}
Capture is defined as the **post-return** register state plus sentinel-based
callee-saved checks — the `call` itself necessarily touches some state. For an
arbitrary *mid-routine* snapshot of the full register file, use the
[emulator tier](emulator.md).
:::

## ABI preservation

`ASSERT_ABI_PRESERVED(&r)` verifies the routine restored every callee-saved
register (and the stack pointer). The trampoline seeds those registers with
distinctive sentinels (`0x1111…`, `0x2222…`, …) before the call, so a routine
that clobbers `rbx` without restoring it is caught even if it happened to leave a
plausible-looking value.

## Flags

CPU flags come back in `r.flags`. Assert individual bits:

```c
ASSERT_FLAG_SET(&r, CF);     // carry set
ASSERT_FLAG_CLEAR(&r, ZF);   // zero clear
```

On x86-64 the masks are `CF`, `PF`, `ZF`, `SF`, `OF`. On AArch64 the condition
flags (`NZCV`) map onto the same assertions.

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
pointer (`rdi` on x86-64, `x8` on AArch64) and small ones in the return
registers. `ASM_SRET` handles both:

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

A small struct (≤16 bytes) lands in the captured `r.ret`/`r.rdx`
(or `vec[0]`/`vec[1]` for an all-FP small struct).

## Struct-by-value parameters

Small structs pass as their "eightbytes" through the integer/FP register paths —
pass them with the ordinary `ASM_CALL2` / `ASM_FCALL*` macros. Large structs go
through `asm_call_capture_bigstruct` (an inline stack copy on x86-64, by-pointer
on AArch64). See the [API reference](api-reference.md#capture-functions) for the
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
