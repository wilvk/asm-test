// examples/dotnet/footprint — code working-set + locality, from the raw address stream.
//
//     using (var ww = new AsmTrace()) Work();
//     // ww.Addresses = every executed instruction's ABSOLUTE address, in order (with repeats).
//
// A whole-window scope needs no byMethod map for this: the ABSOLUTE addresses alone answer two
// microarchitectural questions with pure arithmetic — how many distinct 4 KB code pages the
// window touched (the instruction working-set / I-cache pressure proxy) and how far consecutive
// instructions jump (a locality histogram: straight-line runs vs far calls/returns). Counts and
// distances, never time. The ctor auto-inits the single-step tier.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== footprint: code working-set (4 KB pages) + jump-distance locality ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false))   // whole-window: Addresses only, no method map
            Work();
        Report.Print(ww);
        return 0;
    }
}
