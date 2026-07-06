// examples/dotnet/annotated — reporting (presentation only), split from Program.cs.
// Prints the COMPLETE labelled instruction stream of the closed scope, each instruction
// next to the method it ran in, at full width with no truncation. No tracing logic here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be disassembled.");
            return;
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
    }
}
