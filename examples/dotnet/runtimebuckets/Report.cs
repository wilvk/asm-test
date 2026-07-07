// examples/dotnet/runtimebuckets — reporting (presentation only). Symbolizes the closed scope's
// addresses into per-module buckets and prints them ranked. No tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        ulong[] addrs = ww.Addresses;
        if (addrs.Length == 0) { Console.WriteLine("no instructions captured."); return; }

        // A whole window is ~1M addresses and symbolize_bucket scans /proc/self/maps per address —
        // so resolve by PAGE, not per address: a 4 KB page belongs to exactly one mapping (there
        // is no self perf-map here to split it finer), so we symbolize ONE representative per
        // distinct page and weight it by how many captured instructions fell on that page. Exact
        // module attribution, execution-weighted, at ~hundreds of resolves instead of ~1M.
        const ulong PageSize = 4096;
        var pageWeight = new Dictionary<ulong, long>();   // page -> instructions on it
        foreach (ulong a in addrs)
        {
            ulong pg = a / PageSize;
            pageWeight.TryGetValue(pg, out long c);
            pageWeight[pg] = c + 1;
        }

        // Aggregate execution counts by module label (one single-address resolve per page).
        var byModule = new Dictionary<string, ulong>();
        ulong total = 0;
        foreach (var kv in pageWeight)
        {
            ulong rep = kv.Key * PageSize;                // a representative address on the page
            HwBucket[] one = HwTrace.SymbolizeBuckets(new[] { rep }, pid: 0, cap: 1);
            string label = one.Length > 0 ? one[0].Label : "[unknown]";
            byModule.TryGetValue(label, out ulong c);
            byModule[label] = c + (ulong)kv.Value;
            total += (ulong)kv.Value;
        }
        if (byModule.Count == 0)
        {
            Console.WriteLine("# self-skip: SymbolizeBuckets returned nothing (non-Linux, or lib missing).");
            return;
        }

        var ranked = new List<HwBucket>();
        foreach (var kv in byModule) ranked.Add(new HwBucket(kv.Key, kv.Value));
        ranked.Sort((a, b) => b.Count.CompareTo(a.Count));

        Console.WriteLine($"symbolized {total} captured addresses across {pageWeight.Count} code pages into "
                          + $"{ranked.Count} module bucket(s)" + (ww.Truncated ? " (window truncated)" : "") + ".\n");
        Console.WriteLine($"  {"insns",12}{"share",9}  module / JIT symbol");
        Console.WriteLine("  " + new string('-', 74));
        long max = 1;
        foreach (HwBucket b in ranked) if ((long)b.Count > max) max = (long)b.Count;
        foreach (HwBucket b in ranked)
        {
            double pct = total > 0 ? 100.0 * b.Count / total : 0;
            Console.WriteLine($"  {b.Count,12}{pct,8:F1}%  {Basename(b.Label)}");
        }

        Console.WriteLine("\n-> the runtime lump resolves to its real modules — the CoreCLR runtime (libcoreclr),\n"
                          + "   the JIT (libclrjit), libc, and the JIT's own emitted code (the memfd:doublemapper /\n"
                          + "   [anon] mapping). This is the surviving post-close primitive: it reads /proc/self/maps\n"
                          + "   + the perf-map, not the freed scope.");
    }

    // Trim a long mapped-file path to its basename for display; keep pseudo-names ("[anon]") whole.
    static string Basename(string label)
    {
        if (label.StartsWith("[", StringComparison.Ordinal)) return label;
        int slash = label.LastIndexOf('/');
        return slash >= 0 && slash < label.Length - 1 ? label.Substring(slash + 1) : label;
    }
}
