# Cross-system benchmarking

`make bench-report` runs **two kinds of benchmark on your system** and merges
them, with a system descriptor, into one normalized JSON report. Collect reports
from several systems — your machines, the CI legs — and `scripts/bench-compare`
diffs them into a cross-system matrix. This is how asm-test answers "how does
this perform, and what works, across architectures and operating systems?"

The [runner tier](benchmarks.md) (`--bench`) measures one thing — real cycles on
the host. This guide is the layer on top that adds a **host-independent**
performance metric and a **feature/trace-completeness** sweep, and persists both
so systems can be compared over time.

## The two dimensions

| Dimension | What it measures | Comparable across… | Producer |
|---|---|---|---|
| **Performance — real time** | cycles / ticks per call on the host CPU | one host only (cyc ≠ ticks) | native `BENCH` (`test_bench`) |
| **Performance — work** | deterministic instructions / basic blocks per call, per guest ISA | **any host, any OS** | `emu-bench` |
| **Feature** | which trace tiers/backends work here, and whether a trace is **complete** | systems (it's the point) | `asmfeatures` |
| **Feature — capture depth** | how many instructions each box's **hardware** backend captures for a routine, vs the deterministic truth | systems (Zen vs Intel vs Apple) | `asmfeatures` (`native-hw` ladder) |

The deterministic **instruction count** is the cross-architecture performance
metric: because it is a function of the guest code alone, an AArch64 count and an
x86-64 count are directly comparable, and the number is identical on every host.
The **real-cycle** numbers answer "how fast on *this* box" and must never be
compared across architectures (or across the `cyc`/`ticks` unit line).

## Running it

```sh
make bench-report        # write build/bench-report-<os>-<arch>.json (+ a summary)
make bench-record        # the same, and persist into the benchmarks/ tree
make emu-bench           # just the deterministic cross-ISA counts
make features            # just the capability + trace-completeness probe (JSON)
make bench-check         # gate: the committed golden counts vs a fresh run
make docker-bench        # reproduce a leg in the CI container
make docker-bench DOCKER_PLATFORM=linux/arm64   # the aarch64 leg (see caveats)
```

`make features` alone prints the live capability report — a quick "what can this
box trace, and how completely?"

## The feature & trace-completeness metric

The feature probe records, per tier/backend: `available`, a `skip_reason` when
not, its `fidelity` (native vs virtual), and — the completeness metric —
`complete`: whether a trace it actually produced here was whole (`true`) or
`truncated` (`false`). Completeness is measured two ways:

- **Emulator guests** run a representative routine traced; they are exact by
  construction, so they are the completeness *reference* (always `complete`).
- **The host-native tier** runs a tiny routine *and a loop* under the
  auto-escalating tier (`asmtest_trace_call_auto`) and reports the chosen backend
  plus `complete`. This is where completeness actually varies: a fixed-window
  backend (AMD LBR's 16-deep stack) can truncate a loop and the escalation to a
  ceiling-free backend is what restores completeness. On an x86-64 Linux host the
  single-step floor traces even a 200-trip loop completely (602 instructions).

An **absent** capability is data, not a failure — the probe always exits 0 and
records why the tier self-skipped, exactly as the trace tiers'
`available()`/`skip_reason()` do. See the
[trace parity matrix](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/trace-parity-matrix.md) for the static
version of the same information.

## Instruction-capture depth — the hardware-ceiling ladder

The feature probe also answers a sharper cross-box question: **how many
instructions does each box's hardware actually capture?** Trace backends have
different ceilings, so for the same routine they capture different amounts — and
that difference is a property of the silicon, not the software.

The probe runs a **ladder** of x86-64 routines of growing length and, for each,
compares the instructions captured to the emulator's deterministic **truth**
(host-independent — the same `add3` / `sum_to_n` fixtures the golden uses, where
`insns(sum_to_n) = 3n+2` and the loop executes `n` branches):

| rung | truth insns | branches | what it exercises |
|---|---|---|---|
| `math.add3` | 4 | 0 | straight-line baseline — every backend captures it |
| `loop.sum_to_4` | 14 | 4 | a loop that fits inside a 16-deep window |
| `loop.sum_to_16` | 50 | 16 | a loop at the AMD-LBR window edge |
| `loop.sum_to_64` | 194 | 64 | 4× past the window — a fixed-window backend truncates |
| `loop.sum_to_200` | 602 | 200 | 12× past the window — deep truncation |

Each rung is measured through **two** tiers, emitted as two feature rows:

- **`native-hw`** — the box's *raw top hardware backend*, no escalation. This is
  where the ceiling is **visible**: an unbounded backend (Intel PT, single-step)
  captures every rung whole (`trace_insns == insns_truth`), while a fixed-window
  backend (AMD LBR's 16-deep branch stack) reconstructs only its last window, so
  its captured count **plateaus** below truth on the deep rungs. The row carries
  `insns_truth` so the divergence is self-describing.
- **`native-auto`** — the same rung under `asmtest_trace_call_auto`, which
  *escalates* past a truncating backend to a ceiling-free floor. This is where the
  ceiling is **restored**: it reports what the box *ultimately* captures (complete
  wherever a floor exists) and which backend won.

`scripts/bench-compare` renders the `native-hw` rows as the **capture-depth matrix**
(workload × box). The expected shape across microarchitecture classes:

| workload (truth) | Zen 2 (LBR) | Zen 5 (LBR) | Intel + PT | Intel / mac (single-step) | Apple Silicon |
|---|---|---|---|---|---|
| add3 (4) | 4 ✓ | 4 ✓ | 4 ✓ | 4 ✓ | n/a |
| sum_to_16 (50) | 50 ✓ | 50 ✓ | 50 ✓ | 50 ✓ | n/a |
| sum_to_64 (194) | ~50 ✗ | ~50 ✗ | 194 ✓ | 194 ✓ | n/a |
| sum_to_200 (602) | ~50 ✗ | ~50 ✗ | 602 ✓ | 602 ✓ | n/a |

The two AMD columns share the same 16-deep ceiling — the capture limit is an
LBR-family property, so Zen 2 and Zen 5 truncate at the same rung (the `~50`
plateau is reconstruction-dependent and illustrative). Intel PT and single-step
are unbounded; Apple Silicon has no x86-64 host backend, so it has no hardware
capture (`n/a`) and relies on the emulator floor. **Caveat:** the LBR ceiling is
only observed where LBR is actually *permitted* — a bare AMD box without `perf`
branch-stack permission falls back to single-step and captures completely, so the
truncation row appears only on a box where the fixed-window backend is live.

### The orthogonal axis — call-nesting depth (designed next rung)

The loop ladder sweeps *branch density*. LBR's 16-deep stack independently bounds
*call-nesting depth*: a recursion 32 frames deep overflows the call stack even
though no single loop does. A `recurse.tri(n)` fixture (triangular recursion,
`tri(n) = n + tri(n-1)`) at depths {4, 16, 32} is the designed second axis — it
isolates the call-stack ceiling from the branch-record ceiling. It is not yet
wired in (a single-step box captures it completely, so its divergence is only
observable on a live-LBR box); the loop ladder above is the landed metric.

## Comparing systems

```sh
scripts/bench-compare report-a.json report-b.json ...   # compare given reports
scripts/bench-compare                                    # read the committed tree
```

It emits Markdown: a systems table, the deterministic instruction-count matrix
(name × ISA — compared directly), the real-time matrix (name × system, grouped by
unit, virtualized cells flagged `*`), and the feature & trace-completeness grid
(capability × system, `✓ +complete` / `✗ reason`). In CI the `benchmarks-compare`
job runs it over every leg's uploaded report and posts the result to the job
summary.

## Persistence — the repo is the benchmark database

`make bench-record` commits results under a top-level `benchmarks/` tree, keyed by
a stable `box_id` (`<vendor>-<os>-<arch>-<hash>`, so the same silicon under a
different OS is a distinct record). The two metrics are stored differently on
purpose:

| What | Where | Role |
|---|---|---|
| Deterministic emu counts | `benchmarks/golden/emu-insns.json` | **golden** — host/OS-independent, one file for all systems, gated on every CI leg (`make bench-check`); a diff = an intended code change |
| Feature report | `benchmarks/boxes/<box_id>/features.json` | per-box capability + completeness record |
| Real cycles/ticks | `benchmarks/boxes/<box_id>/perf-history.jsonl` | append-only per-box history (trend, never asserted exactly) |

The golden file works like the ABI manifest / conformance corpus: because the
counts can't vary by host, the committed file must match a fresh run everywhere,
so `make bench-check` is a real regression gate on every OS × arch leg. Real
cycles are noisy and box-specific, so they accumulate as history for trend, not
as an exact assertion.

## Operating systems & the virtualized caveat

The runner captures the descriptor per-OS-portably (Linux `/proc/cpuinfo`, macOS
`sysctl`, Windows CIM), and the OS is a first-class axis: it decides which native
tiers exist (DynamoRIO is Linux-only; Intel PT / AMD LBR are Linux bare-metal;
single-step covers Linux, macOS-Intel, and Windows-VEH; the emulator runs
everywhere), so the feature grid is largely an OS story.

Under `make docker-bench DOCKER_PLATFORM=linux/arm64` the aarch64 leg runs under
qemu-user, where **real cycle counts are meaningless** — export
`ASMTEST_VIRTUALIZED=1` (the runner marks those rows, and `bench-compare` flags
them `*`). The **emulated counts and the feature probe stay fully valid** there,
which is exactly why both dimensions exist.

## Report schema (`asmtest-bench-report/v1`)

```json
{
  "schema": "asmtest-bench-report/v1",
  "system": { "box_id": "...", "os": "linux", "os_version": "...",
              "arch": "x86_64", "cpu": "...", "uarch": "zen5",
              "vendor": "...", "cc": "...", "virtualized": false,
              "asmtest_version": "...", "commit": "...", "timestamp": "..." },
  "performance": {
    "native":   { "unit": "cyc",  "benchmarks": [ { "name": "...", "median": 7.1, ... } ] },
    "emulated": { "unit": "insn", "benchmarks": [ { "name": "...", "arch": "arm64",
                    "kind": "insns", "value": 3, "unit": "insn",
                    "deterministic": true, "complete": true, "blocks": 1 } ] }
  },
  "features": [ { "tier": "native-hw", "backend": "amd_lbr", "arch": "x86_64",
                  "scope": "host", "available": true, "skip_reason": "",
                  "fidelity": "native", "complete": false, "trace_insns": 50,
                  "insns_truth": 602, "note": "loop.sum_to_200" },
                { "tier": "native-auto", "backend": "single_step", "arch": "x86_64",
                  "scope": "host", "available": true, "skip_reason": "",
                  "fidelity": "native", "complete": true, "trace_insns": 602,
                  "insns_truth": 602, "note": "loop.sum_to_200" } ]
}
```

A `native-hw` row carries the instructions its raw hardware backend **captured**
(`trace_insns`) against the deterministic **truth** (`insns_truth`); `complete` is
whether they matched. Above, an AMD-LBR box captured only 50 of 602 (`✗`), while
its `native-auto` tier escalated to single-step and captured all 602 (`✓`). Every
performance row follows the `asmtest_bench_result_t` shape — `kind`, `value`,
`unit`, `deterministic`, `complete` — so a reporter groups and compares only within
a `unit`, never across it.

## In your CI

Each per-push leg (`ubuntu-latest`, `ubuntu-24.04-arm`, `macos-latest`) runs
`make bench-check` (the golden gate) and `make bench-report`, uploads its
per-system JSON, and the `benchmarks-compare` job merges them. See
[CI & Docker](../reference/ci.md). Extending to the Windows and Intel-macOS legs,
and auto-committing per-box history on the nightly schedule, are the tracked
follow-ups in
[the plan](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/cross-arch-benchmarking-plan.md).
