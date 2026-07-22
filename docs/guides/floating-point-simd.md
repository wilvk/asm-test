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

### Wide vectors — SVE (scalable, AArch64 Linux)

AArch64's Scalable Vector Extension has an *implementation-defined* vector length
(VL) — 16 to 256 bytes, fixed by the silicon, not the ISA. A fixed-true-width
type is therefore impossible, so `svec_t` is a **256-byte container** (the
architectural VLmax) of which only the low `asmtest_sve_vl()` bytes are live
after a capture, and `spred_t` is the matching 32-byte predicate container
(PL = VL/8). Fill and compare only up to the runtime VL:

```c
extern void sve_addd(void); // svec sve_addd(svec a, svec b), SVE

TEST(simd, sve_add_doubles) {
    svec_t a = {{0}}, b = {{0}};
    for (int i = 0; i < 32; i++) {        // fill to VLmax so any VL is covered
        a.f64[i] = i + 1;
        b.f64[i] = 10.0 * (i + 1);
    }
    svec_t z[32];
    spred_t p[16];
    ASM_SVCALL_2(z, p, sve_addd, a, b);   // self-skips cleanly without SVE

    unsigned long vl = asmtest_sve_vl();  // vector length in BYTES (>= 16)
    for (unsigned long i = 0; i < vl / 8; i++)
        ASSERT_DEQ(z[0].f64[i], 11.0 * (i + 1));  // loop to vl/8, not a fixed count

    svec_t want = {{0}};
    for (unsigned long i = 0; i < vl / 8; i++)
        want.f64[i] = 11.0 * (i + 1);
    ASSERT_SVEC_EQ(z, 0, want.u8);        // compares exactly vl live bytes
}
```

- `ASM_SVCALL_1`/`ASM_SVCALL_2` marshal `svec_t` arguments into `z0..z7`, call the
  routine, and capture the whole `z` file into `svec_t z[32]` (`z[0]` is the
  return) plus the predicate file into `spred_t p[16]`. The predicate *arguments*
  are all-false; a routine that needs predicate arguments calls
  `asm_call_capture_sve` directly (`iargs`/`zargs`/`pargs`).
- `asmtest_cpu_has_sve()` gates a direct call; `asmtest_sve_vl()` returns the VL
  in bytes (0 where SVE is absent). `ASSERT_SVEC_EQ` / `ASSERT_SPRED_EQ` are
  **VL-aware** — they compare only the low VL (resp. VL/8) live bytes, never the
  untouched container tail — so the same test is correct at any VL.
- **Linux AArch64 only.** Apple silicon has no *non-streaming* SVE — plain SVE
  instructions `SIGILL` on macOS arm64 — so the probe returns 0 and the macros
  self-skip there, exactly as on x86-64.
- **Validated on real SVE silicon** — CI's `ubuntu-24.04-arm` leg is a
  Neoverse-N2 host (SVE2, VL=16 bytes), where this suite *executes* the `ptrue`
  / `fadd` rather than emulating it, and the job fails if it ever self-skips
  there. For the vector lengths no reachable hardware provides,
  `make docker-sve-sweep` runs the suite under qemu-user at VQ 1/3/8/16 →
  VL 16/48/128/256 bytes (including a non-power-of-two) to flush out
  VL-assumption bugs.

> **AVX-512 (`zmm`), SVE, and the emulator.** Native 512-bit capture is wired:
> `asm_call_capture_vec512` with the `ASM_VCALL512_*` macros and
> `ASSERT_VEC512_EQ`, gated at runtime by `asmtest_cpu_has_avx512f()` — on both
> System V and the Win64 tier (`asm_call_capture_vec512_win64`). AArch64 SVE is
> wired too — see the **Wide vectors — SVE** section above
> (`asm_call_capture_sve` / `ASM_SVCALL_*`, gated by `asmtest_cpu_has_sve()`).
> The [emulator tier](emulator.md) exposes the YMM/ZMM registers
> but its bundled Unicorn does not *execute* AVX/AVX-512 instructions
> (`UC_ERR_INSN_INVALID`), so wide-vector capture is **native-only** for now; the
> emulator path self-skips until Unicorn ships wide-vector execution.

## Mixed integer and FP arguments

A routine taking both integer and `double` arguments — the canonical
ptr+len+scalar shape — is called with `ASM_MIXCALL`, whose two parenthesized
groups marshal into the integer and FP register files respectively:

```c
extern double scale_buf(const double *buf, long n, double s);

regs_t r;
ASM_MIXCALL(&r, scale_buf, (buf, n), (0.5));
ASSERT_FP_EQ(&r, expected);
```

Each group needs at least one element, up to 6 integer and 8 double arguments —
the register files the FP capture path loads. The
[API reference](../reference/api-reference.md#capture-functions) documents the underlying
`asm_call_capture_fp` / `asm_call_capture_vec` (and their `_n` stack-spilling
variants) for direct use.

The [emulator tier](emulator.md) also marshals doubles and vectors and captures
the XMM/NEON file, so you can inspect SIMD state mid-routine, not just at the ABI
boundary.
