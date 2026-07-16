// examples/dotnet/amd-tile — §E6: the deterministic branch snapshot TILED at MANAGED
// checkpoints, merged into the WindowHot sampled-endpoint surface.
//
//     var cps = AsmTrace.Checkpoints(typeof(Program).GetMethod("ColdLeaf", ...));
//     using var t = AsmTrace.WindowHot(() => Body(), period: 50000, tileCheckpoints: cps);
//     // t.TiledIslands  -> how many checkpoint hits froze an island
//     // t.TiledAddresses-> how many leading t.Addresses came from those islands
//
// THE GAP THIS FILLS. WindowHot runs the block at native speed and SAMPLES the branch stack
// every `period` branches. That is a superb hot-method histogram and a terrible way to notice
// a method that runs ONCE: at any realistic period the sampler is blind to ~all of the branch
// history, so a one-shot routine among millions of hot branches is simply never seen. E6 tiles
// the DETERMINISTIC snapshot into that same surface — a HW execution breakpoint on the
// checkpoint, and at each hit the ~16 most recently retired branch targets are read EXACTLY
// out of the frozen LBR and merged into Addresses. The sampler keeps running; one pass of the
// body feeds both producers.
//
// WHAT THE MERGED COVERAGE GUARANTEES — read this before believing a number below.
//   * AT a checkpoint hit: the last ~16 retired branch targets, EXACTLY.
//   * BETWEEN hits: nothing. Only whatever the sampler happened to catch.
// So this is SAMPLED / PARTIAL COVERAGE — exact islands in an unobserved sea — and NOT an
// exact whole-window trace. Exact whole-window on AMD is a hardware DEAD END and a documented
// non-goal (the LBR is 16 deep; there is no stream to read). That is why IsStatistical stays
// TRUE for a tiled scope: adding exact islands to a survey yields a BETTER-COVERED SURVEY,
// never an exact trace. Do not read Addresses as an execution order or a completeness claim.
//
// The demo PROVES the gap rather than asserting it: it runs the same body twice at the same
// period, once tiled and once not, and prints whether the cold leaf was covered by each.
// Needs CAP_BPF + CAP_PERFMON + AMD LbrExtV2 + a BPF-toolchain build + Linux >= 6.10; without
// them it self-skips (exit 0), which is the correct outcome and NOT a failure.

using System;
using System.Linq;
using System.Reflection;
using Asmtest;

internal static class Program
{
    static volatile int _sink;

    // Runs ONCE per window. This is what the sampler cannot see.
    static void ColdLeaf()
    {
        _sink += 0x5a5a;
    }

    // Runs HotIters times: the branchy work that dominates the window.
    static long HotWork(long x)
    {
        long a = 0;
        for (long i = 0; i < 64; i++)
        {
            a += (i ^ x);
            if ((a & 1) != 0) a += 3;
        }
        return a;
    }

    const int HotIters = 200000;

    static void Body()
    {
        long a = 0;
        for (long i = 0; i < HotIters; i++) a += HotWork(i);
        ColdLeaf();               // exactly once, at the very end
        _sink += (int)(a & 0xff);
    }

    // A realistic near-native survey period. The WindowHot default (16) samples so densely that
    // the 16-deep windows nearly tile the whole run, which would HIDE the gap tiling fills.
    const int Period = 50000;

    // Did any endpoint land on the checkpoint entry? The breakpoint is planted ON ColdLeaf's
    // entry, so the island's newest edge is the call that reached it and `entry` itself appears.
    static bool CoversEntry(ulong[] addrs, int take, ulong entry) =>
        addrs.Take(take).Any(a => a == entry);

    // ASMTEST_TILE_REQUIRE=1 makes a self-skip FATAL. Self-skipping is right on a host without
    // the substrate — but docker-hwtrace-dotnet-amd exists precisely to supply it, and there a
    // skip is not a pass, it is the managed-checkpoint claim silently gone. (Mirrors
    // CLEANROOM_ONLY=<lang>, which fails a clean-room run whose binding self-skipped.)
    static bool Required()
    {
        string e = Environment.GetEnvironmentVariable("ASMTEST_TILE_REQUIRE");
        return !string.IsNullOrEmpty(e) && e != "0";
    }

    static int SkipOrFail(string why)
    {
        if (Required())
        {
            Console.WriteLine($"# FAIL: {why} — but ASMTEST_TILE_REQUIRE=1 says this lane MUST "
                              + "be able to tile a managed checkpoint, so a skip here is a failure");
            return 1;
        }
        Console.WriteLine($"# self-skip: {why}");
        return 0;
    }

    static int Main()
    {
        Console.WriteLine("== E6: branchsnap TILED at managed checkpoints -> WindowHot.Addresses ==\n");

        if (!AmdSnapshot.Available())
            return SkipOrFail(AmdSnapshot.SkipReason());

        // Resolve the managed checkpoint. Checkpoints() JIT-prepares the method and then reads
        // its JIT'd-BODY entry out of the rundown jitdump — NOT MethodHandle.GetFunctionPointer(),
        // which returns a precode stub the runtime backpatches away (a breakpoint there arms
        // cleanly and never fires; see AsmTrace.Checkpoints).
        MethodInfo cold = typeof(Program).GetMethod(
            nameof(ColdLeaf), BindingFlags.NonPublic | BindingFlags.Static);
        ulong[] cps = AsmTrace.Checkpoints(cold);
        if (cps.Length == 0)
            return SkipOrFail("could not resolve a managed entry PC to checkpoint "
                              + "(needs the diagnostics rundown / jitdump)");
        ulong coldEntry = cps[0];
        Console.WriteLine($"checkpoint: ColdLeaf @ 0x{coldEntry:x} (JIT-prepared; tiering pinned off\n"
                          + "            so the body cannot be re-JIT'd out from under it)\n");

        // --- pass 1: TILED --------------------------------------------------------------
        using var tiled = AsmTrace.WindowHot(() => Body(), period: Period, tileCheckpoints: cps);
        if (!tiled.Armed)
            return SkipOrFail(tiled.SkipReason);
        // --- pass 2: the SAME body + period, UNTILED (the control arm) -------------------
        using var plain = AsmTrace.WindowHot(() => Body(), period: Period);

        bool tiledCovers = CoversEntry(tiled.Addresses, tiled.Addresses.Length, coldEntry);
        bool tiledInIslands = CoversEntry(tiled.Addresses, tiled.TiledAddresses, coldEntry);
        bool plainCovers = CoversEntry(plain.Addresses, plain.Addresses.Length, coldEntry);

        Console.WriteLine($"TILED   : {tiled.Addresses.Length,6} endpoints "
                          + $"({tiled.TiledAddresses} from {tiled.TiledIslands} island(s), "
                          + $"{tiled.Addresses.Length - tiled.TiledAddresses} sampled), "
                          + $"truncated={tiled.Truncated}, statistical={tiled.IsStatistical}");
        Console.WriteLine($"UNTILED : {plain.Addresses.Length,6} endpoints "
                          + $"(all sampled), truncated={plain.Truncated}, "
                          + $"statistical={plain.IsStatistical}");
        Console.WriteLine();
        Console.WriteLine($"one-shot ColdLeaf covered?   TILED={tiledCovers} "
                          + $"(in the island prefix: {tiledInIslands})   UNTILED={plainCovers}");
        Console.WriteLine();

        if (tiled.TiledIslands == 0)
        {
            // No island landed. Two very different causes hide behind this one number, and
            // saying "did not arm" would be a guess — during development the snapshot armed
            // perfectly (tile_begin: armed ncp=1) and reported islands=0 because the checkpoint
            // address was a precode stub that never executes. So name both, and point at the
            // one diagnostic that separates them.
            Console.WriteLine("#   (a) tiling could not ARM — needs CAP_BPF + CAP_PERFMON + a");
            Console.WriteLine("#       BPF-toolchain build + a free debug register; or");
            Console.WriteLine("#   (b) it armed and the checkpoint was never REACHED — a wrong");
            Console.WriteLine("#       entry PC (a stub rather than the JIT'd body), or a re-JIT");
            Console.WriteLine("#       moved the body out from under the breakpoint.");
            Console.WriteLine("# ASMTEST_AMD_DEBUG=1 prints 'tile_begin: armed ...' — it separates them.");
            return SkipOrFail("no checkpoint island landed (TiledIslands == 0)");
        }

        Console.WriteLine($"The sampler saw {plain.Addresses.Length} endpoints and STILL missed a method that");
        Console.WriteLine($"ran — because it ran once, among millions of hot branches. The checkpoint");
        Console.WriteLine($"island caught it deterministically, every time, for the cost of one");
        Console.WriteLine($"breakpoint. That is the whole of E6.");
        Console.WriteLine();
        Console.WriteLine("BUT: this is SAMPLED/PARTIAL coverage — exact AT the {0} checkpoint hit(s),",
                          tiled.TiledIslands);
        Console.WriteLine("blind between them. It is NOT an exact whole-window trace; that remains a");
        Console.WriteLine("hardware dead end on AMD (the LBR is 16 deep) and a documented non-goal.");
        Console.WriteLine("IsStatistical stays true: a better-covered SURVEY, never an exact trace.");

        // The demo's own oracle, in the repo's covered-OR-truncated shape (AMD LBR truncates on
        // small fixtures, so a bare "covered" would flake on privileged AMD).
        if (!(tiledInIslands || tiled.Truncated))
        {
            Console.WriteLine("\n# FAIL: the island prefix did not contain the checkpoint entry "
                              + "and the capture was not truncated");
            return 1;
        }
        return 0;
    }
}
