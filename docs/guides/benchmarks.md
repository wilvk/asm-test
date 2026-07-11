# Benchmarks

Benchmark mode times a routine in **cycles per call**. A `BENCH` case looks like
a test, but its body is one measured iteration; the runner auto-calibrates a
repeat count and reports the distribution.

## Defining a benchmark

```c
#include "asmtest.h"

extern long add_signed(long a, long b);

BENCH(arith, add_signed) {
    add_signed(123, 456);     // the one measured operation
}
```

`BENCH(suite, name) { ... }` registers in a **separate list** from `TEST`, so
benchmarks never run during `make test`. They run only under `--bench`.

## Running

```sh
make bench                         # build and run all BENCH cases
./build/test_bench --bench         # same, directly
./build/test_bench --bench --list  # list benchmarks without running
./build/test_bench --bench --filter='*add*'
./build/test_bench --bench --bench-reps=1000   # pin the inner repeat count
```

For each benchmark the runner reports **min / median / mean cycles per call**.
The counter is `rdtsc` on x86-64 and `cntvct_el0` on AArch64, read by the inline
`asmtest_cycle_counter()`.

| Flag | Effect |
|---|---|
| `--bench` | Run benchmarks instead of tests |
| `--bench-reps=N` | Pin the inner repeat count (otherwise auto-calibrated) |
| `--bench-format=text\|json` | Output format — `text` (default) or machine-readable JSON |
| `--filter`, `--list` | Honored in bench mode too |

For CI ingestion, `--bench-format=json` emits one JSON object for the run —
`unit`, `rounds`, and a `benchmarks` array with `min` / `median` / `mean` /
`stddev` / `cv` / `reps` per case:

```sh
./build/test_bench --bench --bench-format=json > bench.json
```

## How calibration works

The runner doubles an inner repeat count until a timing round spans enough of the
counter to be resolvable (capped), then times several rounds and reports the
min/median/mean per call. `--bench-reps=N` fixes the count for reproducible
comparisons across runs or machines.

## Keeping results alive

A benchmark that computes a value the compiler can prove is unused may be
optimized away entirely. `BENCH_USE(x)` funnels a pure-C result through a
volatile sink so it survives:

```c
BENCH(math, isqrt_c_reference) {
    BENCH_USE(isqrt_reference(req));
}
```

Calls into the **routine under test** need no such help — an external assembly
symbol is opaque to the optimizer.

## Isolation

Each benchmark runs in-process under the same signal and timeout guard as a test,
so a crashing or hanging routine is reported as an error rather than taking the
process down.

:::{note}
On AArch64 the virtual timer (`cntvct_el0`) ticks coarser than a core cycle, so
results are reported as **ticks** rather than cycles. Compare like-for-like on
one machine; absolute cross-architecture numbers are not directly comparable.
:::

## Comparing systems & architectures

The `BENCH` tier above measures **real cycles on the host** — the honest answer
to "how fast on this machine," but not comparable across architectures (`cyc` ≠
`ticks`) or across machines. For a **host-independent** cross-architecture
metric — deterministic instructions/basic blocks per call for x86-64, AArch64,
RISC-V, ARM32, and the Win64 ABI — and to run both this real-cycle tier and a
live feature/trace-completeness sweep across systems and diff the results, see
[Cross-system benchmarking](cross-system-benchmarking.md) (`make bench-report`).
