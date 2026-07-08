// examples/dotnet/single-method — reporting (presentation only), split from Program.cs.
// Renders the one-method scope: its named body, the exact stepped instruction stream, and
// (for the in-process leg) that the stream is JUST the body — no runtime amplification.

using System;
using Asmtest;

internal static class Report
{
    public static void PrintInProcess(AsmTrace m, long r)
    {
        Console.WriteLine("-- in-process lazy-arm (only the body is single-stepped) --");
        if (!m.Armed)
        {
            Console.WriteLine($"# self-skip: {m.SkipReason}");
            return;
        }
        Console.WriteLine($"armed '{m.Name}', Work(21,8) = {r}.");
        PrintPath(m);
        Console.WriteLine("-> exactly one JIT'd body, stepped in isolation — none of the ~1M "
                        + "instructions of runtime a whole-window capture would also record.\n");
    }

    public static void PrintOutOfProcess(AsmTrace o, long r)
    {
        Console.WriteLine("-- out-of-process (outOfProcess: true — crash-proof, no TF on this thread) --");
        if (!o.Armed)
        {
            Console.WriteLine($"# self-skip: {o.SkipReason}");
            return;
        }
        Console.WriteLine($"armed '{o.Name}' out of band, Work(21,8) = {r}; truncated: {o.Truncated}");
        Console.WriteLine("-> same one-method body, stepped by the reverse-attach helper: safe "
                        + "for ANY body (a whole-window in-process step is not).");
    }

    static void PrintPath(AsmTrace m)
    {
        string path = m.Path ?? "";
        if (path.Length == 0)
        {
            Console.WriteLine("    (no rendered listing — this build has no Capstone disassembler)");
            Console.WriteLine($"    truncated: {m.Truncated}");
            return;
        }
        Console.WriteLine("rendered listing — the body's executed instructions:");
        int shown = 0;
        foreach (string line in path.Split('\n'))
        {
            if (line.Length == 0 || line[0] == ';') continue;
            if (shown++ < 24) Console.WriteLine($"    {line}");
        }
        if (shown > 24) Console.WriteLine($"    … ({shown - 24} more)");
        Console.WriteLine($"truncated: {m.Truncated}");
    }
}
