// examples/dotnet/footprint — reporting (presentation only). Distinct-page working-set and a
// jump-distance histogram from AsmTrace.Addresses. No tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    const ulong PageSize = 4096;

    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        ulong[] addrs = ww.Addresses;
        if (addrs.Length == 0) { Console.WriteLine("no instructions captured."); return; }

        // Working-set: distinct 4 KB pages touched (I-cache / working-set proxy).
        var pages = new HashSet<ulong>();
        foreach (ulong a in addrs) pages.Add(a / PageSize);

        // Locality: |addr[i+1] - addr[i]| bucketed by magnitude. Adjacent (<16 B) is a
        // straight-line run; a far jump is a call/return/branch across the code image.
        // Buckets: 0 (same), <16, <256, <4K (same page-ish), <64K, <1M, >=1M.
        var edges = new (string Label, ulong Max)[]
        {
            ("straight-line (<16 B)", 16),
            ("near   (<256 B)", 256),
            ("page   (<4 KB)", 4096),
            ("mid    (<64 KB)", 64 * 1024),
            ("far    (<1 MB)", 1024 * 1024),
            ("far    (>=1 MB)", ulong.MaxValue),
        };
        var hist = new long[edges.Length];
        long backward = 0;
        for (int i = 1; i < addrs.Length; i++)
        {
            ulong hi = addrs[i], lo = addrs[i - 1];
            if (hi < lo) backward++;
            ulong d = hi >= lo ? hi - lo : lo - hi;
            for (int b = 0; b < edges.Length; b++)
                if (d < edges[b].Max) { hist[b]++; break; }
        }

        long transitions = addrs.Length - 1;
        Console.WriteLine($"captured {addrs.Length} instruction executions"
                          + (ww.Truncated ? " (truncated)" : "") + ".\n");
        Console.WriteLine($"code working-set: {pages.Count} distinct 4 KB pages "
                          + $"({(ulong)pages.Count * PageSize / 1024} KB of code addresses touched).\n");

        Console.WriteLine("jump-distance locality (|addr[i+1] - addr[i]|):");
        Console.WriteLine($"  {"distance",-22}{"count",10}  {"share",-8} bar");
        Console.WriteLine("  " + new string('-', 70));
        long max = 1;
        foreach (long h in hist) if (h > max) max = h;
        for (int b = 0; b < edges.Length; b++)
        {
            double pct = transitions > 0 ? 100.0 * hist[b] / transitions : 0;
            string bar = new string('#', (int)Math.Round(28.0 * hist[b] / max));
            Console.WriteLine($"  {edges[b].Label,-22}{hist[b],10}  {pct,6:F1}%  {bar}");
        }

        Console.WriteLine($"\nbackward transitions (loops / returns): {backward} of {transitions} "
                          + $"({(transitions > 0 ? 100.0 * backward / transitions : 0):F1}%).");
        Console.WriteLine("-> most transitions are straight-line (the CPU streams sequential bytes); the far\n"
                          + "   buckets are calls/returns across the runtime's code image. Distances only — a\n"
                          + "   structural view, no time (the single-step tier has no timestamps).");
    }
}
