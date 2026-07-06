// examples/dotnet/callgraph — reconstruct the dynamic CALL TREE the per-method/per-assembly
// aggregates flatten away, from the labelled execution stream.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // walk ww.Disassembly with a shadow stack: a method change right after a `call` is an
//     // edge caller -> callee; a `ret` pops. Count edges + emit an indented first-entry tree.
//
// Approximate by construction: only LABELLED instructions are in Disassembly, so a call that
// lands in a managed method THROUGH a native runtime stub shows as a RuntimeBefore gap, and the
// edge is inferred from the mnemonic (`call`/`ret`) of the last labelled instruction. The exact,
// noise-free call tree is the out-of-process descent example (Ptrace + Descent); this is the
// honest in-process reconstruction. Single-step WEAK tier; the ctor auto-inits it.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // Fans out across several methods: string.Join (-> JoinCore -> Buffer.Memmove, and the
    // array-store helper) then the Console write path (get_Out -> StreamWriter -> ...).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== dynamic call graph reconstructed from the labelled stream ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
