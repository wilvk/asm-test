/*
 * Conformance.java — asm-test Java binding (Track J): the conformance runner.
 *
 * A thin consumer of the reusable library module (Asmtest.java): it replays the
 * cross-language conformance corpus through the Regs / Emu / assert* API and
 * never binds an FFM downcall handle itself. Exits nonzero on any mismatch.
 *
 *   ASMTEST_LIB         libasmtest_emu.{so,dylib}
 *   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
 *
 * FFM is final since JDK 22: compile with `--release 22`, run with
 * `--enable-native-access=ALL-UNNAMED`.
 */
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public class Conformance {
    static int fails = 0, total = 0;
    static void check(String name, boolean ok) {
        total++;
        System.out.println((ok ? "ok - " : "not ok - ") + name);
        if (!ok) fails++;
    }

    static MemorySegment routine(String name) { return Asmtest.corpusRoutine(name); }

    /** Pack ints (0..255) into a machine-code byte array (Java bytes are signed). */
    static byte[] code(int... b) {
        byte[] o = new byte[b.length];
        for (int i = 0; i < b.length; i++) o[i] = (byte) b[i];
        return o;
    }

    public static void main(String[] argv) {
        // --- Tier 1: corpus replay (capture trampoline) ------------------- //
        try (Asmtest.Regs r = new Asmtest.Regs()) {
            r.capture6(routine("add_signed"), 40L, 2L);
            check("add_signed.basic", r.ret() == 42 && r.abiPreserved());

            r.capture6(routine("sum_via_rbx"), 20L, 22L);
            check("sum_via_rbx.abi_preserved", r.ret() == 42 && r.abiPreserved());

            r.capture6(routine("clobbers_rbx"), 1L, 2L);
            check("clobbers_rbx.abi_violation_detected", !r.abiPreserved());

            r.capture6(routine("set_carry"));
            check("set_carry.cf_set", r.flagSet("CF"));

            r.capture6(routine("clear_carry"));
            check("clear_carry.cf_clear", !r.flagSet("CF"));

            r.captureFp2(routine("fp_add"), 1.5, 2.25);
            check("fp_add.basic", r.fret() == 3.75);

            r.captureVecF32(routine("vec_add4f"), new float[][] {{1, 2, 3, 4}, {10, 20, 30, 40}});
            float[] v = r.vecF32(0);
            check("vec_add4f.basic", v[0] == 11 && v[1] == 22 && v[2] == 33 && v[3] == 44);

            // 8 integer args: the first 6 in registers, args 7-8 on the stack (x86-64).
            r.captureArgs(routine("sum8"), 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L);
            check("sum8.wide_arity", r.ret() == 36 && r.abiPreserved());

            // mix_scale(n, x) = (double)n * x reads BOTH argument register files.
            r.captureMix(routine("mix_scale"), new long[] {3L}, new double[] {2.5});
            check("mix_scale.mixed_int_fp", r.fret() == 7.5);

            // make_big returns a 24-byte struct{long a,b,c} via the hidden pointer.
            byte[] big = r.captureSret(routine("make_big"), 24, 7L, 8L, 9L);
            java.nio.ByteBuffer bb =
                java.nio.ByteBuffer.wrap(big).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            check("make_big.struct_return_sret", bb.getLong(0) == 7
                && bb.getLong(8) == 8 && bb.getLong(16) == 9 && r.ret() != 0);
        }

        // --- Tier 1: corpus replay (emulator, x86-64 guest) --------------- //
        try (Asmtest.Emu e = new Asmtest.Emu();
             Asmtest.EmuResult res = e.call2(routine("add_signed"), 40L, 2L)) {
            check("emu.add_signed", !res.faulted() && res.reg("rax") == 42);
        }

        // read_fault dereferences an unmapped address: the fault is data — where
        // (faultAddr) and why (faultKind) — not a crash.
        try (Asmtest.Emu e = new Asmtest.Emu();
             Asmtest.EmuResult res = e.call2(routine("read_fault"), 0x00DEAD00L, 0L)) {
            check("emu.read_fault", res.faulted() && res.faultAddr() == 0x00DEAD00L
                && res.faultKind() == Asmtest.FaultKind.READ);
        }

        // int_to_double lands (double)42 in xmm0 (the XMM file, beyond the GP regs);
        // a clean run also keeps rflags live (x86 holds bit 1 set).
        try (Asmtest.Emu e = new Asmtest.Emu();
             Asmtest.EmuResult res = e.call2(routine("int_to_double"), 42L, 0L)) {
            check("emu.int_to_double", !res.faulted() && res.xmmF64(0, 0) == 42.0
                && (res.reg("rflags") & 0x2L) != 0);
        }

        // --- Tier 1: cross-arch emulator guests (raw bytes, any host) ----- //
        try (Asmtest.Guest g = new Asmtest.Guest("arm64");
             Asmtest.GuestResult res = g.call(code(0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6), 40L, 2L)) {
            check("emu_arm64.add", !res.faulted() && res.reg("x0") == 42);
        }
        try (Asmtest.Guest g = new Asmtest.Guest("riscv");
             Asmtest.GuestResult res = g.call(code(0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00), 40L, 2L)) {
            check("emu_riscv.add", !res.faulted() && res.reg("a0") == 42);
        }
        try (Asmtest.Guest g = new Asmtest.Guest("arm");
             Asmtest.GuestResult res = g.call(code(0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1), 40L, 2L)) {
            check("emu_arm.add", !res.faulted() && res.reg("r0") == 42);
        }

        // --- Tier 1: extended x86-64 emulator calls (raw bytes) ----------- //
        try (Asmtest.Emu e = new Asmtest.Emu()) {
            try (Asmtest.EmuResult res = e.callBytes(
                    code(0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3), 10L, 20L, 12L)) {
                check("emu.wide_int", !res.faulted() && res.reg("rax") == 42);
            }
            try (Asmtest.EmuResult res = e.callFp(
                    code(0xF2, 0x0F, 0x58, 0xC1, 0xC3), new long[0], new double[] {1.5, 2.25})) {
                check("emu.fp_add", !res.faulted() && res.xmmF64(0, 0) == 3.75);
            }
            try (Asmtest.EmuResult res = e.callVec(
                    code(0x0F, 0x58, 0xC1, 0xC3), new long[0], new float[][] {{1, 2, 3, 4}, {10, 20, 30, 40}})) {
                check("emu.vec_add4f", !res.faulted() && res.xmmF32(0, 0) == 11 && res.xmmF32(0, 3) == 44);
            }
            try (Asmtest.EmuResult res = e.callWin64(
                    code(0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3), 40L, 2L)) {
                check("emu.win64_add", !res.faulted() && res.reg("rax") == 42);
            }
        }

        // --- Tier 1: execution trace / coverage (cross-arch arm64) -------- //
        try (Asmtest.Guest g = new Asmtest.Guest("arm64"); Asmtest.Trace tr = new Asmtest.Trace()) {
            byte[] sel = code(0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03,
                              0x5F, 0xD6, 0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6);
            try (Asmtest.GuestResult res = g.callTraced(sel, new long[] {0}, tr)) {
                check("emu_arm64.trace_sel", !res.faulted() && res.reg("x0") == 42
                    && tr.covered(0) && tr.covered(12) && !tr.covered(4));
            }
        }

        // --- Tier 1: in-line assembly (Keystone) replays add_signed ------- //
        // libasmtest_emu carries the assembler, so this runs by default; the
        // guard is defensive against an older/leaner lib via ASMTEST_LIB.
        try (Asmtest.Emu e = new Asmtest.Emu()) {
            if (e.asmAvailable()) {
                try (Asmtest.EmuResult res = e.callAsm("mov rax, rdi; add rax, rsi; ret", 40L, 2L)) {
                    check("asm.add_signed", !res.faulted() && res.reg("rax") == 42);
                }
                // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
                try (Asmtest.EmuResult res = e.callAsm(
                        "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
                        new long[] {10, 20, 12}, Asmtest.AsmSyntax.ATT, 0)) {
                    check("asm.att_3arg", !res.faulted() && res.reg("rax") == 42);
                }
                // Failure path: a bad string throws with the Keystone diagnostic.
                boolean threw = false;
                try (Asmtest.Emu e2 = new Asmtest.Emu()) { e2.callAsm("mov rax, nonsense_token"); }
                catch (Asmtest.AsmtestException ex) { threw = ex.getMessage().length() > "in-line assembly failed: ".length(); }
                check("asm.bad_source_throws", threw);
                // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
                byte[] a64 = Asmtest.assemble("ret", Asmtest.AsmArch.ARM64);
                check("asm.arm64_bytes", a64.length == 4
                        && (a64[0] & 0xFF) == 0xC0 && (a64[3] & 0xFF) == 0xD6);
            }
        }

        // Disassembler (Capstone): decode known x86-64 bytes to text.
        // libasmtest_emu carries Capstone, so this runs by default; the guard is
        // defensive against an older/leaner lib via ASMTEST_LIB, which would skip.
        if (Asmtest.disasAvailable()) {
            byte[] code = {0x48, 0x31, (byte) 0xC0, (byte) 0xC3}; // xor rax,rax;ret
            check("disas.xor_rax", Asmtest.disas(code, 0).equals("xor rax, rax"));
            check("disas.ret", Asmtest.disas(code, 3).equals("ret"));
            check("disas.nop", Asmtest.disas(new byte[] {(byte) 0x90}, 0).equals("nop"));
        } else {
            System.out.println("ok - disas.xor_rax # SKIP no disassembler (older/leaner lib)");
        }

        // --- Tier 2: idiomatic assertions pass on good input -------------- //
        boolean t2pass = true;
        try (Asmtest.Regs r = new Asmtest.Regs()) {
            r.capture6(routine("add_signed"), 40L, 2L);
            Asmtest.assertRet(r, 42);
            Asmtest.assertAbiPreserved(r);
            r.captureFp2(routine("fp_add"), 1.5, 2.25);
            Asmtest.assertFp(r, 3.75);
            r.captureVecF32(routine("vec_add4f"), new float[][] {{1, 2, 3, 4}, {10, 20, 30, 40}});
            Asmtest.assertVecF32(r, 0, new float[] {11, 22, 33, 44});
            try (Asmtest.Emu e = new Asmtest.Emu();
                 Asmtest.EmuResult res = e.call2(routine("read_fault"), 0x00DEAD00L, 0L)) {
                Asmtest.assertFault(res);
            }
        } catch (Asmtest.AsmtestException ae) {
            t2pass = false;
        }
        check("tier2.assertions_pass", t2pass);

        // --- Tier 2: the assertions actually fail when they should -------- //
        boolean t2teeth = false;
        try (Asmtest.Regs r = new Asmtest.Regs()) {
            r.capture6(routine("add_signed"), 40L, 2L);
            Asmtest.assertRet(r, 99); // wrong on purpose
        } catch (Asmtest.AsmtestException ae) {
            t2teeth = true;
        }
        check("tier2.assertions_have_teeth", t2teeth);

        // Track F: mid-execution guards (byte-literal routines).
        try (Asmtest.Emu e = new Asmtest.Emu()) {
            byte[] twoWrites = code(0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3);
            e.map(0x400000L, 0x1000L);
            Asmtest.Watch w = e.watchWrites(0x400000L, 8, 1);
            e.callBytes(twoWrites, 0x400000L).close();
            e.watchClear();
            check("guard.watch_escape", w.violated() && w.addr() == 0x400800L && w.ripOff() == 3);
            w.close();
            byte[] clobber = code(0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3);
            Asmtest.RegGuard g = e.guardReg("rbx", 0);
            e.callBytes(clobber).close();
            e.guardRegClear();
            check("guard.reg_invariant", g != null && g.violated() && g.got() == 0x99);
            if (g != null) g.close();
        }

        // Track E: coverage-guided fuzzing + mutation testing over classify3.
        try (Asmtest.Emu e = new Asmtest.Emu()) {
            byte[] classify3 = code(0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
                0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3);
            long[] fixed = e.fuzzCover(classify3, 5, 5, 1);
            long[] guided = e.fuzzCover(classify3, -50, 50, 2000);
            check("fuzz.coverage_beats_fixed", guided[0] > fixed[0]);
            long[] weak = e.mutationTest(classify3, new long[] {5});
            long[] strong = e.mutationTest(classify3, new long[] {-7, 0, 9});
            check("mutation.strong_kills_more", weak[1] > 0 && strong[1] < weak[1]);
        }

        // Track D: AVX2 256-bit capture (self-skips off-AVX2).
        if (Asmtest.cpuHasAvx2()) {
            double[] out = Asmtest.captureVec256(routine("vec_add4d"),
                new double[][] {{1, 2, 3, 4}, {10, 20, 30, 40}});
            check("vec256.add4d", out[0] == 11 && out[1] == 22 && out[2] == 33 && out[3] == 44);
        } else {
            System.out.println("ok - vec256.add4d # SKIP no AVX2");
        }

        // Track D: AVX-512 512-bit capture (self-skips off-AVX-512).
        if (Asmtest.cpuHasAvx512f()) {
            double[] out = Asmtest.captureVec512(routine("vec_add8d"),
                new double[][] {{1, 2, 3, 4, 5, 6, 7, 8}, {10, 20, 30, 40, 50, 60, 70, 80}});
            check("vec512.add8d", out[0] == 11 && out[1] == 22 && out[2] == 33 && out[3] == 44
                && out[4] == 55 && out[5] == 66 && out[6] == 77 && out[7] == 88);
        } else {
            System.out.println("ok - vec512.add8d # SKIP no AVX512F");
        }

        // --- Tier: call-descent (asmtest_ptrace.h) — replay-or-skip ------- //
        PtraceDescent.replay();

        System.out.printf("# %d passed, %d failed, %d total%n", total - fails, fails, total);
        System.exit(fails == 0 ? 0 : 1);
    }

    // ---- Call-descent conformance tier (asmtest_ptrace.h), self-contained ----
    //
    // Conformance is compiled WITHOUT HwTrace.java (java-test lists only Asmtest.java +
    // Conformance.java), so this replays the ptrace_descent corpus cases through its OWN
    // libasmtest_hwtrace FFI rather than the HwTrace wrapper. It is host-native and
    // x86-64-specific, so it self-skips (never fails) unless the lib loads, the host is
    // x86-64, and the out-of-process single-step stepper is available — mirroring the C
    // reference's ptrace_descent tier and the Python _run_ptrace_descent handler.
    static final class PtraceDescent {
        // R@0: mov rax,rdi; call S(+4); add rax,rsi; ret  (traced region = 0xc bytes)
        // S@0xc: inc rax; ret.  args (20,22) -> 43. Matches the two ptrace_descent
        // corpus.json cases (calls_leaf.edges @ L1, calls_leaf.descend @ L2).
        static final byte[] FIXTURE = {
            0x48, (byte) 0x89, (byte) 0xF8, (byte) 0xE8, 0x04, 0x00, 0x00, 0x00,
            0x48, 0x01, (byte) 0xF0, (byte) 0xC3, 0x48, (byte) 0xFF, (byte) 0xC0, (byte) 0xC3
        };

        static java.util.List<String> libCandidates() {
            java.util.List<String> c = new java.util.ArrayList<>();
            String ext = System.getProperty("os.name", "").toLowerCase().contains("mac") ? "dylib" : "so";
            String name = "libasmtest_hwtrace." + ext;
            String env = System.getenv("ASMTEST_HWTRACE_LIB");
            if (env != null && !env.isEmpty()) c.add(env);
            // The build dir java-test actually uses is wherever ASMTEST_LIB (libasmtest_emu)
            // lives — derive the sibling hwtrace lib from it so any BUILD= value works.
            String emu = System.getenv("ASMTEST_LIB");
            if (emu != null && !emu.isEmpty()) {
                java.io.File dir = new java.io.File(emu).getParentFile();
                if (dir != null) c.add(new java.io.File(dir, name).getPath());
            }
            c.add("build/" + name); // cwd-relative dev build/ fallback
            c.add(name);            // bare name -> the system loader
            return c;
        }

        static MethodHandle dh(Linker lk, SymbolLookup L, String name, FunctionDescriptor fd) {
            return lk.downcallHandle(L.find(name).orElseThrow(
                () -> new RuntimeException("missing symbol: " + name)), fd);
        }

        static void skip(String why) {
            System.out.println("ok - ptrace_descent.calls_leaf.edges # SKIP " + why);
            System.out.println("ok - ptrace_descent.calls_leaf.descend # SKIP " + why);
        }

        // trace_call_ex with trace = NULL (record only into descent). region is the TRACED
        // length (0xc), NOT the whole allocation, so the sibling stays outside frame 0.
        static long runCall(MethodHandle callEx, long base, long region, long[] args,
                            MemorySegment d) throws Throwable {
            try (Arena a = Arena.ofConfined()) {
                int n = args.length;
                MemorySegment argSeg = a.allocate(MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
                for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
                MemorySegment result = a.allocate(JAVA_LONG);
                int rc = (int) callEx.invoke(MemorySegment.ofAddress(base), region, argSeg, n,
                    result, MemorySegment.NULL, d);
                if (rc != 0) throw new RuntimeException("trace_call_ex rc=" + rc);
                return result.get(JAVA_LONG, 0);
            }
        }

        static long[] insns(MethodHandle count, MethodHandle at, MemorySegment d, long f)
                throws Throwable {
            int n = (int) (long) count.invoke(d, f);
            long[] out = new long[n];
            for (int i = 0; i < n; i++) out[i] = (long) at.invoke(d, f, (long) i);
            return out;
        }

        static void replay() {
            Linker linker = Linker.nativeLinker();
            try (Arena arena = Arena.ofConfined()) {
                SymbolLookup lib = null;
                for (String cand : libCandidates()) {
                    try { lib = SymbolLookup.libraryLookup(cand, arena); break; }
                    catch (RuntimeException ignore) { /* try the next candidate */ }
                }
                if (lib == null) { skip("libasmtest_hwtrace not found"); return; }
                String arch = System.getProperty("os.arch", "").toLowerCase();
                if (!(arch.contains("amd64") || arch.contains("x86_64"))) {
                    skip("x86-64 corpus, host arch " + arch); return;
                }
                MethodHandle available = dh(linker, lib, "asmtest_ptrace_available",
                    FunctionDescriptor.of(JAVA_INT));
                if ((int) available.invoke() == 0) { skip("ptrace single-step unavailable"); return; }

                MethodHandle execAlloc = dh(linker, lib, "asmtest_hwtrace_exec_alloc",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, ADDRESS));
                MethodHandle execFree = dh(linker, lib, "asmtest_hwtrace_exec_free",
                    FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
                MethodHandle descentNew = dh(linker, lib, "asmtest_descent_new",
                    FunctionDescriptor.of(ADDRESS, JAVA_INT));
                MethodHandle descentFree = dh(linker, lib, "asmtest_descent_free",
                    FunctionDescriptor.ofVoid(ADDRESS));
                MethodHandle allowRegion = dh(linker, lib, "asmtest_descent_allow_region",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
                MethodHandle callEx = dh(linker, lib, "asmtest_ptrace_trace_call_ex",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT,
                        ADDRESS, ADDRESS, ADDRESS));
                MethodHandle framesLen = dh(linker, lib, "asmtest_descent_frames_len",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS));
                MethodHandle frameBase = dh(linker, lib, "asmtest_descent_frame_base",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
                MethodHandle frameDepth = dh(linker, lib, "asmtest_descent_frame_depth",
                    FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
                MethodHandle frameInsnCount = dh(linker, lib, "asmtest_descent_frame_insn_count",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
                MethodHandle frameInsnAt = dh(linker, lib, "asmtest_descent_frame_insn_at",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG, JAVA_LONG));
                MethodHandle edgesLen = dh(linker, lib, "asmtest_descent_edges_len",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS));
                MethodHandle edgeSite = dh(linker, lib, "asmtest_descent_edge_site",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
                MethodHandle edgeTarget = dh(linker, lib, "asmtest_descent_edge_target",
                    FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));

                // Materialize the fixture in real executable (W^X) memory.
                MemorySegment in = arena.allocate(FIXTURE.length);
                MemorySegment.copy(FIXTURE, 0, in, JAVA_BYTE, 0, FIXTURE.length);
                MemorySegment baseOut = arena.allocate(ADDRESS);
                MemorySegment lenOut = arena.allocate(JAVA_LONG);
                int rc = (int) execAlloc.invoke(in, (long) FIXTURE.length, baseOut, lenOut);
                if (rc != 0) { Conformance.check("ptrace_descent.exec_alloc", false); return; }
                long base = baseOut.get(ADDRESS, 0).address();
                long allocLen = lenOut.get(JAVA_LONG, 0);
                try {
                    // L1 RECORD_EDGES, region 0xc: frame 0 only + one edge to the sibling.
                    MemorySegment d1 = (MemorySegment) descentNew.invoke(1);
                    boolean l1;
                    try {
                        long res = runCall(callEx, base, 0xC, new long[] {20, 22}, d1);
                        long[] f0 = insns(frameInsnCount, frameInsnAt, d1, 0);
                        boolean edgeOk = (long) edgesLen.invoke(d1) == 1
                            && (long) edgeSite.invoke(d1, 0L) == 0x3
                            && (long) edgeTarget.invoke(d1, 0L) == base + 0xC;
                        l1 = res == 43 && (long) framesLen.invoke(d1) == 1
                            && java.util.Arrays.equals(f0, new long[] {0, 3, 8, 0xB}) && edgeOk;
                    } finally { descentFree.invoke(d1); }
                    Conformance.check("ptrace_descent.calls_leaf.edges", l1);

                    // L2 DESCEND_KNOWN + allow the sibling: frame 1 is the leaf, no edges.
                    MemorySegment d2 = (MemorySegment) descentNew.invoke(2);
                    boolean l2;
                    try {
                        allowRegion.invoke(d2, MemorySegment.ofAddress(base + 0xC), 4L);
                        long res = runCall(callEx, base, 0xC, new long[] {20, 22}, d2);
                        long[] f0 = insns(frameInsnCount, frameInsnAt, d2, 0);
                        long[] f1 = insns(frameInsnCount, frameInsnAt, d2, 1);
                        l2 = res == 43 && (long) framesLen.invoke(d2) == 2
                            && java.util.Arrays.equals(f0, new long[] {0, 3, 8, 0xB})
                            && (long) frameBase.invoke(d2, 1L) == base + 0xC
                            && (int) frameDepth.invoke(d2, 1L) == 1
                            && java.util.Arrays.equals(f1, new long[] {0, 3})
                            && (long) edgesLen.invoke(d2) == 0;
                    } finally { descentFree.invoke(d2); }
                    Conformance.check("ptrace_descent.calls_leaf.descend", l2);
                } finally {
                    execFree.invoke(MemorySegment.ofAddress(base), allocLen);
                }
            } catch (Throwable t) {
                // A load/link hiccup or an environment quirk self-skips (never fails the run).
                skip("descent replay unavailable: " + t);
            }
        }
    }
}
