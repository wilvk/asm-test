// examples/dotnet/amd-period-sweep — the AMD-LBR statistical survey SWEPT across sample PERIODS.
//
//     foreach (int P in { 4, 16, 64, 256 })
//         using (var h = AsmTrace.WindowHot(() => Hot(...), period: P)) { }  // same block, coarser→finer
//     // h.Addresses.Length = sampled endpoints; h.Methods[0] = top hot method by SAMPLE WEIGHT
//
// amdhot runs ONE AMD-LBR whole-window survey at the default period; this turns the PERIOD knob
// AsmTrace.WindowHot already exposes (no new binding) and runs the SAME hot managed block at four
// periods so the cost/resolution tradeoff shows up in the numbers:
//   - SMALLER period (e.g. 4) = the PMU samples the branch stack more often = MORE endpoint
//     samples = a FINER survey — but more PMIs and more throttling risk (the kernel drops samples;
//     h.Truncated then flags "a prefix");
//   - LARGER period (e.g. 256) = coarser but cheaper — fewer PMIs, fewer endpoints to bucket.
// The survey is SAMPLED, not exact: h.IsStatistical is true, and h.Methods[].Count is a
// branch-target endpoint WEIGHT, not an instruction count. AMD sampling is non-deterministic per
// attempt — a given period can land 0 endpoints on a run; that is honest, so (like amdlbr/) we
// retry a period a few times and report 0 when nothing lands. Needs Zen 3+/LBR + CAP_PERFMON —
// self-skips (exit 0) in the plain lane; the docker-hwtrace-amd-style permissioned lane runs live.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A HOT (long,long)->long method with a big counted loop: its back-edge AND its data-dependent
    // `if` give AMD LBR real branch density to sample, so its own branch targets dominate the
    // survey. NoInlining keeps it a distinct frame (a single call site inlines otherwise), so the
    // attribution lands on Hot at every period rather than dissolving into the caller's closure.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long Hot(long n, long mask)
    {
        long s = 0;
        for (long i = 0; i < n; i++)
        {
            long v = i * 3 - (i & 7);
            if ((v & mask) != 0) s += v;   // data-dependent taken/not-taken branch
            else s -= i;
        }
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== AMD-LBR statistical survey, SWEPT across sample periods (AsmTrace.WindowHot) ==\n");

        if (!HwTrace.Available(HwBackend.AmdLbr))
        {
            Console.WriteLine($"# self-skip: AMD LBR unavailable: {HwTrace.SkipReason(HwBackend.AmdLbr)}");
            return 0; // honest degrade off Zen 3+/LBR or without CAP_PERFMON — nothing to sweep
        }

        Console.WriteLine("AMD LBR samples the branch stack out of band while the block runs at native speed.\n"
                          + "Same hot block, four PERIODS: smaller = more samples (finer, but more PMIs /\n"
                          + "throttling); larger = coarser but cheaper. SAMPLED, not an exact trace.\n");

        int[] periods = { 4, 16, 64, 256 };
        Console.WriteLine("  period   endpoints   top-weight  top hot method                       truncated");
        Console.WriteLine("  ------   ---------   ----------  ----------------------------------   ---------");

        long grand = 0;
        foreach (int p in periods)
        {
            AsmTrace h = null;
            int endpoints = 0;
            // AMD sampling is non-deterministic per attempt — a window can land 0 endpoints. Like
            // amdlbr/, retry the SAME period a few times until something lands (else report 0).
            for (int attempt = 0; attempt < 6; attempt++)
            {
                using (h = AsmTrace.WindowHot(
                    () => grand += Hot(800_000, 5),  // the hot path — Hot owns all the branch density
                    period: p)) { }

                if (!h.Armed)
                {
                    Console.WriteLine($"# self-skip: {h.SkipReason}");
                    return 0; // armed check per honest-degrade contract (Available() passed above)
                }
                endpoints = h.Addresses.Length;
                if (endpoints > 0) break; // caught a survey; else resample the same period
            }

            // Two distinct honest outcomes: zero endpoints sampled at all, vs. endpoints sampled
            // but none resolved to a named managed method (heavy throttling can leave the few
            // survivors in native-runtime code) — both are real properties of sampled surveys.
            string top = endpoints == 0 ? "(0 endpoints sampled)" : "(no named method resolved)";
            long topW = 0;
            if (h.Methods.Count > 0) { top = h.Methods[0].ShortName; topW = h.Methods[0].Count; }
            if (top.Length > 34) top = top.Substring(0, 34);

            Console.WriteLine($"  {p,6}   {endpoints,9}   {topW,10}  {top,-34}   {(h.Truncated ? "yes" : "no")}");
        }

        Console.WriteLine();
        Console.WriteLine("-> the SAME AsmTrace.WindowHot survey at four periods (IsStatistical — these are\n"
                          + "   SAMPLE WEIGHTS: branch-target endpoint hits, NOT instruction counts). Smaller\n"
                          + "   period = the PMU fires more often = a finer survey, but more PMIs and more\n"
                          + "   throttling (Truncated flags dropped/throttled samples); larger = coarser but\n"
                          + "   cheaper. The endpoint counts are SAMPLED, so they wobble run-to-run and 0 on a\n"
                          + "   run is honest, not an error — Hot stays the top method because it owns the\n"
                          + $"   branch traffic at every period. (grand={grand})");
        return 0;
    }
}
