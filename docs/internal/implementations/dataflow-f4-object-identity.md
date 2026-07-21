# Data-flow F4: real object identity via GCBulkType/Node/Edge — implementation

> **Sources.** Actioned from
> [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md) (F4,
> "Full object identity via `GCBulkType`/`Node`/`Edge` is a further step"),
> [gc-move-range-extraction-findings.md](../analysis/gc-move-range-extraction-findings.md) and
> [f4-attach-profiler-probe-findings.md](../analysis/f4-attach-profiler-probe-findings.md).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources may be stale);
> if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

The landed F4 GC-move canonicalization keys managed memory def-use on **addresses**: it forwards
every pre-compaction address to the object's final resting place, and that correctly shipped —
it is not broken, and nothing here fixes it. What address identity *cannot* express is its one
documented residual: **a pre-window record touching memory that a GC then slides a live object
into aliases that object**, forging a def-use edge between two things that were never the same
object ([include/asmtest_valtrace.h:316](../../../include/asmtest_valtrace.h#L316) records this
as deferred). This work adds real **object** identity — a heap snapshot of
`{Address, Size, TypeID}` nodes from the runtime's GCBulkType/GCBulkNode/GCBulkEdge events,
joined with the already-landed `MovedReferences2` range feed — so each memory record keys on
*(object, offset)* where the snapshot has evidence, and degrades to the landed address identity
where it does not. The user-visible outcome: a live-attach managed capture can no longer
attribute a value to the wrong object across a compaction.

## What already exists (verified 2026-07-17)

Everything below was re-checked against the working tree on 2026-07-17.

- [src/dataflow_gcmove.c](../../../src/dataflow_gcmove.c) — the pure address canonicalizer:
  `asmtest_gcmove_canon` walks move batches ascending by step, applying **at most one
  relocation per batch** (the batch models one GC whose old ranges are disjoint);
  `asmtest_gcmove_canonicalize` rewrites every `AT_LOC_MEM_ABS` record's `addr` in place.
  Declared at [include/asmtest_valtrace.h:341,353](../../../include/asmtest_valtrace.h#L341).
- [include/asmtest_valtrace.h:316](../../../include/asmtest_valtrace.h#L316) — "identity via
  GCBulkType / Node / Edge is likewise deferred." A grep for `GCBulkType` across `src/`,
  `examples/`, `include/`, `cli/` finds **only** this comment: nothing of this feature exists yet.
- [examples/gccanon_attach/](../../../examples/gccanon_attach/) — the landed F4 lane this doc
  extends:
  - [gccanonprof.cpp](../../../examples/gccanon_attach/gccanonprof.cpp) — attach-mode
    `ICorProfilerCallback4` profiler (CINTERFACE C-vtable, strict QI below Callback5,
    `InitializeForAttach` entry). Samples S0 = the tracer's live step counter at
    `GarbageCollectionStarted`, records each **relocating** `MovedReferences2` range stamped
    with that S0. Drops ranges when the tracer is not live (`!g_cur_traced || g_cur_s0 == 0`,
    line 180) — a rule T2 relaxes.
  - [gccanon_shm.h](../../../examples/gccanon_attach/gccanon_shm.h) — the probe-local shm
    channel: `gccanon_move_t {old_base,new_base,len,step,gc_seq}`, `gccanon_gcinfo_t` fence
    samples, `GCCANON_MAX_MOVES 65536`.
  - [gccanon_tracer.c](../../../examples/gccanon_attach/gccanon_tracer.c) — drives the shipping
    scoped L0 producer (`asmtest_dataflow_ptrace_attach_jit`, re-declared locally because the
    producer ships no header), mirrors `steps_len`/`recs_len` into shm, drains the stamped
    feed, chains same-boundary GCs (`gccanon_compose`), and asserts: negative control (edge
    missing raw), positive (edge appears canonicalized), false-alias check, freeze witness,
    differential oracle, plus a pure `--selftest`.
  - [victim/Program.cs](../../../examples/gccanon_attach/victim/Program.cs) — plain `dotnet`
    victim (no `CORECLR_*` env): `Region(obj, v)` = `Volatile.Write(ref obj[0], v)` →
    `Park()` call-out (the GC lands here) → `Volatile.Read(ref obj[0])`; a driver thread
    choreographs `GCCANON_GCS_PER_WINDOW` compacting gen2 GCs into the window and prints its
    own ground truth (`GCCANON_VICTIM_ROUND`/`_WINDOW` lines).
  - [attacher/](../../../examples/gccanon_attach/attacher/) — `DiagnosticsClient.AttachProfiler`
    over the diagnostics socket; pins `Microsoft.Diagnostics.NETCore.Client` **0.2.510501** in
    `attacher.csproj`.
- Make targets: `gccanon-attach` at
  [mk/native-trace.mk:3093](../../../mk/native-trace.mk#L3093) (parse-time `GCCANON_MISSING`
  tool gate; fetches pinned CoreCLR headers `GCPROBE_RT_TAG ?= v8.0.8`,
  [mk/native-trace.mk:1368](../../../mk/native-trace.mk#L1368); `GCCANON_PHASES ?= 1 2 3`);
  `docker-gccanon-attach` at [mk/docker.mk:376](../../../mk/docker.mk#L376) (builds
  [Dockerfile.gccanon-attach](../../../Dockerfile.gccanon-attach), runs with
  `--cap-add=SYS_PTRACE`).
- Pure-suite wiring pattern to mirror: `test_dataflow_gcmove` — explicit link rule at
  [mk/dataflow.mk:306](../../../mk/dataflow.mk#L306), run by the `dataflow-test` aggregate
  ([mk/dataflow.mk:369-376](../../../mk/dataflow.mk#L369)), excluded from `make test` via
  `SUITE_EXCLUDES` ([Makefile:60-69](../../../Makefile#L60)).
- Precedent only, do **not** modify: the DR taint tier's in-process fence remap
  `at_gc_remap_live` ([src/dataflow_dr_client_inlined.c:737](../../../src/dataflow_dr_client_inlined.c#L737))
  — it shows a profiler feed driving a live remap, but that tier is in-process; this tier is
  out-of-process and post-pass.

**Prove the baseline green before touching anything.** On a Linux x86-64 Docker host:

```
make docker-gccanon-attach
```

Expected observable output: `GCCANON_SELFTEST fail=0` with plan `1..5`; then three phases
(1, 2, 3 GCs per window) each ending `GCCANON_SUMMARY ... fail=0`, with plans `1..10`,
`1..11`, `1..11` — 37 assertions total. Also run `make dataflow-test` on any host: every pure
suite line including `test_dataflow_gcmove` must pass. (This doc's authoring host is macOS;
the lane itself needs a Linux x86-64 Docker host — see Constraints.)

## The design in one paragraph

A heap snapshot taken **after** the capture gives the set of live objects as
`{addr, size, type_id}` nodes in the *snapshot* address space. The already-landed
`MovedReferences2` feed — extended to keep recording after the capture ends (T2) — supplies
every compaction between the first trace step and that snapshot. A new pure transform (T1) can
therefore compute, for any record `(step s, raw address x)`, **which node occupied `x` at step
`s`** by inverse-walking each node's snapshot address back through every move batch with
`step > s`. Records with an owner are keyed `(node.addr + offset)` — object identity, equal to
the landed forward canonicalization for genuinely-owned bytes. Records with **no** owner whose
forwarded address collides with a node's snapshot range are the false-alias case; they are
re-keyed into a reserved space (bit 63 set — never a canonical Linux x86-64 user address) so
they can no longer alias the object. Everywhere the snapshot is silent, the result is
byte-identical to the landed address path: object identity *refines* address identity, it never
replaces it.

## Tasks

### T1 — Pure transform: object-identity canonicalization (`asmtest_objid_*`)  (M, depends on: none)

**Goal.** A pure, host-portable pass that re-keys `AT_LOC_MEM_ABS` records on
*(object, offset)* given a node table + move set, provably degrading to
`asmtest_gcmove_canonicalize` when the node table is empty.

**Steps.**
1. Declare the API in [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h)
   directly after the Phase-4 increment-2 block (after line 355), as a new
   "Phase 4 (increment 4) — object identity" comment block written in the same voice as the
   increment-2 block at lines 285-317.
2. Implement in a new `src/dataflow_objid.c`, mirroring
   [src/dataflow_gcmove.c](../../../src/dataflow_gcmove.c)'s structure (pure C, no Capstone,
   no Unicorn, `#include "asmtest_valtrace.h"` only).
3. Add the unit suite `examples/test_dataflow_objid.c` and wire it exactly as
   `test_dataflow_gcmove` is wired: explicit link rule in
   [mk/dataflow.mk](../../../mk/dataflow.mk) next to line 306 (link `dataflow.o` +
   `dataflow_gcmove.o` + `dataflow_objid.o` + the test object — objid calls gcmove's canon),
   add `$(BUILD)/test_dataflow_objid` to the `dataflow-test` aggregate's prerequisites and run
   lines (lines 369-376), and add `test_dataflow_objid` to `SUITE_EXCLUDES` in
   [Makefile](../../../Makefile) (~line 67).
4. `make dataflow-test` after each step; `make fmt` before finishing.

**Code.**

```c
/* One heap-snapshot node — the shape of a GCBulkNode entry (Address/Size/TypeID;
 * EdgeCount is a stream-consistency check for the reader, not identity input).
 * `addr` is in the SNAPSHOT space: the address space after EVERY batch in the
 * accompanying move set has been applied. */
typedef struct asmtest_gcnode {
    uint64_t addr;
    uint64_t size;
    uint64_t type_id; /* BulkType join key; 0 = unknown */
} asmtest_gcnode_t;

/* Inverse of asmtest_gcmove_canon: map a SNAPSHOT-space address back to where it
 * was at `step`. Walk batches DESCENDING by step; for each batch with
 * batch.step > step, invert the at-most-one range whose NEW span covers cur.
 * Precondition: within a batch the NEW ranges are disjoint (see below). */
uint64_t asmtest_objid_locate(const asmtest_gcmove_t *moves, size_t nmoves,
                              uint32_t step, uint64_t snap_addr);

/* The node that contained raw_addr at `step`: the unique i with
 * locate(nodes[i].addr) <= raw_addr < locate(nodes[i].addr) + nodes[i].size.
 * Returns 0 and fills *owner/*offset, or -1 when no node owned the byte. */
int asmtest_objid_owner(const asmtest_gcnode_t *nodes, size_t nnodes,
                        const asmtest_gcmove_t *moves, size_t nmoves,
                        uint32_t step, uint64_t raw_addr,
                        size_t *owner, uint64_t *offset);

#define ASMTEST_OBJID_NOOBJ (1ull << 63) /* re-key tag; see below */

/* Rewrite every AT_LOC_MEM_ABS record in place:
 *   owner found            -> addr = nodes[owner].addr + offset
 *   no owner, no collision -> addr = asmtest_gcmove_canon(moves, ..., addr)
 *   no owner, canon(addr) inside some node's [addr, addr+size)
 *                          -> addr = canon(addr) | ASMTEST_OBJID_NOOBJ
 * Returns count of records whose addr changed, (size_t)-1 on NULL trace. */
size_t asmtest_objid_canonicalize(asmtest_valtrace_t *v,
                                  const asmtest_gcnode_t *nodes, size_t nnodes,
                                  const asmtest_gcmove_t *moves, size_t nmoves);
```

Semantics to preserve, spelled out:
- **The owner branch equals the landed forward canon for owned bytes** (an in-object address
  forwards with its object: a move range never splits one object, the affine per-range map is
  the runtime's own `newObjectID = NewRangeBase + (old − OldRangeBase)` formula). So a true
  edge that survives today's `asmtest_gcmove_canonicalize` survives objid unchanged.
- **The no-owner/no-collision branch is exactly today's behavior** — dead-object memory, stack,
  native memory all keep the address-identity floor.
- **The re-key tag is safe** because canonical Linux x86-64 user-space addresses have bit 63
  clear and `AT_LOC_MEM_ABS` records from the ptrace producer are user-space effective
  addresses of the tracee; `asmtest_defuse_build` compares keys for equality only, so a tagged
  key can never collide with an untagged one, and two no-owner records sharing a canon address
  share a tagged key (their mutual edges survive).
- **Fail closed on the inverse walk's precondition**: `locate` needs NEW ranges disjoint within
  a batch. Physically true for a single GC (two live objects cannot be moved onto each other),
  but a *composed* batch (the lane pre-chains same-boundary GCs) can legitimately emit
  overlapping news when an object dies between the window's GCs and a later GC slides another
  object into its image. Mirror `rvec_disjointify`'s running-max policy from
  [gccanon_tracer.c:274-301](../../../examples/gccanon_attach/gccanon_tracer.c#L274) but on the
  **new** spans, dropping both sides *for the inverse walk only* — an undecidable owner becomes
  "no owner" (a conservative miss, address-identity floor), never a guessed identity.

**Tests.** `examples/test_dataflow_objid.c`, TAP-style like
[examples/test_dataflow_gcmove.c](../../../examples/test_dataflow_gcmove.c):
1. *Round-trip*: for synthetic multi-batch move sets, `locate(canon(x, s), s) == x` for every
   x inside old ranges, both sides of each boundary.
2. *Owner*: object at A, moved A→B at step 5 — owner of A at step 4 is the node (offset
   preserved); owner of A at step 5 is none; owner of B at step 5 is the node.
3. *The false alias, killed*: node O with snapshot range covering X; O's location at step 2 is
   elsewhere; a write record at (step 2, X) and a read record at (step 7, X). Assert first that
   `asmtest_gcmove_canonicalize` leaves both keys equal (the bug is real — this is the
   suite-level negative control), then that `asmtest_objid_canonicalize` keys the write
   `X | ASMTEST_OBJID_NOOBJ` and the read `O.addr + (X − O.addr_at_step7... ) == O.addr + off`,
   and that `asmtest_defuse_build` over the objid trace has no edge between them while the
   gcmove trace has one.
4. *Degradation differential*: randomized traces + move sets, `nnodes == 0` — every record's
   post-objid `addr` byte-identical to a `gcmove_canonicalize` copy.
5. *New-overlap conservative drop*: a composed batch with overlapping new spans — owner lookup
   declines (no owner), never returns either candidate.

A failure prints `not ok N - ...` and the binary exits non-zero; a pass prints `ok N` per case
and `make dataflow-test` stays green end to end.

**Docs.** Header comment block in `asmtest_valtrace.h` (part of this task). No changelog yet —
the feature is user-visible only once the lane lands (T6).

**Done when.**
- `make dataflow-test` passes on the dev host with the new suite listed and green.
- Deleting the re-key branch (mutation) fails exactly test 3; deleting the disjointify fails
  exactly test 5; the differential (test 4) passes unmodified.

### T2 — Record post-capture moves in the lane's profiler feed  (S, depends on: none)

**Goal.** The profiler keeps stamping relocating ranges after the capture ends, so the
snapshot space (T3) is reachable from the trace space through one move set.

**Steps.**
1. [examples/gccanon_attach/gccanon_shm.h](../../../examples/gccanon_attach/gccanon_shm.h):
   add `uint32_t flags;` to `gccanon_move_t` (`#define GCCANON_MOVE_POST 1u`) and a
   `volatile uint32_t post_moves_total;` channel counter. The header is probe-local ("a lane's
   channel, not an API" — its own words), so the layout change is free; both sides are rebuilt
   together by the lane.
2. [gccanonprof.cpp](../../../examples/gccanon_attach/gccanonprof.cpp): in `Prof_GCStarted`,
   also sample `g_cur_done = g_ch->tracer_done`. In `Prof_MovedReferences2`, replace the drop
   rule at line 180: record with `flags=0` when `(g_cur_traced && g_cur_s0 != 0)` as today, OR
   with `flags=GCCANON_MOVE_POST` when `(!g_cur_traced && g_cur_done && g_cur_s0 != 0)` —
   `g_cur_s0` is then the tracer's final `steps_len`, frozen in shm, which is strictly greater
   than every record's step, i.e. "after the whole trace", which is exactly what a
   trace-to-snapshot translation move means. Bump `post_moves_total`.
3. [gccanon_tracer.c](../../../examples/gccanon_attach/gccanon_tracer.c): swap lines 1032-1035
   so `g_ch->tracer_done = 1;` is published **before** `g_ch->magic = 0;` (otherwise a GC in
   the gap is invisible to both rules). In the drain (line 1060ff), partition on `flags`:
   everything that exists today — `moves`, `gccanon_compose`, assertions 1-11 — consumes
   `flags==0` records **only**, so phase 1's `collapsed_groups == 0` assertion (check 8) and
   every other landed assertion are untouched. Keep the full set aside for T5. Print the new
   counter in the `GCCANON_FEED` line.
4. `make docker-gccanon-attach` — all 37 landed assertions must stay green.

**Code.** As above; note that multiple post-capture GCs share the S_end stamp and are therefore
chained by the **existing** `gccanon_compose` in `gc_seq` order — no new composition machinery.

**Tests.** No new assertion yet (nothing consumes the post moves until T5); the observable is
the `GCCANON_FEED` line now reporting `post_moves=<n>` with n > 0 in every phase (the victim
keeps fragmenting after the capture), and the full lane staying at 37 green.

**Docs.** Internal-only: the shm header's own comment block documents the new flag (extend the
`gc_seq` commentary). No user-facing docs — probe-local channel.

**Done when.**
- `make docker-gccanon-attach` → `GCCANON_SELFTEST fail=0` + three `fail=0` summaries
  (unchanged 37), and each phase's `GCCANON_FEED` line shows a non-zero `post_moves`.

### T3 — The heap-snapshot dumper: EventPipe GCBulkType/Node/Edge  (M, depends on: none)

**Goal.** A dotnet console tool that forces the heap dump on a live pid, parses
GCBulkNode/GCBulkEdge/BulkType fail-closed, and emits a plain-text node table.

**Steps.**
1. Create `examples/gccanon_attach/dumper/` (`dumper.csproj` + `Program.cs`), mirroring
   [attacher/attacher.csproj](../../../examples/gccanon_attach/attacher/attacher.csproj)
   (net8.0, explicit `AssemblyName gccanon_dumper`, header comment in the same style).
   Package pins: `Microsoft.Diagnostics.NETCore.Client` **0.2.510501** (the attacher's
   existing pin) and `Microsoft.Diagnostics.Tracing.TraceEvent` **3.2.5** (latest stable,
   MIT — verified on nuget.org 2026-07-17). NuGet pins live in the csproj, exactly as the
   attacher's does; `scripts/third-party-digests.txt` covers curl'd tarballs, not NuGet, so no
   digest line (same posture as the attacher).
2. Implement `gccanon_dumper <pid> <out-file> [timeout-s]`:
   - `new DiagnosticsClient(pid).StartEventPipeSession(...)` with one provider:
     `Microsoft-Windows-DotNETRuntime`, Informational, keywords **0x1980001** — TraceEvent's
     `ClrTraceEventParser.Keywords.GCHeapSnapshot` = `GC | GCHeapCollect (0x800000) |
     GCHeapDump (0x100000) | GCHeapAndTypeNames (0x1000000) | Type (0x80000)`. **Session
     enable with the `ManagedHeapCollect` 0x800000 keyword is what forces the full blocking
     induced gen2 GC**; `GCHeapDump` alone forces nothing, and the bulk node/edge events fire
     only during that forced GC. Mirror
     `dotnet-gcdump`'s `EventPipeDotNetHeapDumper` (see Research notes).
   - Parse the session stream with `EventPipeEventSource` + the Clr parser. Select the dump GC
     by `GCStart` with `Depth >= 2 && Type == NonConcurrentGC && Reason == Induced`; process
     until the matching `GCStop` (PerfView's `DotNetHeapDumpGraphReader` selection rule).
   - Consume `GCBulkNode` (`Values[]{Address, Size, TypeID, EdgeCount}`) and `GCBulkEdge`
     entries **hard-failing on any `Index` gap** and on a leftover/short edge stream
     (sum of `EdgeCount` must equal total edge entries). A dropped EventPipe buffer must be a
     red run, never a silently short node table — the house fail-closed rule. On failure,
     retry the whole dump (fresh session → fresh forced GC) up to 3 times, then exit non-zero.
   - Collect `BulkType` (`Values[]{TypeID, ..., Name, ...}`) over the whole session (it is
     emitted outside the GC window too) and join `node.TypeID == BulkType.TypeID` for names.
   - Ignore `GCBulkEdge.ReferencingFieldID` — it is hardcoded 0 in the runtime ("FUTURE").
   - Write `<out-file>`: one header `GCCANON_NODES count=<N> edges=<E> types=<T> pid=<pid>`,
     then per node `node 0x<addr> 0x<size> 0x<typeid> <typename-or-?>`, then a final
     `GCCANON_NODES_END` sentinel line (the tracer polls for a complete file). Print a
     `GCCANON_DUMP nodes=N edges=E index_gaps=0 reason=Induced` summary to stdout.
3. Sanity-check the event payload shapes against the **pinned** runtime rather than trusting
   this doc: the lane already fetches `dotnet/runtime` v8.0.8 sparsely; extend the sparse
   checkout set in the `gccanon-attach` recipe
   ([mk/native-trace.mk:3104-3106](../../../mk/native-trace.mk#L3104)) with `src/coreclr/vm`
   and diff the five event definitions in `src/coreclr/vm/ClrEtwAll.man` against Research
   notes (they are version 0, unchanged since introduction — but verify at v8.0.8, since the
   research read `main`).
4. Build inside the lane's container:
   `dotnet build -c Release examples/gccanon_attach/dumper/dumper.csproj -o $(BUILD)/gccanon_dumper_out`,
   giving `$(BUILD)/gccanon_dumper_out/gccanon_dumper.dll`. Run it once by hand against a running
   `gccanon_victim` before wiring (T5). The lane already builds the victim to
   `$(BUILD)/gccanon_victim_out/gccanon_victim.dll`
   ([mk/native-trace.mk:3119](../../../mk/native-trace.mk#L3119)), so no separate victim build is
   needed for this check.

**Code.** As above. Keep the tool single-file and dependency-light; no shm — its only output
is the node-table file and its exit code.

**Tests.** Standalone acceptance (pre-T5): inside the `asmtest-gccanon-attach` container,
start a victim (`GCCANON_VICTIM_SECONDS=90 dotnet $(BUILD)/gccanon_victim_out/gccanon_victim.dll &`),
run `dotnet $(BUILD)/gccanon_dumper_out/gccanon_dumper.dll <pid> /tmp/nodes.txt 60`, observe
`GCCANON_DUMP nodes=<N> (N>0) ... index_gaps=0 reason=Induced`, a node table whose count matches
the header, at least one
node whose typename contains `Int64[]` (the victim's `long[]` objects), and the victim's log
gaining one extra induced gen2 (`GC seq=...` line from the profiler if attached). Failure looks
like a non-zero exit with the index-gap or no-induced-GC message.

**Docs.** Internal-only (a lane tool). The csproj header comment carries the design note.

**Done when.**
- `dotnet build` of the dumper succeeds in the lane image (network restore, same SKIP posture
  as the attacher when NuGet is unreachable).
- A manual run against a live victim produces a complete node table with `index_gaps=0` and a
  joined `Int64[]` typename, and forces exactly one induced gen2 GC in the victim.

### T4 — Spike: pin the snapshot-space convention  (S, depends on: T2, T3)

**Goal.** Decide, by measurement, whether GCBulkNode addresses are pre- or post-relocation
relative to the dump GC's **own** `MovedReferences2` ranges — i.e. whether the dump GC's
ranges belong in the trace-to-snapshot translation set.

Why this is genuinely open: the PerfView reader treats node addresses as stable inside the
blocking induced GC and never consults the moved-range events; but
[gc-move-range-extraction-findings.md](../analysis/gc-move-range-extraction-findings.md)
measured that `MovedReferences2` fires **before** physical relocation. Whether the profiler
walk that feeds GCBulkNode runs before or after the dump GC's compaction decides the
convention, and no source in the research states it outright. House rule: measure, don't
assume.

**Steps.**
1. With T2 and T3 in place, run: victim → attach profiler → tracer capture (any phase) →
   dumper. The profiler stamps the dump GC's own ranges as post-capture moves (it is a traced
   GC by the T2 rule — `tracer_done` is set); the victim's `GCCANON_VICTIM_ROUND` lines report
   the sentinel object's pinned data address before and after every round, bracketing the dump.
2. Find the sentinel object's node in the table (the node whose range covers one of the
   victim-reported addresses; typename `Int64[]`). Exactly one of these holds:
   - node.addr matches the victim's address **after** the dump GC → nodes are post-move →
     the dump GC's ranges **belong** in the translation set;
   - node.addr matches the address **before** it → nodes are pre-move → the dump GC's own
     ranges must be **excluded** (T5 filters the last post-capture `gc_seq` out).
3. Record the measured answer in a new findings note
   `docs/internal/analysis/f4-objid-snapshot-space-findings.md`, following the sibling
   probe-findings format (verdict line, evidence lines, reproduce command), and hard-code the
   convention in the tracer with a comment citing the note.
4. Keep the check alive forever: T5's known-object assertion (the sentinel object's owner
   lookup must succeed on the traced store/load) fails loudly if a future runtime flips the
   convention.

**Code.** None beyond a temporary flag in the tracer while measuring; the permanent artifact
is the findings note + the chosen filter in T5.

**Tests.** The spike *is* the test; its reproduce command goes in the findings note.

**Docs.** The findings note (internal analysis doc — the established pattern for spikes).

**Done when.**
- The findings note exists with a GO verdict on one convention, evidence quoting the victim's
  own addresses and the node table, and a reproduce command.

### T5 — The lane: alias fixture, objid join, and the assertion battery  (L, depends on: T1, T2, T3, T4)

**Goal.** On a live attach, the false def-use edge that address identity forges is reproduced
as a failing case and eliminated by object identity, while every landed assertion and the true
edge stay green.

**Steps.**
1. **Victim — the alias fixture** ([victim/Program.cs](../../../examples/gccanon_attach/victim/Program.cs)),
   new mode `GCCANON_ALIAS_FIXTURE=1`:
   - New traced region:
     ```csharp
     [MethodImpl(MethodImplOptions.NoInlining)]
     static long RegionAlias(long[] doomed, long[] live, long v)
     {
         Volatile.Write(ref doomed[0], v);      // STORE at X — into the object about to die
         long p = Park();                       // the GC collects `doomed`, slides `live` to X
         return Volatile.Read(ref live[0]) + p; // LOAD at X — a DIFFERENT object
     }
     ```
     `doomed`'s last use is the store, so the argument reference is dead across `Park()`
     (CoreCLR's precise liveness) once the driver has also nulled its static.
   - Driver choreography (extends `Fragment()`'s ordering discipline, whose comment explains
     why order is everything): allocate `doomed = new long[8]`, then **immediately**
     `live = new long[8]` (consecutive allocations, contiguous), promote both to gen2 with a
     forced compacting collect, seed `live[0] = Sentinel3`, null the `doomed` static, and put
     exactly **one** compacting gen2 GC in the window with **no other garbage dropped**: the
     only hole is `doomed`'s span, so the compaction slides `live` — same element type, same
     header size — exactly onto `doomed`'s start: `&live[0]` after == `&doomed[0]` before == X.
   - The victim asserts its own choreography per round and prints
     `GCCANON_VICTIM_ALIAS ok=<0|1> x=0x<X> live_new=0x<addr>`; rounds with `ok=0` are
     re-rolled (same posture as the existing `moved=1` filtering).
   - Add `Sentinel2` (the store value) and `Sentinel3` (the pre-seeded load value) constants;
     mirror them in the tracer as `GCCANON_SENTINEL2/3` next to `GCCANON_SENTINEL`
     ([gccanon_tracer.c:69](../../../examples/gccanon_attach/gccanon_tracer.c#L69)).
2. **Tracer — node table + objid pass** ([gccanon_tracer.c](../../../examples/gccanon_attach/gccanon_tracer.c)):
   - New argv: `gccanon_tracer <pid> <tid> <method> <timeout> <expect-gcs|alias> <nodes-file>`.
     After the capture completes (after `tracer_done`), print `GCCANON_TRACER_WAIT_NODES` and
     poll for the nodes file's `GCCANON_NODES_END` sentinel (timeout 60 s → a `not ok`, not a
     skip — the dumper is installable, so its absence is a failure by the CLAUDE.md rule).
   - Parse the node table into `asmtest_gcnode_t[]`. Build the translation move set = composed
     `flags==0` moves + composed post-capture moves (filtered per T4's convention), via the
     existing `gccanon_compose`.
   - **Numeric phases (1/2/3), two added assertions each:** (a) *known-object calibration* —
     the sentinel store/load records' `asmtest_objid_owner` succeeds and both key to the same
     `node.addr + 0`, and the node's typename is `Int64[]` (this is also T4's permanent guard);
     (b) *preservation* — `asmtest_objid_canonicalize` on a fresh restore of the snapshotted
     addresses (reuse the existing `addr_snap[]` restore machinery at lines 1119-1129 and
     1269-1270) followed by `asmtest_defuse_build` still contains the true store→load edge the
     landed positive (check 6) proved, now at the node-based key.
   - **Alias phase, four assertions:** (1) fixture provocation — a `Sentinel2` 8-byte MEM_ABS
     write and a `Sentinel3` 8-byte MEM_ABS read found, `store.step < load.step`, and
     `store.addr == load.addr` (the aliasing signature X); (2) NEGATIVE CONTROL — after
     `asmtest_gcmove_canonicalize` over the window moves, `asmtest_defuse_build` **contains**
     the store→load edge at one key: address identity forges the false edge, the bug is real
     and live; (3) THE POSITIVE — restore addresses, run `asmtest_objid_canonicalize` with the
     node table + translation set: the store keys `... | ASMTEST_OBJID_NOOBJ`, the load keys
     the live object's node, and the edge is **gone**; (4) no collateral damage — the alias
     trace's remaining edge count and every register edge are identical between the gcmove and
     objid builds (objid must remove exactly the false edge, nothing else).
3. **Make wiring** ([mk/native-trace.mk](../../../mk/native-trace.mk)): build the dumper next
   to the attacher build (line 3122) with the same SKIP-on-restore-failure posture; extend
   `run_phase` to watch the tracer log for `GCCANON_TRACER_WAIT_NODES` and then run the dumper
   against the still-live victim (the victim's 90 s budget covers it; the kill stays after the
   tracer exits); set `GCCANON_PHASES ?= 1 2 3 alias`, with `run_phase` treating the token
   `alias` as `GCCANON_GCS_PER_WINDOW=1 GCCANON_ALIAS_FIXTURE=1` plus the tracer's `alias`
   argument. Pass the nodes-file path (under `$(BUILD)`) to both sides; `rm -f` it per phase
   next to the shm cleanup.
4. **Docker** ([Dockerfile.gccanon-attach](../../../Dockerfile.gccanon-attach)): no new apt
   packages (dotnet-sdk-8.0 covers the dumper; network at run time is already this lane's
   posture for headers and NuGet). Update the header comment to describe the objid phase.
5. Run `make docker-gccanon-attach` twice consecutively; both runs fully green.

**Code.** As above; the tracer changes stay in the lane (no shipping-source change — the tier
pattern this lane already follows: it links the shipping transform, it never edits it).

**Tests.** The lane is the test. New totals: selftest `1..5` unchanged; phases 1/2/3 gain two
checks each (`1..12`, `1..13`, `1..13`); alias phase `1..4` plus the shared checks it reuses —
print the phase's own plan and keep `GCCANON_SUMMARY ... fail=0` as the machine-checkable
outcome. A regression in the landed behavior shows as one of the original 37 flipping; a
failure of this increment shows as alias-(2) passing while alias-(3) fails (transform wrong) or
alias-(1) failing (fixture did not provoke — re-roll logic broken).

**Docs.** Internal-only at this stage; T6 carries the visible record.

**Done when.**
- `make docker-gccanon-attach` on a Linux x86-64 Docker host: `GCCANON_SELFTEST fail=0`, four
  phase summaries `fail=0`, and the alias phase's negative control explicitly shows the false
  edge present under address identity before objid removes it.
- Two consecutive runs agree (the landed lane's own reproducibility bar).
- On a host missing dotnet/git/capstone, `make gccanon-attach` still prints the parse-time
  `SKIP: not found: ...` line and `1..0 # skipped` (unchanged gate).

### T6 — Retire the deferral, update the plan, changelog  (S, depends on: T1-T5)

**Goal.** The tree stops describing object identity as deferred, and the record says what
landed.

**Steps.**
1. [include/asmtest_valtrace.h:316](../../../include/asmtest_valtrace.h#L316): replace
   "Full object identity via GCBulkType / Node / Edge is likewise deferred." with a sentence
   pointing at the Phase-4 increment-4 block T1 added (the live feed is the gccanon lane; the
   pure transform is `asmtest_objid_canonicalize`).
2. [docs/internal/archive/plans/live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
   F4 section: append an `UPDATE` block in the section's established voice — the further step
   landed; the address-identity residual ("a pre-window record touching memory that a GC then
   slides a live object into aliases that object") is retired by the objid pass; cite the lane
   phase and the snapshot-space findings note. Do **not** rewrite the landed increments'
   history (they correctly shipped address identity — that framing is binding).
3. [CHANGELOG.md](../../../CHANGELOG.md) `## [Unreleased]` under `### Added`: one entry —
   real object identity for managed memory def-use on the live-attach tier
   (GCBulkNode heap snapshot joined with the MovedReferences2 feed; false-alias edge
   reproduced live and eliminated; `make docker-gccanon-attach`).
4. `make docs` (or `make docker-docs`) to prove the Sphinx build stays warning-free —
   `docs/internal/**` is excluded from the published build, but the changelog is not.

**Code.** Comment/doc text only.

**Tests.** `make dataflow-test` and `make docker-gccanon-attach` unchanged-green after the
comment edit (it is load-bearing prose, not code). `make docs` exits 0.

**Docs.** This task *is* the docs task. No user-facing Sphinx page exists for the valtrace
API today (grep for `gcmove`/`dataflow` over `docs/*.rst` returns nothing), so changelog +
header comment is the whole user-visible surface — record that reason in the commit message.

**Done when.**
- `grep -rn "likewise deferred" include/` returns nothing.
- The changelog entry exists under `[Unreleased]`; `make docs` passes.

## Task order & parallelism

- **Independent starts:** T1 (pure transform), T2 (profiler feed), T3 (dumper) touch disjoint
  files and can proceed concurrently as three workstreams.
- **Ordered:** T4 needs T2+T3 (it measures with both halves live). T5 needs everything before
  it. T6 is last.
- **Critical path:** T3 → T4 → T5 → T6 (the dumper and the convention spike gate the lane).
  T1 is off the critical path until T5 consumes it — start it first anyway; it is the piece
  with host-portable tests and the most design risk.

```
T1 ──────────────┐
T2 ──┐           ├─→ T5 ─→ T6
T3 ──┴─→ T4 ─────┘
```

## Constraints & gates

- **No hardware or credential gate.** The lane runs on any Linux x86-64 Docker host
  (`make docker-gccanon-attach`, `--cap-add=SYS_PTRACE`). Nothing here may self-skip for a
  missing installable dependency (CLAUDE.md); the only tolerated skips are the lane's existing
  parse-time tool gate on a bare host and its established no-network SKIP for the CoreCLR
  header fetch / NuGet restore.
- **Pins.** `dotnet/runtime` headers at `GCPROBE_RT_TAG ?= v8.0.8`
  ([mk/native-trace.mk:1368](../../../mk/native-trace.mk#L1368)) — reuse, do not add a second
  fetch; `Microsoft.Diagnostics.NETCore.Client` 0.2.510501 (existing);
  `Microsoft.Diagnostics.Tracing.TraceEvent` 3.2.5, MIT (new — csproj pin, the attacher's
  pattern; `scripts/third-party-digests.txt` is for curl'd tarballs and is not extended here).
- **CoreCLR only.** On Mono/MAUI a separate `MonoProfiler` provider (its own GCHeapDump
  keyword and `GCHeapDumpObjectReference` events) replaces the CoreCLR bulk events; the lane's
  victim is CoreCLR .NET 8, and Mono is explicitly out of scope — record it, don't gate on it.
- **Shipping F4 is untouched.** `src/dataflow_gcmove.c` and its one-relocation-per-batch rule
  are load-bearing and stay exactly as landed; objid is additive. The landed F4 correctly
  shipped address identity — no wording anywhere may describe it as a defect being fixed.
- **The dump perturbs the target** (one forced blocking gen2 GC per capture, after the traced
  window). It runs post-detach, so the capture itself is unperturbed; state it in the lane
  output rather than hiding it.
- If a gate blocks validation (e.g. no Linux x86-64 Docker host available), record the exact
  command that could not run and its expected output in the PR description; do not merge the
  lane phases unvalidated.

## Research notes (verified 2026-07-17)

Event definitions (primary source — the manifest is the effective spec; Microsoft Learn omits
these payloads): `dotnet/runtime` `src/coreclr/vm/ClrEtwAll.man`
(<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/ClrEtwAll.man>), read from `main`
@2026-07; all five events are version 0 with payloads unchanged since introduction (T3 step 3
re-verifies at the pinned v8.0.8):

- Keywords: `GCHeapDump` 0x100000, `GCHeapSurvivalAndMovement` 0x400000, `Type` 0x80000,
  `ManagedHeapCollect` 0x800000, `GCHeapAndTypeNames` 0x1000000.
- `BulkType` id 15, keyword **Type** (not GCHeapDump): `Count u32, ClrInstanceID u16,
  Values[Count]{TypeID u64, ModuleID u64, TypeNameID u32, Flags u32, CorElementType u8,
  Name UnicodeString, TypeParameterCount u32, TypeParameters[] u64}`.
- `GCBulkNode` id 18, GCHeapDump: `Index u32, Count u32, ClrInstanceID u16,
  Values[Count]{Address ptr, Size u64, TypeID u64, EdgeCount u64}`.
- `GCBulkEdge` id 19, GCHeapDump: `Index, Count, ClrInstanceID,
  Values[Count]{Value ptr, ReferencingFieldID u32}` — `ReferencingFieldID` is hardcoded 0
  ("// FUTURE") in `eventtrace_gcheap.cpp`
  (<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/eventtrace_gcheap.cpp>).
- `GCBulkSurvivingObjectRanges` id 21 / `GCBulkMovedObjectRanges` id 22 under
  GCHeapSurvivalAndMovement: `{RangeBase, RangeLength}` / `{OldRangeBase, NewRangeBase,
  RangeLength}`.

Trigger semantics: `eventtrace.cpp` (~line 2462, `main` @2026-07 — line will drift) forces a
full blocking GC iff public provider + `CLR_MANAGEDHEAPCOLLECT_KEYWORD` (0x800000) on
enable/capture-state (an EventPipe session enable qualifies) → `ETW::GCLog::ForceGC`.
GCHeapDump alone does **not** force a GC, and bulk node/edge fire only during the forced GC
(`s_forcedGCInProgress && GCHeapDump`).
<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/eventtrace.cpp>.
`dotnet-gcdump` uses TraceEvent `Keywords.GCHeapSnapshot = GC | GCHeapCollect | GCHeapDump |
GCHeapAndTypeNames | Type = 0x1980001`
(<https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/ClrTraceEventParser.cs>,
<https://github.com/dotnet/diagnostics/blob/main/src/Tools/dotnet-gcdump/DotNetHeapDump/EventPipeDotNetHeapDumper.cs>).

Join algorithm (PerfView `DotNetHeapDumpGraphReader`,
<https://github.com/microsoft/perfview/blob/main/src/EtwHeapDump/DotNetHeapDumpGraphReader.cs>):
select the dump GC by `GCStart` with `Depth>=2 && Type==NonConcurrentGC && Reason==Induced`,
process to the matching `GCStop`; nodes and edges are two parallel flattened streams consumed
in `Index` order, each node owning the next `EdgeCount` edge entries; the reader **throws** on
any Index gap and on leftover/short edge data; type join is `node.TypeID == BulkType.TypeID`
(BulkType collected outside the GC window too). The reader never consults the moved/surviving
range events — addresses are stable because the walk runs inside the blocking induced gen2 GC.
Cross-GC identity uses `newObjectID = NewRangeBase + (oldObjectID − OldRangeBase)` — the
documented MovedReferences2 formula; ObjectIDs are invalid during the callback and valid after
`GarbageCollectionFinished`
(<https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback4-movedreferences2-method>).

Same fence, same walk (why the profiler feed and the bulk events are one dataset):
`proftoeeinterfaceimpl.cpp` `HeapWalkHelper` is a single per-object callback (via
`GCProfileWalkHeapWorker`/`DiagWalkHeap` in `gcenv.ee.cpp`) feeding BOTH the ICorProfiler
`ObjectReferences` callback AND `ETW::GCLog::ObjectReference` (which buffers
GCBulkNode/GCBulkEdge); the moved/surviving ranges fan out from one
`MovedReferenceContextForEtwAndProfapi` to `MovedReferences2`/`SurvivingReferences2` AND the
ETW bulk range events — identical ranges from the same walk (BGC survivor ranges are ETW-only
by design, moot here because `COR_PRF_MONITOR_GC` already disables concurrent GC in the
victim).
<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/proftoeeinterfaceimpl.cpp>,
<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/gcenv.ee.cpp>,
<https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/eventtrace_gcheap.cpp>.

The profiler alternative, and why this doc still uses the events: the research verdict is that
profiler callbacks carry byte-identical identity data at the same fence, in-process, with no
event-serialization or Index-gap/drop failure mode (`ObjectReferences` under
`COR_PRF_MONITOR_GC`, attach-allowed;
<https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback-objectreferences-method>,
<https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/cor-prf-monitor-enumeration>).
This tier chooses the event path anyway, for four reasons recorded here so the decision is
auditable: (1) identity needs per-node **Size**, which rides the GCBulkNode payload but not the
`ObjectReferences` callback (size would need `ICorProfilerInfo` calls at the fence — not
verified by the research); (2) this tier is out-of-process/post-pass, and a node table is far
too large for the 2 MB shm channel that fits the move feed; (3) the drop failure mode is made
LOUD, not silent — the reader hard-fails on Index gaps exactly as PerfView's does, so a lossy
session is a red lane, never a wrong edge; (4) the forced-GC trigger gives a well-defined
snapshot fence that the landed MovedReferences2 feed brackets (T2), which is precisely the
"join with the range feed" the plan names. The profiler route stays on record as the fallback
if EventPipe proves lossy in practice — same walk, same fence, proven above. Note
"ObjectsAllocated" does not exist: `ObjectAllocated` is per-object but launch-time-only
(`COR_PRF_ENABLE_OBJECT_ALLOCATED` is init-only), `ObjectsAllocatedByClass` is counts-only
(<https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilercallback-objectallocated-method>);
neither suits an attach tier, so per-allocation identity is out of scope.

Mono caveat: a separate `MonoProfiler` provider (own GCHeapDump keyword,
`GCHeapDumpStart/Stop/GCHeapDumpObjectReference`, ids ~8467ff in the same manifest) replaces
the CoreCLR bulk events on Mono/MAUI — manifest-verified only; Mono emission code was not
inspected.

Packages: `Microsoft.Diagnostics.Tracing.TraceEvent` latest stable **3.2.5**, MIT
(<https://www.nuget.org/packages/Microsoft.Diagnostics.Tracing.TraceEvent>, checked
2026-07-17). `Microsoft.Diagnostics.NETCore.Client` 0.2.510501 is the pin already in
[attacher.csproj](../../../examples/gccanon_attach/attacher/attacher.csproj).

Known caveats carried from the research: the Learn GC-events pages omit the bulk payloads (the
manifest + `eventtrace_gcheap.cpp` are the effective spec); EventPipe loss under buffer
pressure is inferred from dotnet-gcdump's Drop-mode fallback and the reader's Index-gap
exceptions, not from a primary statement; exact `eventtrace.cpp`/`gcenv.ee.cpp` line numbers
are `main` @2026-07 and will drift; the manifest was read from `main`, not the v8.0.8 tag —
hence T3 step 3.

## Out of scope

- **Everything the landed F4 already does** — address canonicalization, S0 stamping, the
  same-window GC composition — stays as shipped; this doc only adds the identity layer on top.
- The DR taint tier's GC remap (`at_gc_remap_live`) and any in-process shadow work — see
  [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md) for taint-oracle work; the DR tier
  precedent here is read-only.
- The PT-derived value path and its managed story —
  [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md).
- Producer-side correctness items (sub-register aliasing, F6 residuals) —
  [dataflow-producer-correctness.md](dataflow-producer-correctness.md).
- Bindings exposure of the def-use/slice surface (including any future binding for the objid
  pass) — [dataflow-bindings-slice-codeimage.md](dataflow-bindings-slice-codeimage.md).
- Whole-window managed composition — [managed-wholewindow-compose.md](managed-wholewindow-compose.md).
- IL/bytecode attribution of managed steps —
  [native-il-bytecode-attribution.md](native-il-bytecode-attribution.md).
