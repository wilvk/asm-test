// examples/dotnet/assemblies — reporting (presentation only), split from Program.cs.
// Groups the closed scope's labelled methods by declaring assembly and lists them.
// No tracing logic here.

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

        // Group the labelled methods by their declaring assembly.
        var byAsm = new SortedDictionary<string, List<AsmMethod>>(StringComparer.Ordinal);
        foreach (AsmMethod m in ww.Methods)
        {
            string asm = string.IsNullOrEmpty(m.Assembly) ? "(in-scope / no assembly tag)" : m.Assembly;
            if (!byAsm.TryGetValue(asm, out var list)) byAsm[asm] = list = new List<AsmMethod>();
            list.Add(m);
        }

        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; {ww.Methods.Count} methods across "
                          + $"{byAsm.Count} assemblies ({ww.LabelledInstructions} instructions labelled).\n");
        Console.WriteLine("assembly  ->  methods (by instruction count):");
        foreach (var kv in byAsm)
        {
            long total = 0;
            foreach (var m in kv.Value) total += m.Count;
            kv.Value.Sort((x, y) => y.Count.CompareTo(x.Count));
            Console.WriteLine($"\n  [{kv.Key}]  — {kv.Value.Count} method(s), {total} instructions");
            foreach (AsmMethod m in kv.Value)
                Console.WriteLine($"      {m.Count,6}  {m.ShortName}");
        }
    }
}
