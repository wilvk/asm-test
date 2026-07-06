// examples/dotnet/rundown — name WARM + ReadyToRun (R2R) methods via an in-process
// jitdump rundown (§D0.2), dependency-free and with NO launch knob.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true))
//         Work();          // Work calls Console.WriteLine — whose write path is R2R BCL
//     // ww.Methods now names the R2R BCL write path (StreamWriter::WriteLine, ConsolePal
//     // ::Write, the write syscall) — methods the cold-only §D0.1 listener never sees.
//
// The cold-only JitMethodMap (§D0.1) sees only methods JIT'd INSIDE the scope. Two kinds
// are missed: WARM methods (JIT'd before the scope) and — the bigger set — the ReadyToRun
// BCL, which is AOT-precompiled and never JIT'd at all. `withRundown: true` asks the
// runtime — over its own diagnostics socket, no NuGet, no DOTNET_PerfMapEnabled — for a
// perf-map rundown of PerfMapType.JitDump, which (unlike the text perf-map, which is
// JIT-only) runs the R2R rundown into /tmp/jit-<pid>.dump. AsmTrace folds that in, so warm
// AND R2R methods get named. Self-skips to the cold-only result where diagnostics are off.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // Cold (JIT'd inside the scope). It calls Console.WriteLine, whose whole write path —
    // SyncTextWriter/StreamWriter/ConsolePal/encoding/the write syscall — is R2R BCL.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        Console.WriteLine("[Work] running inside the scope (its R2R write path is what we name)");
    }

    static int Main()
    {
        Console.WriteLine("== §D0.2: naming WARM + R2R methods via an in-process jitdump rundown ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }

        int r2r = 0;
        foreach (AsmMethod m in ww.Methods) if (m.Name.Contains("[PreJIT]")) r2r++;
        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {ww.Methods.Count} methods labelled ({r2r} R2R); {ww.LabelledInstructions} instructions.\n");

        Console.WriteLine("methods that executed in the window (JIT, warm, and R2R BCL):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,8}  {m.Name}");

        // The R2R Console write path — precompiled BCL, never JIT'd, invisible to §D0.1.
        // (Console::WriteLine(string) itself is an inlined forwarder, so its work shows up
        // as these R2R callees: SyncTextWriter/StreamWriter::WriteLine, ConsolePal::Write.)
        long writePath = ww.InstructionsIn("StreamWriter::WriteLine")
                       + ww.InstructionsIn("ConsolePal::Write");
        Console.WriteLine();
        if (!ww.RundownEnabled)
            Console.WriteLine("-> rundown self-skipped (diagnostics off?); only cold methods are named.");
        else if (writePath > 0)
            Console.WriteLine($"-> the R2R BCL Console write path is NAMED ({writePath} instructions in\n"
                              + $"   StreamWriter::WriteLine + ConsolePal::Write, both [PreJIT]=ReadyToRun) —\n"
                              + $"   precompiled methods the cold-only §D0.1 path can never see. Program::Main\n"
                              + $"   (warm, JIT'd) is named too: {ww.InstructionsIn("Program::Main")} instructions.");
        else
            Console.WriteLine($"-> rundown enabled; {r2r} R2R ([PreJIT]) methods named above, which the cold-only\n"
                              + "   path cannot see.");
        return 0;
    }
}
