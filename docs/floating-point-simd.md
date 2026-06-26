# Floating-point & SIMD

The capture trampoline has FP and vector paths that marshal `double` and 128-bit
vector arguments into the FP/SIMD argument registers and capture the FP/vector
return. Callee-saved **integer** registers stay checked across these calls too,
so `ASSERT_ABI_PRESERVED` still works.

## Floating-point calls

`ASM_FCALLn` marshals `double` arguments into the FP argument registers
(`xmm0`–`xmm7` / `d0`–`d7`) and captures the scalar return into `r.fret`:

```c
#include "asmtest.h"

extern double fp_add(double a, double b);

TEST(fp, add_returns_double) {
    regs_t r;
    ASM_FCALL2(&r, fp_add, 1.5, 2.25);
    ASSERT_FP_EQ(&r, 3.75);
}
```

| Macro | Arguments |
|---|---|
| `ASM_FCALL1/2/3(&r, fn, x[, y[, z]])` | 1–3 `double` args |
| `ASM_FCALLN(&r, fn, …)` | Any number of `double`s; the 9th+ spill to the stack per the ABI |

### Exact vs. tolerant comparison

Floating-point results are rarely bit-exact. `ASSERT_FP_NEAR` compares within a
ULP (unit-in-the-last-place) budget:

```c
TEST(fp, near_tolerates_rounding) {
    regs_t r;
    ASM_FCALL2(&r, fp_add, 0.1, 0.2);   // classically one ULP off 0.3
    ASSERT_FP_NEAR(&r, 0.3, 1);          // within 1 ULP — passes
}
```

| Macro | Checks |
|---|---|
| `ASSERT_FP_EQ(&r, expected)` | `r.fret` bit-equals `expected` |
| `ASSERT_FP_NEAR(&r, expected, ulps)` | `r.fret` within `ulps` of `expected` |

## SIMD / vector calls

`ASM_VCALLn` marshals 128-bit vector arguments and captures the **entire vector
file** into `r.vec[]` (the return is `vec[0]`). The argument type is `vec128_t`,
a union giving byte, 32/64-bit integer, and float/double lane views:

```c
typedef union {
    unsigned char u8[16];
    uint32_t      u32[4];
    uint64_t      u64[2];
    float         f32[4];
    double        f64[2];
} vec128_t;
```

```c
extern vec128_t vadd_f64(vec128_t a, vec128_t b);

TEST(simd, adds_two_lanes) {
    regs_t r;
    vec128_t a = {.f64 = {1.0, 2.0}};
    vec128_t b = {.f64 = {3.0, 4.0}};
    vec128_t want = {.f64 = {4.0, 6.0}};
    ASM_VCALL2(&r, vadd_f64, a, b);
    ASSERT_VEC_EQ(&r, 0, &want);     // compare returned vec[0] to want
}
```

| Macro | Arguments |
|---|---|
| `ASM_VCALL1/2/3(&r, fn, v0[, v1[, v2]])` | 1–3 `vec128_t` args |
| `ASM_VCALLN(&r, fn, …)` | Any number of vectors; the 9th+ spill to the stack |

### Vector assertions

`ASSERT_VEC_EQ(&r, idx, expect_ptr)` compares the whole 128-bit lane `r.vec[idx]`
to `*expect_ptr`. For per-lane numeric checks (with optional ULP tolerance):

| Macro | Compares |
|---|---|
| `ASSERT_DEQ(actual, expected)` | one `double` lane, exact |
| `ASSERT_DNEAR(actual, expected, ulps)` | one `double` lane, within `ulps` |
| `ASSERT_FEQ(actual, expected)` | one `float` lane, exact |
| `ASSERT_FNEAR(actual, expected, ulps)` | one `float` lane, within `ulps` |

```c
ASSERT_DEQ(r.vec[0].f64[0], 4.0);
ASSERT_DNEAR(r.vec[0].f64[1], 6.0, 2);
```

### Wide vectors — AVX2 (256-bit)

For routines that operate on 256-bit `ymm` registers, `ASM_VCALL256n` marshals
`vec256_t` arguments (the 256-bit analog of `vec128_t`) into `ymm0..7` and
captures the whole `ymm` file into a caller-provided `vec256_t out[16]`
(`out[0]` is the return). It is **x86-64 + AVX2 only** and **self-skips** the
test (via `SKIP`) on a host without AVX2 — gate a direct call with
`asmtest_cpu_has_avx2()`:

```c
TEST(simd, avx2_add4d) {
    vec256_t a = {.f64 = {1, 2, 3, 4}};
    vec256_t b = {.f64 = {10, 20, 30, 40}};
    vec256_t out[16];
    ASM_VCALL256_2(out, vec_add4d, a, b);     // skips cleanly if no AVX2
    ASSERT_DEQ(out[0].f64[3], 44.0);          // the 4th lane is the upper 128 bits
    vec256_t want = {.f64 = {11, 22, 33, 44}};
    ASSERT_VEC256_EQ(out, 0, want.u8);        // full 32-byte compare
}
```

The lane scalars use the same `ASSERT_DEQ`/`FEQ` macros; `ASSERT_VEC256_EQ(out,
idx, expect)` is the 32-byte whole-register compare. Lane counts double versus
`vec128_t` — `f64[0..3]`, `f32[0..7]`, `u64[0..3]`.

> **AVX-512 (`zmm`), SVE, and the emulator.** The `asmtest_cpu_has_avx512f()`
> probe is in place, but native 512-bit capture, AArch64 SVE, and a Win64 wide
> path are not yet wired. The [emulator tier](emulator.md) exposes the YMM/ZMM
> registers but its bundled Unicorn does not *execute* AVX instructions
> (`UC_ERR_INSN_INVALID`), so wide-vector capture is **native-only** for now;
> the emulator path self-skips until Unicorn ships AVX execution.

## Mixed integer and FP arguments

A routine taking both integer and `double` arguments is handled by the FP capture
path, which fills the integer **and** FP argument registers. The
[API reference](api-reference.md#capture-functions) documents the underlying
`asm_call_capture_fp` / `asm_call_capture_vec` (and their `_n` stack-spilling
variants) for direct use.

The [emulator tier](emulator.md) also marshals doubles and vectors and captures
the XMM/NEON file, so you can inspect SIMD state mid-routine, not just at the ABI
boundary.
