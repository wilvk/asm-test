# asm-test — Cross-system performance & feature benchmarking

An implementation plan for a **repeatable way to run both the performance
benchmarks and a feature benchmark on any system**, emit one normalized
per-system report, collect reports from many systems, and diff them into a
cross-system matrix. It turns the findings of
[cross-arch benchmarking analysis](../analysis/2026-07-11-cross-arch-benchmarking.md)
into buildable phases.

> Status legend: **planned** unless noted. This plan follows the house rules the
> tracing plans use — one normalized result shape every producer fills, static
> `available()`/`skip_reason()` self-skip with an absent capability treated as
> *data* not an error, and no untested claims. Update this file as phases land.

Siblings: the [analysis](../analysis/2026-07-11-cross-arch-benchmarking.md) it
derives from, the user guide [benchmarks](../../guides/benchmarks.md), and the
[trace parity matrix](../analysis/trace-parity-matrix.md) whose capability
matrices this plan instantiates *live*, per system.

---

## 1. What "run the benchmarks on different systems" means here

Two benchmark **dimensions**, both run by one runner and merged into one report:

- **Performance benchmark — how costly is the code?**
  - *Real time*: cycles / ticks per call on the host CPU, from the existing
    native `BENCH` tier (`--bench --bench-format=json`,
    [src/asmtest.c](../../../src/asmtest.c)). Host arch only.
  - *Deterministic work*: dynamic instruction / basic-block count per call, per
    guest ISA, from the emulated `EMU_BENCH` tier
    (`asmtest_trace_t.insns_total` / `blocks_*`,
    [include/asmtest_trace.h](../../../include/asmtest_trace.h)). All four guest
    ISAs, any host.
- **Feature benchmark — what does this system actually support?**
  A live capability sweep: probe every trace tier/backend, the emulator guest
  set, the disassembler/assembler, and each language binding's `available()`,
  recording `{available, skip_reason, fidelity}`. This is the
  [trace-parity matrices](../analysis/trace-parity-matrix.md) (backend × OS/arch,
  vendor/uarch, binding, packaging) **instantiated as a per-system report**
  instead of a hand-maintained table — the honest "what works *here*" answer.

Run on "different systems" means: one runner produces a self-describing JSON per
system (a laptop, each CI matrix leg, a Docker platform), and an aggregator
merges N of them into one comparison. Nothing about the runner is
system-specific — the *report* carries the system identity.

## 2. The normalized artifacts (the parity anchor)

Everything downstream (aggregator, CI, docs, bindings) reads these shapes, so
they land first.

### 2.1 System descriptor

Captured by the runner so every number is attributable:

```json
{ "os": "linux", "arch": "x86_64", "kernel": "6.17.0-35",
  "cpu": "AMD Ryzen 9 9950X", "uarch": "zen5", "vendor": "AMD",
  "cc": "gcc 13.2.0", "virtualized": false, "asmtest_version": "…",
  "timestamp": "…" }
```

`virtualized` is set when the runner detects emulation (e.g. `linux/arm64` under
qemu-user) — the flag the report uses to mark real-time numbers untrustworthy
there (see §6).

### 2.2 Performance result — `asmtest_bench_result_t`

The metric-shape from the [analysis §5.2](../analysis/2026-07-11-cross-arch-benchmarking.md),
one record per (routine, arch, metric):

```c
typedef struct {
    asmtest_arch_t arch;
    enum { BM_CYCLES, BM_INSNS, BM_BLOCKS, BM_MODEL_COST } kind;
    double         value;
    const char    *unit;       /* "cyc" | "ticks" | "insn" | "block" | "model-cyc" */
    bool           deterministic;
    bool           complete;   /* mirrors trace.truncated for count metrics       */
} asmtest_bench_result_t;
```

The `unit` field is what makes the cyc-vs-ticks-vs-insn separation structural: a
reporter groups and compares only within a unit, never across.

### 2.3 Feature report record

```json
{ "tier": "hwtrace", "backend": "amd_lbr", "available": true,
  "skip_reason": "", "fidelity": "exact-≤16", "scope": "host" }
```

One row per probed capability. `scope` distinguishes host-native tiers
(`host`) from the always-present emulator guests (`guest:arm64`, …).

### 2.4 The per-system report

`{ "system": <descriptor>, "performance": [<bench_result>…], "features": [<feature>…] }`
— a single JSON file, `bench-report-<os>-<arch>.json`. Its schema is a
documented deliverable (Phase 6).

## 3. Producers

### Phase 1 — Feature-benchmark probe *(smallest, ships first)*

A small C program `tools/asmfeatures.c` (mirroring
[scripts/gen-manifest.c](../../../scripts/gen-manifest.c)'s "emit JSON" role),
built as `build/asmfeatures`, that enumerates capabilities using **only existing
APIs** and prints the `features[]` array:

- Trace tiers via the cross-tier resolver
  `asmtest_trace_resolve(policy, out, cap)` / `asmtest_trace_choice_t`
  ([include/asmtest_trace_auto.h](../../../include/asmtest_trace_auto.h)) — the
  ordered, available cascade with fidelity.
- Hardware backends via `asmtest_hwtrace_available(backend)` +
  `asmtest_hwtrace_skip_reason(backend, …)`
  ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)) for
  `{INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP}`.
- DynamoRIO via `asmtest_dr_available()` + its skip reason
  ([include/asmtest_drtrace.h](../../../include/asmtest_drtrace.h)).
- Emulator guest set (always present) and `asmtest_disas_available()` /
  `asm_available()` for the Capstone/Keystone tiers.

Because an absent capability is reported as `available:false` + a reason, this
program **never fails** on a system that lacks a tier — the whole point of the
parity discipline. `make features` writes `asmtest_features.json` (the target
name parallels `make manifest`). Per-binding `available()` rows are gathered by
the orchestrator (§4) invoking each installed wrapper's one-line probe, so the
feature report spans the bindings axis too.

### Phase 2 — Performance producers

- *Real time*: **already exists** — `build/test_bench --bench
  --bench-format=json --bench-reps=N`. Phase 2 only wraps its output into
  `asmtest_bench_result_t` rows (unit `cyc`/`ticks`, `deterministic:false`).
- *Deterministic work*: implement the **`EMU_BENCH`** tier from the analysis
  (§4) — an emulated benchmark form that runs a guest routine once under
  `emu_<arch>_call_traced` with a zeroed `asmtest_trace_t` and emits
  `insns_total` / `blocks_total` / `blocks_len` as `BM_INSNS` / `BM_BLOCKS`
  rows. Add `emu-bench --format=json` and one worked routine authored for all
  four guest ISAs (Keystone strings). Deterministic ⇒ no calibration; a single
  run is the answer, and it can also assert bounds. `BM_MODEL_COST` (Capstone
  per-class weighting) is a follow-on, not required for the first cut.

## 4. The runner — `make bench-report`

An orchestrator (`scripts/bench-report.sh`, POSIX) that on the current system:

1. Builds `test_bench`, `emu-bench`, and `asmfeatures`.
2. Captures the **system descriptor** (`uname -mrs`, `/proc/cpuinfo` or
   `sysctl`, `cc --version`; sets `virtualized` from a qemu/`/proc` probe).
3. Runs the three producers, plus each **installed binding's** `available()`
   probe.
4. Merges into one `bench-report-<os>-<arch>.json` (§2.4), printing a
   human-readable summary too.

Reproduce a specific leg in a container:

- `make docker-bench` — runs `bench-report` in the CI image (the Linux x86-64
  leg), consistent with the existing `docker-*` targets.
- `make docker-bench DOCKER_PLATFORM=linux/arm64` — the arm64 leg under
  qemu-user. Real-time numbers are marked `virtualized:true` (untrustworthy;
  §6); the **emulated counts and the feature probe stay fully valid**, which is
  the argument for having both dimensions.

## 5. The aggregator — `scripts/bench-compare`

Ingests any number of `bench-report-*.json` and emits a cross-system comparison
(Markdown, optionally HTML), stdlib-only so it needs no extra dependency:

- **Performance matrix** — rows = `suite.name`, columns grouped by
  `(arch, unit)`; `cyc` and `ticks` in **separate columns** (never subtracted);
  per-host ratios where a within-host baseline is defined; deterministic `insn`
  counts compared directly across arches (their whole point).
- **Feature matrix** — rows = capability (`tier/backend`), columns = system;
  cell = ✓ / the `skip_reason`. This is the parity matrix, generated from real
  probes across the collected systems.
- Optional **regression gate**: compare one leg's report against a committed
  baseline for that same leg; fail on real-time median regression beyond a
  threshold (per-host, so it sidesteps the cross-unit problem) or on a feature
  that *regressed* from available→unavailable.

## 6. House rules & sharp edges

- **Never subtract across units.** The `unit` field is load-bearing; the
  aggregator groups by it and refuses cross-unit arithmetic.
- **Absent capability is data, not failure.** The feature probe and the runner
  exit 0 when a tier is unavailable, recording the `skip_reason` — mirroring
  every trace tier's `available()` discipline.
- **qemu can't time.** Under `linux/arm64` emulation real cycles are meaningless
  (consistent with the project's qemu-user ptrace/singlestep limitation); the
  runner sets `virtualized:true` and the aggregator visibly quarantines those
  real-time cells. Real ARM cycles come only from the real `ubuntu-24.04-arm`
  runner; the deterministic counts and feature probe run anywhere.
- **Reproducibility.** Pin `--bench-reps` for real-time runs; emulated counts
  are already deterministic.
- **No new heavy deps.** The probe is C over existing libraries; the aggregator
  is one script. Prefer the `docker-*` path per the repo's tooling rule.

## 7. CI wiring — Phase 5

Extend [.github/workflows/ci.yml](../../../.github/workflows/ci.yml): on each
matrix leg (`ubuntu-latest` x86-64, `ubuntu-24.04-arm` aarch64, `macos-latest`
arm64, and the nightly `macos-13` x86-64) run `make bench-report` and
`actions/upload-artifact` the per-leg JSON. A dependent **merge job** downloads
all leg artifacts, runs `scripts/bench-compare`, and publishes the combined
Markdown/HTML as an artifact (and, optionally, commits a dated snapshot under
`docs/internal/analysis/benchmarks/`). This upgrades today's *informational*
`make bench` step (which discards its numbers) into a captured, cross-system,
comparable record.

## 8. Documentation — Phase 6 *(required deliverable, not an afterthought)*

The plan is not done until these ship; the docs build is warnings-as-errors, so
each new page is added to the toctree and cross-links resolve.

1. **New user guide — `docs/guides/cross-system-benchmarking.md`.** How to run
   `make bench-report` on your machine and in Docker; the two dimensions
   (performance vs feature) and what each answers; how to compare systems with
   `scripts/bench-compare`; reading the output (cyc vs ticks vs insn; the
   feature grid); and the qemu/virtualized caveat. Linked from
   [docs/index.md](../../index.md) and the README Quick start (`make
   bench-report`).
2. **JSON schema reference.** Document the per-system report shape (§2) — system
   descriptor, `asmtest_bench_result_t`, feature record — as a reference page so
   external CI can ingest it. Parallels how `asmtest_abi.json` is documented.
3. **Extend [docs/guides/benchmarks.md](../../guides/benchmarks.md)** with a
   "Comparing systems & architectures" section that draws the cyc-vs-ticks line
   and points cross-ISA questions at the emulated/count tier and the new guide.
4. **Extend [docs/reference/ci.md](../../reference/ci.md)** with the
   `bench-report` job, its artifacts, and the merge job.
5. **Update [docs/reference/features.md](../../reference/features.md)** to note
   the feature matrix can be generated live per system by `make features`, not
   only hand-maintained.
6. **Update [trace-parity-matrix.md](../analysis/trace-parity-matrix.md)** with
   the benchmark-metric rows once `EMU_BENCH` lands, closing the loop with the
   analysis that motivated this plan.
7. **`make help`** entries for `bench-report`, `features`, `emu-bench`,
   `docker-bench`.

## 9. Phase summary

| Phase | Deliverable | Depends on | Value on its own |
|---|---|---|---|
| 0 | Result shapes + JSON schema (§2) | — | the parity anchor |
| 1 | Feature-benchmark probe (`make features`, `asmtest_features.json`) | 0 | live per-system capability report from existing APIs |
| 2 | Performance producers: wrap native `BENCH` JSON + implement `EMU_BENCH` | 0 | real-time + deterministic cross-ISA cost |
| 3 | `make bench-report` runner + `make docker-bench` + system descriptor | 1,2 | one merged per-system report, reproducible in Docker |
| 4 | `scripts/bench-compare` aggregator | 3 | cross-system performance + feature matrix |
| 5 | CI per-leg artifacts + merge job | 3,4 | captured, comparable cross-system record each push |
| 6 | Documentation (guide, schema, CI, features, parity, README, help) | 1–5 | the feature is usable & discoverable |

## 10. Verification

- `asmtest_features.json` validates against the schema; on the dev host (Zen 5
  Ryzen 9 9950X) it reports AMD LBR available, Intel PT skipping with a reason,
  DynamoRIO available, single-step available, all four emu guests present.
- `make bench-report` produces a valid merged report on the x86-64 and
  `linux/arm64` Docker legs; the arm leg marks real-time rows `virtualized` yet
  still carries emulated counts + feature rows.
- `scripts/bench-compare` merges ≥2 leg reports into a single matrix; cyc and
  ticks land in separate columns; `insn` counts compare across arches.
- `make docker-docs` builds clean with the new guide and schema pages in the
  toctree.
