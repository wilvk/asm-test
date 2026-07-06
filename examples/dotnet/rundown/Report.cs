// examples/dotnet/rundown — reporting (presentation only), split from Program.cs.
// Prints the JIT/warm/R2R method breakdown of the closed rundown scope. No tracing here.

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
    }
}
