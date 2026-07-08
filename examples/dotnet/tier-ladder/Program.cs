// examples/dotnet/tier-ladder — this host's honest trace-tier degradation ladder,
// read straight from the PUBLIC cascade API instead of a per-example private self-skip.
//
//     HwTrace.ResolveTiers(TracePolicy.Best)  // the full cross-tier cascade, most-faithful first
//     HwTrace.AutoTier(TracePolicy.Best)       // the single top pick a scope would arm
//     HwTrace.DegradationNote()                // one honest sentence naming why each rung fell
//
// Every OTHER managed example here ARMS a scope and then self-skips PRIVATELY when its tier is
// unavailable (single-method checks Available(SingleStep); amdlbr checks Available(AmdLbr)).
// This example is the inverse: it never arms anything — no AsmTrace, no NativeCode, no
// EFLAGS.TF, no PMU — it just interrogates the SAME resolver those scopes consult, and prints
// the whole ladder: which backends this host has (with the reason each missing one is gone),
// the ordered cascade the resolver would walk, the single top pick, and how the trace-POLICY
// knobs (CeilingFree, NativeOnly) drop rungs off that ladder. Pure enumeration/probing: it
// cannot crash and never needs a hardware gate — the only skip is the trivial one where the
// binding could not load the native lib (nothing to resolve).
//
// On a Zen 5 host with no CAP_PERFMON the ladder reads: Intel PT unavailable (wrong arch),
// AMD LBR unavailable (needs CAP_PERFMON), single-step available — so the cascade falls to the
// portable single-step floor. Grant CAP_PERFMON and the AMD LBR rung reappears above it; the
// output is rendered from the API, so it stays honest either way.

using System;
using Asmtest;

internal static class Program
{
    // The four hardware backends, most-faithful first — the order the resolver walks.
    static readonly HwBackend[] Backends =
    {
        HwBackend.IntelPt, HwBackend.AmdLbr, HwBackend.SingleStep, HwBackend.CoreSight,
    };

    static int Main()
    {
        Console.WriteLine("== trace-tier degradation ladder: the public cascade API (no tracing) ==\n");

        string lib = HwTrace.LibraryPath();
        if (string.IsNullOrEmpty(lib))
        {
            // The one honest skip: with no lib loaded there is no host to probe — not a
            // hardware gate, the cascade API simply has nothing to resolve.
            Console.WriteLine($"# self-skip: libasmtest_hwtrace not loaded ({HwTrace.DegradationNote()})");
            return 0;
        }
        Console.WriteLine($"library: {lib}\n");

        // 1) Per-backend availability — the raw rungs, most-faithful first, each with its
        //    reason. This is exactly what every scope's private Available()/SkipReason()
        //    self-skip reads; here we read all four at once.
        Report.Backends(Backends);

        // 2) The backend-only hardware cascade (Resolve/Auto over HwPolicy) — the HwTrace-tier
        //    subset: which hardware backends are available, in order, and the single top one.
        Report.HwCascade();

        // 3) The full CROSS-tier cascade (ResolveTiers/AutoTier over TracePolicy) — the same
        //    walk but spanning tiers (HwTrace -> DynamoRIO -> single-step -> emulator); each
        //    TierChoice carries its Tier, its Backend, and its native-vs-virtual Fidelity.
        TierChoice[] best = HwTrace.ResolveTiers(TracePolicy.Best);
        Report.Cascade("cross-tier cascade — ResolveTiers(Best)", best);
        Report.Top("AutoTier(Best)", HwTrace.AutoTier(TracePolicy.Best));

        // 4) The policy knobs DROP rungs off that ladder. CeilingFree drops the fixed-window
        //    backend (AMD LBR); NativeOnly drops the emulator floor (never crosses into
        //    virtual fidelity). Diff each restricted cascade against Best to name what fell.
        TierChoice[] ceilFree = HwTrace.ResolveTiers(TracePolicy.CeilingFree);
        TierChoice[] nativeOnly = HwTrace.ResolveTiers(TracePolicy.NativeOnly);
        Report.PolicyDrop("CeilingFree", best, ceilFree, "the fixed-window backend (AMD LBR)");
        Report.PolicyDrop("NativeOnly", best, nativeOnly, "the emulator floor (virtual fidelity)");

        // 5) The one-sentence summary a scope's self-skip (and a bug report) carries.
        Console.WriteLine($"summary: {HwTrace.DegradationNote()}");
        return 0;
    }
}
