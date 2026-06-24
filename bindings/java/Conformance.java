/*
 * Conformance.java — asm-test Java binding (Track J), via the Foreign Function
 * & Memory API (Project Panama, java.lang.foreign).
 *
 * Replays the conformance corpus through the opaque-handle FFI layer (no struct
 * layout needed): downcall handles to asmtest_corpus_routine for addresses,
 * asmtest_capture6 / _fp2 + accessors for capture, and asmtest_emu_call2 +
 * accessors for the emulator. Exits nonzero on any mismatch.
 *
 *   ASMTEST_LIB         libasmtest_emu.{so,dylib}
 *   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib}
 *
 * FFM is a preview API in JDK 21: compile with `--release 21 --enable-preview`,
 * run with `--enable-preview --enable-native-access=ALL-UNNAMED`.
 */
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public class Conformance {
    static final Linker LINKER = Linker.nativeLinker();
    static final Arena ARENA = Arena.ofConfined();
    static SymbolLookup emu, corpus;

    static MethodHandle h(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(lk.find(name).orElseThrow(
            () -> new RuntimeException("missing symbol: " + name)), fd);
    }

    static int fails = 0, total = 0;
    static void check(String name, boolean ok) {
        total++;
        if (ok) {
            System.out.println("ok - " + name);
        } else {
            fails++;
            System.out.println("not ok - " + name);
        }
    }

    public static void main(String[] argv) throws Throwable {
        String emuPath = System.getenv("ASMTEST_LIB");
        String corpusPath = System.getenv("ASMTEST_CORPUS_LIB");
        if (emuPath == null || corpusPath == null) {
            System.err.println("set ASMTEST_LIB and ASMTEST_CORPUS_LIB");
            System.exit(2);
        }
        emu = SymbolLookup.libraryLookup(emuPath, ARENA);
        corpus = SymbolLookup.libraryLookup(corpusPath, ARENA);

        MethodHandle corpusRoutine = h(corpus, "asmtest_corpus_routine",
            FunctionDescriptor.of(ADDRESS, ADDRESS));
        MethodHandle regsNew = h(emu, "asmtest_regs_new", FunctionDescriptor.of(ADDRESS));
        MethodHandle regsFree = h(emu, "asmtest_regs_free", FunctionDescriptor.ofVoid(ADDRESS));
        MethodHandle capture6 = h(emu, "asmtest_capture6", FunctionDescriptor.ofVoid(
            ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG));
        MethodHandle captureFp2 = h(emu, "asmtest_capture_fp2",
            FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, JAVA_DOUBLE, JAVA_DOUBLE));
        MethodHandle regsRet = h(emu, "asmtest_regs_ret", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        MethodHandle regsFret = h(emu, "asmtest_regs_fret", FunctionDescriptor.of(JAVA_DOUBLE, ADDRESS));
        MethodHandle regsFlagSet = h(emu, "asmtest_regs_flag_set",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
        MethodHandle checkAbi = h(emu, "asmtest_check_abi",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
        MethodHandle emuOpen = h(emu, "emu_open", FunctionDescriptor.of(ADDRESS));
        MethodHandle emuClose = h(emu, "emu_close", FunctionDescriptor.ofVoid(ADDRESS));
        MethodHandle emuResNew = h(emu, "asmtest_emu_result_new", FunctionDescriptor.of(ADDRESS));
        MethodHandle emuResFree = h(emu, "asmtest_emu_result_free", FunctionDescriptor.ofVoid(ADDRESS));
        MethodHandle emuCall2 = h(emu, "asmtest_emu_call2", FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS));
        MethodHandle emuFaulted = h(emu, "asmtest_emu_result_faulted",
            FunctionDescriptor.of(JAVA_INT, ADDRESS));
        MethodHandle emuReg = h(emu, "asmtest_emu_x86_reg",
            FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS));

        java.util.function.Function<String, MemorySegment> routine = name -> {
            try {
                return (MemorySegment) corpusRoutine.invoke(ARENA.allocateUtf8String(name));
            } catch (Throwable t) {
                throw new RuntimeException(t);
            }
        };
        MemorySegment NULL = MemorySegment.NULL;

        // add_signed.basic
        MemorySegment r = (MemorySegment) regsNew.invoke();
        capture6.invoke(r, routine.apply("add_signed"), 40L, 2L, 0L, 0L, 0L, 0L);
        check("add_signed.basic",
            (long) regsRet.invoke(r) == 42 && (int) checkAbi.invoke(r, NULL, 0L) == 0);

        capture6.invoke(r, routine.apply("sum_via_rbx"), 20L, 22L, 0L, 0L, 0L, 0L);
        check("sum_via_rbx.abi_preserved",
            (long) regsRet.invoke(r) == 42 && (int) checkAbi.invoke(r, NULL, 0L) == 0);

        capture6.invoke(r, routine.apply("clobbers_rbx"), 1L, 2L, 0L, 0L, 0L, 0L);
        check("clobbers_rbx.abi_violation_detected", (int) checkAbi.invoke(r, NULL, 0L) != 0);

        capture6.invoke(r, routine.apply("set_carry"), 0L, 0L, 0L, 0L, 0L, 0L);
        check("set_carry.cf_set",
            (int) regsFlagSet.invoke(r, ARENA.allocateUtf8String("CF")) == 1);

        capture6.invoke(r, routine.apply("clear_carry"), 0L, 0L, 0L, 0L, 0L, 0L);
        check("clear_carry.cf_clear",
            (int) regsFlagSet.invoke(r, ARENA.allocateUtf8String("CF")) == 0);

        captureFp2.invoke(r, routine.apply("fp_add"), 1.5, 2.25);
        check("fp_add.basic", (double) regsFret.invoke(r) == 3.75);
        regsFree.invoke(r);

        // Emulator: faults as data via the opaque handle.
        MemorySegment e = (MemorySegment) emuOpen.invoke();
        MemorySegment res = (MemorySegment) emuResNew.invoke();
        emuCall2.invoke(e, routine.apply("add_signed"), 40L, 2L, res);
        check("emu.add_signed",
            (int) emuFaulted.invoke(res) == 0
            && (long) emuReg.invoke(res, ARENA.allocateUtf8String("rax")) == 42);
        emuResFree.invoke(res);
        emuClose.invoke(e);

        // --- Tier-2 idiomatic assertions: AssertionError with a message --- //
        MemorySegment r2 = (MemorySegment) regsNew.invoke();
        boolean t2pass = true;
        try {
            capture6.invoke(r2, routine.apply("add_signed"), 40L, 2L, 0L, 0L, 0L, 0L);
            if ((long) regsRet.invoke(r2) != 42)
                throw new AssertionError("ret: got " + (long) regsRet.invoke(r2) + ", want 42");
            if ((int) checkAbi.invoke(r2, NULL, 0L) != 0)
                throw new AssertionError("ABI not preserved");
            captureFp2.invoke(r2, routine.apply("fp_add"), 1.5, 2.25);
            if ((double) regsFret.invoke(r2) != 3.75)
                throw new AssertionError("fp return");
        } catch (AssertionError ae) {
            t2pass = false;
        }
        check("tier2.assertions_pass", t2pass);

        boolean t2teeth = false;
        try {
            capture6.invoke(r2, routine.apply("add_signed"), 40L, 2L, 0L, 0L, 0L, 0L);
            if ((long) regsRet.invoke(r2) != 99)
                throw new AssertionError("ret: got " + (long) regsRet.invoke(r2) + ", want 99");
        } catch (AssertionError ae) {
            t2teeth = true;
        }
        check("tier2.assertions_have_teeth", t2teeth);
        regsFree.invoke(r2);

        System.out.printf("# %d passed, %d failed, %d total%n", total - fails, fails, total);
        System.exit(fails == 0 ? 0 : 1);
    }
}
