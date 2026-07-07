// examples/dotnet/runtimegaps — reporting (presentation only). Ranks the RuntimeBefore bursts
// in the labelled stream and aggregates them by the method they precede. No tracing here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    const int Top = 12;

    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so the labelled stream is empty.");
            return;
        }
        var stream = ww.Disassembly;
        if (stream.Count == 0) { Console.WriteLine("no labelled instructions captured."); return; }

        // Total runtime instructions elided between labelled runs, and the per-method sum of the
        // runtime bursts that immediately PRECEDE each method (the runtime cost to reach it).
        long totalRuntime = 0;
        var byMethod = new Dictionary<string, long>();
        var single = new List<AsmInstruction>();       // the individual largest bursts
        foreach (AsmInstruction i in stream)
        {
            totalRuntime += i.RuntimeBefore;
            if (i.RuntimeBefore > 0)
            {
                byMethod.TryGetValue(i.ShortMethod, out long c);
                byMethod[i.ShortMethod] = c + i.RuntimeBefore;
                single.Add(i);
            }
        }

        Console.WriteLine($"{stream.Count} labelled instructions; {totalRuntime} native-runtime instructions "
                          + "ran between them\n(RyuJIT / GC / PAL — elided from the labelled stream).\n");

        // The single largest bursts (one gap = one entry into a method through the runtime).
        single.Sort((a, b) => b.RuntimeBefore.CompareTo(a.RuntimeBefore));
        Console.WriteLine("largest single runtime bursts (runtime insns, then the method they precede):");
        Console.WriteLine($"  {"burst",10}  method / first instruction");
        Console.WriteLine("  " + new string('-', 72));
        for (int k = 0; k < single.Count && k < Top; k++)
        {
            AsmInstruction i = single[k];
            Console.WriteLine($"  {i.RuntimeBefore,10}  {Clip(i.ShortMethod, 44)}  ({i.Text})");
        }

        // Aggregated: which method the runtime spends the most getting INTO.
        var ranked = new List<KeyValuePair<string, long>>(byMethod);
        ranked.Sort((a, b) => b.Value.CompareTo(a.Value));
        Console.WriteLine("\nby method entered (summed preceding runtime insns):");
        for (int k = 0; k < ranked.Count && k < Top; k++)
            Console.WriteLine($"  {ranked[k].Value,10}  {Clip(ranked[k].Key, 56)}");

        Console.WriteLine("\n-> the biggest bursts are JIT compilation + the runtime call machinery reaching the\n"
                          + "   first execution of a method. RuntimeBefore turns \"N runtime instructions\" into\n"
                          + "   WHERE they ran. Counts only — no time; the single-step tier has no timestamps.");
    }

    static string Clip(string s, int n) => s.Length <= n ? s : s.Substring(0, n - 1) + "…";
}
