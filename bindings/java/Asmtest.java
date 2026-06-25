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
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_DOUBLE;
import static java.lang.foreign.ValueLayout.JAVA_FLOAT;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public final class Asmtest {
    private Asmtest() {}

    /** Thrown by the assert* helpers on a failed check. */
    public static final class AsmtestException extends RuntimeException {
        public AsmtestException(String message) { super(message); }
    }

    /** Target architecture for {@link Emu#assemble} (mirrors asm_arch_t). */
    public enum AsmArch {
        X86_64(0), ARM64(1), RISCV64(2), ARM32(3);
        final int v; AsmArch(int v) { this.v = v; }
    }

    /** Input assembly syntax (x86 only); mirrors asm_syntax_t. */
    public enum AsmSyntax {
        INTEL(0), ATT(1);
        final int v; AsmSyntax(int v) { this.v = v; }
    }

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Arena ARENA = Arena.ofShared();
    private static final SymbolLookup CORPUS;

    private static final MethodHandle CORPUS_ROUTINE, REGS_NEW, REGS_FREE, CAPTURE6,
        CAPTURE_FP2, CAPTURE_VEC_F32, REGS_RET, REGS_FRET, REGS_VEC_F32, REGS_FLAG_SET,
        CHECK_ABI, EMU_OPEN, EMU_CLOSE, EMU_RES_NEW, EMU_RES_FREE, EMU_CALL2, EMU_CALL_ASM6,
        ASM_BYTES, ASM_LAST_ERROR, EMU_FAULTED, EMU_REG;

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
        CAPTURE_VEC_F32 = h(emu, "asmtest_capture_vec_f32",
            FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS, JAVA_INT));
        REGS_RET = h(emu, "asmtest_regs_ret", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        REGS_FRET = h(emu, "asmtest_regs_fret", FunctionDescriptor.of(JAVA_DOUBLE, ADDRESS));
        REGS_VEC_F32 = h(emu, "asmtest_regs_vec_f32",
            FunctionDescriptor.of(JAVA_FLOAT, ADDRESS, JAVA_INT, JAVA_INT));
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
        EMU_CALL_ASM6 = hOpt(emu, "asmtest_emu_call_asm6", FunctionDescriptor.of(
            JAVA_INT, ADDRESS, ADDRESS, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_LONG,
            JAVA_LONG, JAVA_LONG, JAVA_LONG, JAVA_INT, JAVA_LONG, ADDRESS));
        ASM_BYTES = hOpt(emu, "asmtest_asm_bytes", FunctionDescriptor.of(
            JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT));
        ASM_LAST_ERROR = hOpt(emu, "asmtest_asm_last_error", FunctionDescriptor.of(ADDRESS));
        EMU_FAULTED = h(emu, "asmtest_emu_result_faulted", FunctionDescriptor.of(JAVA_INT, ADDRESS));
        EMU_REG = h(emu, "asmtest_emu_x86_reg", FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS));
    }

    private static MethodHandle h(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(lk.find(name).orElseThrow(
            () -> new RuntimeException("missing symbol: " + name)), fd);
    }

    /** Like h, but null when the symbol is absent — for optional entry points
     *  such as the in-line assembler (present only in the emu+asm lib). */
    private static MethodHandle hOpt(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return lk.find(name).map(s -> LINKER.downcallHandle(s, fd)).orElse(null);
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

    /** The Keystone diagnostic from the most recent assemble (thread-local; "" on success). */
    public static String asmError() {
        if (ASM_LAST_ERROR == null) return "";
        try {
            MemorySegment p = (MemorySegment) ASM_LAST_ERROR.invoke();
            return p.equals(MemorySegment.NULL) ? "" : p.reinterpret(256).getUtf8String(0);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /**
     * Assemble {@code src} for {@code arch}/{@code syntax} at load address
     * {@code addr} and return the machine-code bytes. Multi-arch (unlike
     * {@link Emu#callAsm}, which runs on the x86-64 guest). Throws
     * AsmtestException with the Keystone diagnostic on failure.
     */
    public static byte[] assemble(String src, AsmArch arch, AsmSyntax syntax, long addr) {
        if (ASM_BYTES == null) throw new AsmtestException("in-line assembler not in this build");
        try {
            MemorySegment buf = ARENA.allocate(256);
            int n = (int) ASM_BYTES.invoke(arch.v, syntax.v, str(src), addr, buf, 256);
            if (n == 0) throw new AsmtestException("assemble failed: " + asmError());
            if (n > 256) { buf = ARENA.allocate(n); n = (int) ASM_BYTES.invoke(arch.v, syntax.v, str(src), addr, buf, n); }
            byte[] out = new byte[n];
            MemorySegment.copy(buf, JAVA_BYTE, 0, out, 0, n);
            return out;
        } catch (AsmtestException ae) { throw ae; }
        catch (Throwable t) { throw rethrow(t); }
    }
    /** Convenience: Intel syntax at the emulator load base. */
    public static byte[] assemble(String src, AsmArch arch) {
        return assemble(src, arch, AsmSyntax.INTEL, 0x00100000L);
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
        /** Call fn with up to eight 128-bit vector args (each four float32 lanes),
         *  capturing the vector register file. Read the vector return with vecF32(0). */
        public void captureVecF32(MemorySegment fn, float[][] vectors) {
            int nvec = vectors.length;
            try {
                MemorySegment lanes = ARENA.allocate(JAVA_FLOAT.byteSize() * 4 * Math.max(nvec, 1));
                for (int i = 0; i < nvec; i++) {
                    for (int l = 0; l < 4; l++) lanes.setAtIndex(JAVA_FLOAT, (long) i * 4 + l, vectors[i][l]);
                }
                CAPTURE_VEC_F32.invoke(h, fn, lanes, nvec);
            } catch (Throwable t) { throw rethrow(t); }
        }
        public long ret() {
            try { return (long) REGS_RET.invoke(h); } catch (Throwable t) { throw rethrow(t); }
        }
        public double fret() {
            try { return (double) REGS_FRET.invoke(h); } catch (Throwable t) { throw rethrow(t); }
        }
        /** The four float32 lanes of vector register {@code index} (0 = the vector return). */
        public float[] vecF32(int index) {
            float[] out = new float[4];
            try { for (int l = 0; l < 4; l++) out[l] = (float) REGS_VEC_F32.invoke(h, index, l); }
            catch (Throwable t) { throw rethrow(t); }
            return out;
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
        /** Whether the loaded native lib carries the in-line assembler (Keystone). */
        public boolean asmAvailable() { return EMU_CALL_ASM6 != null; }
        /**
         * Assemble x86-64 {@code src} in {@code syntax} via Keystone and run it with
         * the first {@code args} (up to six) integer args, stopping after
         * {@code maxInsns} instructions (0 = run to {@code ret}). Returns an
         * EmuResult. Only when {@link #asmAvailable()}; throws AsmtestException
         * carrying the Keystone diagnostic if the string fails to assemble.
         */
        public EmuResult callAsm(String src, long[] args, AsmSyntax syntax, long maxInsns) {
            if (EMU_CALL_ASM6 == null) throw new AsmtestException("in-line assembler not in this build");
            long[] a = new long[6];
            int nargs = Math.min(args == null ? 0 : args.length, 6);
            for (int i = 0; i < nargs; i++) a[i] = args[i];
            EmuResult res = new EmuResult();
            int ok;
            try {
                ok = (int) EMU_CALL_ASM6.invoke(h, str(src), syntax.v,
                    a[0], a[1], a[2], a[3], a[4], a[5], nargs, maxInsns, res.h);
            } catch (Throwable t) { throw rethrow(t); }
            if (ok == 0) { res.close(); throw new AsmtestException("in-line assembly failed: " + asmError()); }
            return res;
        }
        /** Convenience: Intel syntax, run to ret, with the given integer args. */
        public EmuResult callAsm(String src, long... args) {
            return callAsm(src, args, AsmSyntax.INTEL, 0);
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
    public static void assertVecF32(Regs r, int index, float[] want) {
        float[] got = r.vecF32(index);
        for (int i = 0; i < want.length; i++) {
            if (got[i] != want[i])
                throw new AsmtestException("vec[" + index + "] lane " + i + ": got " + got[i] + ", want " + want[i]);
        }
    }
    public static void assertNoFault(EmuResult res) {
        if (res.faulted()) throw new AsmtestException("unexpected fault");
    }
    public static void assertEmuReg(EmuResult res, String name, long want) {
        long got = res.reg(name);
        if (got != want) throw new AsmtestException("emu " + name + ": got " + got + ", want " + want);
    }
}
