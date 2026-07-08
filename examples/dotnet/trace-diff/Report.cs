// examples/dotnet/trace-diff — reporting (presentation only). Runs the routine twice, set-diffs
// the two traces' basic-block sets, and prints per-instruction execution-count deltas. No tracing
// policy beyond the choice of the two inputs lives here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    // One run in its own trace, so BlockOffsets()/InsnOffsets() are EXACTLY this input's path
    // (single-step, region-scoped — deterministic, zero runtime noise). Returns the covered
    // block set, a per-offset instruction execution-count map, and the routine's return value.
    static Run Once(NativeCode code, string name, long a, long b)
    {
        var tr = HwTrace.Create(blocks: 64, instructions: 256);
        tr.Register(name, code);
        long r = 0;
        tr.Region(name, () => r = code.Call(a, b));

        var blocks = new HashSet<ulong>();
        foreach (ulong off in tr.BlockOffsets()) blocks.Add(off);
        var counts = new Dictionary<ulong, int>();
        foreach (ulong off in tr.InsnOffsets())
            counts[off] = counts.TryGetValue(off, out int c) ? c + 1 : 1;

        var run = new Run { Blocks = blocks, Counts = counts, Result = r,
                            Total = tr.InsnsTotal(), Truncated = tr.Truncated() };
        tr.Free();
        return run;
    }

    public static void Print(NativeCode code)
    {
        // A: a+b = 42 <= 100 -> jle taken, the `dec` block is skipped.
        // B: a+b = 120 > 100 -> jle falls through, the `dec` block runs.
        Run A = Once(code, "runA", 20, 22);
        Run B = Once(code, "runB", 60, 60);

        // Self-check the hand-assembled routine — proves the bytes decode + run as intended.
        Console.WriteLine($"run A: add2(20,22) = {A.Result}   (a+b = 42 <= 100 -> jle TAKEN, dec skipped)");
        Console.WriteLine($"run B: add2(60,60) = {B.Result}   (a+b = 120 > 100 -> jle falls through, dec runs)");
        Console.WriteLine($"instructions executed: A={A.Total}, B={B.Total}"
                          + (A.Truncated || B.Truncated ? "  (a stream was truncated)" : "") + "\n");

        // --- basic-block coverage DELTA (B relative to A) ---
        var allBlocks = new SortedSet<ulong>();
        foreach (ulong o in A.Blocks) allBlocks.Add(o);
        foreach (ulong o in B.Blocks) allBlocks.Add(o);

        Console.WriteLine("basic-block coverage delta:\n");
        Console.WriteLine($"  {"block",-8} {"A",-4} {"B",-4} {"delta",-9} entry instruction");
        Console.WriteLine("  " + new string('-', 58));
        foreach (ulong off in allBlocks)
        {
            bool inA = A.Blocks.Contains(off), inB = B.Blocks.Contains(off);
            string delta = inA == inB ? "common" : inB ? "+ ON in B" : "- OFF in B";
            Console.WriteLine($"  0x{off:x2}{"",-4} {(inA ? "yes" : " . "),-4} {(inB ? "yes" : " . "),-4} "
                              + $"{delta,-9} {Disas(code, off)}");
        }

        Console.WriteLine();
        foreach (ulong off in allBlocks)
            if (B.Blocks.Contains(off) && !A.Blocks.Contains(off))
                Console.WriteLine($"-> block 0x{off:x} ({Disas(code, off)}) is NEWLY EXECUTED in B — the a+b>100\n"
                                  + "   path run A never reached. This is the exact block the change turned ON.");
        foreach (ulong off in allBlocks)
            if (A.Blocks.Contains(off) && !B.Blocks.Contains(off))
                Console.WriteLine($"-> block 0x{off:x} ({Disas(code, off)}) is a block ENTRY only in A (the jle-taken\n"
                                  + "   target); in B the same instruction still runs but mid-block, so it is no longer a\n"
                                  + "   distinct block head (see its nonzero count in BOTH columns of the table below).");

        // --- per-instruction execution-count DELTA (B - A) ---
        var allInsns = new SortedSet<ulong>();
        foreach (ulong o in A.Counts.Keys) allInsns.Add(o);
        foreach (ulong o in B.Counts.Keys) allInsns.Add(o);

        Console.WriteLine("\nper-instruction execution-count delta (B - A):\n");
        Console.WriteLine($"  {"insn",-8} {"A",-4} {"B",-4} {"B-A",-6} instruction");
        Console.WriteLine("  " + new string('-', 58));
        foreach (ulong off in allInsns)
        {
            int ca = A.Counts.TryGetValue(off, out int x) ? x : 0;
            int cb = B.Counts.TryGetValue(off, out int y) ? y : 0;
            int d = cb - ca;
            string ds = d > 0 ? $"+{d}" : d.ToString();
            Console.WriteLine($"  0x{off:x2}{"",-4} {ca,-4} {cb,-4} {ds,-6} {Disas(code, off)}");
        }
        Console.WriteLine("\n-> a nonzero B-A on an offset is an instruction whose execution count changed:\n"
                          + "   exact, not sampled — the regression/coverage delta between the two traces.");
    }

    // Decode the single instruction at code[off] for display (the block's entry instruction),
    // via the internal Capstone wrapper (this example compiles the binding source).
    static string Disas(NativeCode code, ulong off)
    {
        var buf = new byte[64];
        HwNative.asmtest_disas(0 /* X86_64 */, code.Base, (UIntPtr)code.Length,
                               (ulong)code.Base.ToInt64(), off, buf, (UIntPtr)buf.Length);
        int z = Array.IndexOf<byte>(buf, 0);
        if (z < 0) z = buf.Length;
        return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(buf, 0, z);
    }

    sealed class Run
    {
        public HashSet<ulong> Blocks;
        public Dictionary<ulong, int> Counts;
        public long Result;
        public ulong Total;
        public bool Truncated;
    }
}
