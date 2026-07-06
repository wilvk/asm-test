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
// STRONG whole-window PT tier (forward-look on this AMD host).

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
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== labelling a cold managed method in a whole-window scope (§D0.1) ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        long r;
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true))
            r = ColdPath(7, 100);   // COLD: JIT compiles it here, then the body runs
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }

        Console.WriteLine($"ColdPath(7,100) = {r}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {ww.MethodsObserved} methods observed; {ww.LabelledInstructions} labelled by method.\n");

        Console.WriteLine("managed methods that executed in the window (by instruction count):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,8}  {m.Name}");

        Console.WriteLine($"\n-> the arbitrary COLD method 'ColdPath' is identified BY NAME: "
                          + $"{ww.InstructionsIn("ColdPath")} instructions.");
        Console.WriteLine($"(the native runtime — RyuJIT, GC, PAL — is the unlabelled remainder: "
                          + $"{ww.Addresses.Length - ww.LabelledInstructions} instructions.)");
        return 0;
    }
}
