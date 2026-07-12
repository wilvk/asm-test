# asm-test — .NET example ideation: capabilities not yet demonstrated

The `.NET` example suite (examples/dotnet) had grown to ~29 whole-window-centric demos, but a
cluster of **bound, public** `AsmTrace`/`HwTrace`/`Ptrace` APIs was exercised only in the parity
self-test (`bindings/dotnet/hwtrace/HwTraceProgram.cs`), never in a shipped example — and several
real tracing *workflows* (regression diff, perf triage, cost awareness, crash-proof demonstration)
had no example at all. This plan records the ideation follow-through: what LANDED and what is
DEFERRED, with reasons.

## Landed

One binding fix + 13 new examples, each dependency-free, each self-skipping cleanly (exit 0) where
its tier is unavailable. All validated in the `asmtest-dotnet` container on this Zen 5 box.

### Binding fix — `AmdSnapshot` (unblocks `amd-snapshot`)

`asmtest_amd_snapshot_trace` / `asmtest_amd_snapshot_available` (src/branchsnap.c, the deterministic
boundary LBR snapshot) existed in the native lib but had **no managed DllImport**. Added the two
P/Invokes + a public `AmdSnapshot` class (`Available()` / `SkipReason()` / `Trace(NativeCode, exitOff,
Action, HwTrace)`) to [HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs). The run callback reuses
`StealthRunFn` (the Cdecl `void(void*)` upcall). `branchsnap.o` is already linked into
`libasmtest_hwtrace.so`, so the symbols resolve; without a BPF-toolchain build / `CAP_BPF` the wrapper
returns `ENOSYS`/`EUNAVAIL` and the example self-skips.

### New examples

| Example | Uniquely shows | Tier / gate |
|---|---|---|
| `single-method` | one JIT'd body stepped in isolation (managed peer of `region`); the SAFE single-step posture + `outOfProcess:true` | single-step / none |
| `perf-triage-drill` | survey → drill (hottest managed method, then its exact body) | single-step / none |
| `concurrent-isolation` | two overlapping `EFLAGS.TF` windows, neither truncates the other | single-step / none |
| `async-stitch` | one operation stitched across an `await`/`Task.Run` hop (`AsmStitchedTrace`) | single-step / none |
| `trace-diff` | exact coverage DELTA between two runs (blocks turned on/off) | single-step / none |
| `coverage-guided-fuzz` | per-input marginal coverage delta (AFL keep/discard) | single-step / none |
| `trace-cost-overhead` | slowdown-multiplier + stop-count table across every tier | mixed / none |
| `descend-all` | auto-descend unknown callees + guardrails (`DescentLevel.DescendAll`) | ptrace / none |
| `crashproof-showdown` | the fatal boundary observed: in-proc child dies (exit 133) vs OOP survives | single-step + ptrace / none |
| `crashproof-survey` | Method vs Window vs WindowHot fidelity/cost/safety table | mixed / AMD leg gated |
| `tier-ladder` | this host's degradation cascade (`ResolveTiers`/`AutoTier`/`DegradationNote`) | none (enumeration) |
| `amd-period-sweep` | the statistical survey swept across sample periods (`WindowHot(period:)`) | AMD LBR / `CAP_PERFMON` |
| `amd-snapshot` | the deterministic boundary LBR snapshot (`AmdSnapshot.Trace`) | AMD snapshot / `CAP_BPF`+`CAP_PERFMON` |

All 14 are wired into `make hwtrace-dotnet-example` (mk/native-trace.mk) and the
[examples/dotnet/README.md](../../../../examples/dotnet/README.md) index. On the plain lane the AMD trio
(`amd-period-sweep`, `amd-snapshot`, and `crashproof-survey`'s AMD leg) self-skip; the permissioned
lane (`--cap-add=PERFMON`, plus `--cap-add=BPF` for the snapshot) runs them live.

## Deferred (with reasons)

Five ideation candidates were NOT shipped — each would land as an always-self-skipping, duplicative,
misconceived, or CI-flaky example on this box. Deferring is the honest call; the capability is not lost.

- **`exception-anatomy`** (name the CLR's throw/unwind machinery over an `AsmTrace.Window`) — the OOP
  single-stepper reliably captures a *single* in-scope throw (as `localscope_oop_managed` shows), but a
  battery of in-window exception dispatches (~5 throws + filter/finally funclets) SIGABRTs the stepper
  ~15% of runs (the runtime re-enters under step during two-pass unwind). Too flaky for the CI lane;
  needs the same E3 sibling-thread-publish fix as the deep-BCL gap. Deferred until then.

- **`verify-tier-up`** (capture the same method at Tier0 vs Tier1) — `AsmTrace.Method` resolves the
  body via `RuntimeHelpers.PrepareMethod`, which forces the **tier0** compile, so a second scope can
  never step the tier1 body of the same method. It reliably prints "no re-tier observed." Capturing a
  specific higher tier needs the attach-over-re-JIT path below.
- **`managed-rejit-versioned`** (`render_versioned` / `TraceAttachedVersioned` over a tier0→tier1
  re-JIT) — a real tier-up **relocates** to a new address (the documented `codeimage` caveat), so
  forcing the payoff needs a `CodeImage(pid)` foreign timeline + attach against an own-spawned child
  with careful timing. High-risk; the mechanism is already shown on a controlled blob by `codeimage`.
- **`attached-blockstep`** (`Ptrace.TraceAttachedBlockstep` against an externally-stopped process) —
  needs a stopped foreign process with resolvable JIT addresses; the cost angle is already covered by
  `trace-cost-overhead`'s block-step column and the mechanism by `blockstep`.
- **`deep-bcl-jitdump`** (`JitdumpFind(..., wantBytes:1)` recovering a foreign method's bytes) — needs
  the target to emit `/tmp/jit-<pid>.dump` (`DOTNET_PerfMapEnabled=3`); jitdump-based naming is already
  demonstrated by `rundown`, and the bytes-recovery angle isn't worth the fragile plumbing.

## Non-goals

- A dedicated permissioned CI lane for the AMD/snapshot dotnet examples — they self-skip in the plain
  lane and are validated on a self-hosted Zen runner (mirroring `docker-hwtrace-amd`).
- Exact whole-window on AMD (hardware dead end — see [amd-tracing-plan.md](../../plans/amd-tracing-plan.md)).
