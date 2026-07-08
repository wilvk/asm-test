// examples/dotnet/crashproof-survey — reporting (presentation only), split from Program.cs.
// Renders the fidelity/cost/safety comparison table across the three legs, then a one-line
// per-form detail proving what each actually captured over the shared Work workload.

using System;
using Asmtest;

internal static class Report
{
    public static void PrintTable(AsmTrace m, long mResult,
                                  AsmTrace w, long windowResult,
                                  AsmTrace h, bool amdRan, string amdSkip, long hotResult)
    {
        // The three "captured" cells — the punchline of each leg.
        string mCap = m.Armed
            ? $"{CountInsns(m.Path)} insns, ONE body ({BodyName(m)})"
            : $"self-skip: {m.SkipReason}";
        string wCap = w.Armed
            ? $"{w.Methods.Count} methods, {w.Addresses.Length} insns (whole block)"
            : $"self-skip: {w.SkipReason}";
        string hCap = (amdRan && h != null && h.Armed)
            ? $"{h.Methods.Count} weighted hot methods (sampled)"
            : "self-skip (AMD) — see below";

        Console.WriteLine("FIDELITY / COST / SAFETY across the three crash-proof-capable forms:\n");
        var rows = new[]
        {
            new[] { "FORM",               "fidelity",     "crash-proof?",         "captured over Work" },
            new[] { "AsmTrace.Method",    "exact-body",   "only-the-body-safe",   mCap },
            new[] { "AsmTrace.Window",    "exact-window", "yes (out-of-process)", wCap },
            new[] { "AsmTrace.WindowHot", "sampled",      "yes (AMD LBR)",        hCap },
        };
        PrintGrid(rows);
        Console.WriteLine();

        // Per-leg detail — the exact evidence behind each row.
        Console.WriteLine("-- AsmTrace.Method (exact one body; in-process, only the body is stepped) --");
        if (m.Armed)
        {
            Console.WriteLine($"   Work(7,12) = {mResult}; body '{BodyName(m)}'; truncated: {m.Truncated}");
            string first = FirstInsn(m.Path);
            Console.WriteLine(first.Length > 0
                ? $"   exact stream, first insn: {first}"
                : "   (armed; no rendered listing — this build has no Capstone disassembler)");
            Console.WriteLine("   safe because it steps ONLY this JIT'd body — never the runtime around it.");
        }
        else Console.WriteLine($"   # self-skip: {m.SkipReason}");

        Console.WriteLine("\n-- AsmTrace.Window (exact whole block; out-of-process, crash-proof) --");
        if (w.Armed)
        {
            Console.WriteLine($"   block result (3x Work) = {windowResult}; {w.MethodsObserved} methods observed, "
                              + $"{w.Methods.Count} named; {w.Addresses.Length} instructions"
                              + (w.Truncated ? " (truncated)" : "") + ".");
            Console.WriteLine($"   Work's own share, named: {w.InstructionsIn("Work")} instructions.");
            int shown = 0;
            foreach (AsmMethod am in w.Methods)
            {
                if (shown++ >= 6) break;
                Console.WriteLine($"     {am.Count,6}  {am.ShortName}"
                                  + (am.Tier.Length > 0 ? $"  [{am.Tier}]" : ""));
            }
            Console.WriteLine("   this thread was NEVER EFLAGS.TF-armed — the helper stepped it out of band.");
        }
        else Console.WriteLine($"   # self-skip: {w.SkipReason}");

        Console.WriteLine("\n-- AsmTrace.WindowHot (sampled AMD-LBR survey; out-of-band, near-native) --");
        if (amdRan && h != null && h.Armed)
        {
            Console.WriteLine($"   hot loop result = {hotResult}; statistical={h.IsStatistical}; "
                              + $"{h.Addresses.Length} sampled endpoints"
                              + (h.Truncated ? " (a prefix — dropped/throttled)" : "")
                              + $"; {h.Methods.Count} methods named.");
            Console.WriteLine("   hot methods by SAMPLE WEIGHT (endpoint hits — NOT instruction counts):");
            int shown = 0;
            foreach (AsmMethod am in h.Methods)
            {
                if (shown++ >= 6) break;
                Console.WriteLine($"     {am.Count,8}  {am.ShortName}"
                                  + (am.Tier.Length > 0 ? $"  [{am.Tier}]" : ""));
            }
            Console.WriteLine($"   Work weight={h.InstructionsIn("Work")} — the sampled hot path pops out.");
        }
        else
        {
            Console.WriteLine($"   # self-skip (AMD): {amdSkip}");
            Console.WriteLine($"   (the hot loop still ran uninstrumented; result = {hotResult}.)");
        }

        Console.WriteLine("\n-> same Work, three answers: EXACT one body (Method), EXACT whole block out of\n"
                          + "   process (Window), and a SAMPLED hot-method survey (WindowHot). All three avoid\n"
                          + "   the fatal in-process whole-window single-step — that is the crash-proof menu.");
    }

    // Count rendered instruction lines in a scope listing (non-empty, non-comment).
    static int CountInsns(string path)
    {
        if (string.IsNullOrEmpty(path)) return 0;
        int n = 0;
        foreach (string line in path.Split('\n'))
            if (line.Length > 0 && line[0] != ';') n++;
        return n;
    }

    static string FirstInsn(string path)
    {
        if (string.IsNullOrEmpty(path)) return "";
        foreach (string line in path.Split('\n'))
            if (line.Length > 0 && line[0] != ';') return line.Trim();
        return "";
    }

    // The scope's Name is the call-site (member:line); the traced body is the known workload.
    static string BodyName(AsmTrace m) => "Work";

    // Fixed-width grid: pad every column to its widest cell (last column free).
    static void PrintGrid(string[][] rows)
    {
        int cols = 0;
        foreach (var r in rows) if (r.Length > cols) cols = r.Length;
        var width = new int[cols];
        foreach (var r in rows)
            for (int c = 0; c < r.Length; c++)
                if (r[c].Length > width[c]) width[c] = r[c].Length;

        for (int ri = 0; ri < rows.Length; ri++)
        {
            var r = rows[ri];
            var sb = new System.Text.StringBuilder("  ");
            for (int c = 0; c < r.Length; c++)
            {
                bool last = c == r.Length - 1;
                sb.Append(last ? r[c] : r[c].PadRight(width[c] + 2));
            }
            Console.WriteLine(sb.ToString());
            if (ri == 0) // underline the header
            {
                var u = new System.Text.StringBuilder("  ");
                for (int c = 0; c < cols; c++)
                {
                    string dash = new string('-', width[c]);
                    u.Append(c == cols - 1 ? dash : dash.PadRight(width[c] + 2));
                }
                Console.WriteLine(u.ToString());
            }
        }
    }
}
