# Examples

This page is a tour of what asm-test is *best at*, organised by use case. Every
example in the use-case tour below is a real, buildable suite that ships in the
repository — the snippets are lifted from the files under
[`examples/`](https://github.com/wilvk/asm-test/tree/main/examples), so you can
run each one as you read. (The later [by-audience](#by-audience) section adds a
few illustrative patterns too; those are marked.)

Each suite is the pair `examples/foo.s` (the routine under test, with an Intel
`foo.asm` alongside it) and `examples/test_foo.c` (the `TEST(...)` cases). The
Makefile discovers `examples/test_*.c` and builds one binary per suite under
`build/`, so a `make test` picks them all up — except the suites owned by other
targets (`make bench`, `make usecases`, the emulator/trace tiers, and the
intentional-failure demos). See [Writing tests](writing-tests.md) for the
discovery rules.

| Use case | Suite | Run it |
| --- | --- | --- |
| [Correctness, registers & ABI](#correctness-registers-and-abi) | `test_capture.c` + `flags.s` | `./build/test_capture` |
| [Differential / property testing](#differential-property-testing) | `test_refmatch.c` + `refmatch.s` | `./build/test_refmatch` |
| [Verifying branchless bit hacks](#verifying-branchless-bit-hacks) | `test_bittricks.c` + `bittricks.s` | `./build/test_bittricks` |
| [Routines that call back into C](#routines-that-call-back-into-c) | `test_callback.c` + `callback.s` | `./build/test_callback` |
| [Micro-benchmarking](#micro-benchmarking) | `test_bench.c` + `bench.s` | `./build/test_bench --bench` |
| [Robustness: hangs & crashes](#robustness-hangs-and-crashes) | `test_robust.c` + `robust.s` | `make demo-robust` |

---

## Correctness, registers and ABI

**Best for:** proving a routine returns the right value *and* obeys the calling
convention — that it restores callee-saved registers and leaves the flags it
claims to. This is the core thing other test setups can't see, because they only
observe a return value.

`ASM_CALLn` drives the routine through the real ABI and captures the full
register file and flags into a `regs_t`; you then assert on it. See
[ABI capture & registers](../guides/abi-capture.md) for the complete model.

```c
#include "asmtest.h"

extern long set_carry(void);
extern long clear_carry(void);
extern long sum_via_rbx(long a, long b);   /* uses a callee-saved scratch reg */

TEST(capture, return_value_captured) {
    regs_t r;
    ASM_CALL2(&r, sum_via_rbx, 20, 22);
    ASSERT_EQ(r.ret, 42);
}

TEST(capture, callee_saved_preserved) {
    regs_t r;
    ASM_CALL2(&r, sum_via_rbx, 1, 2);
    ASSERT_EQ(r.ret, 3);
    ASSERT_ABI_PRESERVED(&r);          /* rbx/rbp/r12–r15 restored on return */
}

TEST(capture, carry_flag_set) {
    regs_t r;
    ASM_CALL0(&r, set_carry);
    ASSERT_FLAG_SET(&r, CF);           /* also: PF, ZF, SF, OF */
}
```

The same suite shows **guard-page buffers** — `asmtest_guarded_alloc` places the
allocation against an unmapped page so a one-past-the-end write faults instead of
corrupting silently:

```c
TEST(guard, in_bounds_write_ok) {
    unsigned char *p = asmtest_guarded_alloc(8);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)i;       /* p[8] would SIGSEGV — caught, not silent */
    ASSERT_EQ(p[7], 7);
    asmtest_guarded_free(p, 8);
}
```

---

(differential-property-testing)=
## Differential / property testing

**Best for:** optimized routines where hand-picked inputs miss the bug. Write the
obvious-but-slow version in C as a *reference model*, write the fast version in
assembly, then let the framework fuzz thousands of random inputs and report the
first input where they disagree — with the seed, so the failure reproduces. This
is the framework's signature capability; see [Property testing](../guides/property-testing.md).

The routines under test use `cmov`/`csel` (branchy, conditional logic is where
subtle bugs hide). The C models are the plain, obvious version:

```c
#include "asmtest.h"

extern long imax(long, long);
extern long iclamp(long, long, long);

/* C reference models — the obvious, readable implementation. */
static long ref_imax(long a, long b) { return a > b ? a : b; }
static long ref_iclamp(long x, long lo, long hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* A generator draws one input tuple from the seeded RNG per trial. */
static int gen_pair(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, -1000, 1000);
    a[1] = asmtest_rng_range(rng, -1000, 1000);
    return 2;                          /* arity: imax takes 2 args */
}
static int gen_clamp(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    long lo = asmtest_rng_range(rng, -1000, 1000);
    long hi = asmtest_rng_range(rng, lo, 1000);    /* keep lo <= hi */
    a[0] = asmtest_rng_range(rng, -1500, 1500);    /* x may fall outside [lo,hi] */
    a[1] = lo;
    a[2] = hi;
    return 3;
}

TEST(refmatch, max_matches_model) {
    ASSERT_MATCHES_REF2(imax, ref_imax, gen_pair, 10000);    /* 10k random trials */
}
TEST(refmatch, clamp_matches_model) {
    ASSERT_MATCHES_REF3(iclamp, ref_iclamp, gen_clamp, 10000);
}
```

`ASSERT_MATCHES_REFn` matches the routine's arity (`REF1`/`REF2`/`REF3`). The
seed is fixed by default so a failure is reproducible, and overridable with
`ASMTEST_SEED` so CI can vary the inputs run to run.

The corresponding `refmatch.s` deliberately includes a buggy `imax_wrong` (it
keeps the *smaller* operand) used by the failure demo — fuzzing flags it on the
first input where `a != b`.

---

## Verifying branchless bit hacks

**Best for:** code that's correct on every example you'd think to write by hand
yet wrong at one edge — the all-zero / all-ones word, an exact power of two, the
top bit. This is the same property engine as above, pointed at a SWAR popcount,
round-up-to-power-of-two, and a byte-reversal, each fuzzed against an
*independent* naive bit-loop (not the same trick).

The pattern worth copying: **pin the known corners as fixed assertions, then fuzz
the whole domain** against a model.

```c
extern unsigned long next_pow2(unsigned long x);

/* Independent model: double until we reach/pass x. */
static long ref_next_pow2(long x) {
    unsigned long v = (unsigned long)x, p = 1;
    while (p < v) p <<= 1;
    return (long)p;
}
static int gen_pos(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, 1, 1L << 40);
    return 1;
}

TEST(bittricks, next_pow2_known_values) {
    ASSERT_EQ(next_pow2(2), 2);            /* exact power: must NOT round to 4 */
    ASSERT_EQ(next_pow2(3), 4);
    ASSERT_EQ(next_pow2((1UL << 20) + 1), 1UL << 21);
}
TEST(bittricks, next_pow2_matches_model) {
    ASSERT_MATCHES_REF1(next_pow2, ref_next_pow2, gen_pos, 20000);
}
```

You can also assert a *property* that holds regardless of the "right" answer —
e.g. byte-reversal is its own inverse:

```c
TEST(bittricks, reverse_byte_is_its_own_inverse) {
    for (unsigned b = 0; b < 256; b++)
        ASSERT_EQ(reverse_byte(reverse_byte(b)), b);
}
```

---

## Routines that call back into C

**Best for:** higher-order routines — a qsort-style comparator, a map/filter
over an array — where the assembly takes a **function pointer and calls back
into C** per element. This is a distinct ABI discipline worth testing on its
own: across each callback invocation the routine must keep its live state
(pointer, counter, callback, accumulator) in **callee-saved registers** and keep
the stack **16-byte aligned at every call site**.

`callback.s` ships `sum_map(arr, n, fn)` and `count_if(arr, n, pred)`; the test
just passes ordinary C function pointers and asserts on the result:

```c
#include "asmtest.h"

extern long sum_map(const long *arr, long n, long (*fn)(long));
extern long count_if(const long *arr, long n, long (*pred)(long));

static long dbl(long x) { return x * 2; }
static long is_even(long x) { return (x % 2) == 0; }

TEST(callback, sum_map_doubles) {
    long a[] = {1, 2, 3, 4, 5};
    ASSERT_EQ(sum_map(a, 5, dbl), 30); /* 2*(1+2+3+4+5) */
}

TEST(callback, count_if_even) {
    long a[] = {1, 2, 3, 4, 5, 6};
    ASSERT_EQ(count_if(a, 6, is_even), 3);
}

/* A callback that itself recurses back into another asm-callback routine — a
 * stress for stack alignment and callee-saved preservation across the boundary. */
static const long g_vals[] = {2, 4, 6, 8};
static long count_even_inner(long x) {
    return count_if(g_vals, 4, is_even) + (x & 1); /* 4 + (x&1) */
}

TEST(callback, nested_callback_preserves_abi) {
    long a[] = {1, 2}; /* sum of (4+1) + (4+0) = 9 */
    ASSERT_EQ(sum_map(a, 2, count_even_inner), 9);
}
```

Ships as [`examples/callback.s`](https://github.com/wilvk/asm-test/blob/main/examples/callback.s)
+ `test_callback.c`; run `./build/test_callback`.

---

## Micro-benchmarking

**Best for:** proving a routine is not just correct but actually *faster*. A
`BENCH(...)` body is one measured call; the runner auto-calibrates a repeat count
and reports min/median/mean cycles per call (`rdtsc` / `cntvct_el0`). See
[Benchmarks](../guides/benchmarks.md).

```c
#include "asmtest.h"

extern long add_signed(long a, long b);
extern long sum_to_n(long n);

/* Single instruction: cost is dominated by the call itself. */
BENCH(arith, add_signed) {
    add_signed(2, 3);
}

/* A counted loop — visibly costlier per call. */
BENCH(arith, sum_to_1000) {
    sum_to_n(1000);
}

/* Through the capture trampoline (real-ABI path); BENCH_USE keeps the
 * compiler from discarding the result. */
BENCH(arith, add_via_capture) {
    regs_t r;
    ASM_CALL2(&r, add_signed, 40, 2);
    BENCH_USE(r.ret);
}
```

Calls into the routine under test can't be optimized away, so no sink is needed
except for pure-C values you compute in the body — that's what `BENCH_USE`
guards. Run with:

```sh
./build/test_bench --bench           # or: make bench
./build/test_bench --bench --bench-reps=100000   # pin the repeat count
```

---

## Robustness: hangs and crashes

**Best for:** test suites where a buggy routine might loop forever or segfault and
you don't want it to take down the whole run. Each test runs in a forked child
with an `alarm()` timeout, so a hang becomes a reported *timeout* and a crash a
reported *failure* — and the next test still runs. See the
[test runner](../guides/runner.md) for the isolation model.

```c
#include "asmtest.h"

extern long spin_forever(void);
extern long crash_null(void);

TEST(robust, hang_is_reported_as_timeout) {
    regs_t r;
    ASM_CALL0(&r, spin_forever);    /* never returns; alarm() -> timeout failure */
    ASSERT_EQ((long)r.ret, 0);      /* unreachable */
}

TEST(robust, survives_the_timeout) {
    ASSERT_EQ(1 + 1, 2);            /* runs only because the hang was contained */
}

TEST(robust, crash_is_reported_as_failure) {
    regs_t r;
    ASM_CALL0(&r, crash_null);      /* SIGSEGV -> reported failure */
    ASSERT_EQ((long)r.ret, 0);      /* unreachable */
}

TEST(robust, survives_the_crash) {
    ASSERT_EQ(2 * 2, 4);           /* runs only because the crash was contained */
}
```

This suite is intentionally excluded from `make test` (it fails on purpose).
Drive it with a short timeout so the spin is caught quickly:

```sh
make demo-robust                   # fork-isolated, short timeout
./build/test_robust --timeout=2    # the survives_* tests prove the run continued
./build/test_robust --no-fork --timeout=2   # in-process model still catches the hang/SIGSEGV
```

---

## By audience

The use cases above combine differently depending on what you're building. Each
subsection below opens with a shipped suite, then gives **real-world patterns** —
concrete, named kernels these audiences write. Several ship as their own suites
(called out with a file reference and a run command); the rest are illustrative,
with the framework code exact and the `extern` routine under test left to you.

### SIMD / crypto / codec / DSP / math-kernel authors

**You need:** to trust hand-vectorised kernels lane by lane, and to prove the
fast path equals a scalar reference. `ASM_VCALLn` marshals 128-bit vectors into
the vector registers and captures the whole vector file; on x86-64 `ASM_VCALL256n`
does the same for 256-bit AVX2, and on AArch64 Linux `ASM_SVCALL_*` captures the
scalable SVE z/predicate file at the host's vector length — each self-skips a host
without the feature. See [Floating-point & SIMD](../guides/floating-point-simd.md).

```c
extern void vec_add4f(void);            /* vec128 vec_add4f(vec128 a, vec128 b) */

TEST(simd, adds_four_floats_lanewise) {
    regs_t r;
    vec128_t a = {.f32 = {1.0f, 2.0f, 3.0f, 4.0f}};
    vec128_t b = {.f32 = {10.0f, 20.0f, 30.0f, 40.0f}};
    ASM_VCALL2(&r, vec_add4f, a, b);
    ASSERT_FEQ(r.vec[0].f32[0], 11.0f);     /* return is r.vec[0] */
    ASSERT_FEQ(r.vec[0].f32[3], 44.0f);     /* the 4th lane needs all 128 bits */
    ASSERT_ABI_PRESERVED(&r);               /* callee-saved GP regs untouched */
}

#if defined(__x86_64__)
extern void vec_add4d(void);            /* vec256 vec_add4d(vec256, vec256), AVX2 */
TEST(simd, avx2_adds_four_doubles_256bit) {
    vec256_t a = {.f64 = {1.0, 2.0, 3.0, 4.0}};
    vec256_t b = {.f64 = {10.0, 20.0, 30.0, 40.0}};
    vec256_t out[16];
    ASM_VCALL256_2(out, vec_add4d, a, b);   /* self-skips without AVX2 */
    ASSERT_DEQ(out[0].f64[3], 44.0);
}
#elif defined(__aarch64__) && defined(__linux__)
extern void sve_addd(void);             /* svec sve_addd(svec a, svec b), SVE */
TEST(simd, sve_adds_doubles_at_any_vl) {
    svec_t a = {{0}}, b = {{0}};
    for (int i = 0; i < 32; i++) { a.f64[i] = i + 1; b.f64[i] = 10.0 * (i + 1); }
    svec_t z[32];
    spred_t p[16];
    ASM_SVCALL_2(z, p, sve_addd, a, b);     /* self-skips without SVE */
    unsigned long vl = asmtest_sve_vl();    /* VL in bytes; loop to vl/8 */
    for (unsigned long i = 0; i < vl / 8; i++)
        ASSERT_DEQ(z[0].f64[i], 11.0 * (i + 1));  /* VL-agnostic: any host VL */
}
#endif
```

Pair this with the [differential engine](#differential-property-testing): keep a
plain-C scalar kernel as the reference model and fuzz the vectorised routine
against it — the standard way to catch a tail-handling or saturation bug.

**Real-world patterns.**

*Codec / image — per-byte saturating add* (the pixel-clamp in a video or image
filter, `paddusb` / `uqadd`). The saturation edge (`0xFF + anything`) is the
classic bug; fuzz random 16-byte vectors against a clamping C model and check all
16 lanes:

```c
extern void qadd_u8x16(void);   /* vec128 qadd_u8x16(vec128 a, vec128 b), per-byte saturating */

TEST(codec, saturating_add_matches_model) {
    asmtest_rng_t rng = {0xC0DEC};
    for (int t = 0; t < 5000; t++) {
        vec128_t a, b, want;
        for (int i = 0; i < 16; i++) {
            a.u8[i] = (uint8_t)asmtest_rng_range(&rng, 0, 255);
            b.u8[i] = (uint8_t)asmtest_rng_range(&rng, 0, 255);
            unsigned s = (unsigned)a.u8[i] + b.u8[i];
            want.u8[i] = s > 0xFF ? 0xFF : (uint8_t)s;       /* C model: clamp at 0xFF */
        }
        regs_t r;
        ASM_VCALL2(&r, qadd_u8x16, a, b);
        ASSERT_VEC_EQ(&r, 0, want.u8);                       /* all 16 lanes, edge included */
    }
}
```

Ships as [`examples/qadd.s`](https://github.com/wilvk/asm-test/blob/main/examples/qadd.s)
(`paddusb` / `uqadd`) + `test_qadd.c`; run `./build/test_qadd`.

*DSP / fixed-point — Q15 multiply* (`(a*b + 0x4000) >> 15`, the multiply-accumulate
at the heart of an FIR filter). It's integer-domain, so the property engine drives
it directly — the rounding bias is exactly where these go wrong:

```c
extern long qmul_q15(long a, long b);
static long ref_qmul_q15(long a, long b) { return (a * b + 0x4000) >> 15; }
static int gen_q15(asmtest_rng_t *rng, long *v, int cap) {
    (void)cap;
    v[0] = asmtest_rng_range(rng, -32768, 32767);
    v[1] = asmtest_rng_range(rng, -32768, 32767);
    return 2;
}
TEST(dsp, qmul_q15_matches_model) {
    ASSERT_MATCHES_REF2(qmul_q15, ref_qmul_q15, gen_q15, 50000);
}
```

Ships as [`examples/qmul.s`](https://github.com/wilvk/asm-test/blob/main/examples/qmul.s)
+ `test_qmul.c`; run `./build/test_qmul`.

*Crypto — constant-time equality, proven branch-free* (illustrative — the
constant-time proof needs the emulator, and the routine under test is yours).
Correctness is the easy half; the property that matters is that **no basic block
depends on the secret**.
Because the emulator's coverage *unions* across runs, feed inputs that differ at
different positions into one trace — if the block set never grows, there is no
data-dependent branch:

```c
#include "asmtest_emu.h"
extern long ct_eq(const void *a, const void *b, long n);   /* 1 if equal, branch-free */

TEST(crypto, ct_eq_has_no_secret_dependent_branch) {
    uint64_t blocks[32];
    emu_trace_t tr = {0};
    tr.blocks = blocks; tr.blocks_cap = 32;
    /* Run ct_eq for equal / differ-at-first-byte / differ-at-last-byte, all into
     * the SAME trace (inputs preloaded into guest memory, pointers as args). */
    size_t baseline = 0;
    for (int variant = 0; variant < 3; variant++) {
        emu_result_t r;
        /* ...emu_map/emu_write the two buffers for this variant, then: */
        emu_call_traced(E, (void *)ct_eq, CODE_WINDOW, args, 3, 0, &r, &tr);
        if (variant == 0) baseline = tr.blocks_len;          /* the constant-time block set */
    }
    ASSERT_EQ(tr.blocks_len, baseline);     /* coverage didn't grow -> no branch on the data */
}
```

### Compiler / runtime / libc authors

**You need:** to validate hand-written primitives across ISAs and prove they
honour the ABI. The emulator tier runs *the same algorithm* as raw machine code
on four guest CPUs — x86-64, AArch64, RISC-V, ARM32 — regardless of the host, and
asserts they all agree (one algorithm, four ISAs). See [the emulator tier](../guides/emulator.md).

```c
#include "asmtest_emu.h"

extern long add3(long, long, long);     /* a + b + c, native x86-64 */
/* Raw `a+b+c; ret` for each guest (add x0,x0,x1 / add x0,x0,x2 / ret, etc.) */
static const unsigned char A64_ADD3[] = {0x00,0x00,0x01,0x8b, 0x00,0x00,0x02,0x8b, 0xc0,0x03,0x5f,0xd6};

TEST(emu_crossisa, add3_agrees_across_isas) {
    long args[] = {11, 22, 33};
    long want = 11 + 22 + 33;           /* the C reference */

    emu_arm64_t *e64 = emu_arm64_open();
    emu_arm64_result_t r64;
    ASSERT_TRUE(emu_arm64_call(e64, A64_ADD3, sizeof A64_ADD3, args, 3, 0, &r64));
    ASSERT_NO_FAULT(&r64);
    ASSERT_EQ(r64.regs.x[0], want);     /* AArch64 result in x0 */
    emu_arm64_close(e64);
    /* ...RISC-V (result in a0/x[10]) and ARM32 (r0) guests assert the same. */
}
```

For the ABI discipline itself, `ASSERT_ABI_PRESERVED` on a captured `regs_t` is
the primitive — see [Correctness, registers and ABI](#correctness-registers-and-abi).
Run the cross-ISA suite with `make usecases-emu` (needs libunicorn).

**Real-world patterns.**

*libc — a SIMD `strlen` that must not over-read* (illustrative — the routine
under test is yours). A vectorised `strlen` loads 16 bytes at a time; the
real-world bug is reading past the page that holds the string. Put the
terminating NUL as the **last byte before an unmapped page** in the emulator: a
correct routine stops there and runs clean, an over-reading one faults at the
exact boundary — the bug a guard malloc on real hardware only catches
probabilistically.

```c
#include "asmtest_emu.h"
extern long my_strlen(const char *s);

#define PAGE_BASE 0x00300000UL
#define PAGE_SIZE 0x1000UL

TEST(libc, strlen_stops_at_the_page_boundary) {
    char s[64] = "the quick brown fox";          /* NUL within the page */
    size_t off = PAGE_SIZE - sizeof s;           /* place it flush against the page end */
    emu_write(E, PAGE_BASE + off, s, sizeof s);

    emu_result_t r;
    long args[] = {(long)(PAGE_BASE + off)};
    ASSERT_TRUE(emu_call(E, (void *)my_strlen, CODE_WINDOW, args, 1, 0, &r));
    ASSERT_NO_FAULT(&r);                          /* a 16-byte over-read past the NUL would fault */
    ASSERT_EQ(r.regs.rax, 19);                    /* strlen("the quick brown fox") */
}
```

*Runtime — checked arithmetic* (`__builtin_add_overflow`, compiler-rt `__addvdi3`).
The whole point is the overflow flag, which only register/flag capture sees:

```c
extern long checked_add(long a, long b);         /* sum in rax, OF set on signed overflow */

TEST(runtime, add_sets_overflow_flag_at_the_limit) {
    regs_t r;
    ASM_CALL2(&r, checked_add, 0x7fffffffffffffffL, 1);   /* LONG_MAX + 1 */
    ASSERT_FLAG_SET(&r, OF);
    ASM_CALL2(&r, checked_add, 2, 3);
    ASSERT_FLAG_CLEAR(&r, OF);
    ASSERT_EQ(r.ret, 5);
}
```

Ships as [`examples/checked.s`](https://github.com/wilvk/asm-test/blob/main/examples/checked.s)
(`add` → OF / `adds` → V) + `test_checked.c`; run `./build/test_checked`.

*Compiler intrinsic — a `popcount` / `clz` lowering, validated across ISAs.* When
you hand-write the lowering for `__builtin_popcountll` per target, the cross-ISA
equivalence check above is exactly the proof you want: assemble the x86-64,
AArch64, and RISC-V forms and assert all agree with a scalar C model over fuzzed
words — see [Verifying branchless bit hacks](#verifying-branchless-bit-hacks) for
the `popcount64` suite that does this.

### Reverse engineers / security researchers

**You need:** to run an untrusted snippet without it touching your process, and to
turn an out-of-bounds access into a *precise, reported* fault rather than a crash
or silent garbage. The emulator maps exactly the memory you grant and pins a
fault to the exact byte and kind (READ vs WRITE):

```c
#include "asmtest_emu.h"
extern long sum_longs(const long *p, long n);   /* counted load loop */

#define PAGE_BASE 0x00300000UL
#define PAGE_SIZE 0x1000UL
#define PAGE_END  (PAGE_BASE + PAGE_SIZE)

static emu_t *E;
SETUP(emu_sandbox)    { E = emu_open(); emu_map(E, PAGE_BASE, PAGE_SIZE); }
TEARDOWN(emu_sandbox) { emu_close(E); E = NULL; }

TEST(emu_sandbox, over_read_faults_at_exact_page_boundary) {
    emu_result_t r;
    long args[] = {(long)PAGE_BASE, 512 + 1};        /* one long past the page */
    ASSERT_FALSE(emu_call(E, (void *)sum_longs, 64, args, 2, 0, &r));
    ASSERT_FAULT_AT(&r, EMU_FAULT_READ, PAGE_END);   /* exact kind + address */
}
```

The over-read sandbox above runs with `make usecases-emu`.

**Disassembly — turn recorded offsets into instructions.** Faults, traces, and
coverage are recorded as byte offsets from the routine entry; with
[Capstone](https://www.capstone-engine.org/) linked, the `*_disasm` helpers
annotate them with the instruction at that offset (and self-skip to bare offsets
without it). The x86-64 engine and Capstone decode bytes regardless of the host
ISA, so these hand-assembled examples run anywhere.

A read fault, described with the offending instruction:

```c
#include "asmtest_emu.h"

TEST(disas, fault_names_the_instruction) {
    static const uint8_t code[] = {0x48, 0x8b, 0x07, 0xc3};   /* mov rax, [rdi] ; ret */
    emu_result_t r;
    long args[] = {(long)0xdead0000UL};                       /* unmapped pointer */
    emu_call(E, code, sizeof code, args, 1, 0, &r);
    ASSERT_FAULT_AT(&r, EMU_FAULT_READ, 0xdead0000UL);

    char text[160];
    emu_fault_describe(&r, EMU_ARCH_X86_64, code, sizeof code, EMU_CODE_BASE,
                       text, sizeof text);
    /* with Capstone:
       "read fault accessing 0xdead0000: mov rax, qword ptr [rdi]  (@0x0)" */
    ASSERT_TRUE(strstr(text, "read fault") != NULL);
    if (emu_disas_available())
        ASSERT_TRUE(strstr(text, "mov") != NULL);             /* names the load */
}
```

Decoding one instruction at an offset, and an annotated coverage report:

```c
static const uint8_t code[] = {0x48, 0x31, 0xc0, 0xc3};       /* xor rax, rax ; ret */

char text[64];
size_t n = emu_disas(EMU_ARCH_X86_64, code, sizeof code, EMU_CODE_BASE, 0,
                     text, sizeof text);
//  text -> "xor rax, rax";  n == 3 (its byte length).  offset 3 -> "ret"

emu_trace_report_disasm(&tr, EMU_ARCH_X86_64, code, sizeof code, stdout);
//  coverage: 2 distinct blocks, ...
//    blocks:
//      0x0   xor rax, rax
//      0x3   ret
```

`emu_disas_available()` reports whether this build links Capstone (every helper
degrades to bare offsets when it doesn't, so the same call works either way), and
`emu_disas` is exposed through every language binding (`disas` / `disas_available`).
These live in the `test_emu` suite — run `make emu-test`; see the emulator's
[disassembly section](../guides/emulator.md#disassembly-in-diagnostics-capstone) for the
full helper set (`emu_disas`, `emu_fault_describe`, `emu_trace_disasm`,
`emu_trace_report_disasm`, `emu_coverage_uncovered_disasm`).

### Teaching / learning assembly

**You need:** a tight, test-driven feedback loop while learning. The
[quick start](quickstart.md) walks the minimal `square` routine; a richer
teaching example is a small **RPN interpreter written in assembly** — a stateful
routine where three things are worth proving, each mapping to a framework
feature:

```c
extern long vm_eval(const signed char *code, long n);
#define ADD ((signed char)-1)
#define MUL ((signed char)-3)

/* 1. correctness over representative programs */
TEST(vm, evaluates_a_nested_expression) {
    signed char prog[] = {3, 4, ADD, 5, MUL};   /* (3 + 4) * 5 */
    ASSERT_EQ(vm_eval(prog, 5), 35);
}

/* 2. ABI discipline across the interpreter loop */
TEST(vm, preserves_callee_saved_registers) {
    static const signed char prog[] = {3, 4, ADD, 5, MUL};
    regs_t r;
    ASM_CALL2(&r, vm_eval, prog, 5);
    ASSERT_EQ((long)r.ret, 35);
    ASSERT_ABI_PRESERVED(&r);                   /* live state restored on return */
}

/* 3. the cursor never runs off the end — guard page turns an over-read into a
   reported fault, so a clean pass is positive evidence it stops at code[n] */
TEST(vm, never_reads_past_the_program) {
    const signed char src[] = {6, 7, ADD};      /* 13 */
    size_t n = sizeof src;
    signed char *prog = asmtest_guarded_alloc(n);
    memcpy(prog, src, n);
    ASSERT_EQ(vm_eval(prog, (long)n), 13);
    asmtest_guarded_free(prog, n);
}
```

Learners get an immediate, specific report — file, line, expression, expected vs
actual — instead of a silent wrong answer, which is what makes the loop fast.

---

## Where next

- [Quick start](quickstart.md) — build a suite of your own from scratch.
- [Assertions](../guides/assertions.md) — the full comparison, string, memory, FP and SIMD set.
- [Floating-point & SIMD](../guides/floating-point-simd.md) — `ASM_FCALLn` / `ASM_VCALLn` examples.
- [The emulator tier](../guides/emulator.md) — run a routine inside a virtual CPU to read
  the full register file, catch precise faults, and measure branch coverage.
