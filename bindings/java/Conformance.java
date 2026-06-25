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

        // --- Tier 1: in-line assembly (Keystone) replays add_signed ------- //
        // Only when the loaded lib carries the assembler (libasmtest_emu_asm).
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

        System.out.printf("# %d passed, %d failed, %d total%n", total - fails, fails, total);
        System.exit(fails == 0 ? 0 : 1);
    }
}
