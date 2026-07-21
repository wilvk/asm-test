# Findings: .NET GC-move {old,new,len} range extraction (taint tier, Increment 7)

*Status: findings / research record (2026-07-14). Produced for
[dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md) **Increment 7**
(GC-move umbra shadow remap), whose full path is hard-blocked on getting .NET's
compaction object-move ranges out of the runtime to feed the byte-granular tag-shadow
remap `at_gc_remap` (already landed behind the disabled `ASMTEST_TAINT_GCREMAP` flag,
[dataflow_dr_client_inlined.c:431](../../../src/dataflow_dr_client_inlined.c#L431)). The
plan **assumed** this required an out-of-process EventPipe/nettrace parser. A deep-research
investigation (95 agents, 15 sources, 25 claims adversarially verified, 23 confirmed)
found a **better in-process native mechanism** — the `ICorProfilerCallback4::MovedReferences2`
profiler — which a follow-up **coexistence probe then confirmed works end-to-end under
DynamoRIO (GO, 2026-07-14; see "Probe result" below)**. This doc records the three candidates,
the verified findings, the recommendation, and the probe outcome.*

## The question

Increment 7 makes taint survive .NET GC compaction: when the GC relocates an object, its
shadow tags must move with it, or a compacting collection silently drops or aliases taint.
The remap primitive is already written — `at_gc_remap(old_base, new_base, len)` does the
byte-granular `[old, old+len) -> [new, new+len)` tag copy (source-snapshot then
clear-then-paint, so arbitrary old/new overlap is correct;
[dataflow_dr_client_inlined.c:426-465](../../../src/dataflow_dr_client_inlined.c#L426)),
and a synthetic-triple unit test proves it given a triple. What is **missing** is the live
feed: the concrete per-move `{old, new, len}` triples out of the running .NET runtime
(Linux x86-64, .NET 8), delivered at a point where the remap can run coherently against the
Increment-4 concurrent-writer policy.

The plan's standing assumption ([dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md)
Increment 7, as-planned text) was that this triple must come from the runtime's
`GCBulkMovedObjectRanges` ETW/EventPipe event, parsed **out of process** via nettrace. The
in-proc `EventListener` shipped only *detection* (`GcMoveMap`) because it hands back a
scalar move-count, not the per-range `Values` struct-array. The open question this
investigation answered: **is out-of-process EventPipe actually the right — or only —
extraction mechanism?**

## The investigation

Not a code probe. A deep-research fan-out (95 agents, 15 sources, 25 discrete claims put
through adversarial verification, 23 confirmed at 3-0 / high confidence) over the CoreCLR
profiling API, the diagnostics/EventPipe stack, TraceEvent's schema, and the
`EventListener` surface. Every finding below is 3-0 verified unless explicitly flagged.
The three candidate extraction mechanisms, in the order they were evaluated:

1. **In-process CLR Profiling API** — `ICorProfilerCallback4::MovedReferences2` in a
   native profiler `.so` co-loaded into the target.
2. **Out-of-process EventPipe** — `DiagnosticsClient` + TraceEvent parsing the nettrace
   `GCBulkMovedObjectRanges` event from a separate helper process.
3. **In-process `EventListener`** — the managed listener the detection feed already uses.

## Candidate 1 (RECOMMENDED): ICorProfilerCallback4::MovedReferences2

A native, in-process CLR **profiler** — a DLL/`.so` mapped into the *same* address space as
the target, whose callbacks run in-process ([BOTR profiling.md][botr]). This is the
recommended mechanism.

- **Exact per-range triples, delivered directly.** `MovedReferences2` delivers
  `cMovedObjectIDRanges` and **three parallel arrays** —
  `oldObjectIDRangeStart[]`, `newObjectIDRangeStart[]`, `cObjectIDRangeLength[]`
  ([MovedReferences2 docs][mr2]). The runtime's own affine remap is
  `newObjectID = newObjectIDRangeStart[i] + (oldObjectID - oldObjectIDRangeStart[i])` over
  `oldObjectIDRangeStart[i] <= oldObjectID < oldObjectIDRangeStart[i] + cObjectIDRangeLength[i]`
  — i.e. **precisely** the `[old, old+len) -> [new, new+len)` byte-granular tag copy
  `at_gc_remap` performs. There is no schema parsing and no reconstruction: the payload *is*
  the triple.
- **Use `MovedReferences2`, not `MovedReferences`.** In `MovedReferences2` the length is a
  `SIZE_T`, so ranges larger than 4 GB report correctly; the older `MovedReferences`
  ([MovedReferences docs][mr1]) uses `ULONG` lengths and **truncates** past `MAX_ULONG`.
- **Fires only on real moves.** `MovedReferences2` fires only for compacting collections
  that actually relocate objects; non-compacting / sweeping survivors are reported through
  `SurvivingReferences`/`SurvivingReferences2` instead. A profiler gets one **or** the
  other per collection, never both ([BOTR profiling.md][botr]). This matches the tier's
  "remap only on a real move" requirement exactly — no spurious remaps on sweeps.
- **A natural quiesce fence.** The callback fires while the EE is **fully suspended** — a
  ready-made world-stop for the bulk shadow mutation the remap needs (this is the true
  world-stop the as-landed `at_gc_remap` comment says the live path wants, in place of the
  synthetic test's single-threaded assumption). **Nuance:** it fires *before* physical
  relocation, so at callback time the source bytes are still at `[old, old+len)`, and the
  docs warn ObjectIDs must not be **dereferenced** during the callback (contents may be
  mid-move); `ICorProfilerCallback2::GarbageCollectionFinished` signals all moves complete.
  That "don't dereference" caveat concerns object **contents**, not the `{old,new,len}`
  range **values** (which are the valid, fully-delivered payload) — and it is **doubly
  irrelevant here** because `at_gc_remap` copies the address-keyed **tag shadow**, not
  object bytes, so copying during the callback is safe.
- **Co-located and native — calls the remap directly.** Because the profiler is in the
  target's own address space, the callback can call `at_gc_remap` **directly and
  synchronously** at the GC fence — no cross-process hop, no serialization, no delivery
  channel. This is the decisive structural advantage over Candidate 2.
- **Event mask + its tradeoff.** Requires the `COR_PRF_MONITOR_GC` (`0x80`) event mask, set
  via `SetEventMask` in `Initialize` ([COR_PRF_MONITOR enum][mask]). **Known side-effect:**
  enabling `COR_PRF_MONITOR_GC` **disables concurrent/background GC** — it forces blocking
  collections, and the runtime returns `CORPROF_E_CONCURRENT_GC_NOT_PROFILABLE` if it
  cannot. This is arguably *beneficial* for a remap (clean, deterministic suspension
  fences), but it changes the target's GC behaviour and throughput, so it must be recorded
  as a real tradeoff, not a footnote.
- **Attach works on Linux .NET 8.** Both at **startup** — env vars
  `CORECLR_ENABLE_PROFILING=1`, `CORECLR_PROFILER={CLSID}`,
  `CORECLR_PROFILER_PATH=/path/to/profiler.so` — and to an **already-running** process via
  the diagnostics port (the `dotnet-diagnostic-<pid>` Unix domain socket, .NET Core 3
  preview6+) ([Profiler Attach on CoreCLR][attach]). For a `drrun`-launched process,
  **startup env-var attach is the simplest** wiring.

## Candidate 2 (FALLBACK): out-of-process EventPipe + TraceEvent

The plan's originally-assumed path, retained as the fallback if Candidate 1 cannot coexist
with DynamoRIO (see caveats).

- A separate helper process opens `DiagnosticsClient(pid)` and calls
  `StartEventPipeSession` over the diagnostics socket ([diagnostics-client-library][dcl],
  [EventPipe][ep]); the resulting nettrace stream is parsed by TraceEvent's
  `EventPipeEventSource`, whose `GCBulkMovedObjectRangesTraceData.Values[]` **does** expose
  the per-range `OldRangeBase` / `NewRangeBase` / `RangeLength` (the struct-array the
  in-proc `EventListener` cannot reach).
- **Gated behind the right keyword.** The event is under the `GCHeapSurvivalAndMovement`
  keyword (`0x400000`), **not** the base `GC` keyword — Maoni Stephens confirms this event
  is emitted under that keyword and is used to construct heap graphs
  ([perfview#1084][pv1084]).
- **Why it is the fallback, not the pick.** Cross-process, higher latency, and — decisively
  — EventPipe events can **drop or truncate under load**, which for a shadow remap means
  **silent tag corruption** (a missed move aliases pre/post-move taint with no error). The
  in-proc profiler's *guaranteed-complete* delivery at a suspended-EE fence is exactly what
  makes it the primary recommendation over this path.

## Candidate 3 (DEAD END): in-process EventListener

The mechanism the detection feed (`GcMoveMap`) already uses — and a confirmed dead end for
range extraction.

- The in-proc `EventListener` hands back only the scalar move **Count**, never the `Values`
  struct-array. There is no supported in-proc API that surfaces the per-range structs.
- The one proposed escape hatch — a "raw" `EventListener` (`OnEventWrittenRaw` /
  `RawEventWrittenEventArgs` exposing `ReadOnlySpan<byte> Payload`/`Metadata` so the payload
  could be hand-parsed) — is **not shipped or public through .NET 10**; it exists only as a
  design doc ([raw-eventlistener.md][raw]), reachable today only by reflection. There is no
  supported in-proc workaround, which is why extraction must leave the managed listener.

## Recommendation

**Adopt Candidate 1: a native in-process CLR profiler using
`ICorProfilerCallback4::MovedReferences2`.** It delivers the exact `{old, new, len}` triples
as the runtime's own parallel arrays, fires only on real moves, at a fully-suspended-EE
fence, in the target's own address space — so it can call the already-landed `at_gc_remap`
**directly and synchronously**. This is strictly better on every axis (exactness,
completeness, latency, and structural fit) than the out-of-process EventPipe path the plan
assumed.

Two things must travel with the recommendation, unembellished:

1. **The `COR_PRF_MONITOR_GC` tradeoff.** Enabling GC monitoring disables background GC and
   forces blocking collections in the target. Deterministic fences help the remap, but this
   is an observable change to the target's GC behaviour/throughput and must be recorded as
   such.
2. **DR coexistence is unproven** — see the caveats. The recommendation is contingent on a
   go/no-go probe.

**Fallback:** if the profiler cannot coexist with DynamoRIO, fall back to Candidate 2 — a
separate helper process reads the diagnostics socket and feeds triples to the DR client over
the existing POSIX shm channel, with **no** co-load into the DR'd process (accepting the
drop/truncate risk, which then needs the coherence canary to catch).

## Probe result — **GO** (2026-07-14)

The single load-bearing gap below (DR-vs-profiler coexistence) was resolved by an empirical
probe, which **passed**. A minimal in-process CLR profiler
([examples/gcprofiler_probe/gcprofiler.cpp](../../../examples/gcprofiler_probe/gcprofiler.cpp))
implementing `ICorProfilerCallback4::MovedReferences2`, attached via the startup env vars
(`CORECLR_ENABLE_PROFILING`/`CORECLR_PROFILER`/`CORECLR_PROFILER_PATH`), was run against a
workload that forces compacting GCs (`gcmover`) BOTH natively and under
`drrun -c libasmtest_drtaint_client.so -- dotnet gcmover.dll`. Under DynamoRIO:

- the profiler `.so` loaded, `Initialize` ran, and `SetEventMask(COR_PRF_MONITOR_GC)`
  returned `S_OK`;
- **`MovedReferences2` fired the SAME number of times under DR as natively (120), delivering
  exact per-range `{old,new,len}` triples** (e.g. `{old=0x7ca…800028, new=0x7ca…000040,
  len=480}`) — complete delivery, not truncated by DR;
- the workload ran to completion (`HELLO_GC_MOVER`), the profiler got a clean `Shutdown`,
  exit code 0, **no `SIGSEGV`/`SIGTRAP`/crash/hang**.

So a CLR profiler coexists cleanly with a process under DynamoRIO on Linux, and the recommended
mechanism is confirmed end-to-end. Reproduced by `make dr-gcprofiler-probe` /
`make docker-gcprofiler-probe` (CoreCLR profiler headers fetched pinned from dotnet/runtime
`v8.0.8`). **Build gotchas worth recording** (they cost the probe time): the CoreCLR
`corprof.h`/`cor.h` + PAL compile standalone on Linux only with `-DPAL_STDCPP_COMPAT` (so the
PAL doesn't redefine glibc's `int8_t` etc.) plus the `HOST_UNIX`/`HOST_AMD64`/`BIT64` defines;
the `CINTERFACE` C-vtable path additionally needs `BEGIN_INTERFACE`/`END_INTERFACE`/`CONST_VTBL`
defined (the PAL omits them); and the profiler's `QueryInterface` MUST reject `ICorProfilerCallback5+`
(returning `E_NOINTERFACE`) — a permissive "return `this` for any IID" crashes, because the CLR
then vcalls a slot past the `Callback4` vtable. With those, the whole profiler is ~150 lines
(a generic-stub array-fill covers the ~90 unimplemented callback slots).

**Consequence:** Increment 7 can now adopt the profiler path with confidence — the shim's
`MovedReferences2` calls the already-landed `at_gc_remap` at the GC fence. The Candidate-2
EventPipe fallback is no longer needed unless a later constraint (e.g. not wanting
`COR_PRF_MONITOR_GC` to disable background GC) forces it.

## Caveats / open questions (honest)

- **SINGLE BIGGEST GAP — DR-vs-profiler coexistence is not verified.** No source confirmed
  that an `ICorProfiler` `.so` coexists cleanly with a process running under DynamoRIO on
  Linux. DR's code-cache, signal handling, thread-suspension, and TLS vs the CLR
  profiler-attach path are all plausible conflict surfaces. **This is inference, not a
  tested result, and it MUST be prototyped as a go/no-go probe** before the recommendation
  is load-bearing. A parallel task is building exactly this coexistence probe now, mirroring
  the Increment-2 extension-load-probe pattern
  ([dr-extension-load-probe-findings.md](dr-extension-load-probe-findings.md)). If they
  conflict, the fallback (Candidate 2, out-of-process) sidesteps the co-load entirely.
- **Server GC.** With multiple heaps and parallel collection (which persist even with
  background GC disabled), confirm `MovedReferences2` reports the **complete** set of
  per-heap moves before `GarbageCollectionFinished` fires — the remap must see every range,
  not a subset.
- **Overlapping ranges.** `[old]`/`[new]` ranges that overlap within a single event need
  memmove-style ordering; `at_gc_remap`'s source-snapshot + clear-then-paint already handles
  arbitrary overlap (T4 in the synthetic test), but the live multi-range case should be
  re-checked against this.
- **Source-artifact provenance.** The perfview nettrace sample proving TraceEvent surfaces
  the `Values` array is win-x64 / .NET Core 3.0 (the schema is unchanged through .NET 8, but
  it is not a Linux-.NET-8 artifact per se). The `ICorProfiler` API docs live under
  `learn.microsoft.com` `/framework/` URLs, but `corprof.idl` is shared and unchanged for
  CoreCLR / .NET 8.

## How it wires to the existing tier

The extraction mechanism plugs into work that is **already landed** — it supplies the one
missing input:

- The remap primitive exists: `at_gc_remap`
  ([dataflow_dr_client_inlined.c:431](../../../src/dataflow_dr_client_inlined.c#L431)),
  behind the disabled `ASMTEST_TAINT_GCREMAP` flag, already does the byte-granular
  snapshot/clear/paint over an arbitrary `{old,new,len}` and is proven by the
  synthetic-triple unit test. Nothing about the remap changes; the profiler only *calls* it.
  **Update 2026-07-21:** no longer behind the disabled flag — `at_gc_remap` is
  "now in the main taint build (not the disabled `ASMTEST_TAINT_GCREMAP` flag)
  because the LIVE path drives it"
  ([dataflow_dr_client_inlined.c:492-493](../../../src/dataflow_dr_client_inlined.c#L492)),
  driven by `at_gc_remap_live` at the GC fence (~:737).
- **The profiler shim reuses the co-loaded-native-`.so` pattern of**
  [taint_managed_shim.c](../../../examples/taint_managed_shim.c) — a native `.so` mapped into
  the launched .NET workload that owns plumbing the managed side cannot express. Where
  `taint_managed_shim.c` exports seed/sink markers and maps the shm results channel, the GC
  profiler shim registers `COR_PRF_MONITOR_GC` and, in its `MovedReferences2` callback,
  iterates the parallel arrays and calls `at_gc_remap(old, new, len)` per range **at the GC
  fence** — in-process, synchronous, no cross-process hop.
- The go/no-go gate is the **DR-vs-profiler coexistence probe** (being built in parallel).
  Only once that probe is green does the profiler path become the committed mechanism; until
  then the fallback EventPipe helper (feeding triples over the existing POSIX shm channel)
  remains the contingency, and Increment 7 stays at its landable partial slice (disabled-flag
  remap + synthetic-triple test).
  **Update 2026-07-21:** Increment 7 no longer sits at the partial slice —
  Increment 7 Slice 1+2 and Increment 9 have shipped (commits `e5b196f`,
  `75342d4`, `6e8ad4c`), with the live path driving the remap (see the note
  above).

## Sources

- [ICorProfilerCallback4::MovedReferences2][mr2]
- [ICorProfilerCallback::MovedReferences][mr1]
- [BOTR — CoreCLR profiling design (profiling.md)][botr]
- [Profiler Attach on CoreCLR][attach]
- [COR_PRF_MONITOR enumeration][mask]
- [Diagnostics client library][dcl]
- [EventPipe][ep]
- [perfview#1084 — GCHeapSurvivalAndMovement keyword (Maoni Stephens)][pv1084]
- [raw-eventlistener.md design doc][raw]

[mr2]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback4-movedreferences2-method
[mr1]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback-movedreferences-method
[botr]: https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/botr/profiling.md
[attach]: https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/profiling/Profiler%20Attach%20on%20CoreCLR.md
[mask]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/cor-prf-monitor-enumeration
[dcl]: https://learn.microsoft.com/en-us/dotnet/core/diagnostics/diagnostics-client-library
[ep]: https://learn.microsoft.com/en-us/dotnet/core/diagnostics/eventpipe
[pv1084]: https://github.com/microsoft/perfview/pull/1084
[raw]: https://github.com/dotnet/runtime/blob/main/docs/design/features/raw-eventlistener.md
