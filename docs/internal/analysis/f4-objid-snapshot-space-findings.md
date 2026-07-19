# Findings: F4 object-identity snapshot-space convention (GCBulkNode addresses are POST-move)

*Status: findings / measured record (2026-07-19). Produced for
[dataflow-f4-object-identity.md](../implementations/dataflow-f4-object-identity.md) **T4**
(the snapshot-space spike), which gates the T5 objid join. **Verdict: GO on the POST-move
convention** — GCBulkNode `Address` is the object's FINAL, post-relocation address, so the dump
GC's own move ranges **belong** in the trace-to-snapshot translation set. Measured directly on a
live CoreCLR .NET 8 victim; see "Evidence" below.*

## The question

The T5 objid join keys a captured memory record on *(object, offset)* by inverse-walking a heap
snapshot (GCBulkNode `{Address, Size, TypeID}` nodes) back through the accompanying
`MovedReferences2` move set. The snapshot is produced by forcing one blocking induced gen2 GC
(the `gccanon_dumper`, T3). That dump GC is itself **compacting** — it relocates objects and emits
its own `GCBulkMovedObjectRanges`. The open question: is a GCBulkNode `Address` the object's
address **before** or **after** the dump GC's own compaction?

- **Post-move** → node addresses are the final resting places → the dump GC's own ranges are part
  of the trace→snapshot translation and **must be applied** (included in the move set the inverse
  walk uses).
- **Pre-move** → node addresses predate the dump GC's compaction → the dump GC's own ranges must be
  **excluded** (T5 would filter the dump GC's `gc_seq` out of the translation set).

Why it was genuinely open: PerfView's `DotNetHeapDumpGraphReader` treats node addresses as stable
inside the blocking induced GC and never consults the moved-range events, implying post-move; but
[gc-move-range-extraction-findings.md](gc-move-range-extraction-findings.md) measured that
`MovedReferences2` fires **before** physical relocation. Whether the profiler walk feeding
GCBulkNode runs before or after the dump GC's compaction is not stated by any source. House rule:
measure, don't assume.

## The measurement

Self-contained, in the dumper itself (`--measure` mode,
[examples/gccanon_attach/dumper/Program.cs](../../../examples/gccanon_attach/dumper/Program.cs)):
the session additionally enables the `GCHeapSurvivalAndMovement` keyword (0x400000), so the SAME
dump GC emits `GCBulkMovedObjectRanges` alongside its nodes. For every relocating range
`{old → new}` the kept dump GC reported, we ask whether a node `Address` sits in the NEW span
`[new, new+len)` (a post-move witness — the object now lives at `new`) or the OLD span
`[old, old+len)` (a pre-move witness — the walk saw it at `old`, which after compaction is
vacated). The exclusive-witness tally is the verdict. No profiler, tracer, or shm feed is needed —
the node addresses and the dump GC's own ranges come from one EventPipe session.

**Gotcha, recorded so it is not rediscovered:** `GCBulkMovedObjectRanges` for the dump GC fire
**before** its first `GCBulkNode`. A first cut of `--measure` gated moved-range collection on the
node-state machine (`state == 1`, set only on the first node) and consequently saw `dump_ranges=0`
and reported a spurious "non-compacting" verdict. The ranges must be collected UNGATED and filtered
to the dump GC's number afterward. That this walk-then-move ordering exists at all is the same fact
that makes the answer non-obvious.

## The verdict — POST-move (GO)

```
GCCANON_DUMP nodes=15660 edges=15653 index_gaps=0 reason=Induced depth=2 type=NonConcurrentGC rule_ok=1
GCCANON_SNAPSPACE dump_gc=53 dump_ranges=15001 other_gc_ranges=32683 post_witness=15001 pre_witness=0 verdict=POST (node.Address == NewRangeBase)
GCCANON_SNAPSPACE_WITNESS old=0x7382e6000040 new=0x7382e8c112b8 len=0x78 node@new=1 node@old=0
GCCANON_SNAPSPACE_WITNESS old=0x7382e60001c0 new=0x7382e8c11330 len=0x58 node@new=1 node@old=0
GCCANON_SNAPSPACE_WITNESS old=0x7382e6000320 new=0x7382e8c11388 len=0x58 node@new=1 node@old=0
```

All **15001** of the dump GC's own relocating ranges have a node at their `NewRangeBase` and **zero**
have a node at their `OldRangeBase` (`post_witness=15001 pre_witness=0`). The result is unanimous,
not a majority — decisive. The `other_gc_ranges=32683` line confirms the moved-range capture works
(the victim's own compacting gen2 GCs are seen too), so `dump_ranges=15001` is a real measurement,
not an artefact of a disabled keyword.

Corroboration from the victim's own ground truth and the node table (same run): the snapshot's
`System.Int64[]` sentinel nodes sit in the post-move address band —

```
node 0x7382e8c096b8 0x28 0x...bf20 System.Int64[]
node 0x7382e8c10f98 0x58 0x...bf20 System.Int64[]
GCCANON_VICTIM_ROUND n=33 moved=1 old=0x7382e690d650 new=0x7382e8d87fa8 val=0x5eedcafe12345678
```

— the victim reports its sentinel `long[]` relocating to `new=0x7382e8d8...`, in the same
`0x7382e8xxxxxx` band the snapshot nodes occupy, i.e. the objects' **new** (post-compaction)
addresses, exactly as the moved-range witnesses say.

Interpretation: the profiler's `MovedReferences2` reports `{old → new}` **before** physical
relocation (the earlier finding), while the `GCBulkNode` heap walk runs **after** relocation and
reports **new**. These are consistent, not contradictory: both describe the one `old → new` map,
and the snapshot lives in the `new` (post-move) space. So `asmtest_gcmove_canon`, which forwards a
pre-move (`old`-space) trace address toward its final resting place, lands it exactly where the
snapshot node is — the objid inverse walk (`asmtest_objid_locate`) and the snapshot agree.

## Implication for T5 (the chosen convention)

The dump GC's own move ranges **BELONG** in the trace→snapshot translation set. Concretely: T5 must
drain the post-capture (`GCCANON_MOVE_POST`) move feed **after** the dumper's forced GC has run, so
the dump GC's own ranges are present, and must **include** them — it must NOT filter the last
post-capture `gc_seq` out. The permanent guard against a future runtime flipping this is T5's
known-object assertion: the sentinel store/load records' `asmtest_objid_owner` must resolve to the
sentinel `Int64[]` node's `node.addr + 0`; if the convention ever inverts, that lookup fails loudly.

## Reproduce

On a Linux x86-64 Docker host (from the repo root), inside the lane image:

```
docker build -f Dockerfile.gccanon-attach --build-arg BASE=ubuntu:24.04 -t asmtest-gccanon-attach .
docker run --rm asmtest-gccanon-attach bash -c '
  dotnet build -c Release examples/gccanon_attach/dumper/dumper.csproj -o build/gccanon_dumper_out &&
  dotnet build -c Release examples/gccanon_attach/victim/victim.csproj -o build/gccanon_victim_out &&
  DOTNET_PerfMapEnabled=1 DOTNET_TieredCompilation=0 GCCANON_VICTIM_SECONDS=90 \
    dotnet build/gccanon_victim_out/gccanon_victim.dll >/tmp/vlog 2>&1 &
  for i in $(seq 1 25); do p=$(sed -n "s/.*GCCANON_VICTIM_START pid=\([0-9]*\).*/\1/p" /tmp/vlog|head -1); [ -n "$p" ] && break; sleep 1; done
  for i in $(seq 1 20); do grep -q GCCANON_VICTIM_READY /tmp/vlog && break; sleep 1; done
  dotnet build/gccanon_dumper_out/gccanon_dumper.dll "$p" /tmp/nodes.txt 60 --measure'
```

Expected: `GCCANON_SNAPSPACE ... post_witness>0 pre_witness=0 verdict=POST ...` (the exact counts
scale with the victim's live-object count). CoreCLR .NET 8 only; Mono uses a separate provider and
is out of scope.
