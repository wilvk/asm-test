// examples/dotnet/flatprofile — `perf report` parity: the executed methods ranked by SELF
// instruction count, with an Overhead % and a running cumulative %.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // rank ww.Methods by Count -> self %, cumulative %.
//
// The per-method breakdown the other examples group by tier/assembly, re-cut as the classic
// profiler table: which methods account for the bulk of the executed (labelled) instructions.
// Counts, never time — the single-step WEAK tier has no timestamps. The ctor auto-inits it.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A one-liner that fans out across several BCL methods (string.Join + the Console write
    // path), so the flat profile has more than one interesting row.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma", "delta" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== flat profile: methods by self instruction count (perf report parity) ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
