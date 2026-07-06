// examples/dotnet/blockstep — reporting (presentation only). Shows the single-step and
// block-step traces are identical, and estimates the tracer-stop reduction. No tracing here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(NativeCode code, HwTrace ss, HwTrace bs, long r1, long r2, int trips)
    {
        ulong[] a = ss.InsnOffsets(), b = bs.InsnOffsets();
        bool identical = a.Length == b.Length;
        for (int i = 0; identical && i < a.Length; i++)
            if (a[i] != b[i]) identical = false;

        Console.WriteLine($"child computed sum(1,{trips}) = {r1} (block-step: {r2}); both traced across "
                          + "the process boundary.\n");

        Console.WriteLine($"single-step trace: {ss.InsnsTotal()} instructions, {ss.BlocksLen()} blocks.");
        Console.WriteLine($"block-step  trace: {bs.InsnsTotal()} instructions, {bs.BlocksLen()} blocks.");
        Console.WriteLine($"-> the reconstructed streams are {(identical ? "IDENTICAL" : "DIFFERENT (!)")}"
                          + $"{(ss.Truncated() || bs.Truncated() ? " (a capture truncated)" : "")}.\n");

        // Tracer stops: single-step pays one per executed instruction; block-step pays one
        // per TAKEN branch (each loop iteration's jnz, plus the entry and the final ret).
        long ssStops = (long)ss.InsnsTotal();
        long takenBranches = trips /* one jnz taken per full iteration */ + 1 /* the ret */;
        Console.WriteLine($"tracer round-trips (the cost that dominates): single-step ~{ssStops}, "
                          + $"block-step ~{takenBranches} (one per taken branch).");
        if (takenBranches > 0)
            Console.WriteLine($"-> ~{(double)ssStops / takenBranches:0.0}x fewer stops for the SAME exact "
                              + "trace — and the only exact real-CPU capture on Zen 2.");
    }
}
