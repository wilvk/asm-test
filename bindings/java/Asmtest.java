/*
 * Asmtest.java — asm-test Java binding (Track J): the reusable library module.
 *
 * This is the class a Java project uses; it keeps all Foreign Function & Memory
 * (Project Panama, java.lang.foreign) plumbing inside, so calling code never
 * binds a downcall handle itself. It drives the opaque-handle FFI layer
 * (src/ffi.c), so no C struct layout is mirrored: Regs with capture6 /
 * captureFp2 + accessors, Emu / EmuResult for the emulator (faults as data),
 * and assert* helpers that throw AsmtestException.
 *
 * The shared libraries are taken from the environment, matching how the
 * framework's Makefile invokes the bindings:
 *   ASMTEST_LIB         libasmtest_emu.{so,dylib}    (capture + emulator + accessors)
 *   ASMTEST_CORPUS_LIB  libasmtest_corpus.{so,dylib} (the canonical fixtures)
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

public final class Asmtest {
    private Asmtest() {}

    /** Thrown by the assert* helpers on a failed check. */
    public static final class AsmtestException extends RuntimeException {
        public AsmtestException(String message) { super(message); }
    }

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Arena ARENA = Arena.ofShared();
    private static final SymbolLookup CORPUS;

    private static final MethodHandle CORPUS_ROUTINE, REGS_NEW, REGS_FREE, CAPTURE6,
        CAPTURE_FP2, REGS_RET, REGS_FRET, REGS_FLAG_SET, CHECK_ABI, EMU_OPEN, EMU_CLOSE,
        EMU_RES_NEW, EMU_RES_FREE, EMU_CALL2, EMU_CALL_ASM, EMU_FAULTED, EMU_REG;

    static {
        String emuPath = System.getenv("ASMTEST_LIB");
        if (emuPath == null) {
            throw new IllegalStateException("set ASMTEST_LIB to libasmtest_emu.{so,dylib}");
        }
        SymbolLookup emu = SymbolLookup.libraryLookup(emuPath, ARENA);
        String corpusPath = System.getenv("ASMTEST_CORPUS_LIB");
        CORPUS = corpusPath != null ? SymbolLookup.libraryLookup(corpusPath, ARENA) : null;

        CORPUS_ROUTINE = CORPUS == null ? null
            : h(CORPUS, "asmtest_corpus_routine", FunctionDescriptor.of(ADDRESS, ADDRESS));
        REGS_NEW = h(emu, "asmtest_regs_new", FunctionDescriptor.of(ADDRESS));
        REGS_FREE = h(emu, "asmtest_regs_free", FunctionDescriptor.ofVoid(ADDRESS));
        CAPTURE6 = h(emu, "asmtest_capture6", FunctionDescriptor.ofVoid(
            ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_LONG));
        CAPTURE_FP2 = h(emu, "asmtest_capture_fp2",
            FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, JAVA_DOUBLE, JAVA_DOUBLE));
        REGS_RET = h(emu, "asmtest_regs_ret", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        REGS_FRET = h(emu, "asmtest_regs_fret", FunctionDescriptor.of(JAVA_DOUBLE, ADDRESS));
        REGS_FLAG_SET = h(emu, "asmtest_regs_flag_set",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
        CHECK_ABI = h(emu, "asmtest_check_abi",
            FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
        EMU_OPEN = h(emu, "emu_open", FunctionDescriptor.of(ADDRESS));
        EMU_CLOSE = h(emu, "emu_close", FunctionDescriptor.ofVoid(ADDRESS));
        EMU_RES_NEW = h(emu, "asmtest_emu_result_new", FunctionDescriptor.of(ADDRESS));
        EMU_RES_FREE = h(emu, "asmtest_emu_result_free", FunctionDescriptor.ofVoid(ADDRESS));
        EMU_CALL2 = h(emu, "asmtest_emu_call2", FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS));
        EMU_CALL_ASM = h(emu, "asmtest_emu_call_asm", FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS));
        EMU_FAULTED = h(emu, "asmtest_emu_result_faulted", FunctionDescriptor.of(JAVA_INT, ADDRESS));
        EMU_REG = h(emu, "asmtest_emu_x86_reg", FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS));
    }

    private static MethodHandle h(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(lk.find(name).orElseThrow(
            () -> new RuntimeException("missing symbol: " + name)), fd);
    }

    private static RuntimeException rethrow(Throwable t) {
        return t instanceof RuntimeException re ? re : new RuntimeException(t);
    }

    private static MemorySegment str(String s) { return ARENA.allocateUtf8String(s); }

    /** Resolve a canonical corpus routine (e.g. "add_signed") to its address. */
    public static MemorySegment corpusRoutine(String name) {
        if (CORPUS_ROUTINE == null) {
            throw new IllegalStateException("set ASMTEST_CORPUS_LIB to use corpusRoutine");
        }
        try {
            return (MemorySegment) CORPUS_ROUTINE.invoke(str(name));
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** A captured register/flags snapshot. Use try-with-resources to free it. */
    public static final class Regs implements AutoCloseable {
        private MemorySegment h;
        public Regs() {
            try { h = (MemorySegment) REGS_NEW.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Call fn through the real ABI with six integer args. */
        public void capture6(MemorySegment fn, long a0, long a1, long a2, long a3, long a4, long a5) {
            try { CAPTURE6.invoke(h, fn, a0, a1, a2, a3, a4, a5); } catch (Throwable t) { throw rethrow(t); }
        }
        public void capture6(MemorySegment fn, long a0, long a1) { capture6(fn, a0, a1, 0, 0, 0, 0); }
        public void capture6(MemorySegment fn) { capture6(fn, 0, 0, 0, 0, 0, 0); }
        /** Call fn with two double args, capturing the FP return. */
        public void captureFp2(MemorySegment fn, double f0, double f1) {
            try { CAPTURE_FP2.invoke(h, fn, f0, f1); } catch (Throwable t) { throw rethrow(t); }
        }
        public long ret() {
            try { return (long) REGS_RET.invoke(h); } catch (Throwable t) { throw rethrow(t); }
        }
        public double fret() {
            try { return (double) REGS_FRET.invoke(h); } catch (Throwable t) { throw rethrow(t); }
        }
        public boolean flagSet(String name) {
            try { return (int) REGS_FLAG_SET.invoke(h, str(name)) == 1; } catch (Throwable t) { throw rethrow(t); }
        }
        public boolean abiPreserved() {
            try { return (int) CHECK_ABI.invoke(h, MemorySegment.NULL, 0L) == 0; } catch (Throwable t) { throw rethrow(t); }
        }
        @Override public void close() {
            try { if (h != null) REGS_FREE.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }

    /** An emulator run's outcome — faults surfaced as data, not a crash. */
    public static final class EmuResult implements AutoCloseable {
        private MemorySegment h;
        EmuResult() {
            try { h = (MemorySegment) EMU_RES_NEW.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        public boolean faulted() {
            try { return (int) EMU_FAULTED.invoke(h) != 0; } catch (Throwable t) { throw rethrow(t); }
        }
        /** Read an x86-64 guest register by name (e.g. "rax"). */
        public long reg(String name) {
            try { return (long) EMU_REG.invoke(h, str(name)); } catch (Throwable t) { throw rethrow(t); }
        }
        @Override public void close() {
            try { if (h != null) EMU_RES_FREE.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }

    /** An open emulator (x86-64 Unicorn guest). Use try-with-resources to close. */
    public static final class Emu implements AutoCloseable {
        private MemorySegment h;
        public Emu() {
            try { h = (MemorySegment) EMU_OPEN.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Run fn in the emulator with two integer args; returns an EmuResult. */
        public EmuResult call2(MemorySegment fn, long a0, long a1) {
            EmuResult res = new EmuResult();
            try { EMU_CALL2.invoke(h, fn, a0, a1, res.h); } catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /**
         * Assemble x86-64 {@code src} (Intel syntax) via Keystone and run it with
         * two integer args; returns an EmuResult. Throws AsmtestException if the
         * string failed to assemble. Needs the Keystone-backed native lib.
         */
        public EmuResult callAsm(String src, long a0, long a1) {
            EmuResult res = new EmuResult();
            int ok;
            try { ok = (int) EMU_CALL_ASM.invoke(h, str(src), a0, a1, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            if (ok == 0) { res.close(); throw new AsmtestException("in-line assembly failed: " + src); }
            return res;
        }
        @Override public void close() {
            try { if (h != null) EMU_CLOSE.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }

    // ---- Tier-2 idiomatic assertions: throw AsmtestException on failure ---- //
    public static void assertRet(Regs r, long want) {
        long got = r.ret();
        if (got != want) throw new AsmtestException("ret: got " + got + ", want " + want);
    }
    public static void assertAbiPreserved(Regs r) {
        if (!r.abiPreserved()) throw new AsmtestException("ABI not preserved");
    }
    public static void assertFlag(Regs r, String name, boolean set) {
        if (r.flagSet(name) != set) throw new AsmtestException("flag " + name + ": want " + set);
    }
    public static void assertFp(Regs r, double want) {
        double got = r.fret();
        if (got != want) throw new AsmtestException("fp: got " + got + ", want " + want);
    }
    public static void assertNoFault(EmuResult res) {
        if (res.faulted()) throw new AsmtestException("unexpected fault");
    }
    public static void assertEmuReg(EmuResult res, String name, long want) {
        long got = res.reg(name);
        if (got != want) throw new AsmtestException("emu " + name + ": got " + got + ", want " + want);
    }
}
