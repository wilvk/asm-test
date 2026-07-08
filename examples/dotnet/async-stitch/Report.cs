// examples/dotnet/async-stitch — reporting (presentation only), split from Program.cs.
// Renders the stitched operation: each hop's (Seq, Tid, InsnOffset) as one seq-ordered trace,
// calls out the hop that ran on a different thread yet stitched into the same operation, then
// prints the merged per-hop Path. Self-skips cleanly when no hop was captured in-process.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(AsmStitchedTrace op, int callerTid, long r0, long r1, long r2)
    {
        Console.WriteLine($"operation scope id {op.ScopeId}: {op.HopCount} hop(s) attempted; "
                        + $"Work → {r0}, {r1}, {r2} (this thread = tid {callerTid}).\n");

        if (op.Hops.Count == 0)
        {
            // Nothing captured in-process — single-step unavailable or an unsupported signature.
            // The operation still ran to completion uninstrumented; that is a clean self-skip.
            string why = op.SkipReason.Length != 0 ? op.SkipReason : "no hop captured in-process";
            Console.WriteLine($"# self-skip: {why}");
            return;
        }

        Console.WriteLine("-- stitched hops (one logical operation, merged by Seq) --");
        Console.WriteLine("    seq  tid   merged-offset");
        var seen = new System.Collections.Generic.HashSet<int>();
        foreach (StitchHop h in op.Hops)
        {
            seen.Add(h.Tid);
            Console.WriteLine($"    {h.Seq,-4} {h.Tid,-4}  +{h.InsnOffset}");
        }

        // Highlight the cross-thread stitch: a pool hop's Tid differs from hop 0's, yet its
        // instructions sit at their own offset in the SAME merged trace, ordered by Seq.
        StitchHop first = op.Hops[0];
        StitchHop? pool = FindDifferentThread(op, first.Tid);
        if (pool.HasValue)
            Console.WriteLine($"\n-> hop {pool.Value.Seq} ran on tid {pool.Value.Tid} (a thread-pool "
                            + $"thread), not hop 0's tid {first.Tid} — yet it stitched into the same "
                            + $"operation by Seq across {seen.Count} thread(s).");
        else
            Console.WriteLine($"\n-> all {op.Hops.Count} hops were captured on tid {first.Tid} (the "
                            + "pool reused this thread this run); still one seq-ordered operation.");

        if (op.SkipReason.Length != 0)
            Console.WriteLine($"   note: {op.SkipReason}");

        PrintPath(op);
    }

    static StitchHop? FindDifferentThread(AsmStitchedTrace op, int tid0)
    {
        foreach (StitchHop h in op.Hops)
            if (h.Tid != tid0) return h;
        return null;
    }

    static void PrintPath(AsmStitchedTrace op)
    {
        string path = op.Path ?? "";
        if (path.Length == 0)
        {
            Console.WriteLine("\n    (no rendered listing — this build has no Capstone disassembler)");
            Console.WriteLine($"    truncated: {op.Truncated}");
            return;
        }
        Console.WriteLine("\n-- merged Path (per-hop disassembly, seq order) --");
        int shown = 0;
        foreach (string line in path.Split('\n'))
        {
            if (line.Length == 0) continue;
            if (shown++ < 28) Console.WriteLine($"    {line}");
        }
        if (shown > 28) Console.WriteLine($"    … ({shown - 28} more)");
        Console.WriteLine($"truncated: {op.Truncated}");
    }
}
