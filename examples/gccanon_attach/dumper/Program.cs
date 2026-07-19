using System;
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.IO;
using System.Text;
using System.Threading;
using Microsoft.Diagnostics.NETCore.Client;
using Microsoft.Diagnostics.Tracing;
using Microsoft.Diagnostics.Tracing.Parsers;
using Microsoft.Diagnostics.Tracing.Parsers.Clr;

// F4 increment 4 heap-snapshot dumper (dataflow-f4-object-identity.md T3): force ONE full blocking
// induced gen2 GC on a running victim over its diagnostics port and parse the GCBulkNode /
// GCBulkEdge / BulkType events it emits into a plain-text {Address,Size,TypeID,typename} node table.
//
// Session enable with the GCHeapSnapshot keyword (0x1980001 = GC | GCHeapCollect (0x800000) |
// GCHeapDump (0x100000) | GCHeapAndTypeNames (0x1000000) | Type (0x80000)) is what FORCES the GC:
// the ManagedHeapCollect (0x800000) bit routes through ETW::GCLog::ForceGC. GCHeapDump alone forces
// nothing, and the bulk node/edge events fire ONLY during that forced GC (s_forcedGCInProgress &&
// GCHeapDump), so the dump GC is IDENTIFIED by the fact that it emits nodes — NOT by PerfView's
// "GCStart Depth>=2 && NonConcurrentGC && Induced" rule alone, which the victim's own driver GCs
// (constant Forced/blocking/compacting gen2 collects) would ALSO match. The rule is still checked, as
// a recorded sanity assertion on the node-emitting GC.
//
// Event payloads verified against src/coreclr/vm/ClrEtwAll.man at the PINNED tag v8.0.8 (2026-07-19,
// not main): BulkType (value 15, v0) {TypeID u64, ModuleID, TypeNameID, Flags, CorElementType, Name
// UnicodeString, ...}, GCBulkNode (18, v0) {Address ptr, Size u64, TypeID u64, EdgeCount u64},
// GCBulkEdge (19, v0) {Value ptr, ReferencingFieldID u32} — all version 0, byte-for-byte as the
// research notes recorded, so the TraceEvent 3.2.5 accessors below decode them correctly.
//
// FAIL CLOSED (the house rule): the node and edge streams are two parallel flattened streams whose
// per-batch Index must be gap-free, and every node owns the next EdgeCount edge entries, so the sum
// of EdgeCount must equal the total edge entries. A dropped EventPipe buffer would show as an Index
// gap or a short edge stream — a RED run (retry the whole dump up to 3 times, then exit non-zero),
// never a silently short node table.
//
//   usage: gccanon_dumper <pid> <out-file> [timeout-s]
static class Dumper
{
    // dotnet-gcdump's Keywords.GCHeapSnapshot. The 0x800000 ManagedHeapCollect bit forces the GC.
    const long GCHeapSnapshot = 0x1980001;
    // GCHeapSurvivalAndMovement — enables GCBulkMovedObjectRanges. Added ONLY in --measure mode (the
    // T4 snapshot-space spike), never in the normal dump path.
    const long GCHeapSurvivalAndMovement = 0x400000;

    sealed class Snapshot
    {
        public List<ulong> Addr = new List<ulong>();
        public List<ulong> Size = new List<ulong>();
        public List<ulong> TypeId = new List<ulong>();
        public long EdgeCountSum;      // sum of every node's EdgeCount
        public long EdgeEntries;       // total GCBulkEdge value entries seen
        public Dictionary<ulong, string> TypeNames = new Dictionary<ulong, string>();
        public GCReason Reason;
        public int Depth;
        public GCType Type;
        public bool RuleOk;            // Depth>=2 && NonConcurrentGC && Induced on the node-emitting GC
        public int DumpGcNumber = -1;  // the GC number of the dump we kept
        // --measure only: every relocating range seen, tagged with the GC that emitted it (collected
        // UNGATED by the node state so a range firing before the first node is never missed; the
        // report filters to DumpGcNumber). {gc, old, new, len}.
        public List<(int Gc, ulong Old, ulong New, ulong Len)> Moved =
            new List<(int, ulong, ulong, ulong)>();
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("DUMPER: usage: gccanon_dumper <pid> <out-file> [timeout-s] [--measure]");
            return 2;
        }
        int pid = int.Parse(args[0]);
        string outFile = args[1];
        bool measure = Array.IndexOf(args, "--measure") >= 0;
        int timeoutS = 60;
        for (int a = 2; a < args.Length; a++)
            if (int.TryParse(args[a], out int t)) { timeoutS = t; break; }

        for (int attempt = 1; attempt <= 3; attempt++)
        {
            string why;
            Snapshot snap = DumpOnce(pid, timeoutS, measure, out why);
            if (snap != null)
            {
                WriteTable(outFile, pid, snap);
                Console.WriteLine(
                    $"GCCANON_DUMP nodes={snap.Addr.Count} edges={snap.EdgeEntries} index_gaps=0 " +
                    $"reason={snap.Reason} depth={snap.Depth} type={snap.Type} rule_ok={(snap.RuleOk ? 1 : 0)} " +
                    $"attempt={attempt}");
                if (measure)
                    ReportSnapshotSpace(snap);
                Console.Out.Flush();
                return 0;
            }
            Console.Error.WriteLine($"DUMPER: attempt {attempt}/3 failed: {why}");
            Console.Error.Flush();
        }
        Console.WriteLine("GCCANON_DUMP_FAIL exhausted 3 attempts");
        Console.Out.Flush();
        return 1;
    }

    // One dump attempt. Returns a complete, gap-free Snapshot, or null (with a reason) on any failure
    // — no node-emitting GC within the timeout, an Index gap, or a node/edge-count mismatch.
    static Snapshot DumpOnce(int pid, int timeoutS, bool measure, out string why)
    {
        why = "";
        var snap = new Snapshot();
        // State machine over the ONE dump we keep: 0 = before any heap-walk GC, 1 = collecting the
        // first dump, 2 = that dump finished. With the GCHeapDump keyword enabled EVERY qualifying GC
        // walks the heap, and the target victim forces gen2 GCs constantly — so many dumps stream by,
        // each with its Index reset to 0. We isolate the FIRST complete one and ignore the rest.
        int state = 0;
        bool gap = false;
        int nextNodeIndex = 0, nextEdgeIndex = 0;
        int nodeEvents = 0, edgeEvents = 0;
        int gapExpected = -1, gapGot = -1;
        string gapWhich = "";
        int dumpGcNumber = -1;
        var nodeTrace = new List<string>(); // first few (gc:index+count) for diagnostics
        // per-GCStart, so the node-emitting GC's own reason/depth/type can be captured when nodes land
        var curReason = GCReason.Induced;
        int curDepth = -1;
        var curType = GCType.NonConcurrentGC;
        int curGcNumber = -1;
        var done = new ManualResetEventSlim(false);

        var client = new DiagnosticsClient(pid);
        long keywords = GCHeapSnapshot | (measure ? GCHeapSurvivalAndMovement : 0);
        var providers = new[]
        {
            new EventPipeProvider("Microsoft-Windows-DotNETRuntime", EventLevel.Informational,
                                  keywords)
        };

        EventPipeSession session;
        try
        {
            // A generous circular buffer: the target victim allocates hard, so the base-GC keyword's
            // events compete with the dump's node/edge stream. Default is 256 MB.
            session = client.StartEventPipeSession(providers, requestRundown: false,
                                                   circularBufferMB: 1024);
        }
        catch (Exception e)
        {
            why = "StartEventPipeSession: " + e.GetType().Name + ": " + e.Message;
            return null;
        }

        using (session)
        {
            var source = new EventPipeEventSource(session.EventStream);

            source.Clr.GCStart += (GCStartTraceData d) =>
            {
                // A new GC starting while we are mid-dump means the dump GC already ended (GCs
                // serialize) — finalize even if its GCStop was lost to a buffer drop.
                if (state == 1 && d.Count != dumpGcNumber)
                {
                    state = 2;
                    done.Set();
                }
                curGcNumber = d.Count;
                curDepth = d.Depth;
                curReason = d.Reason;
                curType = d.Type;
            };

            source.Clr.TypeBulkType += (GCBulkTypeTraceData d) =>
            {
                // BulkType is emitted OUTSIDE the GC window too; collect it whole-session.
                for (int i = 0; i < d.Count; i++)
                {
                    var v = d.Values(i);
                    snap.TypeNames[(ulong)v.TypeID] = v.TypeName ?? "?";
                }
            };

            source.Clr.GCBulkNode += (GCBulkNodeTraceData d) =>
            {
                if (state == 2)
                    return; // a later victim-GC dump — ignore
                if (state == 0)
                {
                    // The first heap-walk GC identifies the dump we keep.
                    state = 1;
                    dumpGcNumber = curGcNumber;
                    snap.DumpGcNumber = curGcNumber;
                    snap.Reason = curReason;
                    snap.Depth = curDepth;
                    snap.Type = curType;
                    snap.RuleOk = curDepth >= 2 && curType == GCType.NonConcurrentGC &&
                                  curReason == GCReason.Induced;
                }
                if (curGcNumber != dumpGcNumber)
                    return; // nodes from a different GC — ignore
                nodeEvents++;
                if (nodeTrace.Count < 12)
                    nodeTrace.Add($"{curGcNumber}:{d.Index}+{d.Count}");
                // `Index` is the BATCH sequence number (0,1,2,...), NOT a cumulative node offset;
                // `Count` is the nodes in this batch. A gap in the batch sequence == a dropped buffer.
                if (d.Index != nextNodeIndex && !gap)
                {
                    gap = true;
                    gapWhich = "node";
                    gapExpected = nextNodeIndex;
                    gapGot = d.Index;
                }
                nextNodeIndex = d.Index + 1;
                for (int i = 0; i < d.Count; i++)
                {
                    var v = d.Values(i);
                    snap.Addr.Add((ulong)v.Address);
                    snap.Size.Add((ulong)v.Size);
                    snap.TypeId.Add((ulong)v.TypeID);
                    snap.EdgeCountSum += v.EdgeCount;
                }
            };

            source.Clr.GCBulkEdge += (GCBulkEdgeTraceData d) =>
            {
                if (state != 1 || curGcNumber != dumpGcNumber)
                    return; // only the kept dump's edges
                edgeEvents++;
                // Same as nodes: `Index` is the batch sequence number, `Count` the edges in it.
                if (d.Index != nextEdgeIndex && !gap)
                {
                    gap = true;
                    gapWhich = "edge";
                    gapExpected = nextEdgeIndex;
                    gapGot = d.Index;
                }
                nextEdgeIndex = d.Index + 1;
                snap.EdgeEntries += d.Count; // ReferencingFieldID ignored (hardcoded 0 in the runtime)
            };

            source.Clr.GCBulkMovedObjectRanges += (GCBulkMovedObjectRangesTraceData d) =>
            {
                // --measure spike only. Collect EVERY relocating range with its GC number, UNGATED by
                // the node state machine (a range can fire before the first node of its GC), so the
                // report can ask, for the kept dump GC's own ranges, whether a node.Address sits at
                // the OLD (pre-move walk) or NEW (post-move walk) base.
                if (!measure)
                    return;
                for (int i = 0; i < d.Count; i++)
                {
                    var v = d.Values(i);
                    if ((ulong)v.OldRangeBase == (ulong)v.NewRangeBase)
                        continue; // non-relocating
                    snap.Moved.Add((curGcNumber, (ulong)v.OldRangeBase, (ulong)v.NewRangeBase,
                                    (ulong)v.RangeLength));
                }
            };

            source.Clr.GCStop += (GCEndTraceData d) =>
            {
                if (state == 1 && d.Count == dumpGcNumber)
                {
                    state = 2;
                    done.Set();
                }
            };

            var pump = new Thread(() =>
            {
                try { source.Process(); }
                catch (Exception e) { Console.Error.WriteLine("DUMPER: source.Process: " + e.Message); }
            }) { IsBackground = true };
            pump.Start();

            done.Wait(TimeSpan.FromSeconds(timeoutS));
            try { session.Stop(); } catch { /* already stopping */ }
            pump.Join(5000);

            if (state == 0)
            {
                why = "no node-emitting GC within " + timeoutS + "s";
                return null;
            }
            if (state != 2)
            {
                why = "the first heap dump did not finish within " + timeoutS + "s (state=1)";
                return null;
            }
            if (gap)
            {
                why = $"Index gap in the {gapWhich} stream (dropped EventPipe buffer): expected {gapExpected} got {gapGot}; " +
                      $"nodeEvents={nodeEvents} edgeEvents={edgeEvents} nodes={snap.Addr.Count} edgeEntries={snap.EdgeEntries} " +
                      $"trace=[{string.Join(",", nodeTrace)}]";
                return null;
            }
            if (snap.EdgeCountSum != snap.EdgeEntries)
            {
                why = $"edge-count mismatch: sum(node.EdgeCount)={snap.EdgeCountSum} != edge entries={snap.EdgeEntries}";
                return null;
            }
            return snap;
        }
    }

    // T4 snapshot-space spike: are GCBulkNode addresses PRE- or POST- the dump GC's OWN compaction?
    // For every relocating range {old -> new} the SAME dump GC reported, a POST-move heap walk leaves
    // a node at `new` and none at `old` (vacated); a PRE-move walk leaves a node at `old` and none at
    // `new`. Count the exclusive witnesses and print the verdict + a few concrete ranges.
    static void ReportSnapshotSpace(Snapshot snap)
    {
        var addrs = new List<ulong>(snap.Addr);
        addrs.Sort();
        int dumpRanges = 0, otherRanges = 0;
        int postW = 0, preW = 0;
        var witnesses = new List<string>();
        foreach (var m in snap.Moved)
        {
            if (m.Gc != snap.DumpGcNumber)
            {
                otherRanges++; // a different (victim) GC's relocation — informational only
                continue;
            }
            dumpRanges++;
            bool nw = SpanHasNode(addrs, m.New, m.Len);
            bool ol = SpanHasNode(addrs, m.Old, m.Len);
            if (nw && !ol)
            {
                postW++;
                if (witnesses.Count < 4)
                    witnesses.Add($"old=0x{m.Old:x} new=0x{m.New:x} len=0x{m.Len:x} node@new=1 node@old=0");
            }
            else if (ol && !nw)
            {
                preW++;
                if (witnesses.Count < 4)
                    witnesses.Add($"old=0x{m.Old:x} new=0x{m.New:x} len=0x{m.Len:x} node@new=0 node@old=1");
            }
        }
        // dump_ranges==0 is itself decisive: the heap-dump GC is NON-COMPACTING, so GCBulkNode
        // addresses carry no dump-GC relocation — they are the live (post-any-prior-move) addresses,
        // and the dump GC contributes no ranges to the trace-to-snapshot translation set.
        string verdict = dumpRanges == 0 ? "NONCOMPACTING (dump GC moved nothing; nodes are live addresses)"
                       : postW > preW ? "POST (node.Address == NewRangeBase)"
                       : preW > postW ? "PRE (node.Address == OldRangeBase)"
                       : "AMBIGUOUS";
        Console.WriteLine($"GCCANON_SNAPSPACE dump_gc={snap.DumpGcNumber} dump_ranges={dumpRanges} " +
                          $"other_gc_ranges={otherRanges} post_witness={postW} pre_witness={preW} verdict={verdict}");
        foreach (var w in witnesses)
            Console.WriteLine("GCCANON_SNAPSPACE_WITNESS " + w);
    }

    // First index i in the sorted list with sortedAddrs[i] >= baseAddr; true iff that node is in span.
    static bool SpanHasNode(List<ulong> sortedAddrs, ulong baseAddr, ulong len)
    {
        int lo = 0, hi = sortedAddrs.Count;
        while (lo < hi)
        {
            int mid = (lo + hi) >> 1;
            if (sortedAddrs[mid] < baseAddr)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < sortedAddrs.Count && sortedAddrs[lo] < baseAddr + len;
    }

    static void WriteTable(string outFile, int pid, Snapshot snap)
    {
        var sb = new StringBuilder();
        int types = snap.TypeNames.Count;
        sb.Append("GCCANON_NODES count=").Append(snap.Addr.Count)
          .Append(" edges=").Append(snap.EdgeEntries)
          .Append(" types=").Append(types)
          .Append(" pid=").Append(pid).Append('\n');
        for (int i = 0; i < snap.Addr.Count; i++)
        {
            string name = snap.TypeNames.TryGetValue(snap.TypeId[i], out string n) ? Sanitize(n) : "?";
            sb.Append("node 0x").Append(snap.Addr[i].ToString("x"))
              .Append(" 0x").Append(snap.Size[i].ToString("x"))
              .Append(" 0x").Append(snap.TypeId[i].ToString("x"))
              .Append(' ').Append(name).Append('\n');
        }
        sb.Append("GCCANON_NODES_END\n");
        // Write to a temp file then rename, so the tracer never polls a half-written table.
        string tmp = outFile + ".tmp";
        File.WriteAllText(tmp, sb.ToString());
        if (File.Exists(outFile))
            File.Delete(outFile);
        File.Move(tmp, outFile);
    }

    // Node lines are 4 whitespace-separated fields; a type name can carry spaces (generics), so fold
    // any whitespace to '_' to keep the tracer's parse a clean four-token split.
    static string Sanitize(string s)
    {
        if (string.IsNullOrEmpty(s))
            return "?";
        var sb = new StringBuilder(s.Length);
        foreach (char c in s)
            sb.Append(char.IsWhiteSpace(c) ? '_' : c);
        return sb.ToString();
    }
}
