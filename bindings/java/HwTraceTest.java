/*
 * HwTraceTest.java — standalone live test for the single-step hardware-trace
 * binding (HwTrace.java), mirroring bindings/python/tests/test_hwtrace.py.
 *
 * Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
 * backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
 * on ANY x86-64 Linux — so this asserts a real, live trace in CI/containers,
 * self-skipping only off x86-64 Linux or without the library / Capstone. On a
 * skip it prints "# SKIP ..." and exits 0.
 *
 * Asserts are NOT used (the `assert` keyword is off by default); failures throw,
 * are reported as "not ok", and exit nonzero. Each check prints "ok N - ...".
 *
 * Compile with `--release 21 --enable-preview`, run with `--enable-preview
 * --enable-native-access=ALL-UNNAMED`.
 */
import java.util.Arrays;
import java.util.List;
import java.util.Optional;

public final class HwTraceTest {

    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
    private static final byte[] ROUTINE = {
        0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, 0x48, 0x3D,
        0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, (byte) 0xFF, (byte) 0xC8, (byte) 0xC3
    };

    // mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
    private static final byte[] LOOP = {
        0x48, (byte) 0xC7, (byte) 0xC0, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, (byte) 0xF8, 0x48, (byte) 0xFF, (byte) 0xCE, 0x75, (byte) 0xF8, (byte) 0xC3
    };

    private static int testNo = 0;

    private static void ok(boolean cond, String name) {
        testNo++;
        if (cond) {
            System.out.println("ok " + testNo + " - " + name);
        } else {
            System.out.println("not ok " + testNo + " - " + name);
            throw new AssertionError(name);
        }
    }

    public static void main(String[] args) {
        // The orchestrator's selection invariants hold on every host (even where all
        // backends self-skip and the cascade is empty), so run them before any skip.
        try {
            autoResolveSelectionInvariants();
            crossTierResolveInvariants();
            crossTierNativeOnlyResolvesOnLinuxX86_64();
            autoResolveTracesLive();
        } catch (Throwable t) {
            System.out.println("Bail out! " + t);
            t.printStackTrace();
            System.exit(1);
        }

        if (!HwTrace.available(HwTrace.SINGLESTEP)) {
            System.out.println("# SKIP single-step backend unavailable: "
                + HwTrace.skipReason(HwTrace.SINGLESTEP));
            System.exit(0);
        }

        HwTrace.init(HwTrace.SINGLESTEP);
        try {
            singlestepLiveTrace();
            singlestepLoopNoDepthCeiling();
        } catch (Throwable t) {
            System.out.println("Bail out! " + t);
            t.printStackTrace();
            System.exit(1);
        } finally {
            HwTrace.shutdown();
        }
        System.out.println("# all tests passed");
    }

    // Mirrors test_singlestep_live_trace.
    private static void singlestepLiveTrace() {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        HwTrace.NativeTrace trace = HwTrace.create(64, 64);
        trace.register("add2", code);

        long[] r = new long[1];
        trace.region("add2", () -> r[0] = code.call(20, 22)); // 42 <= 100 → jle taken, dec skipped
        ok(r[0] == 42, "call(20,22): result == 42 (got " + r[0] + ")");

        long[] insns = trace.insnOffsets();
        long[] wantInsns = {0x0, 0x3, 0x6, 0xC, 0x11};
        ok(Arrays.equals(insns, wantInsns),
            "insnOffsets == [0,3,6,12,17] (got " + Arrays.toString(insns) + ")");
        ok(trace.insnsTotal() == 5, "insnsTotal == 5 (got " + trace.insnsTotal() + ")");
        ok(trace.covered(0) && trace.covered(0x11), "covered(0) && covered(17)");
        ok(trace.blocksLen() == 2, "blocksLen == 2 (got " + trace.blocksLen() + ")");
        ok(!trace.truncated(), "!truncated");

        trace.free();
        code.free();
    }

    // Mirrors test_singlestep_loop_no_depth_ceiling.
    private static void singlestepLoopNoDepthCeiling() {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(LOOP);
        HwTrace.NativeTrace trace = HwTrace.create(64, 256);
        trace.register("loop", code);

        long[] r = new long[1];
        trace.region("loop", () -> r[0] = code.call(1, 20));
        ok(r[0] == 20, "call(1,20): result == 20 (got " + r[0] + ")");
        ok(trace.insnsTotal() == 62, "insnsTotal == 62 (got " + trace.insnsTotal() + ")");
        ok(trace.covered(0) && trace.covered(0x7), "covered(0) && covered(7)");
        ok(trace.blocksLen() == 2, "blocksLen == 2 (got " + trace.blocksLen() + ")");
        ok(!trace.truncated(), "!truncated");

        trace.free();
        code.free();
    }

    // Mirrors test_auto_resolve_selection_invariants — holds on every host.
    private static void autoResolveSelectionInvariants() {
        int[] best = HwTrace.resolve(HwTrace.BEST);
        int[] cf = HwTrace.resolve(HwTrace.CEILING_FREE);

        // Every resolved backend is actually available, ordered by descending fidelity
        // (ascending enum), with no duplicates.
        boolean okAvail = true, okOrder = true;
        for (int i = 0; i < best.length; i++) {
            if (!HwTrace.available(best[i])) okAvail = false;
            if (i > 0 && best[i] <= best[i - 1]) okOrder = false;
        }
        ok(okAvail, "resolve(BEST) returns only available backends");
        ok(okOrder, "resolve(BEST) is ascending enum order, no dups");

        // CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
        // subset of BEST.
        boolean cfNoAmd = true, cfSubset = true;
        for (int b : cf) {
            if (b == HwTrace.AMD_LBR) cfNoAmd = false;
            boolean inBest = false;
            for (int x : best) if (x == b) inBest = true;
            if (!inBest) cfSubset = false;
        }
        ok(cfNoAmd, "resolve(CEILING_FREE) never selects AMD_LBR");
        ok(cfSubset, "resolve(CEILING_FREE) is a subset of resolve(BEST)");

        // auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
        int ab = HwTrace.auto(HwTrace.BEST);
        int want = (best.length == 0) ? HwTrace.ASMTEST_HW_EUNAVAIL : best[0];
        ok(ab == want, "auto(BEST) is the head of resolve(BEST) (got " + ab + ")");
    }

    // Mirrors test_cross_tier_resolve_invariants — holds on every host.
    private static void crossTierResolveInvariants() {
        List<HwTrace.TierChoice> best = HwTrace.resolveTiers(HwTrace.TRACE_BEST);
        List<HwTrace.TierChoice> nat = HwTrace.resolveTiers(HwTrace.TRACE_NATIVE_ONLY);
        List<HwTrace.TierChoice> cf = HwTrace.resolveTiers(HwTrace.TRACE_CEILING_FREE);

        // Every HW choice satisfies the hardware-tier probe; the fidelity class matches
        // the tier (only the emulator tier is VIRTUAL).
        boolean okHwAvail = true, okFidelity = true;
        for (HwTrace.TierChoice c : best) {
            if (c.tier() == HwTrace.TIER_HWTRACE && !HwTrace.available(c.backend())) okHwAvail = false;
            int wantFid = (c.tier() == HwTrace.TIER_EMULATOR)
                ? HwTrace.FIDELITY_VIRTUAL : HwTrace.FIDELITY_NATIVE;
            if (c.fidelity() != wantFid) okFidelity = false;
        }
        ok(okHwAvail, "resolveTiers(BEST) HW choices are all available");
        ok(okFidelity, "resolveTiers(BEST) fidelity matches tier (only emulator is VIRTUAL)");

        // The single VIRTUAL emulator floor is the last entry under BEST, exactly once.
        int emuCount = 0;
        for (HwTrace.TierChoice c : best) if (c.tier() == HwTrace.TIER_EMULATOR) emuCount++;
        ok(!best.isEmpty() && best.get(best.size() - 1).tier() == HwTrace.TIER_EMULATOR,
            "resolveTiers(BEST) ends with the emulator floor");
        ok(emuCount == 1, "resolveTiers(BEST) has exactly one emulator entry (got " + emuCount + ")");

        // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
        boolean natNoEmu = true;
        for (HwTrace.TierChoice c : nat) if (c.tier() == HwTrace.TIER_EMULATOR) natNoEmu = false;
        ok(natNoEmu, "resolveTiers(NATIVE_ONLY) drops the emulator floor");
        ok(nat.size() == best.size() - 1,
            "resolveTiers(NATIVE_ONLY) is BEST minus the floor (got " + nat.size()
                + " vs " + best.size() + ")");

        // CEILING_FREE drops AMD LBR.
        boolean cfNoAmd = true;
        for (HwTrace.TierChoice c : cf)
            if (c.tier() == HwTrace.TIER_HWTRACE && c.backend() == HwTrace.AMD_LBR) cfNoAmd = false;
        ok(cfNoAmd, "resolveTiers(CEILING_FREE) never selects AMD_LBR");

        // autoTier(policy) is the head of resolveTiers(policy).
        Optional<HwTrace.TierChoice> one = HwTrace.autoTier(HwTrace.TRACE_BEST);
        ok(one.isPresent(), "autoTier(BEST) is present");
        ok(one.get().tier() == best.get(0).tier() && one.get().backend() == best.get(0).backend(),
            "autoTier(BEST) is the head of resolveTiers(BEST)");
    }

    // Mirrors test_cross_tier_native_only_resolves_on_linux_x86_64.
    private static void crossTierNativeOnlyResolvesOnLinuxX86_64() {
        // On any x86-64 Linux host single-step is a native floor, so even NATIVE_ONLY
        // resolves (the cascade never collapses to nothing here). Off such a host, skip.
        if (!HwTrace.available(HwTrace.SINGLESTEP)) return;

        List<HwTrace.TierChoice> nat = HwTrace.resolveTiers(HwTrace.TRACE_NATIVE_ONLY);
        Optional<HwTrace.TierChoice> pick = HwTrace.autoTier(HwTrace.TRACE_NATIVE_ONLY);
        ok(!nat.isEmpty() && pick.isPresent() && pick.get().fidelity() == HwTrace.FIDELITY_NATIVE,
            "autoTier(NATIVE_ONLY) resolves a NATIVE choice on x86-64 Linux");

        boolean hasSinglestep = false;
        for (HwTrace.TierChoice c : nat)
            if (c.tier() == HwTrace.TIER_HWTRACE && c.backend() == HwTrace.SINGLESTEP) hasSinglestep = true;
        ok(hasSinglestep, "resolveTiers(NATIVE_ONLY) includes the single-step floor");
    }

    // Mirrors test_auto_resolve_traces_live — owns its own init/shutdown.
    private static void autoResolveTracesLive() {
        if (!HwTrace.available(HwTrace.SINGLESTEP)) return; // same guard as the live tests

        int[] best = HwTrace.resolve(HwTrace.BEST);
        int ab = HwTrace.auto(HwTrace.BEST);
        ok(best.length > 0 && ab >= 0, "auto resolves a backend (single-step floor)");

        HwTrace.init(ab);
        try {
            HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
            HwTrace.NativeTrace trace = HwTrace.create(64, 64);
            trace.register("auto", code);

            long[] r = new long[1];
            trace.region("auto", () -> r[0] = code.call(20, 22));
            ok(r[0] == 42, "auto call(20,22): result == 42 (got " + r[0] + ")");
            ok(trace.covered(0), "auto covers block offset 0");
            if (ab == HwTrace.SINGLESTEP) { // the pick off PT/AMD hosts: byte-exact parity
                long[] insns = trace.insnOffsets();
                long[] wantInsns = {0x0, 0x3, 0x6, 0xC, 0x11};
                ok(Arrays.equals(insns, wantInsns),
                    "auto pick (single-step) insnOffsets == [0,3,6,12,17] (got "
                        + Arrays.toString(insns) + ")");
            }

            trace.free();
            code.free();
        } finally {
            HwTrace.shutdown();
        }
    }
}
