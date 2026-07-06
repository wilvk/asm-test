// examples/dotnet/hotspots — reporting (presentation only). Dedups the closed scope's labelled
// stream by address and ranks the hottest instructions. No tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    const int Top = 15;

    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be disassembled.");
            return;
        }

        // The labelled stream repeats (loops); group by address -> execution count per instruction.
        var count = new Dictionary<ulong, long>();
        var seen = new Dictionary<ulong, AsmInstruction>();
        foreach (AsmInstruction i in ww.Disassembly)
        {
            count.TryGetValue(i.Address, out long c); count[i.Address] = c + 1;
            if (!seen.ContainsKey(i.Address)) seen[i.Address] = i;
        }
        var ranked = new List<ulong>(count.Keys);
        ranked.Sort((a, b) => count[b].CompareTo(count[a]));
        long max = ranked.Count > 0 ? count[ranked[0]] : 1;

        double reuse = count.Count > 0 ? (double)ww.Disassembly.Count / count.Count : 0;
        Console.WriteLine($"{ww.Disassembly.Count} labelled instruction executions over {count.Count} distinct "
                          + $"instructions ({reuse:F1}x average reuse — the loop).\n");
        Console.WriteLine($"  {"count",8}  {"heat",-20}  instruction   [method]");
        Console.WriteLine("  " + new string('-', 78));
        for (int k = 0; k < ranked.Count && k < Top; k++)
        {
            AsmInstruction i = seen[ranked[k]];
            string bar = new string('#', (int)Math.Round(20.0 * count[ranked[k]] / max));
            Console.WriteLine($"  {count[ranked[k]],8}  {bar,-20}  0x{i.Address:x} {i.Text}   [{i.ShortMethod}]");
        }
        Console.WriteLine($"\n-> the hottest rows are the loop body (each executed ~200x); the prologue/epilogue sit\n"
                          + "   at 1x. Per-method aggregates hide this — a dynamic trace WITH repeats reveals the loop.");
    }
}
