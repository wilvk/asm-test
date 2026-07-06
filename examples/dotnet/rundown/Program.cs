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

        // Zero config (§Z0): no HwTrace.Available/Init dance — the empty-ctor AsmTrace
        // auto-inits the portable single-step tier and self-skips (Report prints
        // SkipReason) where it cannot run.
        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default,\n"
                          + "auto-inited by the AsmTrace ctor (the STRONG Intel-PT / CEILING\n"
                          + "AMD-LBR tiers are forward-look)\n");

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        return Report.Print(ww) ? 0 : 1;
    }
}
