# F4 attach-mode profiler probe — findings (live-attach data-flow tier)

**Verdict: GO (2026-07-16).** A CLR profiler **can** be attached to an already-running
`dotnet` process that we did **not** launch — one started with **no `CORECLR_*` environment
variables at all** — over the runtime's diagnostics port, and it **does** then receive
`ICorProfilerCallback4::MovedReferences2` GC-move `{old, new, len}` triples. Measured on the
pinned .NET 8 SDK, Linux x86-64:
`accepted=1 init_for_attach=1 mask_s_ok=1 attach_complete=1 pre_attach_moved=0
post_attach_moved=320 victim_end=1 crash=0 victim_rc=0`, with `SetEventMask(COR_PRF_MONITOR_GC)`
returning **`hr=0x00000000` (S_OK)** and **160,032 genuinely relocating ranges** (of 160,064
delivered) observed across 320 post-attach compacting gen2 GCs. The victim survived and exited
cleanly. **None of the four kill criteria tripped.**

This retires the risk the F4 section of
[live-attach-dataflow-followup-plan.md](../plans/live-attach-dataflow-followup-plan.md) named as
"the honest open question to spike first". F4's GC-move feed is **not** restricted to launched
targets, and the ptrace live-attach tier does **not** need the DR tier's startup-env-var wiring.

Reproducible: identical result across **three** consecutive runs (same hr, same
`post_attach_moved=320`, same `reloc_ranges=160032`, `rc=0` each time).

Reproduce with:

```
make docker-attachprof-probe      # the only way on a host without the .NET SDK
make attachprof-probe             # host-direct; self-skips without dotnet/git
```

## The question

The DR taint tier's Increment 7 gets its GC-move ranges from an in-process
`MovedReferences2` profiler wired in with `CORECLR_ENABLE_PROFILING` /`CORECLR_PROFILER` /
`CORECLR_PROFILER_PATH` at process start (see
[gc-move-range-extraction-findings.md](gc-move-range-extraction-findings.md), and the working
startup profiler [examples/gcprofiler_probe/gcprofiler.cpp](../../../examples/gcprofiler_probe/gcprofiler.cpp)).
Those variables are **read at startup**. The live-attach tier attaches to processes asmspy did
not launch, so that wiring is unavailable to it by construction. Hence F4's spike question:

> Can a profiler attach to an already-running .NET process, and still receive
> `ICorProfilerCallback4::MovedReferences2` `{old,new,len}` triples?

If the answer were no, F4's canonicalization would have been **limited to launched targets** —
a materially smaller feature, and worth knowing before building the join.

## What was probed

Deliberately **no DynamoRIO**. This is the out-of-band ptrace tier's question, so the lane is
just the .NET SDK + a C++ toolchain + git — no `DR_AVAILABLE` gate, no `--cap-add=SYS_PTRACE`
(the attach travels the runtime's own diagnostics IPC socket, not ptrace). Artifacts:

- [examples/attachprof_probe/attachprof.cpp](../../../examples/attachprof_probe/attachprof.cpp) —
  the **attach-mode** profiler. Structurally the startup probe's sibling (same `CINTERFACE`
  C-vtable + generic-stub array-fill trick, same strict QI) with the one difference that
  matters, below.
- [examples/attachprof_probe/victim/](../../../examples/attachprof_probe/victim/) — a **plain**
  long-running `dotnet` process, modelled on `gcmover`: each round allocates a fragmented mix of
  garbage and survivors, then forces a **compacting** gen2 GC that relocates them. It reports
  `ATTACHPROF_VICTIM_START pid=<pid>`, a heartbeat per round, and `ATTACHPROF_VICTIM_END`. Run
  with `env -u CORECLR_ENABLE_PROFILING -u CORECLR_PROFILER -u CORECLR_PROFILER_PATH` — the
  no-env condition is the whole point, so it is enforced, not assumed.
- [examples/attachprof_probe/attacher/](../../../examples/attachprof_probe/attacher/) — drives
  `Microsoft.Diagnostics.NETCore.Client`'s
  `DiagnosticsClient.AttachProfiler(TimeSpan, Guid, string)` against the running victim's pid.
  The package worked first try; the raw diagnostics-IPC `AttachProfiler` command was not needed.
- `make attachprof-probe` / `make docker-attachprof-probe` +
  [Dockerfile.attachprof-probe](../../../Dockerfile.attachprof-probe) — build all three, start the
  victim, let it complete ~14 compacting GCs **natively and unprofiled**, attach mid-run, hold
  ~6 s, then assert on the victim's own log.

## The one structural difference from the startup probe

**An attaching profiler never gets `Initialize`.** It gets
`ICorProfilerCallback3::InitializeForAttach(IUnknown *pInfoUnk, void *pvClientData, UINT cb)`,
followed by `ICorProfilerCallback3::ProfilerAttachComplete()`, and the event mask must be set
from `InitializeForAttach`. A profiler that only implements `Initialize` attaches and then goes
silently deaf — every callback slot is a generic `S_OK` stub, so there is no crash and no error
to notice. Two consequences worth recording:

- `ICorProfilerCallback3` in `QueryInterface` is **load-bearing on this path, not optional**:
  the attach path QIs specifically for it (that is where `InitializeForAttach` lives).
- The QI must still **reject `ICorProfilerCallback5` and above** — the startup probe's rule
  carries over unchanged. A permissive "return this" makes the CLR vcall past the Callback4
  vtable.

## Sanity-check of the contractual claims

Confirmed directly in the **pinned** `dotnet/runtime` **v8.0.8** `corprof.h` the lane fetches
(`src/coreclr/pal/prebuilt/inc/corprof.h`), not from memory:

- L579 — `COR_PRF_ALLOWABLE_AFTER_ATTACH` includes **`COR_PRF_MONITOR_GC`**.
- L598 — `COR_PRF_HIGH_ALLOWABLE_AFTER_ATTACH` includes
  **`COR_PRF_HIGH_MONITOR_GC_MOVED_OBJECTS`**.

So the masks are contractually settable post-attach, and the runtime honoured the contract:
`SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x00000000`. As with the startup probe, plain
`COR_PRF_MONITOR_GC` is **sufficient** — `MovedReferences2` fires without touching the high
mask.

## Evidence

The profiler's own lines, from inside the running victim:

```
# plain victim pid=284 (14 compacting-GC rounds done, no CORECLR_* env, pre-attach MovedReferences2=0)
ATTACHER: AttachProfiler ACCEPTED (runtime returned success)
ATTACHPROF: DllGetClassObject — profiler .so loaded into the RUNNING process
ATTACHPROF: CreateInstance — profiler object handed to CLR
ATTACHPROF: InitializeForAttach ENTERED pid=284 cbClientData=0
ATTACHPROF: InitializeForAttach — SetEventMask(COR_PRF_MONITOR_GC=0x80) hr=0x00000000  (S_OK)
ATTACHPROF: ProfilerAttachComplete — attach finished, callbacks live
ATTACHPROF: MovedReferences2 ranges=512 relocating=510  reloc[0]={old=0x7c3726000040 new=0x7c3728c0aec0 len=32 delta=+46182016}
ATTACHPROF: Shutdown (moved_calls=320 moved_ranges=160064 reloc_ranges=160032)
```

Against the four kill criteria, all held:

| # | Kill criterion | Result |
|---|---|---|
| 1 | `InitializeForAttach` never fires (attach rejected / times out) | **held** — accepted, `InitializeForAttach` + `ProfilerAttachComplete` both fired |
| 2 | `SetEventMask` fails (e.g. `CORPROF_E_UNSUPPORTED_FOR_ATTACHING_PROFILER` `0x80131363`) | **held** — `hr=0x00000000` (S_OK) |
| 3 | Profiler attaches but `MovedReferences2` never delivers a range | **held** — 320 calls, 160,064 ranges, **160,032 relocating** |
| 4 | Victim crashes/hangs on attach | **held** — `rc=0`, reached `ATTACHPROF_VICTIM_END`, no fatal signal |

The **`pre_attach_moved=0` / `post_attach_moved=320` split is the load-bearing control**: the
victim was demonstrably doing relocating compacting GCs *before* the attach and the profiler saw
none of them, so the post-attach ranges are attributable to the attach and not to some
accidental startup wiring.

## What surprised us

- **It just worked** — first real run, no iteration on the profiler. Given that the *other*
  managed-attach question in this codebase (DR external attach to a running `dotnet`) is an
  exhaustively-closed NO-GO
  ([dr-managed-attach-probe-findings.md](dr-managed-attach-probe-findings.md)), a second clean
  GO was not the expected prior. The distinction is real and worth stating plainly: **DR attach
  seizes the runtime's threads from outside at arbitrary points and dies; profiler attach is a
  cooperative path the runtime itself implements and drives at its own fences.** They are not
  the same kind of operation, and the DR NO-GO carries no evidence against this one.
- **`MovedReferences2` reports non-relocating ranges too.** The first sample range came back
  `old == new` (a compaction can leave a segment head in place). "Ranges were delivered" would
  therefore have been a **vacuous pass** — the F4 transform exists precisely to follow objects
  whose address *changed*. The probe was tightened to count `old != new` ranges separately and
  sample one, and the assertion now requires a genuine relocation. 160,032 / 160,064 relocate,
  so the distinction did not change the verdict — but it would have hidden a real failure mode.
- **`cbClientData=0`.** `AttachProfiler` supports passing a client-data blob to
  `InitializeForAttach`; the package's 3-arg overload sends none. The wiring step can use it to
  hand the profiler its configuration (e.g. an shm channel name) at attach time, rather than
  needing a side channel.
- **The sibling lanes' self-skip idiom is unsound outside a DR gate.** The probes write
  `command -v dotnet || { echo SKIP; exit 0; }` as a recipe **line** — but `exit 0` ends only
  that line's shell, and make runs the next line regardless. It is harmless in the siblings
  purely because `ifndef DR_AVAILABLE` short-circuits first on a DR-less host. This lane has no
  DR gate, so the bug was live: it printed `1..0 # skipped` and then failed for real. Fixed here
  with a **parse-time** `ATTACHPROF_MISSING` conditional plus a sentinel file for the
  runtime (network) skips. **Any future non-DR lane copying this pattern will hit the same
  trap.**

## What this means for F4

- **F4 is not limited to launched targets.** The plan's stated fallback ("or the capture is
  limited to launched targets") is not needed.
- The remaining F4 work is what the plan already says it is — **pointing a proven feed at a
  landed transform** (`asmtest_gcmove_canonicalize`,
  [src/dataflow_gcmove.c](../../../src/dataflow_gcmove.c)) — with the attach path now proven as
  the delivery mechanism for the live-attach tier.
- Note this tier still differs from the DR tier in the way the plan describes, and this probe
  does **not** speak to that half: DR is in-process and remaps its shadow *at* the fence; this
  tier is out-of-process and post-pass, so it must **stamp triples with the value-trace step
  boundary** the compaction takes effect at and canonicalize before `asmtest_defuse_build`.
  Getting the triples is solved; **stamping them coherently against the trace is the next open
  question**, and it is a different one.
- The probe is a **throwaway research spike**, not product, and is not wired into the main CI
  gate — same posture as its sibling probes.

## Sources

- [Profiler Attach on CoreCLR][attach]
- [ICorProfilerCallback3::InitializeForAttach][ifa]
- [ICorProfilerCallback3::ProfilerAttachComplete][pac]
- [ICorProfilerCallback4::MovedReferences2][mr2]
- [COR_PRF_MONITOR enumeration][mask]
- [Diagnostics client library][dcl]
- Local, pinned: `dotnet/runtime` **v8.0.8** `src/coreclr/pal/prebuilt/inc/corprof.h` L579, L598

[attach]: https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/profiling/Profiler%20Attach%20on%20CoreCLR.md
[ifa]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback3-initializeforattach-method
[pac]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback3-profilerattachcomplete-method
[mr2]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback4-movedreferences2-method
[mask]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/cor-prf-monitor-enumeration
[dcl]: https://learn.microsoft.com/en-us/dotnet/core/diagnostics/diagnostics-client-library
