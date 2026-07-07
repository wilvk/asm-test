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
| `BENCH(suite, name) { … }` | Define and auto-register a benchmark ([Benchmarks](../guides/benchmarks.md)) |
| `BENCH_USE(x)` | Keep a pure-C result from being optimized away |

See [Writing tests](../getting-started/writing-tests.md).

## Calling routines (capture macros)

| Macro | Calls with |
|---|---|
| `ASM_CALL0`…`ASM_CALL6(&r, fn, …)` | 0–6 integer register args |
| `ASM_CALLN(&r, fn, …)` | Any number of integer args (overflow on the stack) |
| `ASM_SRET(&r, fn, &out, …)` | Large struct return via the hidden pointer |
| `ASM_FCALL1`…`ASM_FCALL3(&r, fn, …)` | 1–3 `double` args |
| `ASM_FCALLN(&r, fn, …)` | Any number of `double` args |
| `ASM_MIXCALL(&r, fn, (i…), (f…))` | Mixed integer-file + FP-file args; each parenthesized group needs ≥1 element (up to 6 int + 8 double) |
| `ASM_VCALL1`…`ASM_VCALL3(&r, fn, …)` | 1–3 `vec128_t` args |
| `ASM_VCALLN(&r, fn, …)` | Any number of `vec128_t` args |
| `ASM_VCALL256_1`/`_2(out, fn, …)` | 1–2 `vec256_t` (AVX2 ymm) args; **self-skips** without AVX2. `out` is a `vec256_t[16]` |
| `ASM_VCALL512_1`/`_2(out, fn, …)` | 1–2 `vec512_t` (AVX-512 zmm) args; **self-skips** without AVX-512F. `out` is a `vec512_t[32]` |
| `ASM_CALL_WIN64_0`…`_6` / `_N(&r, fn, …)` | Microsoft x64 convention (args in `rcx/rdx/r8/r9`); needs `ASMTEST_ABI_WIN64` |

See [ABI capture](../guides/abi-capture.md) and [Floating-point & SIMD](../guides/floating-point-simd.md).

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
| `ASSERT_VEC256_EQ(vec, idx, expect_ptr)` | whole 256-bit ymm lane (AVX2 capture) |
| `ASSERT_VEC512_EQ(vec, idx, expect_ptr)` | whole 512-bit zmm lane (AVX-512 capture) |
| `ASSERT_DEQ/DNEAR(actual, expected[, ulps])` | one `double` lane |
| `ASSERT_FEQ/FNEAR(actual, expected[, ulps])` | one `float` lane |

### Property testing

| Macro | Purpose |
|---|---|
| `ASSERT_MATCHES_REF1/2/3(fn, ref, gen, n)` | fuzz `n` integer inputs, compare to a C model; a failing input is shrunk toward boundary values before reporting |
| `ASSERT_MATCHES_FREF1/2/3(fn, ref, gen, n, ulps)` | fuzz `n` `double` inputs through the FP register file, judged by ULP distance (NaN matches only NaN) |

See [Property testing](../guides/property-testing.md).

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

// AVX2 / AVX-512 vector capture (x86-64; gate on the CPU probes below).
void asm_call_capture_vec256(vec256_t *vec, void *fn, const long *iargs,
                             const vec256_t *vargs);                        // ymm
void asm_call_capture_vec512(vec512_t *vec, void *fn, const long *iargs,
                             const vec512_t *vargs);                        // zmm
int  asmtest_cpu_has_avx2(void);      // gate for the _vec256 / ASM_VCALL256 path
int  asmtest_cpu_has_avx512f(void);   // gate for the _vec512 / ASM_VCALL512 path

// Microsoft x64 ("Win64") capture (needs ASMTEST_ABI_WIN64; args are 64-bit).
void asm_call_capture_win64(regs_t *out, void *fn, const long long *args);  // 6
void asm_call_capture_args_win64(regs_t *out, void *fn, const long long *args,
                                 int nargs);
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
| `regs_t` | Captured register/flag snapshot (arch-specific; see [ABI capture](../guides/abi-capture.md#the-register-snapshot)) |
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

See [guard-page buffers](../guides/runner.md#guard-page-buffers).

## Versioning & environment

| Symbol | Meaning |
|---|---|
| `ASMTEST_VERSION` | version string, e.g. `"1.1.0"` |
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
`ASSERT_BLOCKS_AT_LEAST`) are documented on the [Emulator tier](../guides/emulator.md)
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
make shared        # libasmtest.{so,dylib}          — runtime + capture trampoline
make shared-emu    # libasmtest_emu.{so,dylib}      — full superset: emulator (-lunicorn)
                   #   + in-line assembler (Keystone) + disassembler (Capstone)
make manifest      # asmtest_abi.json               — machine-readable struct layout
```

The fuzzing/mutation and guard entry points are part of the emulator tier, and
the in-line assembler and **disassembler** are folded into the same superset
`libasmtest_emu` — so all three tiers come from one library. (The
`asm_available()` / `disas_available()` probes still self-skip gracefully if a
consumer points at an older, leaner lib that lacks them.)

**Contract symbols.**

| Group | Symbols | Notes |
|---|---|---|
| Capture (array form) | `asm_call_capture`, `asm_call_capture_args`, `asm_call_capture_fp`/`_fp_n`, `asm_call_capture_vec`/`_vec_n`, `asm_call_capture_vec256` (AVX2), `asm_call_capture_sret`, `asm_call_capture_bigstruct` | Every call path has a non-variadic array form a binding can target — no cpp expansion to emulate. `_vec256` captures the ymm file; gate on `asmtest_cpu_has_avx2`. |
| Verdict shims | `asmtest_check_abi`, `asmtest_check_flag` | Return `0`/nonzero + a reason string instead of `longjmp`-ing into the runner; for validating a capture across the FFI boundary without the C runner. |
| Opaque-handle accessors | `asmtest_regs_new`/`_free`, `asmtest_regs_ret`/`_flags`/`_fret`/`_vec_f32`/`_flag_set`, `asmtest_capture6`/`asmtest_capture_fp2`/`asmtest_capture_vec_f32`; emulator: `asmtest_emu_result_new`/`_free`/`_ok`/`_faulted`/`_fault_addr`/`_fault_kind`, `asmtest_emu_x86_reg` (GP + `rip`/`rflags`), `asmtest_emu_x86_xmm_f64`/`_f32` | For dynamic-FFI bindings (Node, Ruby, Lua, …) that can't mirror `regs_t` offsets: allocate a handle, call with scalar args, read fields by accessor — the universal FFI subset. |
| Scalar-arg emu wrappers | `asmtest_emu_call2`, `asmtest_emu_call6` (≤6 int args), `asmtest_emu_call_fp2`, `asmtest_emu_call_vec_f32`, `asmtest_emu_call_win64_6`, `asmtest_emu_call6_traced` | Drive the emulator over a 64-byte code window with scalar args — FP, vector, Win64, and traced runs — without marshalling C argument arrays. |
| Cross-arch emu accessors | `asmtest_emu_{arm64,riscv,arm}_result_new`/`_free`, `asmtest_emu_{arm64,riscv,arm}_reg` (register by name), `asmtest_emu_arm64_vec_f64`/`_f32`, `asmtest_emu_riscv_f_f64`, `asmtest_emu_arm_q_f64`/`_f32` | Read a non-x86 guest's per-arch result struct without mirroring its layout; the shared `asmtest_emu_result_*` fault/ok accessors apply to every guest result. |
| Trace handle | `asmtest_emu_trace_new`/`_free`, `asmtest_emu_trace_covered`, `_insns_total`/`_blocks_len`/`_blocks_total`/`_truncated`/`_block_at` | Opaque wrapper over `emu_trace_t` + its buffers, so a dynamic-FFI binding records execution trace / basic-block coverage. |
| Call descent (`asmtest_ptrace.h`) | `asmtest_descent_new`/`_free`, `_set_max_depth`/`_set_insn_budget`/`_set_watchdog_ms`, `_allow_region`/`_deny_region`, `_set_resolver`/`_set_denylist`/`_use_default_denylist`; readers `_edges_len`/`_edge_site`/`_edge_target`/`_edge_depth`, `_frames_len`/`_frame_base`/`_frame_len`/`_frame_depth`/`_frame_parent`/`_frame_insn_count`/`_frame_insn_at`/`_frame_block_count`/`_frame_block_at`, `_truncated`/`_depth_capped`; entry points `asmtest_ptrace_trace_call_ex`/`_trace_attached_ex`/`_trace_attached_versioned_ex` | Opaque descent handle: configure the four-level descent policy in, read edges + nested per-callee frames out through one-scalar-per-call accessors (address getters return `uint64`, so bindings must keep them 64-bit — `BigInt`/boxed `uint64` cdata/unsigned mask, not a lossy `Number`). The `_ex` entry points thread the handle through the ptrace loops. See [native-tracing.md](../guides/tracing/native-tracing.md) ("Call descent levels"). |
| Mid-execution guards | `emu_watch_writes`/`emu_watch_clear`, `emu_guard_reg`/`emu_guard_reg_clear`; opaque handles `asmtest_emu_watch_new`/`_free`/`_violated`/`_addr`/`_size`/`_rip_off`, `asmtest_emu_reg_guard_new`/`_free`/`_violated`/`_got`/`_rip_off` | Arm a write-watchpoint or block-entry register invariant on the handle, run a call, then read the recorded violation by accessor (x86-64 guest, Track F). |
| Coverage-guided fuzzing / mutation | `emu_fuzz_cover1`, `emu_mutation_test1`; opaque stats `asmtest_emu_fuzz_stat_new`/`_free`/`_blocks_reached`/`_corpus_len`/`_iterations`, `asmtest_emu_mutation_stat_new`/`_free`/`_mutants`/`_killed`/`_survived` | Run a one-int-arg routine's coverage-guided input search or bit-flip mutation set inside the emulator; read the result counts by accessor (Track E). |
| Disassembly | `emu_disas`, `emu_disas_available` | Decode the one instruction at a code offset into text (Capstone). Writes into a caller buffer — no opaque handle — and self-skips to empty when absent. Carried by the superset `libasmtest_emu` (Track C). |
| Guard buffers | `asmtest_guarded_alloc`/`_free`, `asmtest_guarded_alloc_under`/`_free_under` | Share a pointer to a guarded buffer with the routine under test. |
| RNG | `asmtest_rng_u64`, `asmtest_rng_long`, `asmtest_rng_range` | Deterministic splitmix64 source. |
| Emulator | `emu_open`/`emu_close`/`emu_map`/`emu_read`/`emu_write`, `emu_call`/`_fp`/`_vec`/`_traced`, `emu_call_win64`, `emu_snapshot`/`emu_restore`/`emu_snapshot_free`, and the per-guest `emu_arm64_*` / `emu_riscv_*` / `emu_arm_*` families | Opaque handle + value-struct result; faults surface as data, not crashes. Snapshot/restore captures registers + all mapped regions so sweeps run history-independent. |
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
