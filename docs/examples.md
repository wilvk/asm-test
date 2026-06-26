# Examples

This page is a tour of what asm-test is *best at*, organised by use case. Every
example here is a real, buildable suite that ships in the repository — the
snippets are lifted from the files under [`examples/`](https://github.com/wilvk/asm-test/tree/main/examples),
so you can run each one as you read.

Each suite is the pair `examples/foo.s` (the routine under test, with an Intel
`foo.asm` alongside it) and `examples/test_foo.c` (the `TEST(...)` cases). The
Makefile discovers `examples/test_*.c` and builds one binary per suite under
`build/`, so a `make test` picks them all up. See [Writing tests](writing-tests.md)
for the discovery rules.

| Use case | Suite | Run it |
| --- | --- | --- |
| [Correctness, registers & ABI](#correctness-registers-and-abi) | `test_capture.c` + `flags.s` | `./build/test_capture` |
| [Differential / property testing](#differential-property-testing) | `test_refmatch.c` + `refmatch.s` | `./build/test_refmatch` |
| [Verifying branchless bit hacks](#verifying-branchless-bit-hacks) | `test_bittricks.c` + `bittricks.s` | `./build/test_bittricks` |
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
[ABI capture & registers](abi-capture.md) for the complete model.

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

## Differential / property testing

**Best for:** optimized routines where hand-picked inputs miss the bug. Write the
obvious-but-slow version in C as a *reference model*, write the fast version in
assembly, then let the framework fuzz thousands of random inputs and report the
first input where they disagree — with the seed, so the failure reproduces. This
is the framework's signature capability; see [Property testing](property-testing.md).

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

## Micro-benchmarking

**Best for:** proving a routine is not just correct but actually *faster*. A
`BENCH(...)` body is one measured call; the runner auto-calibrates a repeat count
and reports min/median/mean cycles per call (`rdtsc` / `cntvct_el0`). See
[Benchmarks](benchmarks.md).

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
[test runner](runner.md) for the isolation model.

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

## Where next

- [Quick start](quickstart.md) — build a suite of your own from scratch.
- [Assertions](assertions.md) — the full comparison, string, memory, FP and SIMD set.
- [Floating-point & SIMD](floating-point-simd.md) — `ASM_FCALLn` / `ASM_VCALLn` examples.
- [The emulator tier](emulator.md) — run a routine inside a virtual CPU to read
  the full register file, catch precise faults, and measure branch coverage.
