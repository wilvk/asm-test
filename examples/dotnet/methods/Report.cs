// examples/dotnet/methods — reporting (presentation only), split from Program.cs.
// Prints the per-managed-method breakdown of the closed byMethod scope. No tracing here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww, long r)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }

        Console.WriteLine($"ColdPath(7,100) = {r}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {ww.MethodsObserved} methods observed; {ww.LabelledInstructions} labelled by method.\n");

        Console.WriteLine("managed methods that executed in the window (by instruction count):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,8}  {m.Name}");

        Console.WriteLine($"\n-> the arbitrary COLD method 'ColdPath' is identified BY NAME: "
                          + $"{ww.InstructionsIn("ColdPath")} instructions.");
        Console.WriteLine($"(the native runtime — RyuJIT, GC, PAL — is the unlabelled remainder: "
                          + $"{ww.Addresses.Length - ww.LabelledInstructions} instructions.)");
    }
}
