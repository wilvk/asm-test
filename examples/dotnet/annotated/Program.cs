// examples/dotnet/annotated — an ANNOTATED EXECUTION TRACE: each executed instruction in
// one column, the method it belongs to in the next. A byMethod+withRundown whole-window
// scope records every stepped instruction (absolute RIP) in execution order; the binding
// disassembles the ones it can NAME (live, on close) and pairs each with its method:
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     foreach (AsmInstruction i in ww.Disassembly)   // labelled insns, execution order
//         print(i.Text, i.ShortMethod);              // "mov rax, rdi"  ->  Program.Work
//
// This demo prints the COMPLETE labelled stream (ww.Disassembly, ~1300 insns for this
// workload) — every instruction, in execution order, at full width, with NO truncation: no
// per-run cap, no row cap, no name clipping. You see the whole control flow end to end —
// the scope arming, Program.Work, string.Join, and the full Console.WriteLine descent
// (Console::WriteLine -> SyncTextWriter -> StreamWriter::WriteLine -> Flush -> the UTF-8
// encode feeding write()). The only things NOT shown are the unlabelled native-runtime
// instructions (RyuJIT/GC/PAL) between named runs — they are not in Disassembly to
// disassemble, so their count is noted instead. Single-step WEAK tier; self-skips cleanly
// where single-step / Capstone is unavailable.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A small workload spanning several methods + assemblies: string.Join + array
    // (System.Private.CoreLib) then the Console write path (System.Console + CoreLib).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== annotated execution trace: each instruction next to its method ==\n");

        // No HwTrace.Init / Available pre-check: the whole-window ctor lazily brings up the
        // portable single-step tier itself and reports honestly via Armed / SkipReason. So the
        // setup is just the `using` — the aspirational zero-config form.
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();

        Report.Print(ww);
        return 0;
    }
}
