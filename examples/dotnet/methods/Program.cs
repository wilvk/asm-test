// examples/dotnet/methods — label a whole-window capture by MANAGED METHOD (§D0.1).
//
//     using (var ww = new AsmTrace(byMethod: true))   // opt into method labelling
//         ColdPath(data);                             // an arbitrary, COLD managed method
//     // ww.Methods is the per-method breakdown; ww.InstructionsIn("ColdPath") names it.
//
// The closest thing to the fully aspirational snippet that needs NO Intel PT: the empty
// scope owns an in-process JIT map (JitMethodMap, a MethodLoadVerbose EventListener) and
// attributes the captured window to managed methods — so an arbitrary cold method is
// identified BY NAME. Runs via the single-step WEAK tier: honest but intrusive (it also
// single-steps the JIT compiling the cold method). The clean, non-intrusive path is the
// STRONG whole-window PT tier (forward-look).

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A genuinely COLD managed method — JIT'd on its first call, inside the scope.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long ColdPath(long a, long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += a * (i + 1);
        
        Console.WriteLine($"-- ColdPath({a},{n}) = {s} --");

        return s;
    }

    static int Main()
    {
        Console.WriteLine("== labelling a cold managed method in a whole-window scope (§D0.1) ==\n");

        // Zero config (§Z0): no HwTrace.Available/Init dance — the empty-ctor AsmTrace
        // auto-inits the portable single-step tier and self-skips (Report prints
        // SkipReason) where it cannot run.
        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default,\n"
                          + "auto-inited by the AsmTrace ctor (the STRONG Intel-PT / CEILING\n"
                          + "AMD-LBR tiers are forward-look)\n");

        long r;
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true))
            r = ColdPath(7, 100);   // COLD: JIT compiles it here, then the body runs
        Report.Print(ww, r);
        return 0;
    }
}
