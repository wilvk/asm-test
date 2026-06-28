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
 * FFM is a preview API in JDK 21: compile with `--release 21 --enable-preview`,
 * run with `--enable-preview --enable-native-access=ALL-UNNAMED`.
 */
import java.lang.foreign.MemorySegment;

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

        System.out.printf("# %d passed, %d failed, %d total%n", total - fails, fails, total);
        System.exit(fails == 0 ? 0 : 1);
    }
}
