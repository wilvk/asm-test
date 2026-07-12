# Cross-Architecture Benchmarking — Design Analysis

Analysis date: 2026-07-11

## Summary

"Benchmark different architectures" is really **two different questions**, and
asm-test already has most of the machinery for both — they just aren't wired
together as a benchmarking feature yet:

1. **How fast on real hardware?** (x86-64 cycles vs AArch64 ticks vs …). The
   native `BENCH` tier already measures this per host; what's missing is
   *orchestration* — running the same suite on each real-hardware CI leg and
   collecting/comparing the JSON. Little-to-no new C code.
2. **How much work does the algorithm cost, independent of the host?**
   (dynamic instruction count / basic-block count per ISA). The Unicorn
   emulator tier already *counts* this for all four guest ISAs on any host via
   `asmtest_trace_t.insns_total`. What's missing is a thin benchmark harness and
   reporter on top. Modest new code, high payoff.

The recommendation is to build **both tiers**, because they answer genuinely
different questions and neither subsumes the other. Tier A is a
CI/reporting task; Tier B is a small feature (`EMU_BENCH`) that turns the
existing trace counters into an architecture-agnostic efficiency benchmark.

---

## 1. What exists today

### 1.1 Native benchmark tier (`BENCH`) — real cycles, host arch only

Defined in [src/asmtest.c](../../../src/asmtest.c) (`BENCH_*`, `run_benchmarks`,
`bench_calibrate`, `bench_measure`) and documented in
[docs/guides/benchmarks.md](../../guides/benchmarks.md):

- `BENCH(suite, name) { ... }` registers in a separate list from `TEST`; runs
  only under `--bench`.
- The runner auto-calibrates an inner repeat count (doubles until a round spans
  `BENCH_TARGET` counter ticks, capped at `BENCH_REPS_CAP`), times
  `BENCH_ROUNDS` rounds, and reports **min / median / mean ± sd cycles per
  call**.
- The counter is the inline `asmtest_cycle_counter()` — `rdtsc` on x86-64,
  `cntvct_el0` on AArch64 — so the unit is `cyc` on x86-64 and `ticks` on
  AArch64 (`ASMTEST_BENCH_UNIT`, [src/asmtest.c](../../../src/asmtest.c) ~L1827).
- `--bench-format=json` already emits a machine-readable object
  (`unit`, `rounds`, and per-case `min/median/mean/stddev/cv/reps`) — the exact
  shape a cross-host comparison needs.

**The hard limit:** this measures the routine running natively on the host CPU.
It can only benchmark the host's ISA. And as the docs already warn, the units
are not commensurable across arches — `rdtsc` reference cycles ≠ `cntvct_el0`
virtual-timer ticks. So "run the same `BENCH` and diff the numbers across
arches" is **not** physically meaningful for absolute values; only *ratios
within one host* (routine A vs routine B on the same machine) compare cleanly.

### 1.2 Emulator tier — instruction/block counting, any host, four ISAs

[include/asmtest_emu.h](../../../include/asmtest_emu.h) +
[src/emu.c](../../../src/emu.c) run **x86-64, AArch64, RISC-V (RV64), and ARM32**
guests regardless of host arch, each via `emu_<arch>_call_traced`. The trace
sink is the engine-neutral `asmtest_trace_t`
([include/asmtest_trace.h](../../../include/asmtest_trace.h)):

```c
uint64_t insns_total;  /* instructions executed (counts past insns_cap)  */
uint64_t blocks_total; /* block entries; a loop counts each pass         */
size_t   blocks_len;   /* distinct basic blocks entered                  */
```

Two properties make this the natural cross-architecture benchmark substrate:

- **It's already free.** The `UC_HOOK_CODE` hook (`on_code`,
  [src/emu.c](../../../src/emu.c) L107) calls `trace_append_insn` on *every*
  executed instruction, and `trace_append_insn` "always bumps `insns_total`"
  even when the ordered `insns[]` buffer is `NULL` (`insns_cap == 0`). So a
  caller who wants only the count passes a zeroed trace with no buffers and reads
  `insns_total` — no allocation, no per-instruction storage.
- **It's deterministic and host-independent.** The count is a function of the
  guest code + inputs, not of the host CPU, its frequency scaling, or system
  noise. Re-running gives the identical number. That is exactly what a
  cross-ISA comparison wants: *how many x86-64 instructions vs AArch64 vs RISC-V
  vs ARM32 does this algorithm retire for the same input?*

Cross-ISA code today is supplied either as raw byte arrays (e.g. `A64_ADD3`,
`RV_ADD3` in [examples/test_emu.c](../../../examples/test_emu.c) L796/L925) or
assembled from text via the Keystone in-line assembler
(`asmtest_emu_call_asm6`, [DESIGN.md](../../../DESIGN.md) Phase 8). Capstone is
already linked for disassembly and can classify instructions
(`asmtest_disas`, `asmtest_disas_is_branch`, `asmtest_disas_is_call`,
[include/asmtest_trace.h](../../../include/asmtest_trace.h)).

---

## 2. The core obstacle

There is no single number that means "speed" across architectures:

| Metric | Cross-arch meaningful? | Available where |
|---|---|---|
| Wall-clock / cycles | Only on real hardware of *that* arch; not comparable between arches | Native `BENCH`, host arch only |
| `rdtsc` cyc vs `cntvct` ticks | **No** — different time bases | Native `BENCH` |
| Dynamic instruction count | **Yes** — deterministic, host-independent | Emulator (`insns_total`), all 4 ISAs |
| Basic-block / branch count | **Yes** | Emulator (`blocks_total`/`blocks_len`) |
| Weighted "cost model" cycles | Approximate, model-defined, comparable *by construction* | Derivable from emulator trace + Capstone |

So the design splits along that line: **Tier A** owns real-hardware speed
(and accepts it can only compare within a host), **Tier B** owns
architecture-agnostic algorithmic cost (and accepts it is not wall-clock).

---

## 3. Tier A — real-cycle cross-host benchmarking (orchestration)

**Goal:** answer "how does this routine perform on real x86-64 vs real ARM
hardware?" using the measurement that already exists.

**What's already there:** `make bench` runs today on three CI legs —
`ubuntu-latest` (x86-64), `ubuntu-24.04-arm` (aarch64), and `macos-latest`
(aarch64) — see [.github/workflows/ci.yml](../../../.github/workflows/ci.yml)
L66. But it runs as an *informational* step: the numbers scroll past in the log
and are discarded.

**What to add (small, mostly CI/tooling):**

1. **Capture, don't just print.** On each matrix leg run
   `./build/test_bench --bench --bench-format=json --bench-reps=N > bench-<os>-<arch>.json`
   and upload it as a CI artifact. Pin `--bench-reps` so runs are reproducible
   and comparable over time on the *same* leg.
2. **A comparison reporter.** A small script (Python/shell in
   [scripts/](../../../scripts/)) that ingests the per-leg JSON and emits a
   table keyed by `suite.name`, columns per `<arch,unit>`. Because units differ,
   it must render `cyc` and `ticks` in **separate columns** and never subtract
   across them — it can show per-host ratios (this routine vs a baseline routine
   on the same host) which *are* comparable.
3. **Regression gate (optional).** Compare a leg's JSON against a committed
   baseline for that leg and fail if median regresses beyond a threshold. This
   is per-host, so it sidesteps the cross-unit problem entirely.

**Cost:** near-zero C changes. The JSON already carries everything
(`unit`, per-case `median`/`cv`). This is a reporting + CI-artifact task.

**Honest limitation to document:** Tier A compares *machines*, not *ISAs* in the
abstract. `ubuntu-latest` and `ubuntu-24.04-arm` differ in microarchitecture,
frequency, and memory system, not just instruction set — so a difference is
"this specific x86 box vs this specific ARM box," which is what most people
actually want when they say "benchmark on ARM," but it must be labelled as such.

---

## 4. Tier B — architecture-agnostic emulated benchmarking (`EMU_BENCH`)

**Goal:** answer "how many instructions / blocks does this algorithm take on
each ISA?" — deterministic, host-independent, and available for all four guest
ISAs on any developer machine or CI leg (no ARM/RISC-V hardware required).

This is the tier that genuinely benchmarks *different architectures* rather than
different machines, and it's the one with real new value.

### 4.1 The measurement (already implemented)

Run the routine under `emu_<arch>_call_traced` with a zeroed
`asmtest_trace_t` (NULL buffers, caps 0) and read back:

- `insns_total` — dynamic instruction count (the primary metric),
- `blocks_total` — dynamic block entries (loop-weighted), and
- `blocks_len` — distinct blocks (static-ish path coverage).

No new emulator code is needed for the counts; `on_code`/`on_block` already feed
them. What's needed is the *harness and reporter* around it.

### 4.2 Proposed surface

A benchmark form parallel to `BENCH`, but emulated and per-guest. Sketch:

```c
/* One case per (routine, guest ISA). The body runs the guest routine once
 * under a traced emu_<arch>_call; the harness reports insns_total etc. */
EMU_BENCH(sort, bubble_x86,   ASMTEST_ARCH_X86_64) { run_bytes(BUBBLE_X86,  args); }
EMU_BENCH(sort, bubble_arm64, ASMTEST_ARCH_ARM64)  { run_bytes(BUBBLE_A64,  args); }
EMU_BENCH(sort, bubble_rv64,  ASMTEST_ARCH_RISCV64){ run_bytes(BUBBLE_RV64, args); }
EMU_BENCH(sort, bubble_arm32, ASMTEST_ARCH_ARM32)  { run_bytes(BUBBLE_A32,  args); }
```

Output (text and JSON, mirroring `--bench-format=json`):

```
# emu-bench — instructions per call (deterministic, host-independent)
suite.name           arch      insns   blocks(distinct)  blocks(total)
sort.bubble          x86_64      142            9              58
sort.bubble          arm64       128            9              54
sort.bubble          rv64        171           11              71
sort.bubble          arm32       160           10              66
```

Because the metric is deterministic, `EMU_BENCH` needs **no calibration, no
rounds, no min/median** — a single run is the answer. That makes it far simpler
than the native tier and cheap enough to run in `make test` (it can also gate:
"bubble_arm64 must stay ≤ N instructions").

### 4.3 Optional: a weighted cost model

Raw instruction count treats a divide and a move as equal. A first-order
refinement, using Capstone (already linked), classifies each executed
instruction and applies a per-class weight (e.g. mul/div heavier, branches with
a misprediction penalty, loads/stores with a memory weight). This yields an
approximate "model cycles" number that is comparable across ISAs *by
construction* (same model, same weights). It is a model, not silicon — but it's
a far better proxy for cross-ISA cost than a raw count, and it's honest about
being a model. This is a clean follow-on, not needed for the first slice.

### 4.4 Ergonomics problem to solve

Unlike the native tier (which copies host-native bytes), the non-x86 guests take
**raw machine code** per ISA. Authoring the "same algorithm" four times is the
real friction. Options, cheapest first:

- **Keystone assembly strings.** The in-line assembler already assembles per-ISA
  text at `EMU_CODE_BASE` (`asmtest_emu_call_asm6`). Write the routine once per
  ISA as an assembly string literal — readable, no external toolchain at test
  time. Best default.
- **Byte arrays** (as the existing emu tests do) — zero deps, but opaque.
- **Pre-built `.o` per arch** via cross-assemblers in a `docker-*` target,
  bytes extracted at build time — most faithful to "real toolchain output,"
  most build machinery. Defer.

---

## 5. Benchmarking as a parity feature

The two tiers above describe *how to measure*. But the project already has a
strong idiom for shipping a cross-cutting capability — the **trace-parity
discipline** ([trace-parity-matrix.md](trace-parity-matrix.md)): one result
shape every backend fills, capability *matrices* across
{OS × arch × uarch × language × packaging}, `available()`/`skip_reason()`
self-skip, a `truncated` completeness bit, an auto-resolution cascade
(`asmtest_trace_call_auto`), and an identical surface across all ten language
bindings. Benchmarking should be designed the same way — as a **first-class,
parity-shaped feature**, not a pair of one-off targets. Doing so also reveals
that most of the substrate already exists.

### 5.1 The bridge: the instruction-count metric already has trace parity

The single most useful realization: **`insns_total` is not emulator-specific.**
It lives in `asmtest_trace_t` ([include/asmtest_trace.h](../../../include/asmtest_trace.h)),
and *every* trace backend fills that same struct — the Unicorn emulator, but
also native DynamoRIO, Intel PT, AMD LBR, single-step, and the out-of-process
ptrace stepper (this is the entire premise of the trace-parity matrix: one
offset basis, one block partition, one shape, so a test swaps backends without
changing assertions). `trace_append_insn` bumps `insns_total` identically no
matter which backend calls it.

Consequences for benchmarking:

- The **dynamic instruction / block count** metric rides the *existing*
  trace-parity substrate directly. Cross-ISA counts come from the emulator
  guests (any host, four ISAs); host-arch counts come from *any native trace
  backend* on the real CPU — and they are **the same number in the same field**,
  because it's the same struct. That's metric-shape parity for free.
- So the "architecture-agnostic cost" tier is not a new subsystem bolted onto
  the emulator; it's a thin **reporter over `asmtest_trace_t`** that already has
  five backends and a resolution cascade behind it. `EMU_BENCH` (§4) is really
  "the emulator instantiation of a `TRACE_BENCH` metric" — the same reporter
  would accept a DynamoRIO or PT trace of a host-arch routine.

### 5.2 One result shape (metric parity)

Mirror the trace tier's single-shape rule with a small benchmark-result record
that **both** tiers fill, so a benchmark reads identically regardless of which
substrate produced it:

```c
typedef struct {
    asmtest_arch_t arch;      /* which ISA was measured                     */
    enum { BM_CYCLES, BM_INSNS, BM_BLOCKS, BM_MODEL_COST } kind;
    double         value;     /* cyc/ticks, or instruction/block count, or model cost */
    const char    *unit;      /* "cyc" | "ticks" | "insn" | "block" | "model-cyc" */
    bool           deterministic; /* true for counts/model; false for wall-clock */
    bool           complete;  /* mirrors trace.truncated for count metrics    */
} asmtest_bench_result_t;
```

This is the benchmarking analogue of "one `asmtest_trace_t` every backend
fills." A CI reporter, a regression gate, or a binding reads the same five
fields whether the number came from `rdtsc` on the host or from `insns_total` of
an ARM32 guest — and the `unit` field is what keeps the cyc-vs-ticks rule (§7)
enforceable rather than aspirational.

### 5.3 Capability matrix — metric × architecture × tier

The parity discipline says: state where each metric is available and
**self-skip with a reason** where it isn't.

| Metric | x86-64 host | AArch64 host | RISC-V / ARM32 | Source of number | Deterministic |
|---|---|---|---|---|---|
| Real cycles/ticks | ✓ `cyc` (native BENCH) | ✓ `ticks` (native BENCH) | ✗ *(no host)* — skip: "no native counter for guest arch" | `asmtest_cycle_counter()` | no |
| Dynamic instruction count | ✓ emu guest **or** native trace backend | ✓ emu guest **or** native trace backend | ✓ emu guest (any host) | `asmtest_trace_t.insns_total` | yes |
| Block / branch count | ✓ (same as above) | ✓ | ✓ | `blocks_total` / `blocks_len` | yes |
| Weighted model cost | ✓ (needs Capstone) | ✓ (Capstone) | ✓ (Capstone ≥ 5 for RV) | Capstone class → weight over the trace | yes (model) |

Two self-skip axes, exactly parallel to the trace tier's static + dynamic
signals:

- **Static:** "real cycles" self-skips for any arch that is not the host (there
  is no real CPU to count); the model-cost metric self-skips without Capstone
  (`asmtest_disas_available()` is already the probe). Each returns a reason
  string, like `asmtest_hwtrace_skip_reason`.
- **Dynamic:** the count metrics inherit `asmtest_trace_t.truncated` — a
  host-arch native-trace benchmark whose backend overflowed (e.g. AMD LBR past
  its window) is flagged incomplete exactly as a coverage trace would be, and
  can re-resolve to a ceiling-free backend before reporting.

### 5.4 Auto-resolution (the benchmark analogue of `trace_call_auto`)

The trace tier ships `asmtest_trace_call_auto` — arm the best available backend,
detect `truncated`, escalate. A benchmark of a given `(arch, routine)` resolves
the same way:

```
bench_resolve(arch, routine):
    if arch == host_arch and want wall-clock:
        return native BENCH        # real cyc/ticks — the only real-time answer
    # architecture-agnostic cost: ride the trace-parity cascade
    if arch in emulator guests:
        return emu_<arch>_call_traced → insns_total     # any host, deterministic
    if arch == host_arch:
        return asmtest_trace_call_auto → insns_total     # real CPU, native backend
    else: skip("arch unavailable: no host CPU and not an emulator guest")
```

The key parity property: **`NATIVE_ONLY` vs emulator-floor is the same boundary
here as for tracing.** A wall-clock benchmark must never silently fall back to
an emulated instruction count — they measure different things (time vs work),
just as native→emulator trace fallback changes execution fidelity. So the
resolver exposes the count and the cycle metrics as *distinct* requests, never
one silently standing in for the other. This is the benchmarking image of the
`ASMTEST_TRACE_NATIVE_ONLY` rule.

### 5.5 Binding parity

Tracing exposes an identical surface across all ten wrappers
([trace-parity-matrix.md](trace-parity-matrix.md) Matrix 5); benchmarking should
too. Because the emulated-count path is pure `asmtest_trace_t` + the existing
opaque-handle FFI (`asmtest_emu_trace_insns_total` is already a binding
accessor, [include/asmtest_trace.h](../../../include/asmtest_trace.h) L99), a
binding can report an instruction-count benchmark today with **no new FFI** — it
already has the accessor. A cross-ISA "instructions per call, per arch" table is
therefore reachable from Python/Rust/Go/… the moment the reporter exists. Real
cycles are inherently native, so the wall-clock tier stays a C-runner feature
(as it is now), consistent with how the bindings already defer the native trace
tiers to the C API.

### 5.6 Packaging axis

The trace matrix's fourth axis — what an installed package can actually reach —
applies unchanged:

| Metric | Reachable from an installed package |
|---|---|
| Dynamic instruction / block count (cross-ISA) | **every slot** — the emulator (`libasmtest_emu`) is bundled on all platform slots, and guest ISAs are slot-independent (Matrix 13) |
| Dynamic count (host arch, native trace) | follows the trace-tier packaging matrix (DynamoRIO on `linux-x86_64`; single-step/ptrace on every Linux slot) |
| Weighted model cost | every slot (Capstone is in the bundled superset) |
| Real cycles/ticks | host runner only (native, not a packaged artifact) |

So the architecture-agnostic benchmark (the cross-ISA one) is the *most*
portable metric of all — it ships everywhere the emulator does, which is
everywhere. That is a strong argument for leading with it.

### 5.7 Why this framing matters

Framing benchmarking as a parity feature converts it from "add a benchmark mode"
into "add a **metric layer** over two substrates that already carry the parity
discipline" — the cycle counter (host-arch real time) and the `asmtest_trace_t`
sink (deterministic, cross-ISA, five backends, an auto-resolver, and binding
accessors). Almost all of the hard parts — self-skip, completeness signalling,
cross-backend shape agreement, the bindings surface, the packaging story — are
inherited rather than rebuilt. The new code is the two reporters and the result
shape; the discipline is already in the tree.

## 6. What each tier answers

| Question | Tier | Metric | Needs real ARM/RISC-V HW? |
|---|---|---|---|
| Is my x86 routine faster than my C reference *on this box*? | A (exists) | cyc/call | no (x86 host) |
| Is the ARM build faster on a real ARM server? | A + CI | ticks/call | yes (arm CI leg — exists) |
| Does my AArch64 rewrite use fewer instructions than the x86 one? | **B (new)** | `insns_total` | **no** |
| Which ISA expresses this kernel most compactly? | **B (new)** | `insns_total`/`blocks` | **no** |
| Approx. relative cost across ISAs under one model | B + cost model | weighted | no |

Tier B is the only one that lets a developer on a single laptop compare four
architectures at all — which is what makes it the higher-leverage addition.

---

## 7. Recommended plan

> Turned into buildable phases — including a cross-system runner (`make
> bench-report`) spanning Linux/macOS/Windows, a live feature-benchmark probe,
> per-box result persistence in git (gated golden counts + real-cycle history),
> an aggregator, CI wiring, and a required documentation phase — in
> [cross-arch-benchmarking-plan.md](../archive/plans/cross-arch-benchmarking-plan.md).

1. **The result shape first (§5.2).** Land `asmtest_bench_result_t` and one
   reporter (text + JSON, mirroring `--bench-format=json`) that renders it. This
   is the parity anchor everything else fills, and it makes the cyc-vs-ticks and
   count-vs-time separations structural rather than conventional.
2. **Tier B, counts only (smallest viable slice).** Add `EMU_BENCH` (or, even
   smaller, a documented pattern + a helper that runs a traced emu call and
   emits an `asmtest_bench_result_t` from `insns_total`/`blocks_*`), and one
   worked example routine authored for all four guest ISAs (Keystone strings).
   Wire a `make emu-bench` target. Deterministic, so it can also assert bounds.
   Ships everywhere the emulator does (§5.6), so it's the highest-leverage start.
3. **Bindings surface (nearly free, §5.5).** Expose the count benchmark through
   the wrappers using the *existing* `asmtest_emu_trace_insns_total` accessor —
   a cross-ISA "instructions per call per arch" table from any binding, no new
   FFI. This is where the parity framing pays off immediately.
4. **Tier A capture + compare.** Change CI to save `--bench-format=json`
   per matrix leg as artifacts and add a `scripts/` comparison reporter
   (separate columns per unit; per-host ratios only). Optionally add a per-host
   regression gate against a committed baseline.
5. **Unify under one resolver (§5.4).** Add `bench_resolve`/an `--arch=` selector
   so a single entry point returns the right metric per `(arch, routine)` — real
   cycles for the host, `insns_total` (emulator or native trace) otherwise —
   with `NATIVE_ONLY`-style separation so time never silently becomes count.
6. **Tier B cost model (follow-on).** Add the Capstone-weighted per-class cost
   number (`BM_MODEL_COST`) to the reporter; document it explicitly as a model.
7. **Docs.** Extend [docs/guides/benchmarks.md](../../guides/benchmarks.md) with
   a "Comparing architectures" section that draws the cyc-vs-ticks line clearly
   and points cross-ISA questions at the emulated/count tier, and add a
   benchmark row-set to the parity discussion in
   [trace-parity-matrix.md](trace-parity-matrix.md) once the metric lands.

## 8. Sharp edges / caveats

- **Never subtract cyc from ticks.** The native cross-host path must keep the
  two time bases in separate columns; the docs already flag this and the
  reporter must enforce it.
- **Emulation ≠ cycles.** `insns_total` is instruction count, not time. A
  weighted model narrows the gap but is still a model; label it as such.
- **Same-algorithm authoring burden** across four ISAs is the main Tier-B cost;
  Keystone strings keep it manageable but it's real. Start with one routine.
- **Microarch, not ISA.** Tier A compares specific machines. That's usually what
  users want, but the report must say "x86 box vs ARM box," not "x86 vs ARM."
- **qemu-user cannot help Tier A.** Per project notes, the arm64 Docker lane
  under qemu-user can't do accurate cycle counting (or ptrace single-step), so
  real-cycle ARM numbers must come from the real `ubuntu-24.04-arm` runner, not
  local emulation. Tier B, by contrast, runs anywhere.

## 9. Bottom line

The framework is already ~80% of the way to cross-architecture benchmarking; the
pieces just live in two different tiers built for other purposes. The native
`BENCH` tier + the existing multi-arch CI matrix give **real-hardware speed per
host** with only reporting glue to add. The emulator's `insns_total`/`blocks_*`
counters give a **deterministic, host-independent, four-ISA algorithmic cost**
metric that only needs a thin `EMU_BENCH` harness on top.

Framed as a **parity feature** (§5), the second tier is even cheaper than it
first looks: the instruction-count metric already rides the `asmtest_trace_t`
substrate that five trace backends fill, a resolver already cascades over, the
bindings already expose an accessor for, and every package slot already carries.
So cross-arch benchmarking is best built not as a new subsystem but as a **metric
layer** — one result shape (`asmtest_bench_result_t`), two reporters — over the
cycle counter and the trace sink, inheriting their self-skip, completeness, and
binding-parity discipline. Build both tiers; lead with the emulated/count metric,
because it's the one that benchmarks *architectures* rather than *machines*, it
works on any developer's laptop, and it ships everywhere the emulator does.
