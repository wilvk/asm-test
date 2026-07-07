// examples/dotnet/amplification — where the captured window's instructions actually GO:
// user code vs BCL vs the native runtime, and the WEAK-tier amplification factor.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // Addresses.Length = the whole window; LabelledInstructions = the managed slice;
//     // the remainder is the native runtime (RyuJIT/GC/PAL) single-step amplifies enormously.
//
// The honest envelope every single-step managed example lives inside: a one-line managed Work()
// single-steps ~1M runtime instructions. This one MEASURES that — total captured vs the managed
// (labelled) slice vs the native-runtime remainder, then splits the managed slice into user code
// and BCL by AsmMethod.Assembly. Counts only; the ctor auto-inits the single-step tier.

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
        Console.WriteLine("== amplification: user vs BCL vs native runtime, and the WEAK-tier factor ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
