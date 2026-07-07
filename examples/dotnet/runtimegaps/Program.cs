// examples/dotnet/runtimegaps — the biggest NATIVE-RUNTIME bursts in the labelled stream.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // each AsmInstruction carries RuntimeBefore: unlabelled runtime insns run just before it.
//
// AsmTrace.Disassembly is the labelled stream; the native-runtime instructions between named
// runs are elided, but their COUNT rides on the next labelled instruction as RuntimeBefore. The
// annotated example prints those gaps inline; this one RANKS them — the largest runtime bursts
// and the managed method each precedes, i.e. where the amplified single-step budget goes. Counts
// only; the ctor auto-inits the single-step tier.

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
        Console.WriteLine("== runtime gaps: the largest native-runtime bursts before labelled methods ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
