// examples/dotnet/localscope_oop_managed — reporting + verdict. Shows the block ran (the
// overflow was caught, proving it survived a case the in-process form crashes on) and the
// managed methods the out-of-process window named. Returns false only when the scope armed
// but named nothing — so the lane exits nonzero on a real §D3 regression.

using System;
using Asmtest;

internal static class Report
{
    public static bool Print(AsmTrace ww, string summary, int sumOfSquares, int aboveThreshold, long fib)
    {
        Console.WriteLine($"block result: {summary}");
        Console.WriteLine($"  (sumOfSquares={sumOfSquares}, aboveThreshold={aboveThreshold}, fib12={fib})");
        Console.WriteLine($"  overflow-caught in-window: {(summary.Contains("overflow caught OOP") ? "YES" : "no")} "
                          + "(the in-process localscope had to omit this — it SIGTRAPs there)\n");

        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return true; // honest degradation (ptrace denied) — block still ran, no crash
        }

        int r2r = 0;
        foreach (AsmMethod m in ww.Methods) if (m.Tier == "PreJIT") r2r++;
        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {ww.Methods.Count} methods named ({r2r} R2R); "
                          + $"{ww.LabelledInstructions} labelled instructions — ALL out of process.\n");

        Console.WriteLine("methods named in the out-of-process window (JIT + R2R BCL):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,6}  {m.ShortName}"
                              + (m.Tier.Length > 0 ? $"  [{m.Tier}]" : ""));
        Console.WriteLine();

        long own = ww.InstructionsIn("Program+<>c") + ww.InstructionsIn("<Main>");
        Console.WriteLine($"the block's OWN compiled code, named: {own} instructions "
                          + "(the lambda body + its local functions).\n");

        if (ww.Methods.Count == 0)
        {
            Console.WriteLine("FAIL: window armed but named no managed method");
            return false;
        }
        Console.WriteLine($"-> {ww.Methods.Count} managed methods named from a whole block of C# — captured\n"
                          + "   OUT OF PROCESS, and the block survived an in-scope exception that crashes the\n"
                          + "   in-process whole-window scope. This thread was never armed with EFLAGS.TF.\n");
        Console.WriteLine("NOTE: this captures the block's OWN code + already-mapped R2R BCL. Methods JIT'd\n"
                          + "FRESH mid-window (a first-call generic instantiation) land outside the pre-window\n"
                          + "code ranges and are elided — publishing them live from the single-stepped thread\n"
                          + "re-enters the runtime under step and aborts it; a sibling-thread publish is the\n"
                          + "documented follow-up (docs/internal/plans/managed-wholewindow-oop-plan.md).");
        return true;
    }
}
