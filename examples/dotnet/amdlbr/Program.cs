// examples/dotnet/amdlbr — the REGION scope on the AMD LBR hardware tier.
//
//     HwTrace.Init(HwBackend.AmdLbr);              // pick the AMD LBR tier
//     using (scope = new AsmTrace(code))           // the SAME region-scope shape as examples/region
//         code.Call(...);                          // AMD LBR records its taken branches out of band
//     // scope.Path = the routine's reconstructed assembly
//
// The region `using (new AsmTrace(code))` scope already works on ANY inited backend — this is
// it on AMD LBR (Zen 3+/LbrExtV2) instead of the portable single-step WEAK tier. The routine's
// control flow is reconstructed from the hardware 16-deep branch-record stack: out-of-band,
// near-native (a handful of PMIs), no EFLAGS.TF and no per-instruction SIGTRAP. AMD LBR is
// REGION-scoped (a registered native routine, not a whole managed block — for that see amdhot's
// statistical whole-window survey). The only requirement over the single-step region form is
// HwTrace.Init(AmdLbr) first (the region ctor does not auto-init). Self-skips (exit 0) off
// Zen 3+/LBR or without CAP_PERFMON.

using System;
using Asmtest;

internal static class Program
{
    // sum(rdi) via a decrement loop — a back-edge per iteration gives AMD LBR real taken
    //   branches to record:  mov rax,0 ; L: add rax,rdi ; dec rdi ; jnz L ; ret
    static readonly byte[] LOOP =
    {
        0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,  // 0x0: mov rax, 0
        0x48, 0x01, 0xF8,                          // 0x7: L: add rax, rdi
        0x48, 0xFF, 0xCF,                          // 0xa: dec rdi
        0x75, 0xF8,                                // 0xd: jnz L (-> 0x7)
        0xC3,                                      // 0xf: ret
    };

    static int Main()
    {
        Console.WriteLine("== AMD-LBR region scope: HwTrace.Init(AmdLbr) + using (new AsmTrace(code)) ==\n");

        if (!HwTrace.Available(HwBackend.AmdLbr))
        {
            Console.WriteLine($"# self-skip: AMD LBR unavailable: {HwTrace.SkipReason(HwBackend.AmdLbr)}");
            return 0;
        }
        HwTrace.Init(HwBackend.AmdLbr);
        Console.WriteLine("backend: AMD LBR (Zen 3+ BRS / Zen 4-5 LbrExtV2) — hardware branch-record\n"
                          + "region trace, out-of-band and near-native, no EFLAGS.TF / no SIGTRAP.\n");

        // AMD LBR delivers its 16-deep stack only AT a PMU sample, so a single capture may
        // land its sample outside the loop and reconstruct nothing — the sampling is
        // non-deterministic per attempt. Like the C test lane, RETRY the same `using` scope
        // until a window lands in-region (a real property of hardware branch-record sampling).
        const long trips = 20_000; // 20k back-edges — far past the 16-deep window, so Tier-B stitches
        var code = NativeCode.FromBytes(LOOP);
        code.Call(trips, 0); // warm the call path before the first window

        long r = 0;
        AsmTrace scope = null;
        string listing = "";
        int attempt = 0;
        for (; attempt < 40; attempt++)
        {
            using (scope = new AsmTrace(code, emit: false)) // region on the AMD LBR tier
                r = code.Call(trips, 0);
            if (!scope.Armed)
            {
                Console.WriteLine($"# self-skip: {scope.SkipReason}");
                code.Free();
                return 0;
            }
            // A window that captured the loop body renders real instructions; an off-loop
            // sample renders "0 of 0". Take the first in-region capture.
            if (scope.Path.Length > 0 && !scope.Path.Contains("0 of 0"))
            {
                listing = scope.Path;
                break;
            }
        }

        if (listing.Length == 0)
        {
            Console.WriteLine($"# every one of {attempt} attempts sampled outside the loop (the tiny\n"
                              + "  in-region window vs. the sampling rate) — AMD armed but nothing landed in-region.");
            code.Free();
            return 0; // honest: armed, but the sampled path did not catch the region this run
        }

        Console.WriteLine($"loop ran ({trips:N0} iters, sum={r}); AMD LBR reconstructed the loop body on\n"
                          + $"attempt {attempt + 1}" + (scope.Truncated ? " (truncated — the run exceeds one 16-deep window; Tier-B\n"
                             + "   stitched what fit, honestly flagged truncated)" : "") + ":\n");
        Console.WriteLine(listing);
        Console.WriteLine("\n-> the SAME `using (new AsmTrace(code))` region scope, on the AMD LBR tier via\n"
                          + "   HwTrace.Init(AmdLbr) instead of single-step — hardware-attributed, out-of-band,\n"
                          + "   no EFLAGS.TF. (AMD LBR is REGION-scoped + sampled, so the retry is inherent —\n"
                          + "   contrast amdhot's region-free statistical whole-window survey.)");
        code.Free();
        return 0;
    }
}
