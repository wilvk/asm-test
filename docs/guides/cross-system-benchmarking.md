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
  "features": [ { "tier": "native-auto", "backend": "single_step", "arch": "x86_64",
                  "scope": "host", "available": true, "skip_reason": "",
                  "fidelity": "native", "complete": true, "trace_insns": 602 } ]
}
```

Every performance row follows the `asmtest_bench_result_t` shape — `kind`, `value`,
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
