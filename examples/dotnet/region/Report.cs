// examples/dotnet/region — reporting (presentation only), split from Program.cs.
// Renders the region-scoped listing from the closed scope. No tracing logic here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace scope, long r)
    {
        if (!scope.Armed)
        {
            Console.WriteLine($"# self-skip: {scope.SkipReason}");
            return;
        }

        Console.WriteLine($"armed '{scope.Name}', add2(20,22) = {r}.");
        Console.WriteLine("rendered listing — EXACTLY the routine's executed instructions:");
        foreach (string line in (scope.Path ?? "").Split('\n'))
            if (line.Length > 0 && line[0] != ';')
                Console.WriteLine($"    {line}");
        Console.WriteLine($"truncated: {scope.Truncated}");
        Console.WriteLine("-> region-scoped gives the clean, isolated assembly path "
                          + "(the dec at 0xc is absent: add2(20,22)=42<=100, so the jle is taken).");
    }
}
