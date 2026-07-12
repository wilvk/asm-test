# asm-test — Cross-system performance & feature benchmarking

An implementation plan for a **repeatable way to run both the performance
benchmarks and a feature benchmark on any system** — across Linux, macOS, and
Windows on x86-64 and AArch64 — emit one normalized per-system report, **persist
each box's results in the git repo**, and diff them into a cross-system matrix
(both live and over history). It turns the findings of
[cross-arch benchmarking analysis](../../analysis/2026-07-11-cross-arch-benchmarking.md)
into buildable phases.

> Status legend: **planned** unless noted. This plan follows the house rules the
> tracing plans use — one normalized result shape every producer fills, static
> `available()`/`skip_reason()` self-skip with an absent capability treated as
> *data* not an error, and no untested claims. Update this file as phases land.

> **Status (2026-07-11): landed.** Phases 0–7 are implemented and live-verified
> on the dev host (Zen 5, Linux x86-64): `tools/emu_bench.c` + `tools/asmfeatures.c`
> (+ `tools/asmbench_fixtures.h`), `scripts/bench-report.sh` /
> `scripts/bench-compare` / `scripts/bench-golden-check.py`, `mk/bench.mk`
> (`make features` / `emu-bench` / `bench-report` / `bench-record` / `bench-check` /
> `bench-compare` / `docker-bench`), the `benchmarks/` tree (golden + this box's
> record), the `benchmarks` + `benchmarks-compare` CI jobs, and the docs
> ([cross-system benchmarking guide](../../../guides/cross-system-benchmarking.md)).
> **Two deviations from the text below**, both deliberate: (1) the golden file is a
> single host/OS-independent `benchmarks/golden/emu-insns.json` (emu-bench emits
> every guest ISA in one run) rather than per-arch `insns-<arch>.json`; (2) the CI
> per-push matrix is the three Linux/macOS legs — the **windows-latest / macos-13
> legs and the nightly auto-commit of per-box records remain follow-ups** (the
> shell runner is portable to them, but those lanes need the emu deps installed and
> auto-commit needs `contents:write`, so they are not shipped untested).

Siblings: the [analysis](../../analysis/2026-07-11-cross-arch-benchmarking.md) it
derives from, the user guide [benchmarks](../../../guides/benchmarks.md), and the
[trace parity matrix](../../analysis/trace-parity-matrix.md) whose capability
matrices this plan instantiates *live*, per system.

---

## 1. What "run the benchmarks on different systems" means here

Two benchmark **dimensions**, both run by one runner and merged into one report:

- **Performance benchmark — how costly is the code?**
  - *Real time*: cycles / ticks per call on the host CPU, from the existing
    native `BENCH` tier (`--bench --bench-format=json`,
    [src/asmtest.c](../../../../src/asmtest.c)). Host arch only.
  - *Deterministic work*: dynamic instruction / basic-block count per call, per
    guest ISA, from the emulated `EMU_BENCH` tier
    (`asmtest_trace_t.insns_total` / `blocks_*`,
    [include/asmtest_trace.h](../../../../include/asmtest_trace.h)). All four guest
    ISAs, any host.
- **Feature benchmark — what does this system actually support?**
  A live capability sweep: probe every trace tier/backend, the emulator guest
  set, the disassembler/assembler, and each language binding's `available()`,
  recording `{available, skip_reason, fidelity}`. This is the
  [trace-parity matrices](../../analysis/trace-parity-matrix.md) (backend × OS/arch,
  vendor/uarch, binding, packaging) **instantiated as a per-system report**
  instead of a hand-maintained table — the honest "what works *here*" answer.

A **system** here is the full `(OS, arch, CPU/uarch)` tuple, not just the ISA —
asm-test runs on **Linux, macOS, and Windows** across x86-64 and AArch64, and the
OS is often the *dominant* axis. It decides which native trace tiers exist at all
(DynamoRIO is Linux-only; Intel PT / AMD LBR are Linux bare-metal; single-step
runs on Linux, macOS-Intel, and Windows-VEH; the emulator runs everywhere — see
[trace-parity Matrix 2](../../analysis/trace-parity-matrix.md)), and it moves
real-cycle numbers on identical silicon (scheduler, thermal, syscall cost). So
the feature benchmark is largely an *OS* story, the real-time performance
benchmark is an *(OS, CPU)* story, and only the deterministic counts are OS- and
host-independent.

Run on "different systems" means: one runner produces a self-describing JSON per
system (a laptop on any OS, each CI matrix leg, a Docker platform), and an
aggregator merges N of them into one comparison. Nothing about the runner is
system-specific — the *report* carries the system identity (§2.1), including its
OS.

## 2. The normalized artifacts (the parity anchor)

Everything downstream (aggregator, CI, docs, bindings) reads these shapes, so
they land first.

### 2.1 System descriptor

Captured by the runner so every number is attributable:

```json
{ "box_id": "amd-zen5-linux-x86_64-9f3c",
  "os": "linux", "os_version": "Ubuntu 24.04 / kernel 6.17.0-35",
  "arch": "x86_64", "cpu": "AMD Ryzen 9 9950X", "uarch": "zen5",
  "vendor": "AMD", "cc": "gcc 13.2.0", "virtualized": false,
  "asmtest_version": "…", "timestamp": "…" }
```

`box_id` (§6.1) is the stable per-machine key and **includes the OS**, so the same
silicon under a different OS is a distinct record — correct, because results
differ by OS. `os` ∈ {`linux`, `macos`, `windows`}. `virtualized` is set when the
runner detects emulation (e.g. `linux/arm64` under qemu-user) — the flag the
report uses to mark real-time numbers untrustworthy there (see §7). The
descriptor is captured **per-OS-portably** (§4.1), since `/proc/cpuinfo` is
Linux-only.

### 2.2 Performance result — `asmtest_bench_result_t`

The metric-shape from the [analysis §5.2](../../analysis/2026-07-11-cross-arch-benchmarking.md),
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
— a single JSON file, `bench-report-<os>-<arch>.json` (the OS is in the name, so a
machine's Linux and Windows reports never collide). Its schema is a documented
deliverable (Phase 7).

## 3. Producers

### Phase 1 — Feature-benchmark probe *(smallest, ships first)*

A small C program `tools/asmfeatures.c` (mirroring
[scripts/gen-manifest.c](../../../../scripts/gen-manifest.c)'s "emit JSON" role),
built as `build/asmfeatures`, that enumerates capabilities using **only existing
APIs** and prints the `features[]` array:

- Trace tiers via the cross-tier resolver
  `asmtest_trace_resolve(policy, out, cap)` / `asmtest_trace_choice_t`
  ([include/asmtest_trace_auto.h](../../../../include/asmtest_trace_auto.h)) — the
  ordered, available cascade with fidelity.
- Hardware backends via `asmtest_hwtrace_available(backend)` +
  `asmtest_hwtrace_skip_reason(backend, …)`
  ([include/asmtest_hwtrace.h](../../../../include/asmtest_hwtrace.h)) for
  `{INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP}`.
- DynamoRIO via `asmtest_dr_available()` + its skip reason
  ([include/asmtest_drtrace.h](../../../../include/asmtest_drtrace.h)).
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
  four guest ISAs (Keystone strings) — **plus the Win64 calling convention** via
  `emu_call_win64_traced`, so the cost of the Windows ABI is benchmarkable on any
  host/OS with no Windows machine (`insns-win64.json`). Deterministic ⇒ no
  calibration; a single run is the answer, and it can also assert bounds.
  `BM_MODEL_COST` (Capstone per-class weighting) is a follow-on, not required for
  the first cut.

## 4. The runner — `make bench-report`

An orchestrator (`scripts/bench-report.sh`, POSIX) that on the current system:

1. Builds `test_bench`, `emu-bench`, and `asmfeatures`.
2. Captures the **system descriptor** per-OS-portably (§4.1) and sets
   `virtualized` from a qemu/`/proc` probe.
3. Runs the three producers, plus each **installed binding's** `available()`
   probe.
4. Merges into one `bench-report-<os>-<arch>.json` (§2.4), printing a
   human-readable summary too.

Reproduce a specific leg in a container:

- `make docker-bench` — runs `bench-report` in the CI image (the Linux x86-64
  leg), consistent with the existing `docker-*` targets.
- `make docker-bench DOCKER_PLATFORM=linux/arm64` — the arm64 leg under
  qemu-user. Real-time numbers are marked `virtualized:true` (untrustworthy;
  §7); the **emulated counts and the feature probe stay fully valid**, which is
  the argument for having both dimensions.

### 4.1 OS-portable capture & orchestration

The runner targets all three OSes, so descriptor capture and the orchestrator
itself are OS-aware; the surface is small because only the probes and the shell
differ:

| Concern | Linux | macOS | Windows |
|---|---|---|---|
| CPU / uarch | `/proc/cpuinfo`, `lscpu` | `sysctl -n machdep.cpu.brand_string` | PowerShell `Get-CimInstance Win32_Processor` |
| OS version | `uname -sr`, `/etc/os-release` | `sw_vers` | `Get-CimInstance Win32_OperatingSystem` |
| Compiler | `cc --version` | `clang --version` | `cl` / `clang --version` |
| Orchestrator | `bench-report.sh` (POSIX) | `bench-report.sh` | Git-Bash `bench-report.sh` **or** a `bench-report.ps1` sibling |

The **producers are already portable**: the native `BENCH` counter is
`rdtsc`/`cntvct_el0` — an *arch* choice identical across Linux/macOS/Windows — and
the emulated counts never touch the host CPU. So the only OS-specific code is the
descriptor probe table above, kept in one place in the runner.

## 5. The aggregator — `scripts/bench-compare`

Ingests any number of `bench-report-*.json` and emits a cross-system comparison
(Markdown, optionally HTML), stdlib-only so it needs no extra dependency:

- **Performance matrix** — rows = `suite.name`, columns grouped by
  `(arch, unit)`; `cyc` and `ticks` in **separate columns** (never subtracted);
  per-host ratios where a within-host baseline is defined; deterministic `insn`
  counts compared directly across arches (their whole point).
- **Feature matrix** — rows = capability (`tier/backend`), columns = each
  `(OS, arch)` system; cell = ✓ / the `skip_reason`. This is trace-parity
  [Matrix 2](../../analysis/trace-parity-matrix.md) generated from real probes — the
  OS axis is where most of the variation lives.
- **Regression gate** (reads the persisted baselines of §6): the host/OS-
  independent **golden** instruction counts are checked exactly (a diff = an
  intended code change); per-box real-time medians get a soft drift alert, not an
  exact gate; a feature regressing available→absent warns.

## 6. Persisting results in the repo (per-box history & comparison) — Phase 5

A report is ephemeral unless stored. To track a box's numbers over time and
compare boxes, committed results live in-repo under a top-level `benchmarks/`
tree — **the benchmark database is the git history**. The two performance metrics
want *opposite* git treatment, which shapes the layout.

### 6.1 Box identity

A stable `box_id` keys a machine across runs, resolved by the runner as:

- an explicit `ASMTEST_BOX_ID` env / `--box=` flag (CI passes the runner label,
  e.g. `gh-ubuntu-latest`, `gh-windows-latest`); else
- a self-describing slug from the descriptor: `<vendor>-<uarch>-<os>-<arch>` + a
  short hash of the CPU-model string, e.g. `amd-zen5-linux-x86_64-9f3c`.

The slug **includes the OS**, so the same silicon under Linux vs Windows is two
box records — correct, because real-cycle numbers and the feature set both differ
by OS. The `box_id` is written into every descriptor (§2.1).

### 6.2 Two storage classes — the metrics persist differently

| Metric | Determinism | Repo role | Location | Gated? |
|---|---|---|---|---|
| Emulated `insn` / `block` counts | function of code — host- **and OS**-independent | **golden baseline**, one set for all systems | `benchmarks/golden/insns-<arch>.json`, `insns-win64.json` | ✅ every push, every OS leg — a diff = an intended code change, reviewed like a snapshot test |
| Feature report | function of the (OS, box) | **per-box capability record** | `benchmarks/boxes/<box_id>/features.json` | soft — a capability regressing available→absent warns |
| Real cycles / ticks | box- and OS-specific, noisy | **per-box history** (trend, not assertion) | `benchmarks/boxes/<box_id>/perf-history.jsonl` (append-only; one line per run, tagged commit + timestamp) | ❌ no exact gate; optional per-box median-drift alert |

The split is the whole point. The **deterministic counts become committed golden
files** — being host- and OS-independent, a *single* set is checked on *every* CI
leg (Linux, macOS, Windows) and any change surfaces as a reviewable diff to
`insns-<arch>.json`. That is exactly the ABI-manifest / `corpus.json` pattern the
repo already uses (`make manifest` / `make conformance`, CI-gated). The
**real-cycle numbers are per-box history** — persisted for longitudinal trend
(this machine + OS, across commits), never asserted exactly, because they move
with the OS scheduler, thermal state, and microarch.

### 6.3 How results get committed

- **Locally**: `make bench-record` runs `bench-report`, then writes the outputs
  into `benchmarks/` under the resolved `box_id`, staged for the contributor to
  commit — anyone can persist their box (and its OS) with a PR.
- **In CI** (§8): the merge job commits each leg's per-box record and any
  refreshed golden files on a **schedule** (nightly / post-merge on `main`), not
  on every PR, to keep history out of feature branches. Golden-file *drift* on a
  PR is a gate failure with a "regenerate: `make bench-record` + commit" message —
  the manifest/conformance ergonomics contributors already know.
- **Bounding growth**: `perf-history.jsonl` is line-oriented and compact; a
  retention step trims to the last N runs per box. (Alternative: an orphan
  `benchmarks` branch to keep `main` clean — noted as a switch if the tree grows,
  not the default, for discoverability.)

### 6.4 What comparison reads

`scripts/bench-compare` (§5) reads the committed tree as its corpus:

- **cross-system**: latest `perf-history` line + `features.json` per box → the
  `(OS, arch)` × capability grid and the per-unit performance matrix;
- **longitudinal**: a box's `perf-history.jsonl` across commits → a per-machine
  trend;
- **golden**: `benchmarks/golden/insns-<arch>.json` vs a fresh run → the exact,
  host/OS-independent regression check.

So the repo *is* the benchmark database: each box persists its identity, OS, and
history; the deterministic counts persist as gated golden files; and comparison
is a read over checked-in data, not a one-shot over transient CI logs.

## 7. House rules & sharp edges

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
- **OS-specific edges, isolated to the descriptor probe.** No `/proc` off Linux
  (macOS `sysctl`, Windows CIM); the orchestrator needs a Git-Bash or `.ps1`
  entry on Windows; macOS may need SIP/`sudo` for a native trace tier (but not
  for the counters or the emulator). None of these touch the producers — the
  cycle counter and emulated counts are OS-agnostic (§4.1).
- **No new heavy deps.** The probe is C over existing libraries; the aggregator
  is one script. Prefer the `docker-*` path per the repo's tooling rule.

## 8. CI wiring — Phase 6

Extend [.github/workflows/ci.yml](../../../../.github/workflows/ci.yml) to run `make
bench-report` on **every OS × arch leg the matrix already has** and
`actions/upload-artifact` the per-leg JSON:

| Leg | OS | Arch | Real cycles | Feature-set highlight |
|---|---|---|---|---|
| `ubuntu-latest` | Linux | x86-64 | ✅ `cyc` | full native tier set (DynamoRIO, single-step, +PT/LBR on bare metal) |
| `ubuntu-24.04-arm` | Linux | AArch64 | ✅ `ticks` | ptrace stepper (stream HW-pending) + emulator |
| `macos-latest` | macOS | AArch64 | ✅ `ticks` | emulator-only |
| `macos-13` (nightly) | macOS | x86-64 | ✅ `cyc` | single-step (macOS-Intel) + emulator |
| `windows-latest` | Windows | x86-64 | ✅ `cyc` | the existing "win64 (real Windows)" job ([ci.yml](../../../../.github/workflows/ci.yml) L534) — VEH single-step + emulator |

A dependent **merge job** downloads every leg artifact, runs
`scripts/bench-compare`, publishes the combined Markdown/HTML, and (§6) commits
the per-box records + refreshed golden files on the nightly schedule. This
upgrades today's *informational* one-arch `make bench` step — which discards its
numbers — into a captured, cross-OS, cross-arch, comparable record.

## 9. Documentation — Phase 7 *(required deliverable, not an afterthought)*

The plan is not done until these ship; the docs build is warnings-as-errors, so
each new page is added to the toctree and cross-links resolve.

1. **New user guide — `docs/guides/cross-system-benchmarking.md`.** How to run
   `make bench-report` on your machine (Linux/macOS/Windows) and in Docker; the
   two dimensions (performance vs feature) and what each answers; the **OS/arch
   coverage** and what differs per OS; how a contributor records their own box
   with `make bench-record`; how to compare systems with `scripts/bench-compare`;
   reading the output (cyc vs ticks vs insn; the feature grid); and the
   qemu/virtualized caveat. Linked from [docs/index.md](../../../index.md) and the
   README Quick start (`make bench-report`).
2. **JSON schema reference.** Document the per-system report shape (§2) — system
   descriptor (incl. `box_id`, `os`), `asmtest_bench_result_t`, feature record —
   as a reference page so external CI can ingest it. Parallels how
   `asmtest_abi.json` is documented.
3. **Document the `benchmarks/` results tree (§6)** — the golden-file vs
   per-box-history split, `box_id`, `make bench-record`, and the golden-file
   regeneration/gate workflow (mirrors the manifest/conformance docs), so
   contributors know how their committed numbers are used.
4. **Extend [docs/guides/benchmarks.md](../../../guides/benchmarks.md)** with a
   "Comparing systems & architectures" section that draws the cyc-vs-ticks line
   and points cross-ISA questions at the emulated/count tier and the new guide.
5. **Extend [docs/reference/ci.md](../../../reference/ci.md)** with the
   `bench-report` job across all five OS × arch legs, its artifacts, and the
   merge/commit job.
6. **Update [docs/reference/features.md](../../../reference/features.md)** to note
   the feature matrix can be generated live per system by `make features`, not
   only hand-maintained.
7. **Update [trace-parity-matrix.md](../../analysis/trace-parity-matrix.md)** with
   the benchmark-metric rows once `EMU_BENCH` lands, closing the loop with the
   analysis that motivated this plan.
8. **`make help`** entries for `bench-report`, `bench-record`, `features`,
   `emu-bench`, `docker-bench`.

## 10. Phase summary

| Phase | Deliverable | Depends on | Value on its own |
|---|---|---|---|
| 0 | Result shapes + JSON schema (§2) | — | the parity anchor |
| 1 | Feature-benchmark probe (`make features`, `asmtest_features.json`) | 0 | live per-system capability report from existing APIs |
| 2 | Performance producers: wrap native `BENCH` JSON + implement `EMU_BENCH` (+ Win64 guest) | 0 | real-time + deterministic cross-ISA/ABI cost |
| 3 | `make bench-report` runner (OS-portable) + `make docker-bench` + descriptor | 1,2 | one merged per-system report, reproducible in Docker |
| 4 | `scripts/bench-compare` aggregator | 3 | cross-system performance + feature matrix |
| 5 | Repo persistence: `benchmarks/` tree, `box_id`, golden files, `make bench-record` | 4 | per-box history + gated deterministic baselines |
| 6 | CI per-leg artifacts (Linux/macOS/Windows) + merge/commit job | 3,4,5 | captured, comparable cross-OS record; scheduled persistence |
| 7 | Documentation (guide, schema, persistence, CI, features, parity, README, help) | 1–6 | the feature is usable & discoverable |

## 11. Verification

- `asmtest_features.json` validates against the schema; on the dev host (Zen 5
  Ryzen 9 9950X, Linux) it reports AMD LBR available, Intel PT skipping with a
  reason, DynamoRIO available, single-step available, all four emu guests present.
- `make bench-report` produces a valid merged report on the x86-64 and
  `linux/arm64` Docker legs; the arm leg marks real-time rows `virtualized` yet
  still carries emulated counts + feature rows.
- **OS coverage**: the `windows-latest` leg reports VEH single-step available and
  DynamoRIO/PT/LBR skipping with reasons; `macos-latest` (arm64) reports
  emulator-only; each leg's report carries the right `os` + `box_id`.
- **Persistence**: `make bench-record` writes under `benchmarks/boxes/<box_id>/`
  and `benchmarks/golden/`; a re-run appends exactly one `perf-history.jsonl`
  line; the **golden `insns-<arch>.json` is byte-identical across all OS legs**
  (host/OS-independent), and a deliberate code change flips the golden diff and
  trips the gate.
- `scripts/bench-compare` merges ≥2 leg reports into a single matrix; cyc and
  ticks land in separate columns; `insn` counts compare across arches.
- `make docker-docs` builds clean with the new guide and schema pages in the
  toctree.
