// examples/dotnet/amplification — reporting (presentation only). Splits the captured window
// into user / BCL / native-runtime and prints the amplification factor. No tracing here.

using System;
using System.Collections.Generic;
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

        long total = ww.Addresses.Length;          // the WHOLE window (runtime included)
        long labelled = ww.LabelledInstructions;    // the managed slice
        long runtime = total - labelled;            // RyuJIT / GC / PAL — the unlabelled rest

        // Split the managed slice into user code vs BCL by declaring assembly / name.
        long bcl = 0, user = 0;
        foreach (AsmMethod m in ww.Methods)
            if (IsBcl(m)) bcl += m.Count; else user += m.Count;

        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; captured {total} instructions in the window"
                          + (ww.Truncated ? " (truncated)" : "") + $" for ONE managed Work() call.\n");
        Console.WriteLine($"  {"bucket",-26}{"insns",12}{"share",9}");
        Console.WriteLine("  " + new string('-', 47));
        Row("user code (Program.*)", user, total);
        Row("BCL (System.*, labelled)", bcl, total);
        Row("native runtime (unlabelled)", runtime, total);
        Console.WriteLine("  " + new string('-', 47));
        Row("total window", total, total);

        double factor = labelled > 0 ? (double)total / labelled : 0;
        Console.WriteLine($"\namplification factor: {factor:F0}x — {total} captured instructions per "
                          + $"{labelled} labelled\n   managed one (the native runtime single-step steps to reach + run the call).");
        Console.WriteLine("-> this is WHY the managed single-step tier is the WEAK tier: a one-line Work() drags\n"
                          + "   the whole runtime through EFLAGS.TF. The clean managed path is STRONG whole-window PT\n"
                          + "   (region-filtered at decode) — forward-look. Counts only; no time is meaningful here.");
    }

    // BCL if the declaring assembly is a framework module, or (for a listener-spelled dotted
    // name that carries no assembly tag) the name is under System./Microsoft.
    static bool IsBcl(AsmMethod m)
    {
        string a = m.Assembly;
        if (a.StartsWith("System", StringComparison.Ordinal) || a.StartsWith("Microsoft", StringComparison.Ordinal))
            return true;
        if (a.Length == 0)
            return m.Name.StartsWith("System.", StringComparison.Ordinal)
                || m.Name.StartsWith("Microsoft.", StringComparison.Ordinal)
                || m.Name.StartsWith("Internal.", StringComparison.Ordinal);
        return false;
    }

    static void Row(string label, long n, long total)
    {
        double pct = total > 0 ? 100.0 * n / total : 0;
        Console.WriteLine($"  {label,-26}{n,12}{pct,8:F2}%");
    }
}
