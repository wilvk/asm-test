// examples/dotnet/hotspots — the most-executed instructions (a loop pops out).
//
//     using (var ww = new AsmTrace(byMethod: true)) HotLoop(200);
//     // group ww.Disassembly by AsmInstruction.Address -> execution count per instruction.
//
// AsmTrace.Addresses / .Disassembly are a DYNAMIC trace (execution order, WITH repeats), so a
// loop body appears ~N times while the prologue sits at 1x. The per-method and per-assembly
// examples aggregate that away; this one keeps it — dedup by address and rank. Single-step WEAK
// tier; the ctor auto-inits it.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A hot loop so instruction addresses REPEAT — the whole point of hotspot analysis.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HotLoop(long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += (i * 3) ^ (i >> 1);
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== hotspots: the most-executed instructions in the window ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true))
            HotLoop(200);
        Report.Print(ww);
        return 0;
    }
}
