// examples/dotnet/localscope — reporting + verdict, split from Program.cs.
// Prints what the block computed (proof it ran, un-elided) and the by-method breakdown of
// the closed whole-window scope. The point it demonstrates: the block's OWN compiled code —
// Program::Main, the lambda display class (Program+<>c), delegate construction — is named,
// so the inline block (not a separate Work() method) is what the scope traced. Returns
// false only when the scope armed but did NOT name the block's own code, so CI exits nonzero
// on a real §D0.1 regression.
//
// Honest scope note: a whole-window single-step captures EVERYTHING the thread runs (~1M
// runtime instructions per arm — JIT, GC, the ctor's own rundown IPC), of which only a small
// slice resolves to a managed name. So this names a SUBSET, not every BCL method each feature
// reaches; for tight per-method attribution of one body, see the methods/ and descent/ demos.

using System;
using Asmtest;

internal static class Report
{
    public static bool Print(AsmTrace ww, string summary, int sumOfSquares, int aboveThreshold, long fib)
    {
        // Always show that the inline block actually executed (armed or not).
        Console.WriteLine($"block result: {summary}");
        Console.WriteLine($"  (sumOfSquares={sumOfSquares}, aboveThreshold={aboveThreshold}, fib12={fib})\n");

        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return true;
        }

        int r2r = 0;
        foreach (AsmMethod m in ww.Methods) if (m.Tier == "PreJIT") r2r++;
        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated — whole-window single-step over a whole block)" : "")
                          + $"; {ww.Methods.Count} methods named ({r2r} R2R); "
                          + $"{ww.LabelledInstructions} labelled instructions.\n");

        // The named window, most-attributed first (the raw runtime remainder is elided; this
        // is the labelled managed stream only).
        Console.WriteLine("methods named in the block's window (JIT, warm, and R2R BCL):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,6}  {m.ShortName}"
                              + (m.Tier.Length > 0 ? $"  [{m.Tier}]" : ""));
        Console.WriteLine();

        // The demonstration: the block's OWN compiled code is what got traced — no Work()
        // method exists, so this is proof the inline block itself is the traced unit.
        long ownMain    = ww.InstructionsIn("Program::Main");
        long ownClosure = ww.InstructionsIn("Program+<>c") + ww.InstructionsIn("MulticastDelegate");
        long ownLocals  = ww.InstructionsIn("g__");   // <Main>g__Fib / <Main>g__MinMax
        Console.WriteLine("the inline block's own code, named (no Work() method — this IS the traced unit):");
        Console.WriteLine($"    Program::Main (the block itself)     : {ownMain} instructions"
                          + (ownMain > 0 ? "" : "  (fell outside the labelled slice this run)"));
        Console.WriteLine($"    lambdas/closures (Program+<>c, etc.) : {ownClosure} instructions");
        Console.WriteLine($"    static local functions (Fib/MinMax)  : {ownLocals} instructions\n");

        // The verdict: the block's own Main must appear in the labelled window — otherwise
        // the §D0.1 byMethod map failed to attribute the inline block that ran in the scope.
        if (ownMain == 0)
        {
            Console.WriteLine("FAIL: scope armed but the inline block's own Program::Main was not named");
            return false;
        }
        Console.WriteLine($"-> {ww.Methods.Count} managed methods named, including the block's own "
                          + "Program::Main —\n   a whole block of ordinary C# (LINQ, tuples, pattern matching, "
                          + "generics, local\n   functions, StringBuilder) is traced and attributed with no "
                          + "dedicated Work() method.");
        return true;
    }
}
