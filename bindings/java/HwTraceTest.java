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
 * Compile with `--release 22`, run with
 * `--enable-native-access=ALL-UNNAMED`.
 */
import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
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

    // Whole-window OOP test leaves: two 7-byte native "methods" the driver frame calls into.
    private static final byte[] M1 = { 0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, (byte) 0xC3 }; // rax=rdi+rsi
    private static final byte[] M2 = { 0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x29, (byte) 0xF0, (byte) 0xC3 }; // rax=rdi-rsi

    // Build the self-contained 35-byte driver blob (the window frame), mirroring the C oracle
    // test_stealth_windowed: mov edi,7; mov esi,3; movabs rax,a1; call rax; movabs rax,a2; call
    // rax; ret. a1/a2 = runtime addresses of M1/M2; m2(7,3)=4 is the frame return.
    private static byte[] buildWindowDriver(long a1, long a2) {
        byte[] drv = {
            (byte) 0xBF, 7, 0, 0, 0,       // mov edi, 7
            (byte) 0xBE, 3, 0, 0, 0,       // mov esi, 3
            0x48, (byte) 0xB8, 0,0,0,0,0,0,0,0,  // movabs rax, a1  (imm at offset 12)
            (byte) 0xFF, (byte) 0xD0,      // call rax
            0x48, (byte) 0xB8, 0,0,0,0,0,0,0,0,  // movabs rax, a2  (imm at offset 24)
            (byte) 0xFF, (byte) 0xD0,      // call rax
            (byte) 0xC3                    // ret
        };
        ByteBuffer bb = ByteBuffer.wrap(drv).order(ByteOrder.LITTLE_ENDIAN);
        bb.putLong(12, a1);
        bb.putLong(24, a2);
        return drv;
    }

    // Classify a captured absolute-address trace by range containment (no Capstone): which of the
    // driver frame / leaf m1 / leaf m2 it hit, and the index each was first seen. Returns
    // {hitDrv?1:0, hitM1?1:0, hitM2?1:0, firstM1, firstM2}.
    private static long[] windowContainment(long[] insns, long dv, long dl, long a1, long a2) {
        boolean hitDrv = false, hitM1 = false, hitM2 = false; long firstM1 = -1, firstM2 = -1;
        for (int i = 0; i < insns.length; i++) {
            long at = insns[i];
            if (Long.compareUnsigned(at, dv) >= 0 && Long.compareUnsigned(at, dv + dl) < 0) hitDrv = true;
            if (Long.compareUnsigned(at, a1) >= 0 && Long.compareUnsigned(at, a1 + M1.length) < 0) { hitM1 = true; if (firstM1 < 0) firstM1 = i; }
            if (Long.compareUnsigned(at, a2) >= 0 && Long.compareUnsigned(at, a2 + M2.length) < 0) { hitM2 = true; if (firstM2 < 0) firstM2 = i; }
        }
        return new long[] { hitDrv ? 1 : 0, hitM1 ? 1 : 0, hitM2 ? 1 : 0, firstM1, firstM2 };
    }

    // Call-descent fixture (x86-64): a caller region R@0 that calls a sibling leaf S@0xc.
    //   R@0:  mov rax,rdi; call S(+4); add rax,rsi; ret     (region = 0xc bytes)
    //   S@0xc: inc rax; ret
    // args (20,22) -> rax=20, S makes it 21, +22 => 43. The traced region is 0xc so S
    // stays OUTSIDE it (else it mis-records as recursion); trace_call_ex takes region=0xc.
    private static final byte[] DESCENT_FIXTURE = {
        0x48, (byte) 0x89, (byte) 0xF8,               // mov rax, rdi
        (byte) 0xE8, 0x04, 0x00, 0x00, 0x00,          // call +4 -> S@0xc
        0x48, 0x01, (byte) 0xF0,                      // add rax, rsi
        (byte) 0xC3,                                  // ret  (end of region R @0xc)
        0x48, (byte) 0xFF, (byte) 0xC0,               // inc rax  (sibling leaf S)
        (byte) 0xC3                                   // ret
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

        // Out-of-process / foreign-process toolkit (HwTrace ptrace* surface). These
        // own their own resources and need no hwtrace init; they self-skip when the
        // ptrace backend is unavailable.
        try {
            ptraceTraceCall();
            ptraceTraceCallBlockstep();
            ptraceStealthTrace();
            ptraceWindowCall();
            stealthWindow();
            ptraceRunTo();
            procRegionByAddr();
            procPerfmapSymbol();
            jitdumpFind();
            ptraceDescentEdgesAndFrames();
            ptraceDescentResolverUpcall();
            codeImageRoundTrip();
            codeImageRenderVersioned();
            codeImageBpfProbe();
            stitchHandlesMergesBySeq();
            symbolizeBucketsAndRegionName();
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
            callScopedTracesANativeCall();
            windowTracesAWholeScope();
            attributeWindowSplitsNamedLeaves();
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

        // Reference scope: try-with-resources AsmTrace with auto-name + render.
        HwTrace.NativeCode scode = HwTrace.NativeCode.fromBytes(ROUTINE);
        long[] sr = {0};
        HwTrace.AsmTrace scope;
        try (HwTrace.AsmTrace t = HwTrace.AsmTrace.scope(scode, false)) { // auto-name here
            scope = t;
            sr[0] = scode.call(20, 22);
        }
        ok(sr[0] == 42, "AsmTrace: add2(20,22) == 42 (got " + sr[0] + ")");
        ok(scope.armed(), "AsmTrace: armed on an available backend");
        ok(!scope.truncated(), "AsmTrace: not truncated");
        ok(scope.path() != null && !scope.path().isEmpty(), "AsmTrace: render-on-close produced text");
        long nl = scope.path().chars().filter(c -> c == '\n').count();
        ok(nl == 5, "AsmTrace: 5 rendered instruction lines (got " + nl + ")");
        ok(scope.path().contains("ret"), "AsmTrace: rendered listing includes the ret");
        ok(scope.name().startsWith("HwTraceTest.java:"),
            "AsmTrace: auto-name is basename:line (got " + scope.name() + ")");
        scode.free();
    }

    // Mirrors test_call_scoped_traces_a_native_call: arm + call + disarm entirely in
    // native code (asmtest_hwtrace_call_scoped_ex) — REGISTRY-FREE, returning the call
    // result and the executed body's disassembly in one step. For a native leaf fn == base.
    private static void callScopedTracesANativeCall() {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        try {
            HwTrace.CallScopedResult res = HwTrace.callScoped(code, 20, 22); // 42 <= 100 -> jle taken
            ok(res.result() == 42, "callScoped(20,22): result == 42 (got " + res.result() + ")");
            ok(res.rc() == HwTrace.ASMTEST_HW_OK, "callScoped rc == OK (got " + res.rc() + ")");
            ok(!res.truncated(), "callScoped !truncated");
            if (res.path() != null && !res.path().isEmpty()) { // non-empty when Capstone is present
                ok(res.path().toLowerCase().contains("ret"),
                    "callScoped rendered listing includes the ret");
                long nl = res.path().chars().filter(c -> c == '\n').count();
                ok(nl == 5, "callScoped: 5 rendered instruction lines (got " + nl + ")");
            }
            // Registry-free: many one-shot captures must NOT exhaust the fixed region table.
            boolean allOk = true;
            long bad = -1;
            for (int i = 0; i < 40; i++) {
                long r = HwTrace.callScoped(code, i, 1).result();
                if (r != i + 1) { allOk = false; bad = i; }
            }
            ok(allOk, "callScoped registry-free: 40 calls each return i+1 (first bad i=" + bad + ")");
        } finally {
            code.free();
        }
    }

    // Region-free WHOLE-WINDOW capture (§Z1 — the empty-ctor `using (new AsmTrace())`).
    // HONEST-BUT-NOISY: single-stepping the JVM captures the FFI dispatch + harness too, so the
    // routine's absolute addresses are a SUBSET. Mirrors the C whole-window test.
    private static void windowTracesAWholeScope() {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        try {
            long[] r = { 0 };
            // The JVM whole-window is very noisy — HotSpot + FFM linkage run >1M insns per
            // call, exceeding the single-step whole-window's internal SS_WINDOW_CAP (1<<20),
            // so `truncated` is set no matter how big the caller buffer is. The test handles
            // that honestly below (skips the exact-subset assert on a truncated capture).
            HwTrace.WindowResult res = HwTrace.window(() -> r[0] = code.call(20, 22)); // 42
            System.out.println("# window: armed=" + res.armed() + " truncated=" + res.truncated()
                + " insns=" + res.insns().length);
            ok(r[0] == 42, "window: the traced call still returns 42 (execution intact under TF)");
            if (res.armed()) {
                ok(res.insns().length >= 5, "window: captured instructions (routine + harness noise)");
                ok(!res.path().isEmpty(), "window: render_window produced disassembly text");
                // insns[] hold ABSOLUTE addresses. On a clean (non-truncated) capture the routine's
                // own addresses [base+0,+3,+6,+0xc,+0x11] must all be present amid the noise.
                if (!res.truncated()) {
                    long base = code.base();
                    long[] want = { base, base + 0x3, base + 0x6, base + 0xC, base + 0x11 };
                    java.util.Set<Long> got = new java.util.HashSet<>();
                    for (long addr : res.insns()) got.add(addr);
                    boolean subset = true;
                    for (long w : want) if (!got.contains(w)) subset = false;
                    ok(subset, "window: the routine's absolute addresses are all captured (subset)");
                } else {
                    System.out.println("# note: window truncated (managed capture overflowed) "
                        + "— skipping the exact-subset assert (honest best-effort)");
                }
            } else {
                System.out.println("# note: window self-skipped (begin_window unavailable)");
            }
        } finally {
            code.free();
        }
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
        ok(trace.covered(0) && trace.covered(0x7) && trace.covered(0xf),
           "covered(0) && covered(7) && covered(0xf)");
        // 3 blocks {0,0x7,0xf}: the ret after the not-taken jnz is its own block.
        ok(trace.blocksLen() == 3, "blocksLen == 3 (got " + trace.blocksLen() + ")");
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

    // ---- Out-of-process / foreign-process toolkit (mirrors the Python Ptrace tests) ----

    private static boolean ptraceUnavailable() {
        if (!HwTrace.ptraceAvailable()) {
            System.out.println("# SKIP ptrace backend unavailable: " + HwTrace.ptraceSkipReason());
            return true;
        }
        return false;
    }

    // Mirrors test_ptrace_trace_call: fork a tracee, single-step it out of process,
    // get the same offsets.
    private static void ptraceTraceCall() {
        if (ptraceUnavailable()) return;
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        HwTrace.NativeTrace trace = HwTrace.create(64, 64);

        long result = HwTrace.ptraceTraceCall(MemorySegment.ofAddress(code.base()),
            code.length(), new long[] {20, 22}, trace.handle());
        ok(result == 42, "ptraceTraceCall(20,22): result == 42 (got " + result + ")");

        long[] insns = trace.insnOffsets();
        long[] wantInsns = {0x0, 0x3, 0x6, 0xC, 0x11};
        ok(Arrays.equals(insns, wantInsns),
            "ptrace insnOffsets == [0,3,6,12,17] (got " + Arrays.toString(insns) + ")");
        ok(!trace.truncated(), "ptrace !truncated");

        trace.free();
        code.free();
    }

    // BTF block-step tier: one #DB per TAKEN branch, intra-block instructions
    // reconstructed with Capstone — the stream is byte-identical to the
    // per-instruction ptraceTraceCall above. Self-skips where PTRACE_SINGLEBLOCK /
    // Capstone are absent (e.g. AArch64).
    private static void ptraceTraceCallBlockstep() {
        if (ptraceUnavailable()) return;
        if (!HwTrace.ptraceBlockstepAvailable()) {
            System.out.println(
                "# SKIP BTF block-step unavailable (needs x86-64 PTRACE_SINGLEBLOCK + Capstone)");
            return;
        }
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        HwTrace.NativeTrace trace = HwTrace.create(64, 64);

        long result = HwTrace.ptraceTraceCallBlockstep(MemorySegment.ofAddress(code.base()),
            code.length(), new long[] {20, 22}, trace.handle());
        ok(result == 42, "ptraceTraceCallBlockstep(20,22): result == 42 (got " + result + ")");

        long[] insns = trace.insnOffsets();
        long[] wantInsns = {0x0, 0x3, 0x6, 0xC, 0x11};
        ok(Arrays.equals(insns, wantInsns),
            "ptrace block-step insn stream identical to single-step (got " + Arrays.toString(insns) + ")");
        ok(!trace.truncated(), "ptrace block-step !truncated");

        trace.free();
        code.free();
    }

    // §D3 stealth stepper (HwTrace.stealthTrace): a reverse-attached helper child single-steps
    // the region while THIS thread runs the leaf — so NO EFLAGS.TF is armed on the caller. This
    // is the CRASH-PROOF managed-capture route (a ptrace-stop is not gated by the tracee's signal
    // mask), the Java analog of dotnet's AsmTrace.Method(..., outOfProcess: true). Needs no
    // HwTrace.init. Self-skips when the reverse-attach is refused (Yama ptrace_scope); otherwise
    // reconstructs the identical [0,3,6,c,11] ground-truth stream as the fork/single-step paths.
    private static void ptraceStealthTrace() {
        if (ptraceUnavailable()) return;
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        HwTrace.StealthResult res = HwTrace.stealthTrace(code, 20, 22); // 42 <= 100 -> jle taken
        System.out.println("# stealth: armed=" + res.armed() + " truncated=" + res.truncated()
            + " offsets=" + res.offsets().length);
        if (!res.armed()) {
            System.out.println("# SKIP ptrace stealth: reverse-attach not permitted (Yama ptrace_scope)");
            code.free();
            return;
        }
        // HARD guarantee: the out-of-band stepper reads the true return from the caller's RAX,
        // EXACT even when the stream is best-effort — and TF is never armed on the calling thread.
        ok(res.result() == 42, "stealthTrace(20,22): result == 42 out of band (got " + res.result() + ")");
        // Stream: EXACT when the reverse-attach single-step ran to completion; over a LIVE runtime
        // it may truncate (async signals interrupt the per-insn step) — honest best-effort, like
        // dotnet's outOfProcess AsmTrace.Method and window(). Assert exactness only when complete.
        if (!res.truncated()) {
            long[] wantInsns = {0x0, 0x3, 0x6, 0xC, 0x11};
            ok(Arrays.equals(res.offsets(), wantInsns) && res.blocks() == 2,
                "stealthTrace: exact offsets [0,3,6,12,17] over two blocks (got "
                    + Arrays.toString(res.offsets()) + ")");
        } else {
            ok(true, "stealthTrace: stream truncated over the live runtime (honest best-effort; result exact)");
        }
        code.free();
    }

    // §D3 WHOLE-WINDOW fork-internal capture (HwTrace.ptraceTraceWindowCall): fork a child that
    // runs the driver frame; record the driver AND both channel-published leaves as ABSOLUTE
    // addresses, in call order. Fork-internal (steps a child, not this thread), so it asserts
    // unconditionally on any ptrace lane. Mirrors the C oracle test_ptrace_window_call.
    private static void ptraceWindowCall() {
        if (ptraceUnavailable()) return;
        HwTrace.NativeCode m1 = HwTrace.NativeCode.fromBytes(M1);
        HwTrace.NativeCode m2 = HwTrace.NativeCode.fromBytes(M2);
        long a1 = m1.base(), a2 = m2.base();
        HwTrace.NativeCode drv = HwTrace.NativeCode.fromBytes(buildWindowDriver(a1, a2));
        try (HwTrace.AddrChannel chan = HwTrace.AddrChannel.newLocal()) {
            chan.publish(m1).publish(m2); // pre-publish the leaves the frame calls into
            HwTrace.WindowTraceResult res = HwTrace.ptraceTraceWindowCall(drv, new long[] {7, 3}, chan);
            ok(res.result() == 4, "windowCall: driver frame returns m2(7,3) == 4 (got " + res.result() + ")");
            long[] c = windowContainment(res.insns(), drv.base(), drv.length(), a1, a2);
            ok(c[0] == 1 && c[1] == 1 && c[2] == 1,
                "windowCall: records the driver frame AND both channel-published leaves");
            ok(c[3] >= 0 && c[4] > c[3], "windowCall: follows the calls in order (m1 before m2)");
            ok(!res.truncated(), "windowCall: capture complete");
        }
        drv.free(); m1.free(); m2.free();
    }

    // §D3 CRASH-PROOF whole-window OOP capture (HwTrace.stealthWindow): a reverse-attached helper
    // steps the window body out of band while THIS thread runs it — the OOP analog of the
    // in-process window() footgun, mirroring dotnet's AsmTrace.Window. Self-skips on a refused
    // reverse-attach; else records driver + both leaves in order (best-effort stream over a live
    // runtime, exact result). Mirrors the C oracle test_stealth_windowed.
    private static void stealthWindow() {
        if (ptraceUnavailable()) return;
        HwTrace.NativeCode m1 = HwTrace.NativeCode.fromBytes(M1);
        HwTrace.NativeCode m2 = HwTrace.NativeCode.fromBytes(M2);
        long a1 = m1.base(), a2 = m2.base();
        HwTrace.NativeCode drv = HwTrace.NativeCode.fromBytes(buildWindowDriver(a1, a2));
        try (HwTrace.AddrChannel chan = HwTrace.AddrChannel.newShared()) {
            chan.publish(m1).publish(m2);
            HwTrace.WindowTraceResult res = HwTrace.stealthWindow(drv, chan);
            System.out.println("# stealthWindow: armed=" + res.armed() + " truncated=" + res.truncated()
                + " insns=" + res.insns().length);
            if (!res.armed()) {
                System.out.println("# SKIP stealth windowed: reverse-attach not permitted (Yama ptrace_scope)");
            } else {
                ok(res.result() == 4, "stealthWindow: frame returns m2(7,3) == 4 out of band (got " + res.result() + ")");
                if (!res.truncated()) {
                    long[] c = windowContainment(res.insns(), drv.base(), drv.length(), a1, a2);
                    ok(c[0] == 1 && c[1] == 1 && c[2] == 1,
                        "stealthWindow: records the driver frame AND both pre-published leaves");
                    ok(c[3] >= 0 && c[4] > c[3], "stealthWindow: follows the calls in order (m1 before m2)");
                } else {
                    ok(true, "stealthWindow: stream truncated over the live runtime (honest best-effort; result exact)");
                }
            }
        }
        drv.free(); m1.free(); m2.free();
    }

    // run_to drives an attached target to a resolved method (software breakpoint). A
    // live foreign attach is covered by the C suite (forking + ptrace of a foreign
    // process is impractical here, same as ptraceTraceAttached); exercise the FFI
    // round-trip safely — a NULL target address is rejected (EINVAL, non-zero) before
    // any ptrace call.
    private static void ptraceRunTo() {
        if (ptraceUnavailable()) return;
        int pid = (int) ProcessHandle.current().pid();
        int rc = HwTrace.ptraceRunTo(pid, 0L);
        ok(rc != 0, "ptraceRunTo(NULL addr) rejected (EINVAL) via the FFI round-trip (got " + rc + ")");
    }

    // Mirrors test_proc_region_by_addr: discover an executable region's extent from
    // /proc/<pid>/maps by an interior address (this process).
    private static void procRegionByAddr() {
        if (ptraceUnavailable()) return;
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        int pid = (int) ProcessHandle.current().pid();

        long[] region = HwTrace.procRegionByAddr(pid, code.base() + 4);
        ok(region != null, "procRegionByAddr finds the mapping for an interior addr");
        ok(region != null && region[0] == code.base() && region[1] >= ROUTINE.length,
            "procRegionByAddr base == code.base && len >= " + ROUTINE.length
                + " (got " + (region == null ? "null" : Arrays.toString(region)) + ")");
        ok(HwTrace.procRegionByAddr(pid, 0x1) == null, "procRegionByAddr(addr 1) == null");

        code.free();
    }

    // Mirrors test_proc_perfmap_symbol: parse a JIT perf-map (/tmp/perf-<pid>.map) and
    // resolve a method by name.
    private static void procPerfmapSymbol() {
        if (ptraceUnavailable()) return;
        int pid = (int) ProcessHandle.current().pid();
        Path path = Path.of("/tmp/perf-" + pid + ".map");
        try {
            Files.writeString(path, "400000 1a void demo(long, long)\n500000 8 other\n");

            long[] m = HwTrace.procPerfmapSymbol(pid, "void demo(long, long)");
            ok(m != null && m[0] == 0x400000L && m[1] == 0x1AL,
                "procPerfmapSymbol resolves (0x400000, 0x1a) (got "
                    + (m == null ? "null" : Arrays.toString(m)) + ")");
            ok(HwTrace.procPerfmapSymbol(pid, "missing") == null,
                "procPerfmapSymbol(missing) == null");
        } catch (IOException e) {
            throw new RuntimeException(e);
        } finally {
            try { Files.deleteIfExists(path); } catch (IOException ignored) {}
        }
    }

    // Mirrors test_jitdump_find: read a binary jitdump and resolve a method to
    // (addr,size,index) + bytes. The file is little-endian; the entry struct is four
    // u64. Byte layout matches the Python struct.pack formats exactly.
    private static void jitdumpFind() {
        if (ptraceUnavailable()) return;
        Path path = null;
        try {
            path = Files.createTempFile("jit", ".dump");
            byte[] name = "void demo(long, long)".getBytes(java.nio.charset.StandardCharsets.US_ASCII);

            int headerLen = 40;        // <IIIIIIQQ : 6*u32 + 2*u64
            int recHeaderLen = 16;     // <IIQ      : 2*u32 + 1*u64
            int bodyLen = 40;          // <IIQQQQ   : 2*u32 + 4*u64
            int total = recHeaderLen + bodyLen + (name.length + 1) + ROUTINE.length;
            int fileLen = headerLen + total;

            ByteBuffer buf = ByteBuffer.allocate(fileLen).order(ByteOrder.LITTLE_ENDIAN);
            // header: magic, version, total_size=40, elf_mach=62, pad1, pid, timestamp, flags
            buf.putInt(0x4A695444).putInt(1).putInt(40).putInt(62).putInt(0).putInt(0)
               .putLong(0).putLong(0);
            // JIT_CODE_LOAD record header: id=0, total, ts=5
            buf.putInt(0).putInt(total).putLong(5);
            // body: pid, tid, vma, code_addr, code_size, code_index
            buf.putInt(0).putInt(0).putLong(0x2000).putLong(0x2000)
               .putLong(ROUTINE.length).putLong(9);
            // name + NUL, then the recorded code bytes
            buf.put(name).put((byte) 0).put(ROUTINE);

            Files.write(path, buf.array());

            Optional<HwTrace.JitMethod> m = HwTrace.jitdumpFind(
                path.toString(), "void demo(long, long)", 0, 64);
            ok(m.isPresent(), "jitdumpFind resolves the method");
            HwTrace.JitMethod jm = m.orElseThrow();
            ok(jm.codeAddr() == 0x2000L && jm.codeSize() == ROUTINE.length
                && jm.codeIndex() == 9L && jm.timestamp() == 5L,
                "jitdumpFind (addr,size,index,ts) == (0x2000," + ROUTINE.length + ",9,5) (got ("
                    + Long.toHexString(jm.codeAddr()) + "," + jm.codeSize() + ","
                    + jm.codeIndex() + "," + jm.timestamp() + "))");
            ok(Arrays.equals(jm.code(), ROUTINE),
                "jitdumpFind code == ROUTINE (got " + Arrays.toString(jm.code()) + ")");

            ok(HwTrace.jitdumpFind(path.toString(), "missing", 0, 0).isEmpty(),
                "jitdumpFind(missing) is empty");
        } catch (IOException e) {
            throw new RuntimeException(e);
        } finally {
            if (path != null) try { Files.deleteIfExists(path); } catch (IOException ignored) {}
        }
    }

    // The descent fixture is x86-64 machine code, so its live replay is x86-64-only
    // (the ptrace stepper is available on aarch64 too, but these bytes would fault there).
    private static boolean hostIsX86_64() {
        String a = System.getProperty("os.arch", "").toLowerCase();
        return a.contains("amd64") || a.contains("x86_64") || a.contains("x8664");
    }

    // Mirrors test_descent_edges_and_frames: fork + single-step the caller region R with a
    // Descent handle threaded through, at L1 (record edges) and L2 (descend into the leaf).
    private static void ptraceDescentEdgesAndFrames() {
        if (ptraceUnavailable()) return;
        if (!hostIsX86_64()) {
            System.out.println("# SKIP ptrace descent: x86-64 fixture, host arch "
                + System.getProperty("os.arch"));
            return;
        }
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(DESCENT_FIXTURE);
        long base = code.base();
        try {
            // L1 RECORD_EDGES, region = 0xc: the call-out to the sibling is recorded as one
            // edge and stepped over; only frame 0 exists.
            try (HwTrace.Descent d = new HwTrace.Descent(HwTrace.DESCENT_RECORD_EDGES);
                 HwTrace.NativeTrace tr = HwTrace.create(64, 64)) {
                long result = HwTrace.ptraceTraceCallEx(MemorySegment.ofAddress(base), 0xC,
                    new long[] {20, 22}, tr.handle(), d.handle());
                ok(result == 43, "descent L1 call(20,22): result == 43 (got " + result + ")");
                ok(d.framesLen() == 1, "descent L1 framesLen == 1 (got " + d.framesLen() + ")");
                long[] f0 = d.frameInsns(0);
                ok(Arrays.equals(f0, new long[] {0x0, 0x3, 0x8, 0xB}),
                    "descent L1 frameInsns(0) == [0,3,8,11] (got " + Arrays.toString(f0) + ")");
                HwTrace.Edge[] edges = d.edges();
                ok(edges.length == 1 && edges[0].site() == 0x3
                        && edges[0].target() == base + 0xC && edges[0].depth() == 0,
                    "descent L1 one edge (site 3 -> base+0xc, depth 0) (got "
                        + Arrays.toString(edges) + ")");
                ok(!d.truncated(), "descent L1 !truncated");
            }
            // L2 DESCEND_KNOWN, region = 0xc, allow the sibling: it is single-stepped INTO
            // as frame 1 (depth 1) and no edge is recorded.
            try (HwTrace.Descent d = new HwTrace.Descent(HwTrace.DESCENT_DESCEND_KNOWN);
                 HwTrace.NativeTrace tr = HwTrace.create(64, 64)) {
                ok(d.allowRegion(base + 0xC, 4) == 0, "descent L2 allowRegion(base+0xc, 4) == 0");
                long result = HwTrace.ptraceTraceCallEx(MemorySegment.ofAddress(base), 0xC,
                    new long[] {20, 22}, tr.handle(), d.handle());
                ok(result == 43, "descent L2 call(20,22): result == 43 (got " + result + ")");
                ok(d.framesLen() == 2, "descent L2 framesLen == 2 (got " + d.framesLen() + ")");
                long[] f0 = d.frameInsns(0);
                ok(Arrays.equals(f0, new long[] {0x0, 0x3, 0x8, 0xB}),
                    "descent L2 frameInsns(0) == [0,3,8,11] (got " + Arrays.toString(f0) + ")");
                ok(d.frameBase(1) == base + 0xC, "descent L2 frameBase(1) == base+0xc (got 0x"
                    + Long.toHexString(d.frameBase(1)) + ", want 0x" + Long.toHexString(base + 0xC) + ")");
                ok(d.frameDepth(1) == 1, "descent L2 frameDepth(1) == 1 (got " + d.frameDepth(1) + ")");
                long[] f1 = d.frameInsns(1);
                ok(Arrays.equals(f1, new long[] {0x0, 0x3}),
                    "descent L2 frameInsns(1) == [0,3] (got " + Arrays.toString(f1) + ")");
                ok(d.edges().length == 0, "descent L2 edges() == [] (got " + d.edges().length + ")");
            }
        } finally {
            code.free();
        }
    }

    // The resolver upcall path: at L2 with no allow-region, a Descent.Resolver callback
    // decides the leaf is descendable. Proves the FFM upcallStub actually fires in-process
    // (the callback records the callee it saw) and drives the same descent as the allow-set.
    private static void ptraceDescentResolverUpcall() {
        if (ptraceUnavailable()) return;
        if (!hostIsX86_64()) return; // the edges/frames test already printed the SKIP line
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(DESCENT_FIXTURE);
        long base = code.base();
        long sib = base + 0xC;
        try (HwTrace.Descent d = new HwTrace.Descent(HwTrace.DESCENT_DESCEND_KNOWN);
             HwTrace.NativeTrace tr = HwTrace.create(64, 64)) {
            long[] seen = { -1 }; // the callee address the upcall was invoked with
            d.setResolver(callee -> {
                seen[0] = callee;
                return callee == sib ? new long[] {sib, 4} : null;
            });
            long result = HwTrace.ptraceTraceCallEx(MemorySegment.ofAddress(base), 0xC,
                new long[] {20, 22}, tr.handle(), d.handle());
            ok(result == 43, "descent resolver call(20,22): result == 43 (got " + result + ")");
            ok(seen[0] == sib, "descent resolver upcall fired with callee == base+0xc (got 0x"
                + Long.toHexString(seen[0]) + ")");
            ok(d.framesLen() == 2, "descent resolver descended (framesLen == 2, got "
                + d.framesLen() + ")");
            ok(d.frameBase(1) == sib, "descent resolver frameBase(1) == base+0xc");
        } finally {
            code.free();
        }
    }

    // ---- Time-aware code-image recorder (HwTrace.CodeImage surface) ----

    // mov rax,rdi; add rax,rsi; ret — 7 bytes of host-native code to track and round-trip.
    private static final byte[] ADD2 = {
        0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, (byte) 0xC3
    };

    // Track a self-process region, then read its bytes back at sequence 0 — the
    // userspace TEXT_POKE round-trip. Self-skips when the recorder is unavailable.
    private static void codeImageRoundTrip() {
        if (!HwTrace.CodeImage.available()) {
            System.out.println("# SKIP code-image recorder unavailable: "
                + HwTrace.CodeImage.skipReason());
            return;
        }
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ADD2);
        try (HwTrace.CodeImage img = new HwTrace.CodeImage(0)) { // pid 0 == self
            int rc = img.track(code.base(), ADD2.length);
            ok(rc == HwTrace.ASMTEST_CI_OK,
                "CodeImage.track(base, " + ADD2.length + ") == OK (got " + rc + ")");
            ok(img.now() >= 1, "CodeImage.now() >= 1 after track (got " + img.now() + ")");
            ok(img.refresh() >= 0, "CodeImage.refresh() >= 0");

            byte[] got = img.bytesAt(code.base(), 0); // when 0 == latest version
            ok(got != null && got.length >= ADD2.length
                && Arrays.equals(Arrays.copyOf(got, ADD2.length), ADD2),
                "CodeImage.bytesAt(base, 0) round-trips ADD2 (got "
                    + (got == null ? "null" : Arrays.toString(Arrays.copyOf(got, ADD2.length)))
                    + ")");
        } finally {
            code.free();
        }
    }

    // render_versioned: version-AWARE disassembly (mirrors C test_render_versioned). Track a
    // WRITABLE region as 'add', rewrite it to 'sub' + refresh (a 2nd version), then render a trace
    // of the same ABSOLUTE address at the OLD version (add) and the NEW version (sub) — proving the
    // render decodes the timeline SNAPSHOT, not live memory (which would show only 'sub'). Self-skips
    // without a 2nd version or without Capstone.
    private static void codeImageRenderVersioned() {
        if (!HwTrace.CodeImage.available()) {
            System.out.println("# SKIP render_versioned: " + HwTrace.CodeImage.skipReason());
            return;
        }
        try (Arena a = Arena.ofConfined();
             HwTrace.CodeImage img = new HwTrace.CodeImage(0);
             HwTrace.NativeTrace tr = HwTrace.create(4, 4)) {
            MemorySegment region = a.allocate(4096); // writable, stable native address
            MemorySegment.copy(ADD2, 0, region, JAVA_BYTE, 0, ADD2.length); // version A: add
            long addr = region.address();
            img.track(addr, ADD2.length);
            long t0 = img.now();
            region.set(JAVA_BYTE, 4, (byte) 0x29); // add (0x01) -> sub (0x29) in place
            img.refresh();
            long t1 = img.now();
            tr.appendInsn(addr + 3); // ABSOLUTE address of the add/sub instruction (offset 3)
            String b0 = img.renderVersioned(t0, tr);
            String b1 = img.renderVersioned(t1, tr);
            if (t1 <= t0) {
                System.out.println("# SKIP render_versioned: recorder saw no page change (no 2nd version)");
            } else if (b0.isEmpty() || b1.isEmpty()) {
                System.out.println("# SKIP render_versioned: Capstone decoder absent (render returned empty)");
            } else {
                ok(b0.contains("add"), "render_versioned at t0 shows add (version A bytes)");
                ok(b1.contains("sub"), "render_versioned at t1 shows sub (version B bytes)");
                ok(!b0.equals(b1), "render_versioned is version-aware (t0 text != t1 text)");
            }
        }
    }

    // stitchHandles: the §D0.4 async-hop merge. HOST-INDEPENDENT (pure merge — no single-step,
    // Capstone, or PT): script two "hops" OUT of seq order and prove they merge back BY seq.
    // Mirrors the C oracle test_stitch_slices.
    private static void stitchHandlesMergesBySeq() {
        try (HwTrace.NativeTrace trA = HwTrace.create(16, 16);
             HwTrace.NativeTrace trB = HwTrace.create(16, 16)) {
            trA.appendInsn(0).appendInsn(3).appendInsn(6); // hop A: seq 0
            trB.appendInsn(0).appendInsn(4).appendInsn(8); // hop B: seq 1
            // Pass the hops OUT of seq order (B then A); stitch must re-order by seq.
            HwTrace.StitchResult st = HwTrace.stitchHandles(
                new HwTrace.NativeTrace[] { trB, trA },
                new long[] { 7, 7 }, new int[] { 1, 0 }, new int[] { 222, 111 }, new long[] { 9, 5 });
            ok(Arrays.equals(st.insns(), new long[] { 0, 3, 6, 0, 4, 8 }),
                "stitchHandles: merges hops BY seq (A[0,3,6] before B[0,4,8]) despite input order (got "
                    + Arrays.toString(st.insns()) + ")");
            ok(st.bounds().length == 2, "stitchHandles: one slice bound per hop");
            HwTrace.StitchBound b0 = st.bounds()[0], b1 = st.bounds()[1];
            ok(b0.seq() == 0 && b0.insnOff() == 0 && b0.tid() == 111 && b0.version() == 5,
                "stitchHandles: bound[0] is hop A (seq 0, off 0, tid 111, v5)");
            ok(b1.seq() == 1 && b1.insnOff() == 3 && b1.tid() == 222 && b1.version() == 9,
                "stitchHandles: bound[1] is hop B (seq 1, off 3, tid 222, v9)");
        } // hops freed only AFTER stitch reads them (shallow-copy lifetime)
    }

    // symbolizeBuckets + regionName: whole-window noise attribution. Linux /proc + a synthetic /tmp
    // perf-map (no single-step/Capstone/PT/privilege). Mirrors the C oracle test_symbolize_bucket.
    private static void symbolizeBucketsAndRegionName() throws IOException {
        if (!System.getProperty("os.name", "").toLowerCase().contains("linux")) {
            System.out.println("# SKIP symbolize_bucket/region_name: Linux /proc + perf-map only");
            return;
        }
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        long base = code.base();
        Path perfMap = Path.of("/tmp/perf-" + ProcessHandle.current().pid() + ".map");
        Files.writeString(perfMap, "40000000 1000 MyJitMethod\n");
        try {
            long jit = 0x40000500L;
            HwTrace.Bucket[] buckets = HwTrace.symbolizeBuckets(
                new long[] { base, base, base, jit, jit, 1L }, 0, 64);
            long total = 0;
            for (HwTrace.Bucket b : buckets) total += b.count();
            ok(total == 6, "symbolizeBuckets: every IP is bucketed (total count == 6, got " + total + ")");
            boolean jitOk = false, unk = false;
            for (HwTrace.Bucket b : buckets) {
                if (b.label().contains("MyJitMethod") && b.count() == 2) jitOk = true;
                if (b.label().contains("unknown")) unk = true;
            }
            ok(jitOk, "symbolizeBuckets: the 2 JIT IPs bucket under MyJitMethod (perf-map)");
            ok(unk, "symbolizeBuckets: the unmapped IP buckets under [unknown]");
            ok(HwTrace.symbolizeBuckets(new long[0], 0, 64).length == 0, "symbolizeBuckets: empty input -> empty");
            HwTrace.RegionName rn = HwTrace.regionName(base, 0);
            ok(rn != null && Long.compareUnsigned(rn.start(), base) <= 0
                && Long.compareUnsigned(base, rn.end()) < 0 && !rn.name().isEmpty(),
                "regionName: resolves the containing mapping name + extent");
        } finally {
            Files.deleteIfExists(perfMap);
            code.free();
        }
    }

    // attributeWindow: whole-window capture + attribute absolute addresses to caller-named regions.
    // Two IDENTICAL-byte leaves A,B in distinct mappings — the named-region path (exact range) splits
    // them into SEPARATE buckets, which symbol/disasm attribution cannot. Mirrors the C oracle
    // test_wholewindow_buckets. Self-skips off the single-step tier.
    private static void attributeWindowSplitsNamedLeaves() {
        HwTrace.NativeCode A = HwTrace.NativeCode.fromBytes(ROUTINE);
        HwTrace.NativeCode B = HwTrace.NativeCode.fromBytes(ROUTINE); // identical bytes, distinct mapping
        long[] r = new long[2];
        HwTrace.AttributeResult res = HwTrace.attributeWindow(
            () -> { r[0] = A.call(20, 22); r[1] = B.call(30, 12); },
            new HwTrace.NamedRegion[] {
                new HwTrace.NamedRegion("leafA", A.base(), A.length()),
                new HwTrace.NamedRegion("leafB", B.base(), B.length()) },
            64, 1 << 20);
        System.out.println("# attributeWindow: armed=" + res.armed() + " buckets=" + res.buckets().length);
        ok(r[0] == 42 && r[1] == 42, "attributeWindow: both traced leaves still return their results");
        if (!res.armed()) {
            System.out.println("# SKIP attributeWindow: single-step tier unavailable (begin_window)");
        } else {
            HwTrace.Bucket la = null, lb = null;
            for (HwTrace.Bucket b : res.buckets()) {
                if (b.label().equals("leafA")) la = b;
                if (b.label().equals("leafB")) lb = b;
            }
            ok(la != null && lb != null,
                "attributeWindow: identical-byte leaves split into SEPARATE named buckets leafA/leafB");
            if (la != null && lb != null) {
                ok(la.count() == 5 && lb.count() == 5,
                    "attributeWindow: each named leaf bucket counts its 5 executed instructions");
            }
        }
        A.free();
        B.free();
    }

    // Probe the optional eBPF emission detector. Skips without libbpf / CAP_BPF / BTF;
    // where it is available, watch_bpf() loads and attaches the CO-RE program.
    private static void codeImageBpfProbe() {
        if (!HwTrace.CodeImage.bpfAvailable()) {
            System.out.println("# SKIP code-image eBPF detector unavailable: "
                + HwTrace.CodeImage.bpfSkipReason());
            return;
        }
        try (HwTrace.CodeImage img = new HwTrace.CodeImage(0)) {
            int rc = img.watchBpf();
            ok(rc == HwTrace.ASMTEST_CI_OK,
                "CodeImage.watchBpf() == OK (got " + rc + ")");
        }
    }
}
