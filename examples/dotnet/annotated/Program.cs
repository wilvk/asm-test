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

        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be disassembled.");
            return 0;
        }
        Console.WriteLine("backend: single-step WEAK tier (auto-inited; the portable x86-64 Linux default)");
        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}\n");

        long runtime = ww.Addresses.Length - ww.LabelledInstructions;
        Console.WriteLine($"captured {ww.Addresses.Length} instructions; {ww.LabelledInstructions} labelled "
                          + $"by method across {ww.Methods.Count} methods");
        Console.WriteLine($"({runtime} native-runtime instructions elided — RyuJIT/GC/PAL, unnamed).\n");

        // Print the COMPLETE labelled stream — every instruction in ww.Disassembly, in
        // execution order, at full width (no per-run cap, no row cap, no name clipping). The
        // leading instructions are the scope arming itself (AsmTrace::.ctor / set_Armed / the
        // begin_window P/Invoke stub), since the scope arms mid-constructor; Program.Work and
        // the BCL it calls follow. Between named runs, the count of elided native-runtime
        // (unlabelled) instructions is noted — those are not in Disassembly to disassemble.
        var all = ww.Disassembly;
        Console.WriteLine($"the complete labelled stream — all {all.Count} instructions, in execution order,");
        Console.WriteLine("no truncation (the leading rows are the scope arming; Program.Work follows):\n");
        Console.WriteLine($"  {"address",-14}  {"instruction",-44}  method");
        Console.WriteLine($"  {new string('-', 14)}  {new string('-', 44)}  {new string('-', 40)}");

        foreach (AsmInstruction i in all)
        {
            if (i.RuntimeBefore > 0)
                Console.WriteLine($"  {"",-14}  ... {i.RuntimeBefore} native-runtime insns ...");
            Console.WriteLine($"  0x{i.Address:x12}  {i.Text,-44}  {i.ShortMethod}");
        }

        Console.WriteLine($"\n-> all {all.Count} labelled instructions shown, each next to the method it ran in\n"
                          + "   (counts mark the unnamed native-runtime instructions elided between named runs).");
        return 0;
    }
}
