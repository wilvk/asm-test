# Tuning AMD LBR window reach

The AMD LBR backend (`ASMTEST_HWTRACE_AMD_LBR` — see
[Hardware tracing](hardware-tracing.md)) reconstructs a routine's exact
instruction and block stream from the CPU's {term}`LBR` branch stack. That stack
is **16 entries deep on every shipping Zen part** — a silicon ceiling, not a
software choice — so how much of a run one capture can reach is a real tuning
question. This page explains what bounds a window, the knobs
`asmtest_hwtrace_options_t` exposes to stretch it
([asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)), how the backend
reports the runs it could not hold, and when a *statistical* survey is the
better tool than a longer exact window.

Every lever here is **fidelity-neutral by construction**: a mis-tuned knob
degrades to an honest `truncated` trace (or simply forgoes the stretch), never
to silently wrong offsets.

---

## What bounds a window

The branch stack records the **last 16 taken branches**, and the kernel
delivers it only inside a perf *sample* — the backend arms a branch-retired
counter so a {term}`PMI` snapshots the stack (see {term}`perf_event`). Three
consequences bound what one capture can hold:

- **The 16-deep window (Tier A).** A routine whose whole path takes ≤ 16
  branches fits one window and reconstructs exact and complete. (The depth is
  read from CPUID at runtime rather than assumed; every shipping part reports
  16.)
- **The data ring and PMI throttling (Tier B).** Past 16 branches,
  {term}`Tier-B stitching` splices the overlapping windows into one gapless
  sequence — so the 16-branch ceiling itself is lifted, and the *remaining*
  ceiling is how many `sample_period=1` windows the perf **data ring** can hold
  before the kernel drops the newest ones, plus the kernel's sample-rate
  throttling (`kernel.perf_event_max_sample_rate`).
- **Too-fast single-shot routines.** The stack arrives only at a PMI, so a tiny
  routine that returns before any sample fires in-region is never captured at
  all. The backend reports that honestly as `truncated`; the deterministic
  boundary snapshot below exists for exactly this case.

## How truncation is reported

Every incomplete capture collapses onto the one loss bit every backend shares:
`asmtest_trace_t.truncated` (see [Execution traces](traces.md)). The AMD
backend sets it when a single window overflowed the 16-deep stack and could not
be stitched further, when the stitch found a **gap** (insufficient overlap
between consecutive windows), when the kernel signalled drops
(`PERF_RECORD_LOST` / throttling), or when no in-region sample fired at all. A
partial capture is **never** emitted as complete.

The recovery idiom is dynamic fallback: re-resolve under
`ASMTEST_HWTRACE_CEILING_FREE` — which skips the one backend with a bounded
completeness ceiling — and re-run on the backend that comes back (typically
{term}`single-step`, which has no ceiling). See
[Auto-selecting a backend](hardware-tracing.md#auto-selecting-a-backend), or
`asmtest_trace_call_auto` for the cross-tier form that escalates automatically.

## The levers

| Lever (`asmtest_hwtrace_options_t`) | What it does | Reach effect | Trade |
|---|---|---|---|
| `data_size` | grows the perf data ring holding the sampled windows (AMD default 256 KB) | longer gapless Tier-B stitch before drops | memory only |
| `lbr_period` | a PMI every N taken branches instead of every one | ~N× fewer interrupts → less throttling/ring pressure | **undercounts self-similar loops** — see below |
| `branch_filter` | drops direct unconditional `jmp` edges from the hardware filter; the decoder follows them statically | measured **1.86×** more instructions per window | none (byte-identical; falls back if perf rejects it) |
| `snapshot` | deterministic boundary snapshot at the region exit instead of sampling | completes the too-fast tiny routine | single-exit regions; needs the BPF substrate |

### Ring size and runner sysctls — `data_size`

The Tier-B stitched capture is bounded by the data ring: a long
`sample_period=1` run emits more windows than the ring holds, the kernel drops
the *newest* samples, and the trace honestly truncates. Raise `data_size`
(bytes, rounded up to power-of-two pages; `0` selects the AMD default of
256 KB) to extend reach. On a dedicated runner, also raise
`kernel.perf_event_max_sample_rate` and set
`kernel.perf_cpu_time_max_percent=0` so sustained per-branch sampling is not
throttled. This lever changes nothing about fidelity — only how much of the run
fits.

### Fewer interrupts — `lbr_period` (period-spaced stitching)

`lbr_period = N` (opt-in; `0` keeps the exact default) takes a PMI every N
taken branches instead of every one, so consecutive 16-deep windows overlap by
`16 − N` entries and still stitch — at ~N× fewer interrupts, cutting the
throttling and ring pressure that end a long capture. `N` must stay below the
LBR depth (the backend clamps it) so an overlap always remains; an over-large
value just produces a stitch gap and an honest `truncated`.

**The caveat that keeps the default at `0`:** a loop's taken edges repeat
identically every iteration, which gives the stitcher no way to tell 1 skipped
iteration from N — so `lbr_period > 1` silently **undercounts self-similar
loops**. This is not theoretical: measured live on the Zen 5 dev box, the
16-deep-overflowing loop fixture reconstructs *fewer* instructions at period 4
than at period 1 (231 vs 297). Since every loop is edge-self-similar, the
lever's reach gain is confined to **distinct-edge straight-line paths** — which
are inherently short. Prefer `branch_filter` for loops; reserve `lbr_period`
for long branchy non-looping paths.

### Stretching each window — `branch_filter` (reduced filter)

A direct unconditional `jmp` has a statically decodable target, so recording it
wastes one of the 16 scarce LBR slots. `branch_filter = 1` (opt-in; `0` keeps
`PERF_SAMPLE_BRANCH_ANY`) asks the hardware to record only
conditional / indirect-jump / call / return branches; the reconstructor follows
the dropped `jmp`s from the registered region bytes itself, so the trace stays
**byte-identical** — the knob changes capture efficiency, never fidelity.

Because each window now spends its slots only on branches the decoder cannot
infer, one window spans more of the routine: measured on the Zen 5 dev box, a
loop whose body carries a direct `jmp` plus a conditional back-edge
reconstructs **1.86× more instructions per 16-deep window** (65 vs 35). This is
the window-stretch lever that *does* help loops. It applies to the sampled and
deterministic-snapshot exact paths (not the statistical survey), and if perf
rejects the filter combination the capture silently retries with the full
filter — the tier stays available, merely forgoing the stretch.

### The deterministic boundary snapshot — `snapshot`

On the AMD backend, `snapshot` opts the begin/end markers into a
**deterministic boundary capture**: a hardware breakpoint at the region's exit
triggers a small eBPF program that reads the frozen 16-entry stack at that one
point (`bpf_get_branch_snapshot`), instead of hoping a `sample_period=1` PMI
lands in-region. This is the fix for the too-fast single-shot routine — the
case the sampled path can only mark `truncated`. Where the substrate supports
it, the backend already selects the snapshot **by default for single-exit
regions**; any arm failure (no BPF toolchain, missing capabilities, no
LbrExtV2) falls back cleanly to the sampled path. Note the snapshot is one
frozen window: it completes small routines, it does not stitch past 16
branches.

## Privileged vs unprivileged lanes

The reach levers sit on top of a privilege gradient. What actually runs where:

| Path | Needs | Containerized lane |
|---|---|---|
| Sampled LBR (+ `lbr_period` / `branch_filter`) | Zen 3+ silicon; `perf_event_paranoid` lowered or `CAP_PERFMON` | `make docker-hwtrace-amd` (PERFMON cap, seccomp unconfined) |
| Deterministic snapshot | the above **plus** `CAP_BPF` + the BPF toolchain; Zen 4+ (LbrExtV2), Linux ≥ 6.10 | `make docker-hwtrace-codeimage` |
| MSR-direct snapshot | root / `CAP_SYS_ADMIN` + the `msr` module (self-hosted runners only) — an exact zero-interrupt Tier-A read with the same 16-deep ceiling | `make docker-hwtrace-msr` (privileged) |
| {term}`single-step` fallback | nothing — no PMU, no perf, no privilege | `make docker-hwtrace` (plain container) |
| Statistical {term}`IBS` survey | any Zen incl. Zen 2; **unprivileged** at the default `perf_event_paranoid=2` | `make docker-hwtrace-ibs` |

`asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)` collapses the silicon +
kernel + privilege chain into one predicate, and
`asmtest_hwtrace_skip_reason()` says which gate failed — so a suite self-skips
with the reason (e.g. perf denied vs. no branch-record hardware) instead of
failing. On a host where you control the knob,
`sudo sysctl kernel.perf_event_paranoid=1` (or granting `CAP_PERFMON`) is what
opens the sampled lane; no amount of tuning opens it from inside an
unprivileged process.

## When the statistical IBS lane is the better tool

Window tuning buys reach for an **exact** trace of one registered region.
Reaching for it is the wrong move when:

- **the host is Zen 2** — there is no branch stack at all; every exact AMD
  hardware path self-skips, and IBS is the only hardware branch source;
- **you cannot get perf privilege** — the IBS lane runs user-only and
  unprivileged at the default `perf_event_paranoid=2`;
- **the target is a live process / JIT runtime you must not perturb** — IBS
  attaches out of band to any same-uid pid at full speed, where an exact window
  needs the begin/end markers in-process;
- **the question is "where is it hot?", not "exactly what ran?"** — a hot-edge
  histogram over a longer window answers it without any ceiling to tune.

The lane is honest about being statistical: it fills its own survey shape,
never `asmtest_trace_t`, and a sampled edge proves presence but absence proves
nothing. See
[the IBS-Op lane](hardware-tracing.md#statistical-edges-out-of-band-the-amd-ibs-op-lane)
and the interactive [asmspy `--sample` view](asmspy.md). The AMD whole-window
statistical survey falls back to IBS on branch-stack-less hosts automatically.

## See also

- [Hardware tracing](hardware-tracing.md) — the backend table, region
  lifecycle, and auto-selection this page tunes.
- [Execution traces](traces.md) — the shared `asmtest_trace_t` shape and the
  `truncated` contract.
- [Native runtime tracing](native-tracing.md) — the ceiling-free DynamoRIO and
  out-of-process stepper tiers a truncated capture escalates to.
- [asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — the
  `asmtest_hwtrace_options_t` field documentation these levers ship with.
- The window-size lever design notes (internal):
  [amd-tracing-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/amd-tracing-plan.md)
  (Matrix 2, "Window-size levers — status").
