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

    // §E6 HIGH-2 fixtures. ColdLeafExtra exists ONLY to make "Program::ColdLeaf" an
    // ambiguous key: it is a strict PREFIX of this name, so a bare-substring resolver that
    // takes the newest JIT'd match resolves ColdLeaf -> ColdLeafExtra's body. The breakpoint
    // then arms cleanly, fires on the wrong method, and merges a real 16-entry island —
    // indistinguishable from success. Checkpoints() anchors on '(' so "ColdLeaf(" cannot
    // match "ColdLeafExtra(".
    static void ColdLeafExtra()
    {
        _sink += 0x3c3c;
    }

    // An OVERLOAD PAIR: both share "Program::Over(", which the '(' anchor cannot separate.
    // Checkpoints() prefers the full signature key ("Program::Over(int32)"), and if that
    // ever fails to match it REFUSES rather than picking whichever JIT'd last.
    static void Over(int x) { _sink += x; }
    static void Over(string s) { _sink += s.Length; }

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

    // Force the collision fixtures to JIT so they are REALLY in the jitdump when Checkpoints()
    // reads it — an unJIT'd decoy proves nothing about ambiguity.
    static void JitTheDecoys()
    {
        ColdLeafExtra();
        Over(1);
        Over("xy");
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
        // ORDER IS THE FIXTURE. A newest-wins bare-name resolver only picks ColdLeafExtra for
        // "Program::ColdLeaf" when ColdLeafExtra is the NEWER jitdump record — so JIT ColdLeaf
        // FIRST and the decoys AFTER. Reversed, the trap never springs and the oracle below
        // passes for the wrong reason (verified: with the decoys JIT'd first, the old resolver
        // resolves ColdLeaf correctly by luck and the prefix-collision check goes green).
        ColdLeaf();        // JIT the real target first...
        JitTheDecoys();    // ...so ColdLeafExtra / Over are NEWER records than it

        const BindingFlags PS = BindingFlags.NonPublic | BindingFlags.Static;
        MethodInfo cold = typeof(Program).GetMethod(nameof(ColdLeaf), PS);
        ulong[] cps = AsmTrace.Checkpoints(cold);
        foreach (string skip in AsmTrace.LastCheckpointSkips)
            Console.WriteLine($"# checkpoint skipped — {skip}");

        // HIGH-2 oracle A: ColdLeaf must resolve to ColdLeaf, NOT to the later-JIT'd
        // ColdLeafExtra whose name contains it. A bare-substring resolver returns
        // ColdLeafExtra's entry here; the breakpoint would arm, fire, and merge a perfect
        // island for the wrong method. So compare against ColdLeafExtra's own resolution.
        ulong[] extraCp = AsmTrace.Checkpoints(typeof(Program).GetMethod(nameof(ColdLeafExtra), PS));
        if (cps.Length == 1 && extraCp.Length == 1 && cps[0] == extraCp[0])
        {
            Console.WriteLine($"# FAIL: ColdLeaf resolved to ColdLeafExtra's body (0x{cps[0]:x}) — "
                              + "a prefix collision silently armed the wrong method");
            return 1;
        }

        // HIGH-2 oracle B: an OVERLOAD pair must never resolve to one shared guess. Either the
        // signature key separates them (two different entries) or resolution REFUSES both.
        // What must never happen is both "resolving" to the same body.
        ulong[] oi = AsmTrace.Checkpoints(typeof(Program).GetMethod(nameof(Over), PS, null, new[] { typeof(int) }, null));
        ulong[] os_ = AsmTrace.Checkpoints(typeof(Program).GetMethod(nameof(Over), PS, null, new[] { typeof(string) }, null));
        if (oi.Length == 1 && os_.Length == 1 && oi[0] == os_[0])
        {
            Console.WriteLine($"# FAIL: Over(int) and Over(string) both resolved to 0x{oi[0]:x} — "
                              + "an overload collision silently armed one body for both");
            return 1;
        }
        Console.WriteLine($"resolution: ColdLeaf distinct from ColdLeafExtra ✓; "
                          + $"Over(int)/{(oi.Length == 1 ? "resolved" : "refused")} vs "
                          + $"Over(string)/{(os_.Length == 1 ? "resolved" : "refused")} not conflated ✓");
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

        // --- the demo's own ORACLES ------------------------------------------------
        // (1) Island content, in the repo's covered-OR-truncated shape — but disjoined with
        //     TiledTruncated (the ISLAND merge lost endpoints), NEVER with Truncated (the
        //     survey-wide flag, which ALSO fires when the SAMPLER's 256 KiB ring goes
        //     near-full). That rule is sound only when the truncation term is CAUSALLY TIED
        //     to the property asserted. It is not, here: with Truncated, clobbering the island
        //     data and growing the hot loop until the sampler's ring filled made this lane
        //     report GREEN while printing "covered? TILED=False". The fixture is also sized
        //     (HotIters) so sampler truncation does not occur at all — but sizing alone is a
        //     drift hazard, so the CORRECT term is what actually guards it.
        // (2) The differential, asserted rather than printed: tiling must cover the one-shot
        //     leaf and the untiled survey must miss it. Printing it and asserting nothing left
        //     the managed lane's headline claim untested.
        int fails = 0;
        if (!(tiledInIslands || tiled.TiledTruncated))
        {
            Console.WriteLine("# FAIL: the island prefix does not contain the checkpoint entry "
                              + "and the island merge lost nothing (TiledTruncated=false)");
            fails++;
        }
        if (!tiledCovers)
        {
            Console.WriteLine("# FAIL: the tiled scope does not cover the one-shot ColdLeaf at all");
            fails++;
        }
        if (plainCovers)
        {
            // Not a tiling bug — it means Period no longer selects the regime where the
            // sampler is blind, so the differential has stopped demonstrating anything and
            // must be re-tuned. Failing loudly beats printing a tie and claiming a win.
            Console.WriteLine($"# FAIL: the UNTILED survey also covered ColdLeaf at period={Period} — "
                              + "the differential no longer isolates what tiling adds; re-tune "
                              + "Period/HotIters so the one-shot leaf is outside the sampler's reach");
            fails++;
        }
        // (3) A complete island is TiledIslands * the 16-deep AMD branch stack. Asserting only
        //     "> 0" is satisfied by island[0] alone — which is present by hardware construction
        //     (the breakpoint sits on the entry, so the newest retired edge IS the call that
        //     reached it) and can never fail, leaving 15 of 16 endpoints unverified.
        const int LbrDepth = 16;
        if (!tiled.TiledTruncated && tiled.TiledAddresses != tiled.TiledIslands * LbrDepth)
        {
            Console.WriteLine($"# FAIL: {tiled.TiledIslands} island(s) merged {tiled.TiledAddresses} "
                              + $"endpoints, expected {tiled.TiledIslands * LbrDepth} "
                              + "(islands * 16-deep branch stack) with no island loss reported");
            fails++;
        }
        if (fails > 0) return 1;
        return 0;
    }
}
