// examples/dotnet/rundown — reporting + verdict, split from Program.cs.
// Prints the JIT/warm/R2R method breakdown of the closed rundown scope and returns
// false when an enabled rundown failed to name any R2R method (the §D0.2 claim this
// example exists to demonstrate) — so the lane exits nonzero on a real regression.

using System;
using Asmtest;

internal static class Report
{
    public static bool Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return true;
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
        {
            Console.WriteLine("-> rundown self-skipped (diagnostics off?); only cold methods are named.");
            return true; // honest degradation, not a failure
        }
        if (writePath > 0)
            Console.WriteLine($"-> the R2R BCL Console write path is NAMED ({writePath} instructions in\n"
                              + $"   StreamWriter::WriteLine + ConsolePal::Write, both [PreJIT]=ReadyToRun) —\n"
                              + $"   precompiled methods the cold-only §D0.1 path can never see. Program::Main\n"
                              + $"   (warm, JIT'd) is named too: {ww.InstructionsIn("Program::Main")} instructions.");
        else
            Console.WriteLine($"-> rundown enabled; {r2r} R2R ([PreJIT]) methods named above, which the cold-only\n"
                              + "   path cannot see.");
        // The verdict: an enabled rundown must name at least one R2R method — Work()
        // ran Console.WriteLine in-window, whose write path is R2R BCL by construction.
        if (r2r == 0)
            Console.WriteLine("FAIL: rundown enabled but no [PreJIT] (R2R) method was named");
        return r2r > 0;
    }
}
