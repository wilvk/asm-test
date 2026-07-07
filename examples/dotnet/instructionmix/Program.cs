// examples/dotnet/instructionmix — what KIND of work runs: a mnemonic-class histogram.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     // classify each AsmInstruction.Text's mnemonic -> move / arith / branch / call / ...
//
// The other examples ask WHICH methods / HOW MANY instructions ran; this asks what those
// instructions ARE — the class mix (data movement vs arithmetic vs control flow vs SIMD) and the
// control-flow density (branches+calls per 100 instructions). Source: the first token of
// AsmInstruction.Text, with branch/call/ret confirmed STRUCTURALLY via Disas where Capstone is
// present. Counts only; the ctor auto-inits the single-step tier.

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
        Console.WriteLine("== instruction mix: the mnemonic-class histogram of the window ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        Report.Print(ww);
        return 0;
    }
}
