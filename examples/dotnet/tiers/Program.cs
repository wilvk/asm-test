// examples/dotnet/tiers — break the executed window down by JIT/compilation TIER.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // group ww.Methods by AsmMethod.Tier -> instructions per tier.
//
// A .NET-specific question none of the other examples answer: how much of the executed code
// ran PRECOMPILED (ReadyToRun / [PreJIT]) versus freshly JIT'd ([MinOptJitted]/[Tier0] cold,
// [OptimizedTier1] hot). The tier tag rides in the jitdump/perf-map method name, exposed as
// AsmMethod.Tier; withRundown is what makes the R2R BCL visible at all. Single-step WEAK tier;
// the ctor auto-inits it, so the setup is just the `using`.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // Spans several tiers: string.Join + the Console write path are R2R BCL ([PreJIT]); this
    // cold Work + warm Program::Main are JIT'd.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== executed instructions by JIT tier (R2R vs cold/hot JIT) ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
