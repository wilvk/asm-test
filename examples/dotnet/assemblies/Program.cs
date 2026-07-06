// examples/dotnet/assemblies — group a whole-window capture BY ASSEMBLY, listing the
// method names called in each. Uses the §D0.2 rundown (withRundown: true) so methods
// carry their assembly tag (jitdump/perf-map spelling "<ret> [<assembly>] Type::Method").
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // group ww.Methods by AsmMethod.Assembly, print AsmMethod.ShortName under each.
//
// Answers "which assemblies did the traced code call, and which methods in each" — e.g.
// the Console.WriteLine path spans System.Console + System.Private.CoreLib. Runs via the
// single-step WEAK tier; self-skips cleanly where single-step / diagnostics are off.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A small workload that touches several assemblies: string.Join + array
    // (System.Private.CoreLib) and the Console write path (System.Console + CoreLib).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine($"[Work] joined='{joined}' len={joined.Length}");
    }

    static int Main()
    {
        Console.WriteLine("== assemblies + methods called inside a whole-window scope ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default\n"
                          + "(the STRONG Intel-PT / CEILING AMD-LBR tiers are forward-look)\n");

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
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
        return 0;
    }
}
