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
        }

        // --- Tier 1: corpus replay (emulator, x86-64 guest) --------------- //
        try (Asmtest.Emu e = new Asmtest.Emu();
             Asmtest.EmuResult res = e.call2(routine("add_signed"), 40L, 2L)) {
            check("emu.add_signed", !res.faulted() && res.reg("rax") == 42);
        }

        // --- Tier 1: in-line assembly (Keystone) replays add_signed ------- //
        try (Asmtest.Emu e = new Asmtest.Emu();
             Asmtest.EmuResult res = e.callAsm("mov rax, rdi; add rax, rsi; ret", 40L, 2L)) {
            check("asm.add_signed", !res.faulted() && res.reg("rax") == 42);
        }

        // --- Tier 2: idiomatic assertions pass on good input -------------- //
        boolean t2pass = true;
        try (Asmtest.Regs r = new Asmtest.Regs()) {
            r.capture6(routine("add_signed"), 40L, 2L);
            Asmtest.assertRet(r, 42);
            Asmtest.assertAbiPreserved(r);
            r.captureFp2(routine("fp_add"), 1.5, 2.25);
            Asmtest.assertFp(r, 3.75);
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
