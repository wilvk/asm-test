// examples/dotnet/crashproof-survey — ONE workload, all THREE crash-proof-capable forms
// side by side, in a fidelity / cost / safety table.
//
//     m = AsmTrace.Method((Func<long,long,long>)Work, emit:false);  // EXACT one body
//     w = AsmTrace.Window(() => { …calls Work… });                  // EXACT whole block, OOP
//     h = AsmTrace.WindowHot(() => { …hot loop of Work… });         // SAMPLED (AMD LBR)
//
// Every OTHER dotnet example here shows ONE of these in isolation — single-method (Method),
// localscope_oop_managed (Window), amdhot (WindowHot). This example's unique contribution is
// the DIRECT COMPARISON: it runs the SAME Work over all three and prints where each lands on
// the fidelity/safety trade — so you can see, at a glance, which form to reach for.
//
//   FORM               fidelity      crash-proof?          captures
//   AsmTrace.Method     exact-body    only the body (safe)  the ONE body's exact instructions
//   AsmTrace.Window     exact-window  yes (out-of-process)  the whole block's methods + insns
//   AsmTrace.WindowHot  sampled       yes (AMD LBR)         a sample-WEIGHTED hot-method survey
//
// The safety column is the point. Method's in-process single-step is safe because it steps ONLY
// the JIT'd body — never the runtime around it (a whole-window in-process step SIGTRAPs on an
// in-window pthread_create). Window is crash-proof for an ENTIRE block: a reverse-attached
// helper steps this thread out of band, so it is never EFLAGS.TF-armed. WindowHot is crash-proof
// AND near-native: AMD LBR samples the branch stack out of band — no TF, no SIGTRAP, a handful
// of PMIs — trading exactness for a statistical survey.
//
// Method + Window run live here (single-step / ptrace). The AMD leg self-skips (exit 0) off
// Zen 3+/LBR or without CAP_PERFMON — expected on the plain lane; the AMD-permissioned lane
// runs it live. Each leg self-skips independently; the example always returns 0.

using System;
using Asmtest;

internal static class Program
{
    // The ONE workload: a (long,long)->long loop with a data-dependent branch, so the stepped
    // stream is non-trivial. NoInlining so it JITs as its own standalone body the Method
    // resolver can find, and so it stays a distinct callee the Window/WindowHot legs can name.
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Work(long a, long b)
    {
        long acc = 0;
        for (long i = 0; i < b; i++)
            acc += (i & 1) == 0 ? a + i : a - i;   // a data-dependent branch inside the loop
        return acc;
    }

    static int Main()
    {
        Console.WriteLine("== crash-proof-capable forms side by side: Method vs Window vs WindowHot ==\n");
        Console.WriteLine("one workload (Work: a (long,long)->long loop) through all three, so you can\n"
                          + "see where each lands on the fidelity / cost / safety trade.\n");

        // ---- Leg 1: AsmTrace.Method — EXACT one body, in-process lazy-arm (only the body is
        // stepped, none of the runtime around it — that is why it is safe). Read after close.
        AsmTrace m;
        long mResult;
        using (m = AsmTrace.Method((Func<long, long, long>)Work, emit: false))
            mResult = (long)m.Invoke(7L, 12L);

        // ---- Leg 2: AsmTrace.Window — EXACT whole block, stepped OUT OF PROCESS by the
        // reverse-attach helper (crash-proof for the entire block). Small block: exact stepping
        // is ~100-1000x, so keep the whole-window work modest. Returns already-closed; read it.
        long windowResult = 0;
        AsmTrace w = AsmTrace.Window(() =>
        {
            long s = 0;
            for (int k = 0; k < 3; k++) s += Work(7, 12);   // the SAME Work, a few times
            windowResult = s;
        });

        // ---- Leg 3: AsmTrace.WindowHot — SAMPLED AMD-LBR survey. Guard on the AMD tier so the
        // self-skip carries the honest reason; run a HOT loop so Work dominates the histogram.
        bool amdRan = HwTrace.Available(HwBackend.AmdLbr);
        string amdSkip = amdRan ? "" : HwTrace.SkipReason(HwBackend.AmdLbr);
        long hotResult = 0;
        AsmTrace h = null;
        if (amdRan)
        {
            h = AsmTrace.WindowHot(() =>
            {
                long s = 0;
                for (int k = 0; k < 40000; k++) s += Work(7, 60);   // hot: Work tops the survey
                hotResult = s;
            });
        }
        else
        {
            // The plain lane: no Zen LBR / no CAP_PERFMON. Run the same hot loop uninstrumented
            // so the workload still happens, and announce the self-skip in the house form.
            for (int k = 0; k < 40000; k++) hotResult += Work(7, 60);
            Console.WriteLine($"# self-skip (AMD): {amdSkip}\n");
        }

        Report.PrintTable(m, mResult, w, windowResult, h, amdRan, amdSkip, hotResult);
        return 0;
    }
}
