// examples/dotnet/flatprofile — reporting (presentation only). Ranks the closed scope's
// methods by self instruction count and prints Overhead % + cumulative %. No tracing here.

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

        // ww.Methods is already sorted by Count (descending); rank as a profiler table.
        var methods = new List<AsmMethod>(ww.Methods);
        long total = ww.LabelledInstructions;
        if (total <= 0) { Console.WriteLine("no labelled instructions captured."); return; }

        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; {total} labelled instructions "
                          + $"across {methods.Count} methods.\n");
        Console.WriteLine($"  {"self",8}  {"overhead",9}  {"cumul",8}  method");
        Console.WriteLine("  " + new string('-', 74));

        long cum = 0;
        for (int i = 0; i < methods.Count && i < Top; i++)
        {
            AsmMethod m = methods[i];
            cum += m.Count;
            double self = 100.0 * m.Count / total;
            double cml = 100.0 * cum / total;
            Console.WriteLine($"  {m.Count,8}  {self,8:F2}%  {cml,7:F1}%  {Clip(m.ShortName, 44)}");
        }
        if (methods.Count > Top)
        {
            long rest = 0;
            for (int i = Top; i < methods.Count; i++) rest += methods[i].Count;
            Console.WriteLine($"  {rest,8}  {100.0 * rest / total,8:F2}%  {"100.0",7}%  "
                              + $"... {methods.Count - Top} more method(s)");
        }

        Console.WriteLine($"\n-> Overhead % is each method's share of the {total} LABELLED instructions; the\n"
                          + "   cumulative column is the classic `perf report` running total. Counts only —\n"
                          + "   the single-step WEAK tier has no timestamps, so there is no \"time\" column.");
    }

    static string Clip(string s, int n) => s.Length <= n ? s : s.Substring(0, n - 1) + "…";
}
