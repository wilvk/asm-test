// examples/dotnet/amdhot — the STATISTICAL AMD-LBR whole-window survey, as an INLINE `using`
// scope (the unified inline-using form; AsmTrace.WindowHot(Action) is the delegate sibling).
//
//     using (ww = new AsmTrace(HwBackend.AmdLbr))   // ctor arms the sampler
//         { ...managed block... }                    // runs inline at native speed; Dispose drains
//     foreach (var m in ww.Methods) Console.WriteLine($"{m.Count}  {m.ShortName}");  // hot weights
//
// AMD LBR (Zen 3+/LbrExtV2) SAMPLES the branch stack out of band while the block runs at
// NATIVE speed, and the sampled branch-target endpoints are bucketed by managed method into
// a HOT-METHOD histogram. This is the honest AMD whole-window shape: exact whole-window is a
// hardware dead end on AMD (16-deep stack + throttle), so — like AutoFDO/BOLT — the answer is
// a statistical survey, not an exact trace. Its wins over the other whole-window forms:
//   - vs in-process single-step (localscope): crash-proof — no EFLAGS.TF, no SIGTRAP, so it
//     survives exceptions / pthread_create that SIGTRAP the single-step tier (exit 133);
//   - vs out-of-process ptrace (localscope_oop_managed): near-native (a handful of PMIs) vs
//     ~100-1000x per single-step.
// ww.IsStatistical is true; ww.Methods[].Count is a SAMPLE WEIGHT (endpoint hits), not an
// instruction count; ww.Truncated means "a prefix" (dropped samples), a coverage signal.
// Needs Zen 3+/LBR + CAP_PERFMON — self-skips (exit 0) in the plain lane; the
// docker-hwtrace-amd-style permissioned lane runs it live.

using System;
using System.Collections.Generic;
using System.Linq;
using Asmtest;

internal static class Program
{
    // A HOT method: called tens of thousands of times inside the window, so its own branch
    // targets dominate the sampled endpoints and it tops the survey.
    static long HotSum(int n)
    {
        long s = 0;
        for (int i = 0; i < n; i++) s += i * 3 - (i & 7);
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== AMD LBR statistical whole-window survey (AsmTrace.WindowHot) ==\n");
        Console.WriteLine("AMD LBR samples the branch stack out of band while the block runs at native\n"
                          + "speed — crash-proof (no EFLAGS.TF/SIGTRAP), a handful of PMIs. SAMPLED, not exact.\n");

        long total = 0;
        AsmTrace ww;
        using (ww = new AsmTrace(HwBackend.AmdLbr))   // ctor arms the branch-stack sampler
        {
            for (int k = 0; k < 40000; k++)     // the hot path: HotSum dominates the survey
                total += HotSum(60);
            total += Enumerable.Range(0, 2000)  // some LINQ, so it shows lower in the histogram
                               .Where(x => x % 3 == 0).Select(x => x * x).Sum();
        }                                             // Dispose drains + attributes

        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0; // honest degrade (no Zen LBR / no CAP_PERFMON) — the block still ran
        }

        Console.WriteLine($"statistical={ww.IsStatistical}; {ww.Addresses.Length} sampled branch-target "
                          + $"endpoints{(ww.Truncated ? " (a prefix — dropped/throttled samples)" : "")}; "
                          + $"{ww.Methods.Count} methods named.\n");
        Console.WriteLine("hot methods by SAMPLE WEIGHT (branch-target endpoint hits — NOT instruction counts):");
        foreach (AsmMethod m in ww.Methods)
            Console.WriteLine($"    {m.Count,8}  {m.ShortName}"
                              + (m.Tier.Length > 0 ? $"  [{m.Tier}]" : ""));

        long hot = ww.InstructionsIn("HotSum");
        Console.WriteLine($"\n(total={total})");
        Console.WriteLine($"-> a whole block of managed C# surveyed OUT-OF-BAND by AMD LBR, crash-proof and\n"
                          + $"   near-native. HotSum weight={hot} — the sampled hot path pops out, as expected.\n"
                          + "   Weights are SAMPLED (statistical), not an exact instruction trace.");
        return ww.Methods.Count > 0 ? 0 : 1;
    }
}
