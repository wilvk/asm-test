# API reference

A consolidated index of the public API in `asmtest.h` (and `asmtest_emu.h` for
the emulator tier). Each entry links to the guide that explains it in context.

## Definition & registration

| Macro | Purpose |
|---|---|
| `TEST(suite, name) { … }` | Define and auto-register a test |
| `SETUP(suite) { … }` | Per-test setup for a suite |
| `TEARDOWN(suite) { … }` | Per-test teardown for a suite |
| `SKIP(reason)` | Mark the current test skipped |
| `BENCH(suite, name) { … }` | Define and auto-register a benchmark ([Benchmarks](benchmarks.md)) |
| `BENCH_USE(x)` | Keep a pure-C result from being optimized away |

See [Writing tests](writing-tests.md).

## Calling routines (capture macros)

| Macro | Calls with |
|---|---|
| `ASM_CALL0`…`ASM_CALL6(&r, fn, …)` | 0–6 integer register args |
| `ASM_CALLN(&r, fn, …)` | Any number of integer args (overflow on the stack) |
| `ASM_SRET(&r, fn, &out, …)` | Large struct return via the hidden pointer |
| `ASM_FCALL1`…`ASM_FCALL3(&r, fn, …)` | 1–3 `double` args |
| `ASM_FCALLN(&r, fn, …)` | Any number of `double` args |
| `ASM_VCALL1`…`ASM_VCALL3(&r, fn, …)` | 1–3 `vec128_t` args |
| `ASM_VCALLN(&r, fn, …)` | Any number of `vec128_t` args |

See [ABI capture](abi-capture.md) and [Floating-point & SIMD](floating-point-simd.md).

## Assertions

### Value

| Macro | Passes when |
|---|---|
| `ASSERT_TRUE(x)` / `ASSERT_FALSE(x)` | `x` nonzero / zero |
| `ASSERT_EQ/NE/LT/LE/GT/GE(a, b)` | signed comparison |
| `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE(a, b)` | unsigned comparison (hex report) |
| `ASSERT_STREQ(a, b)` | `strcmp(a, b) == 0` |
| `ASSERT_MEM_EQ(ptr, expect, len)` | `len` bytes equal (hexdump diff) |

### Register & flags

| Macro | Checks |
|---|---|
| `ASSERT_ABI_PRESERVED(&r)` | callee-saved registers restored |
| `ASSERT_FLAG_SET(&r, FLG)` / `ASSERT_FLAG_CLEAR(&r, FLG)` | a flag bit (`CF`/`PF`/`ZF`/`SF`/`OF`) |
| `ASSERT_REG_EQ(&r, field, val)` | a named `regs_t` field (unsigned) |

### Floating-point & SIMD

| Macro | Checks |
|---|---|
| `ASSERT_FP_EQ(&r, expected)` | `r.fret` bit-equals `expected` |
| `ASSERT_FP_NEAR(&r, expected, ulps)` | `r.fret` within `ulps` |
| `ASSERT_VEC_EQ(&r, idx, expect_ptr)` | whole 128-bit lane `r.vec[idx]` |
| `ASSERT_DEQ/DNEAR(actual, expected[, ulps])` | one `double` lane |
| `ASSERT_FEQ/FNEAR(actual, expected[, ulps])` | one `float` lane |

### Property testing

| Macro | Purpose |
|---|---|
| `ASSERT_MATCHES_REF1/2/3(fn, ref, gen, n)` | fuzz `n` inputs, compare to a C model |

See [Property testing](property-testing.md).

(capture-functions)=
## Capture functions

```c
void asm_call_capture(regs_t *out, void *fn, const long *args);            // 6 slots
void asm_call_capture_args(regs_t *out, void *fn, const long *args, int nargs);
void asm_call_capture_fp(regs_t *out, void *fn, const long *iargs,
                         const double *fargs);                            // 6 + 8
void asm_call_capture_fp_n(regs_t *out, void *fn, const long *iargs,
                           const double *fargs, int nfargs);
void asm_call_capture_vec(regs_t *out, void *fn, const long *iargs,
                          const vec128_t *vargs);                         // 6 + 8
void asm_call_capture_vec_n(regs_t *out, void *fn, const long *iargs,
                            const vec128_t *vargs, int nvargs);
void asm_call_capture_sret(regs_t *out, void *fn, void *result,
                           const long *args, int nargs);
void asm_call_capture_bigstruct(regs_t *out, void *fn, const long *iargs,
                                int niargs, const void *sptr, size_t ssize);
```

:::{note}
The `_args`, `_fp_n`, `_vec_n`, `_sret`, and `_bigstruct` paths use the
frame-pointer register (`rbp` / `x29`) to build a variable frame, so that one
register is reported preserved but not independently verified by
`ASSERT_ABI_PRESERVED` — the other callee-saved registers still are.
:::

## Types

| Type | Meaning |
|---|---|
| `regs_t` | Captured register/flag snapshot (arch-specific; see [ABI capture](abi-capture.md#the-register-snapshot)) |
| `vec128_t` | 128-bit vector with byte/int/float/double lane views |
| `asmtest_rng_t` | Seedable splitmix64 RNG state |

## Property-test helpers

```c
typedef int  (*asmtest_gen_fn)(asmtest_rng_t *rng, long *args, int cap);
typedef long (*asmtest_ref1_fn)(long);
typedef long (*asmtest_ref2_fn)(long, long);
typedef long (*asmtest_ref3_fn)(long, long, long);

long asmtest_rng_long(asmtest_rng_t *rng);
long asmtest_rng_range(asmtest_rng_t *rng, long lo, long hi);
```

## Guard-page allocations

| Function | Guard |
|---|---|
| `asmtest_guarded_alloc(n)` / `asmtest_guarded_free(…)` | trailing page (overrun faults) |
| `asmtest_guarded_alloc_under(n)` / `asmtest_guarded_free_under(…)` | leading page (underrun faults) |

See [guard-page buffers](runner.md#guard-page-buffers).

## Versioning & environment

| Symbol | Meaning |
|---|---|
| `ASMTEST_VERSION` | version string, e.g. `"1.0.0"` |
| `ASMTEST_VERSION_NUM` | numeric version for `#if` compares |
| `asmtest_cycle_counter()` | inline cycle/tick counter used by `BENCH` |

| Environment variable | Effect |
|---|---|
| `ASMTEST_SEED` | seed for property-test RNG and `--shuffle` (decimal or `0x`-hex) |
| `ASMTEST_TIMEOUT` | default per-test timeout in seconds (same as `--timeout`) |

## Emulator API

The emulator tier lives in `asmtest_emu.h`. Its functions
(`emu_open`/`emu_call`/`emu_call_traced`/…, the per-guest `emu_arm64_*`,
`emu_riscv_*`, `emu_arm_*`, and `emu_call_win64`) and assertions
(`ASSERT_NO_FAULT`, `ASSERT_FAULT`, `ASSERT_FAULT_AT`, `ASSERT_EMU_REG_EQ`,
`ASSERT_EMU_FP_EQ`, `ASSERT_EMU_VEC_EQ`, `ASSERT_BLOCK_COVERED`,
`ASSERT_BLOCKS_AT_LEAST`) are documented on the [Emulator tier](emulator.md)
page.

## Binding ABI (multi-language)

The surface below is the **binding ABI** — the symbols a foreign-function
binding (Python, Rust, …) loads from the shared library and calls directly. It
is deliberately macro-free: every entry point is a real exported function over
pointers and arrays, so a code generator parsing the header sees the whole
contract. The `ASM_CALLn` / `ASSERT_*` conveniences are cpp macros layered on
top and are **not** part of this surface.

**Build the loadable artifacts** (see [Using asm-test in your
project](integration.md)):

```sh
make shared       # libasmtest.{so,dylib}      — runtime + capture trampoline
make shared-emu   # libasmtest_emu.{so,dylib}  — adds the emulator (-lunicorn)
make manifest     # asmtest_abi.json           — machine-readable struct layout
```

**Contract symbols.**

| Group | Symbols | Notes |
|---|---|---|
| Capture (array form) | `asm_call_capture`, `asm_call_capture_args`, `asm_call_capture_fp`/`_fp_n`, `asm_call_capture_vec`/`_vec_n`, `asm_call_capture_sret`, `asm_call_capture_bigstruct` | Every call path has a non-variadic array form a binding can target — no cpp expansion to emulate. |
| Verdict shims | `asmtest_check_abi`, `asmtest_check_flag` | Return `0`/nonzero + a reason string instead of `longjmp`-ing into the runner; for validating a capture across the FFI boundary without the C runner. |
| Opaque-handle accessors | `asmtest_regs_new`/`_free`, `asmtest_regs_ret`/`_flags`/`_fret`/`_vec_f32`/`_flag_set`, `asmtest_capture6`/`asmtest_capture_fp2`; emulator: `asmtest_emu_result_new`/`_free`/`_ok`/`_faulted`, `asmtest_emu_x86_reg` | For dynamic-FFI bindings (Node, Ruby, Lua, …) that can't mirror `regs_t` offsets: allocate a handle, call with scalar args, read fields by accessor — the universal FFI subset. |
| Guard buffers | `asmtest_guarded_alloc`/`_free`, `asmtest_guarded_alloc_under`/`_free_under` | Share a pointer to a guarded buffer with the routine under test. |
| RNG | `asmtest_rng_u64`, `asmtest_rng_long`, `asmtest_rng_range` | Deterministic splitmix64 source. |
| Emulator | `emu_open`/`emu_close`/`emu_map`/`emu_read`/`emu_write`, `emu_call`/`_fp`/`_vec`/`_traced`, `emu_call_win64`, and the per-guest `emu_arm64_*` / `emu_riscv_*` / `emu_arm_*` families | Opaque handle + value-struct result; faults surface as data, not crashes. |
| Layout | `regs_t`, `vec128_t`, `emu_*_regs_t`, `emu_result_t` (and per-guest result structs), `emu_trace_t` | Field offsets are published in `asmtest_abi.json` and pinned by `_Static_assert` in the headers. |
| Constants | `ASMTEST_SENTINEL_*`, the flag masks (`ASMTEST_CF`/`ZF`/…), `ASMTEST_VERSION`/`_NUM` | Real `#define`s; the manifest also emits the sentinel and flag values so a generator needn't re-read the header for them. |

**Layout manifest.** `asmtest_abi.json` (from `make manifest`) records, for the
active host arch, each struct's `size`/`align` and field `offset`/`size`, plus
the sentinels, flag masks, and version. Bindings consume it instead of
hand-transcribing offsets — a wrong offset silently validates garbage, so this
is the contract's correctness anchor. The matching `_Static_assert`s in
`asmtest.h` / `asmtest_emu.h` keep the header, the trampoline's hard-coded
stores, and the manifest from drifting apart.

The full rollout across languages is tracked in the multi-language bindings
plan (`docs/plans/multi-language-bindings-plan.md`).
