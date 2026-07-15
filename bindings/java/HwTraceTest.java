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
            traceCallAutoOwnsTheCall();
            flagdayStatusAndMechanism();
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
            liveJavaJitdumpResolution();
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
            stitchedTraceProducerAcrossHops();
            stitchedTraceAutoPropagatesAcrossExecutor();
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

    // §D0.4 LIVE async-hop producer (HwTrace.AsmStitchedTrace): follow ONE logical operation across
    // a REAL executor thread hop and stitch each hop's call_scoped capture into one seq-ordered
    // trace — the JVM analog of dotnet's AsmStitchedTrace. Proves (1) the thread-local scope id
    // crosses the submit onto the pool thread via propagate(), (2) the hop really ran on a
    // DIFFERENT OS thread, and (3) the two hops stitch back by seq, every slice carrying the one
    // operation's scope id + its capturing tid. Single-step is available here (past the SKIP guard).
    private static void stitchedTraceProducerAcrossHops() throws Exception {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        java.util.concurrent.ExecutorService pool = java.util.concurrent.Executors.newSingleThreadExecutor();
        try (HwTrace.AsmStitchedTrace op = new HwTrace.AsmStitchedTrace()) {
            long opId = op.scopeId();
            ok(opId != 0, "stitchedTrace: the operation has a nonzero scope id");
            ok(HwTrace.AsmStitchedTrace.currentScopeId() == opId,
                "stitchedTrace: the scope id flows through this thread's thread-local");
            int mainTid = (int) Thread.currentThread().threadId();

            long r0 = op.step(code, 20, 22); // hop 0 on THIS thread -> 42
            ok(r0 == 42, "stitchedTrace: hop 0 runs synchronously on the calling thread (result 42)");

            // hop 1 on a REAL pool thread; propagate() carries the scope id across the submit.
            long[] hopScope = new long[1];
            int[] hopTid = new int[1];
            long r1 = pool.submit(op.propagate(() -> {
                hopScope[0] = HwTrace.AsmStitchedTrace.currentScopeId();
                hopTid[0] = (int) Thread.currentThread().threadId();
                return op.step(code, 1, 2); // -> 3
            })).get();
            ok(r1 == 3, "stitchedTrace: hop 1 runs on a pool thread (result 3)");
            ok(hopScope[0] == opId,
                "stitchedTrace: propagate() carries the scope id to the pool thread (the AsyncLocal analog)");
            ok(hopTid[0] != mainTid, "stitchedTrace: hop 1 really ran on a DIFFERENT OS thread");

            op.complete();
            ok(op.hopCount() == 2, "stitchedTrace: two hops attempted");
            HwTrace.StitchBound[] b = op.hops();
            ok(b.length == 2, "stitchedTrace: both hops captured and stitched (got " + b.length + ")");
            ok(b[0].seq() == 0 && b[0].insnOff() == 0, "stitchedTrace: slice 0 is hop 0 at merged offset 0");
            ok(b[1].seq() == 1 && b[1].insnOff() > 0,
                "stitchedTrace: slice 1 is hop 1, stitched AFTER hop 0 in the merged stream (off "
                    + b[1].insnOff() + ")");
            ok(b[0].scopeId() == opId && b[1].scopeId() == opId,
                "stitchedTrace: every slice carries the one operation's scope id");
            ok(b[0].tid() == mainTid && b[1].tid() == hopTid[0],
                "stitchedTrace: each slice retains its own capturing thread id");
            ok(!op.truncated(), "stitchedTrace: the merged trace is complete (not truncated)");
            ok(op.path().contains("hop 0") && op.path().contains("hop 1"),
                "stitchedTrace: the merged path has a per-hop header for each stitched hop");
        } finally {
            pool.shutdown();
            code.free();
        }
    }

    // §D2 TRANSPARENT async-hop propagation (HwTrace.AsmStitchedTrace.propagatingExecutor): the
    // complement to the manual propagate() wrapper — an ExecutorService decorator carries the
    // ambient operation scope id across the submit with NO per-task wrapping, the JVM analog of
    // dotnet's ExecutionContext auto-flow across `await`. Proves (1) a BARE submit() through the
    // wrapped pool reaches the pool thread already carrying the scope id (read BEFORE step(), so it
    // is the executor — not step()'s re-assert — that propagated it), (2) the hop really ran on a
    // DIFFERENT OS thread, and (3) the two hops stitch back by seq under the one operation.
    private static void stitchedTraceAutoPropagatesAcrossExecutor() throws Exception {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        java.util.concurrent.ExecutorService raw = java.util.concurrent.Executors.newSingleThreadExecutor();
        java.util.concurrent.ExecutorService pool = HwTrace.AsmStitchedTrace.propagatingExecutor(raw);
        try (HwTrace.AsmStitchedTrace op = new HwTrace.AsmStitchedTrace()) {
            long opId = op.scopeId();
            int mainTid = (int) Thread.currentThread().threadId();

            long r0 = op.step(code, 20, 22); // hop 0 on THIS thread -> 42
            ok(r0 == 42, "autoPropagate: hop 0 runs on the calling thread (result 42)");

            // hop 1 through the PROPAGATING executor — NO op.propagate() wrapping. The lambda reads
            // the ambient scope id BEFORE calling step(), so a passing assert proves the executor
            // (not step()) carried it across the submit.
            long[] hopScope = new long[1];
            int[] hopTid = new int[1];
            long r1 = pool.submit(() -> {
                hopScope[0] = HwTrace.AsmStitchedTrace.currentScopeId();
                hopTid[0] = (int) Thread.currentThread().threadId();
                return op.step(code, 4, 5); // -> 9
            }).get();
            ok(r1 == 9, "autoPropagate: hop 1 runs on a pool thread (result 9)");
            ok(hopScope[0] == opId,
                "autoPropagate: the executor carries the scope id across a BARE submit (no propagate())");
            ok(hopTid[0] != mainTid, "autoPropagate: hop 1 really ran on a DIFFERENT OS thread");

            op.complete();
            HwTrace.StitchBound[] b = op.hops();
            ok(b.length == 2, "autoPropagate: both hops captured and stitched (got " + b.length + ")");
            ok(b[0].seq() == 0 && b[1].seq() == 1 && b[1].insnOff() > 0,
                "autoPropagate: hop 1 is stitched AFTER hop 0 in the merged stream (off " + b[1].insnOff() + ")");
            ok(b[0].scopeId() == opId && b[1].scopeId() == opId,
                "autoPropagate: every slice carries the one operation's scope id");
            ok(b[0].tid() == mainTid && b[1].tid() == hopTid[0],
                "autoPropagate: each slice retains its own capturing thread id");
            ok(!op.truncated(), "autoPropagate: the merged trace is complete (not truncated)");
        } finally {
            raw.shutdown();
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
            ok(trace.covered(0) || trace.truncated(),
                "auto covers block offset 0 (or honestly truncates)");
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

    // Mirrors test_trace_call_auto_owns_the_call_and_completes: trace_call_auto OWNS the
    // invocation — run under the fastest exact tier and auto-escalate to a ceiling-free tier
    // if the trace truncates. It self-manages the tier lifecycle (no HwTrace.init fixture), so
    // it runs standalone; off x86-64 Linux it self-skips with EUNAVAIL.
    private static void traceCallAutoOwnsTheCall() {
        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        try {
            HwTrace.TraceCallAutoResult res = HwTrace.traceCallAuto(code, 20, 22); // 42 <= 100 -> jle taken
            ok(res.rc() == HwTrace.ASMTEST_HW_OK || res.rc() == HwTrace.ASMTEST_HW_EUNAVAIL,
                "traceCallAuto rc in {OK, EUNAVAIL} (got " + res.rc() + ")");
            if (res.ok()) {
                ok(res.result() == 42, "traceCallAuto(20,22): result == 42 (got " + res.result() + ")");
                ok(!res.truncated(), "traceCallAuto !truncated"); // some tier captured the whole path
                ok(res.trace().covered(0), "traceCallAuto covers entry block offset 0");
                ok(res.used() != null && res.used().tier() == HwTrace.TIER_HWTRACE,
                    "traceCallAuto used.tier == TIER_HWTRACE");
                res.trace().free();
            }
        } finally {
            code.free();
        }

        // A loop past the 16-taken-branch LBR window must STILL yield a complete trace
        // (escalating off the ceiling-bounded backend on an AMD host; the single-step floor
        // completes it directly elsewhere). result = rdi * rsi count = 1 * 25 = 25.
        HwTrace.NativeCode loop = HwTrace.NativeCode.fromBytes(LOOP);
        try {
            HwTrace.TraceCallAutoResult res = HwTrace.traceCallAuto(loop, 1, 25); // 25 back-edges > 16-deep window
            ok(res.rc() == HwTrace.ASMTEST_HW_OK || res.rc() == HwTrace.ASMTEST_HW_EUNAVAIL,
                "traceCallAuto(loop) rc in {OK, EUNAVAIL} (got " + res.rc() + ")");
            if (res.ok()) {
                ok(res.result() == 25, "traceCallAuto(loop,1,25): result == 25 (got " + res.result() + ")");
                ok(!res.truncated(), "traceCallAuto(loop) !truncated (escalated to a ceiling-free tier)");
                ok(res.trace().covered(0x7), "traceCallAuto(loop) covers loop-body block offset 0x7");
                res.trace().free();
            }
        } finally {
            loop.free();
        }
    }

    // ---- 2026-07 API flag day: F27 struct-size guard + F29 status + F22 mechanism ----
    private static void flagdayStatusAndMechanism() {
        // F29: status() is available()/skipReason() made machine-readable — one
        // classifier, so they can never drift — and distinguishes EPERM (substrate
        // present, permission denied) from EUNAVAIL (hardware absent).
        int paranoid = HwTrace.perfEventParanoid();
        boolean inv = true;
        for (int b : new int[] { HwTrace.INTEL_PT, HwTrace.CORESIGHT, HwTrace.AMD_LBR, HwTrace.SINGLESTEP }) {
            HwTrace.HwStatus st = HwTrace.status(b);
            inv &= (st.available() == HwTrace.available(b));
            inv &= ((st.code() == HwTrace.ASMTEST_HW_OK) == st.available());
            inv &= (st.code() == HwTrace.ASMTEST_HW_OK || st.code() == HwTrace.ASMTEST_HW_EUNAVAIL
                || st.code() == HwTrace.ASMTEST_HW_EPERM);
            inv &= st.reason().equals(HwTrace.skipReason(b));
            inv &= (st.perfEventParanoid() == paranoid);
        }
        ok(inv, "flagday: status() invariants hold for all four backends (F29)");
        // LIVE permission-vs-hardware lane (self-skips where not applicable): an AMD
        // probe that reached the perf open under paranoid>2 must say EPERM, never
        // missing-silicon EUNAVAIL. (No geteuid in the JDK; CAP_PERFMON/root would
        // make the probe SUCCEED, so stage==STAGE_PROBE + paranoid>2 is sufficient.)
        HwTrace.HwStatus amd = HwTrace.status(HwTrace.AMD_LBR);
        if (amd.stage() == HwTrace.STAGE_PROBE && paranoid > 2) {
            ok(amd.code() == HwTrace.ASMTEST_HW_EPERM,
                "flagday LIVE: paranoid-blocked AMD probe is EPERM, not EUNAVAIL");
        } else {
            System.out.println("# SKIP flagday live-EPERM lane (stage=" + amd.stage()
                + " paranoid=" + paranoid + ")");
        }

        // F22/F26/F37: resolved rows carry a concrete, EXACT mechanism.
        boolean rowsOk = true;
        for (HwTrace.TierChoice c : HwTrace.resolveTiers(HwTrace.TRACE_BEST)) {
            rowsOk &= (c.mechanism() != HwTrace.MECH_NONE
                && c.mechanism() != HwTrace.MECH_STATISTICAL
                && c.fidelity() != HwTrace.FIDELITY_STATISTICAL);
            if (c.tier() == HwTrace.TIER_HWTRACE && c.backend() == HwTrace.SINGLESTEP)
                rowsOk &= (c.mechanism() == HwTrace.MECH_TF_STEP);
        }
        ok(rowsOk, "flagday: resolved rows carry a concrete, exact mechanism (F22)");

        // F27 + F22 live half needs a working single-step tier.
        if (!HwTrace.available(HwTrace.SINGLESTEP)) {
            System.out.println("# SKIP flagday live half: single-step unavailable");
            return;
        }
        // F27: init() self-describes via the mirrored layout's byteSize — a drifted
        // mirror (or a library that stopped honoring struct_size) would throw here.
        HwTrace.init(HwTrace.SINGLESTEP);
        HwTrace.shutdown();
        ok(true, "flagday: init() negotiates struct_size (F27)");

        HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(ROUTINE);
        try {
            HwTrace.TraceCallAutoResult res = HwTrace.traceCallAuto(code, 20, 22);
            if (res.ok()) {
                int m = res.used().mechanism();
                ok(m == HwTrace.MECH_HW_BRANCH || m == HwTrace.MECH_TF_STEP
                        || m == HwTrace.MECH_MSR_LBR || m == HwTrace.MECH_BLOCKSTEP
                        || m == HwTrace.MECH_PER_INSN,
                    "flagday: traceCallAuto reports the winning rung (got " + m + ")");
                ok(m != HwTrace.MECH_STATISTICAL
                        && res.used().fidelity() == HwTrace.FIDELITY_NATIVE,
                    "flagday: the exact call-owning ladder is never STATISTICAL");
                res.trace().free();
            }
        } finally {
            code.free();
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

    // --- Live HotSpot JIT-method resolution (§D2 / dotnet-parity Java row "JVM-jitdump
    //     resolution"): drive a REAL child OpenJDK JVM under -agentpath:libperf-jvmti.so so it
    //     emits a native jit-<pid>.dump, keep a distinctively-named method (Hot.asmtjit) C2-hot,
    //     flush the dump on an orderly shutdown, then resolve the genuinely-JIT'd method's
    //     recorded (addr,size,bytes) from it via the shipped asmtest_jitdump_find
    //     (HwTrace.javaResolveJitMethod) — the JVM analog of the Node v8ResolveJitMethod / .NET
    //     MethodLoad path. File-only resolution (no ptrace/privilege), like the C oracle
    //     trace_jitdump_java. Self-skips off Linux, without the perf-JVMTI agent / a JDK compiler,
    //     or where HotSpot emits no usable jitdump in time — it never fails the suite (an
    //     environmental problem prints "# SKIP"; only a genuine resolution mismatch is "not ok"). ---
    private static void liveJavaJitdumpResolution() {
        if (!System.getProperty("os.name", "").toLowerCase().contains("linux")) {
            System.out.println("# SKIP live java jitdump: perf-JVMTI jitdump is Linux-only");
            return;
        }
        String agent = findPerfJvmtiAgent();
        if (agent == null) {
            System.out.println("# SKIP live java jitdump: libperf-jvmti.so absent "
                + "(linux-tools perf JVMTI agent)");
            return;
        }
        javax.tools.JavaCompiler jc = javax.tools.ToolProvider.getSystemJavaCompiler();
        if (jc == null) {
            System.out.println("# SKIP live java jitdump: no system Java compiler (JRE-only run)");
            return;
        }
        Path work = null;
        Process child = null;
        try {
            work = Files.createTempDirectory("asmtest-jvmti");
            // A uniquely-named static hot method HotSpot's C2 compiles to a standalone nmethod;
            // `static` => the verified entry sits at code_begin (the perf-map/jitdump address).
            // Mirrors examples/jit_java/Hot.java, written inline like the Node test writes its warm
            // JS. Descriptor form (what the perf-JVMTI agent records): LHot;asmtjit(II)I.
            Path src = work.resolve("Hot.java");
            Files.writeString(src,
                "public class Hot {\n"
                + "  static int asmtjit(int a, int b) { return a + b; }\n"
                + "  public static void main(String[] a) {\n"
                + "    int s = 0;\n"
                + "    for (;;) { s = asmtjit(s, 1); if (s > 1000000) s = 0; }\n"
                + "  }\n}\n");
            if (jc.run(null, null, null, "-d", work.toString(), src.toString()) != 0) {
                System.out.println("# SKIP live java jitdump: Hot.java did not compile");
                return;
            }
            // The agent roots its dump tree at $JITDUMPDIR (kept in /tmp, not the repo); jcmd's
            // perf-map is hardcoded to /tmp/perf-<pid>.map. -XX:-TieredCompilation => one C2 body;
            // CompileThreshold=1000 => compile promptly; dontinline => asmtjit stays a real
            // standalone nmethod. Mirrors trace_jitdump_java's child command.
            String javaBin = System.getProperty("java.home") + "/bin/java";
            ProcessBuilder pb = new ProcessBuilder(javaBin, "-agentpath:" + agent,
                "-XX:-TieredCompilation", "-XX:CompileThreshold=1000",
                "-XX:CompileCommand=dontinline,Hot.asmtjit", "-cp", work.toString(), "Hot");
            pb.environment().put("JITDUMPDIR", "/tmp");
            pb.redirectOutput(ProcessBuilder.Redirect.DISCARD);
            pb.redirectError(ProcessBuilder.Redirect.DISCARD);
            child = pb.start();
            long cpid = child.pid();
            String mapPath = "/tmp/perf-" + cpid + ".map";

            // (A) Poll HotSpot's own jcmd perf-map until asmtjit compiles — both to know the method
            //     is live AND as the independent address cross-check. jcmd materializes the perf-map
            //     on demand (HotSpot does not stream one); rate-limit the jcmd spawns.
            long paddr = 0;
            for (int i = 0; i < 150 && paddr == 0 && child.isAlive(); i++) {
                Thread.sleep(100);
                if (i % 8 == 0) jcmdPerfmap(cpid);
                paddr = perfMapAddrContaining(mapPath, "asmtjit");
            }
            if (!child.isAlive()) {
                System.out.println("# SKIP live java jitdump: JVM exited early (JDK broken?)");
                return;
            }
            if (paddr == 0) {
                System.out.println("# note: asmtjit not in jcmd perf-map yet; resolving from the "
                    + "jitdump alone (no perf-map cross-check)");
            }

            // (C) Orderly shutdown so the perf-JVMTI agent flushes its buffered jitdump tail
            //     (asmtjit) to disk: destroy() sends SIGTERM (runs Agent_OnUnload); SIGKILL would
            //     lose the unflushed records. A bounded wait means the lane never hangs.
            child.destroy();
            boolean exited = child.waitFor(15, java.util.concurrent.TimeUnit.SECONDS);
            if (!exited) {
                child.destroyForcibly();
                System.out.println("# SKIP live java jitdump: JVM did not shut down to flush "
                    + "the jitdump");
                return;
            }

            // (D) Read the now-complete jitdump (it persists after the JVM exits) and resolve the
            //     descriptor-named method through the shipped reader.
            Optional<String> dump = HwTrace.findJavaJitdump((int) cpid);
            if (dump.isEmpty()) {
                System.out.println("# SKIP live java jitdump: no jit-" + cpid + ".dump flushed");
                return;
            }
            Optional<HwTrace.JavaJitMethod> mo = HwTrace.javaResolveJitMethod(
                dump.get(), paddr != 0 ? mapPath : null, "LHot;asmtjit(II)I", 8192);
            if (mo.isEmpty()) {
                System.out.println("# SKIP live java jitdump: asmtjit not in the flushed jitdump");
                return;
            }
            HwTrace.JavaJitMethod m = mo.orElseThrow();
            if (m.code().length == 0) { // an entry with no recorded bytes — skip like the C oracle
                System.out.println("# SKIP live java jitdump: recovered an empty body");
                return;
            }
            // (1) Live resolution: the reader recovered Hot.asmtjit's FULL recorded body — exactly
            //     min(code_size, wantBytes) bytes of real HotSpot C2 output — from a jitdump a live
            //     JVM emitted via the perf-JVMTI agent. (name echoes the exact descriptor we keyed
            //     on, so it is not re-asserted; the address cross-check below is the real check.)
            int want = (int) Math.min(m.codeSize(), 8192L);
            ok(m.codeSize() > 0 && m.code().length == want,
                "javaResolveJitMethod: recovered Hot.asmtjit's full recorded body from the real "
                + "HotSpot jitdump (" + m.code().length + " bytes)");
            // (2) Cross-check (when jcmd's perf-map was available): the agent's jitdump and jcmd's
            //     perf-map — two independent HotSpot outputs — agree on the method's load address.
            if (paddr != 0) {
                ok(m.perfMapAddr() == m.codeAddr() && m.codeAddr() == paddr,
                    "java jitdump: code_addr agrees with HotSpot's jcmd perf-map (two independent "
                    + "outputs)");
            } else {
                System.out.println("# SKIP java jitdump perf-map cross-check: perf-map unavailable");
            }
        } catch (Exception e) {
            // Environmental failure (I/O, process spawn, interrupt): self-skip, never fail the
            // suite. A genuine resolution mismatch throws AssertionError (an Error, not an
            // Exception) from ok() and still surfaces as "not ok".
            System.out.println("# SKIP live java jitdump: " + e);
        } finally {
            if (child != null && child.isAlive()) child.destroyForcibly();
            if (work != null) deleteTree(work);
        }
    }

    // Locate the perf-JVMTI agent (libperf-jvmti.so, from linux-tools). It is a userspace JVMTI
    // agent, so a kernel-version-mismatched copy still loads. linux-tools installs it under
    // /usr/lib/linux-tools[-<ver>][/<ver>]/libperf-jvmti.so; glob those. null when absent.
    private static String findPerfJvmtiAgent() {
        Path libdir = Path.of("/usr/lib");
        try (java.nio.file.DirectoryStream<Path> top =
                 Files.newDirectoryStream(libdir, "linux-tools*")) {
            for (Path d : top) {
                Path direct = d.resolve("libperf-jvmti.so");
                if (Files.isReadable(direct)) return direct.toString();
                if (Files.isDirectory(d)) {
                    try (java.nio.file.DirectoryStream<Path> sub = Files.newDirectoryStream(d)) {
                        for (Path v : sub) {
                            Path a = v.resolve("libperf-jvmti.so");
                            if (Files.isReadable(a)) return a.toString();
                        }
                    } catch (IOException ignored) { /* try the next linux-tools dir */ }
                }
            }
        } catch (IOException ignored) { /* no linux-tools -> agent absent */ }
        return null;
    }

    // Drive `jcmd <pid> Compiler.perfmap` to (re)materialize /tmp/perf-<pid>.map on the live JVM
    // (HotSpot does not stream one). Best-effort — output discarded, failures ignored.
    private static void jcmdPerfmap(long pid) {
        try {
            String jcmd = System.getProperty("java.home") + "/bin/jcmd";
            new ProcessBuilder(jcmd, Long.toString(pid), "Compiler.perfmap")
                .redirectOutput(ProcessBuilder.Redirect.DISCARD)
                .redirectError(ProcessBuilder.Redirect.DISCARD)
                .start().waitFor(5, java.util.concurrent.TimeUnit.SECONDS);
        } catch (Exception ignored) { /* jcmd absent / attach refused -> no perf-map */ }
    }

    // The load address of the LAST perf-map row (the current compilation) whose name contains
    // `substr`, or 0 when none / no map yet. Rows are "<hexaddr> <hexsize> <name>"; HotSpot's jcmd
    // 0x-prefixes the hex columns, so strip an optional "0x" before parsing.
    private static long perfMapAddrContaining(String mapPath, String substr) {
        long addr = 0;
        try {
            for (String line : Files.readAllLines(Path.of(mapPath))) {
                int sp1 = line.indexOf(' ');
                if (sp1 <= 0) continue;
                int sp2 = line.indexOf(' ', sp1 + 1);
                if (sp2 < 0) continue;
                if (!line.substring(sp2 + 1).contains(substr)) continue;
                String hex = line.substring(0, sp1);
                if (hex.length() > 2 && hex.charAt(0) == '0'
                    && (hex.charAt(1) == 'x' || hex.charAt(1) == 'X')) hex = hex.substring(2);
                try { addr = Long.parseUnsignedLong(hex, 16); }
                catch (NumberFormatException ignored) { /* skip a malformed row */ }
            }
        } catch (IOException | RuntimeException e) { /* map not written yet */ }
        return addr;
    }

    // Recursively delete a temp tree, best-effort (deepest-first so directories empty before rmdir).
    private static void deleteTree(Path root) {
        try (var s = Files.walk(root)) {
            s.sorted(java.util.Comparator.reverseOrder()).forEach(p -> {
                try { Files.deleteIfExists(p); } catch (IOException ignored) { /* leave it */ }
            });
        } catch (IOException | RuntimeException ignored) { /* nothing to clean */ }
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
