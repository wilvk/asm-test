// examples/dotnet/perf-triage-drill — reporting (presentation only), split from Program.cs.
// Renders Phase 1 (the by-method survey histogram + which method the survey picked) and
// Phase 2 (the isolated drill into that one body). No tracing happens here.

using System;
using Asmtest;

internal static class Report
{
    // PHASE 1: the whole-window by-method histogram, with our workloads flagged USER so the
    // hottest one stands out from the native-runtime remainder the survey also single-stepped.
    public static void Survey(AsmTrace w, long sink, AsmMethod hottest, bool found)
    {
        Console.WriteLine("-- Phase 1: whole-window by-method SURVEY (new AsmTrace(byMethod, withRundown)) --");
        Console.WriteLine($"sink={sink}; captured {w.Addresses.Length} instructions"
                          + (w.Truncated ? " (truncated)" : "")
                          + $"; {w.MethodsObserved} methods observed; {w.LabelledInstructions} labelled by method"
                          + $"; rundown={w.RundownEnabled}.\n");

        Console.WriteLine("by-method histogram (top methods by captured instruction count):");
        int shown = 0;
        foreach (AsmMethod m in w.Methods)
        {
            if (shown++ >= 14) break;
            string flag = m.Name.Contains("Program") ? "  <- USER" : "";
            Console.WriteLine($"    {m.Count,8}  {m.Name}{flag}");
        }
        long remainder = w.Addresses.Length - w.LabelledInstructions;
        Console.WriteLine($"    ({remainder} unlabelled — the native runtime: RyuJIT, GC, PAL — the survey's amplification.)\n");

        if (found)
            Console.WriteLine($"-> hottest USER method: {hottest.Name}"
                              + $" ({hottest.Count} instructions). Phase 2 drills into exactly this body.\n");
        else
            Console.WriteLine("-> no USER method resolved by name in the survey; Phase 2 falls back to Hot.\n");
    }

    // PHASE 2: the isolated drill — only the picked body was stepped, none of the runtime.
    public static void Drill(AsmTrace m, string tag, long r)
    {
        Console.WriteLine($"-- Phase 2: DRILL into '{tag}' (AsmTrace.Method(del).Invoke) --");
        if (!m.Armed)
        {
            Console.WriteLine($"# self-skip: {m.SkipReason}");
            return;
        }
        Console.WriteLine($"armed '{m.Name}', {tag}(6,7) = {r}.");
        PrintPath(m);
        Console.WriteLine("-> exactly one JIT'd body, stepped in isolation — the survey found it, the drill\n"
                        + "   isolated it: zero of the runtime amplification Phase 1 had to record.");
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
        Console.WriteLine("rendered listing — the drilled body's executed instructions:");
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
