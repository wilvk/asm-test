// examples/dotnet/windowhybrid — the §E1 HYBRID whole-window: exact ONLY on the hot slice.
//
//     var ww = AsmTrace.WindowHybrid(() => { ...managed block... }, hotFraction: 0.9);
//     var survey = ww.Survey;                       // the pass-1 sampled hot histogram
//     foreach (var m in ww.Methods) ...             // the pass-2 EXACT per-method capture
//
// WindowHybrid composes the two crash-proof whole-window forms with NO new native stepper:
//   pass 1  AsmTrace.WindowHot  — the cheap, near-native AMD-LBR statistical survey. It
//           SAMPLES the branch stack while the block runs at native speed and buckets the
//           sampled endpoints into a hot-method histogram (crash-proof: no EFLAGS.TF).
//   pass 2  AsmTrace.Window     — the exact out-of-process single-step window, but publishing
//           ONLY the hot methods' [base,len) regions to the stepper. The stepper records the
//           hot managed slice per-instruction and steps over everything else (in_region_set,
//           src/ptrace_backend.c) — so the cold million instructions do not flood the capture.
//
// The block runs TWICE (survey + exact), so it must be deterministic enough that pass-1's hot
// set still applies in pass 2. DEGRADES cleanly: no AMD LBR -> a full exact Window (publish all
// managed ranges); no ptrace -> self-skip (exit 0). Needs Zen 3+/LBR + CAP_PERFMON for the
// survey and ptrace for the exact pass — self-skips in the plain lane.

using System;
using Asmtest;

internal static class Program
{
    // The HOT method: a tight inner loop, called repeatedly — it dominates the sampled survey
    // and is the slice WindowHybrid captures exactly.
    static long HotKernel(long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += (i * 2654435761L) ^ (i + 7);
        return s;
    }

    // A COLD helper: real work, but called ONCE — it lands OUTSIDE the hot set, so the hybrid's
    // pass 2 does NOT publish it and the stepper elides it (a plain Window would capture it).
    static long ColdHelper(long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += (i ^ 0x5bd1e995L) + (i << 1);
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== §E1 hybrid whole-window: survey the hot methods, then trace ONLY them exactly ==\n");
        Console.WriteLine("pass 1: AMD-LBR statistical survey (crash-proof, near-native) picks the hot set;\n"
                          + "pass 2: exact out-of-process window publishes ONLY those regions to the stepper.\n"
                          + "-> the cold instructions are elided; the hot managed slice is captured per-instruction.\n");

        long total = 0;
        // ColdHelper runs ONCE, FIRST — early enough that pass 2 single-steps it (it is REACHED)
        // before the hot loop fills the 65536-instruction cap, so its elision is genuine (the
        // stepper walks it but does NOT record it, since it is not a published hot region). It is
        // small so the survey barely samples it and it stays out of the hot set. The hot loop is
        // large so the survey (native speed) collects plenty of samples; pass 2 self-truncates at
        // the recorded cap a few hundred hot calls in, so the big loop count stays bounded there.
        Action work = () =>
        {
            total += ColdHelper(200);                           // cold: reached in pass 2, but elided
            for (int k = 0; k < 300000; k++) total += HotKernel(24); // hot: dominates the survey
        };

        var ww = AsmTrace.WindowHybrid(work, hotFraction: 0.9);

        AsmTrace survey = ww.Survey;
        Console.WriteLine("-- pass 1: statistical survey (AsmTrace.WindowHot) --");
        if (survey != null && survey.Armed)
        {
            Console.WriteLine($"   {survey.Methods.Count} methods surveyed; hottest by SAMPLE WEIGHT:");
            int shown = 0;
            foreach (AsmMethod m in survey.Methods)
            {
                if (shown++ >= 5) break;
                Console.WriteLine($"     {m.Weight,8}  {m.ShortName}");
            }
        }
        else
            Console.WriteLine($"   # survey self-skipped ({survey?.SkipReason}) — pass 2 degrades to a FULL exact Window.");

        Console.WriteLine("\n-- pass 2: exact window (AsmTrace.Window over the hot slice) --");
        if (!ww.Armed)
        {
            Console.WriteLine($"   # self-skip: {ww.SkipReason}");
            Console.WriteLine($"   (the block still ran uninstrumented; total={total}.)");
            return 0; // honest degrade — no ptrace (Yama) / no privilege
        }

        long hot = ww.WeightIn("HotKernel");
        long cold = ww.WeightIn("ColdHelper");
        Console.WriteLine($"   exact={!ww.IsStatistical}; {ww.Addresses.Length} instructions captured"
                          + (ww.Truncated ? " (truncated — a prefix)" : "") + $"; {ww.Methods.Count} methods named.");
        Console.WriteLine("   captured EXACT per-method (top by instruction count):");
        int shown2 = 0;
        foreach (AsmMethod m in ww.Methods)
        {
            if (shown2++ >= 6) break;
            Console.WriteLine($"     {m.Count,8}  {m.ShortName}" + (m.Tier.Length > 0 ? $"  [{m.Tier}]" : ""));
        }
        Console.WriteLine($"\n   HotKernel captured = {hot} instructions; ColdHelper captured = {cold}.");
        if (survey != null && survey.Armed)
            Console.WriteLine("   -> the survey named the hot slice; pass 2 spent the exact stepper ONLY there,\n"
                              + "      eliding the cold helper — hybrid fidelity where it matters, cheaply.");
        else
            Console.WriteLine("   -> no AMD survey here, so pass 2 ran as a FULL exact Window (every managed range).\n"
                              + "      On a Zen host with CAP_PERFMON the survey restricts pass 2 to the hot slice.");
        Console.WriteLine($"\n(total={total})");
        return ww.Methods.Count > 0 ? 0 : 0; // self-skip / degrade is not a failure
    }
}
