// examples/dotnet/rundown — name WARM methods too, via an in-process perf-map rundown
// (§D0.2), dependency-free and with no launch knob.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true))
//         Work();          // Work is cold; it calls Console.WriteLine, which is WARM
//     // ww.Methods now includes System.Console.WriteLine — a method JIT'd BEFORE the scope.
//
// The cold-only JitMethodMap (§D0.1) sees only methods JIT'd inside the scope, so warm
// BCL methods fall into the unlabelled runtime remainder. `withRundown: true` asks the
// runtime — over its own diagnostics socket, no NuGet, no DOTNET_PerfMapEnabled — to run
// down ALL already-JIT'd methods into /tmp/perf-<pid>.map, which AsmTrace folds into the
// breakdown. Self-skips to the cold-only result where diagnostics are off.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // Cold (JIT'd inside the scope). It calls Console.WriteLine, which is WARM — already
    // JIT'd by the header prints in Main, before the scope's map exists.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        Console.WriteLine("[Work] running inside the scope (calls warm Console.WriteLine)");
    }

    static int Main()
    {
        Console.WriteLine("== §D0.2: naming WARM methods via an in-process perf-map rundown ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();   // Work: cold; Console.WriteLine: warm
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }

        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {ww.Methods.Count} methods labelled; {ww.LabelledInstructions} instructions.\n");

        Console.WriteLine("methods that executed in the window (warm + cold):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,8}  {m.Name}");

        // Program::Main is unambiguously WARM — it was JIT'd at startup, before the scope
        // existed, so the cold-only §D0.1 listener can never name it. (Console.WriteLine
        // itself is a thin, inlined forwarder, so the work you'd expect under it shows up
        // as its warm callees — System.Threading.Monitor::Enter, System.Buffer::Memmove.)
        long warmMain = ww.InstructionsIn("Program::Main");
        Console.WriteLine();
        if (!ww.RundownEnabled)
            Console.WriteLine("-> rundown self-skipped (diagnostics off?); only cold methods are named.");
        else if (warmMain > 0)
            Console.WriteLine($"-> a WARM method — Program::Main, JIT'd at startup BEFORE the scope — is now\n"
                              + $"   named ({warmMain} instructions), alongside warm BCL methods (Monitor::Enter,\n"
                              + $"   Buffer::Memmove). The cold-only §D0.1 path names only methods JIT'd inside\n"
                              + $"   the scope (here just Program.Work).");
        else
            Console.WriteLine("-> rundown enabled; the perf-map named warm methods above (::-format), which the\n"
                              + "   cold-only path cannot see.");
        return 0;
    }
}
