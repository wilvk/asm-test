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
        ASM_BYTES, ASM_LAST_ERROR, EMU_FAULTED, EMU_FAULT_ADDR, EMU_FAULT_KIND, EMU_REG,
        EMU_XMM_F64, EMU_XMM_F32;

    // Extended x86 emulator calls (raw bytes), the cross-arch guests, and the
    // opaque trace handle — the surface that reaches the new corpus tiers.
    private static final MethodHandle EMU_CALL, EMU_CALL_FP, EMU_CALL_VEC,
        EMU_CALL_WIN64, EMU_CALL_TRACED, TRACE_NEW, TRACE_FREE, TRACE_COVERED,
        ARM64_OPEN, ARM64_CLOSE, ARM64_CALL, ARM64_CALL_TRACED, ARM64_RES_NEW,
        ARM64_RES_FREE, ARM64_REG, RISCV_OPEN, RISCV_CLOSE, RISCV_CALL,
        RISCV_RES_NEW, RISCV_RES_FREE, RISCV_REG, ARM_OPEN, ARM_CLOSE, ARM_CALL,
        ARM_RES_NEW, ARM_RES_FREE, ARM_REG;

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
        EMU_FAULT_ADDR = h(emu, "asmtest_emu_result_fault_addr", FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        EMU_FAULT_KIND = h(emu, "asmtest_emu_result_fault_kind", FunctionDescriptor.of(JAVA_INT, ADDRESS));
        EMU_REG = h(emu, "asmtest_emu_x86_reg", FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS));
        EMU_XMM_F64 = h(emu, "asmtest_emu_x86_xmm_f64", FunctionDescriptor.of(JAVA_DOUBLE, ADDRESS, JAVA_INT, JAVA_INT));
        EMU_XMM_F32 = h(emu, "asmtest_emu_x86_xmm_f32", FunctionDescriptor.of(JAVA_FLOAT, ADDRESS, JAVA_INT, JAVA_INT));

        // Extended x86 emulator calls (array form, so raw bytes run directly).
        // Declared void: each writes its result via the out-handle, which the
        // accessors read; the C bool return is unused.
        FunctionDescriptor callFd = FunctionDescriptor.ofVoid(
            ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT, JAVA_LONG, ADDRESS);
        FunctionDescriptor fpFd = FunctionDescriptor.ofVoid(
            ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT, ADDRESS, JAVA_INT, JAVA_LONG, ADDRESS);
        FunctionDescriptor tracedFd = FunctionDescriptor.ofVoid(
            ADDRESS, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT, JAVA_LONG, ADDRESS, ADDRESS);
        EMU_CALL = h(emu, "emu_call", callFd);
        EMU_CALL_FP = h(emu, "emu_call_fp", fpFd);
        EMU_CALL_VEC = h(emu, "emu_call_vec", fpFd);  // same shape (vectors as ADDRESS)
        EMU_CALL_WIN64 = h(emu, "emu_call_win64", callFd);
        EMU_CALL_TRACED = h(emu, "emu_call_traced", tracedFd);

        // Opaque trace handle.
        TRACE_NEW = h(emu, "asmtest_emu_trace_new", FunctionDescriptor.of(ADDRESS, JAVA_LONG, JAVA_LONG));
        TRACE_FREE = h(emu, "asmtest_emu_trace_free", FunctionDescriptor.ofVoid(ADDRESS));
        TRACE_COVERED = h(emu, "asmtest_emu_trace_covered", FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));

        // Cross-arch guests (raw bytes, any host).
        FunctionDescriptor regFd = FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS);
        ARM64_OPEN = h(emu, "emu_arm64_open", FunctionDescriptor.of(ADDRESS));
        ARM64_CLOSE = h(emu, "emu_arm64_close", FunctionDescriptor.ofVoid(ADDRESS));
        ARM64_CALL = h(emu, "emu_arm64_call", callFd);
        ARM64_CALL_TRACED = h(emu, "emu_arm64_call_traced", tracedFd);
        ARM64_RES_NEW = h(emu, "asmtest_emu_arm64_result_new", FunctionDescriptor.of(ADDRESS));
        ARM64_RES_FREE = h(emu, "asmtest_emu_arm64_result_free", FunctionDescriptor.ofVoid(ADDRESS));
        ARM64_REG = h(emu, "asmtest_emu_arm64_reg", regFd);
        RISCV_OPEN = h(emu, "emu_riscv_open", FunctionDescriptor.of(ADDRESS));
        RISCV_CLOSE = h(emu, "emu_riscv_close", FunctionDescriptor.ofVoid(ADDRESS));
        RISCV_CALL = h(emu, "emu_riscv_call", callFd);
        RISCV_RES_NEW = h(emu, "asmtest_emu_riscv_result_new", FunctionDescriptor.of(ADDRESS));
        RISCV_RES_FREE = h(emu, "asmtest_emu_riscv_result_free", FunctionDescriptor.ofVoid(ADDRESS));
        RISCV_REG = h(emu, "asmtest_emu_riscv_reg", regFd);
        ARM_OPEN = h(emu, "emu_arm_open", FunctionDescriptor.of(ADDRESS));
        ARM_CLOSE = h(emu, "emu_arm_close", FunctionDescriptor.ofVoid(ADDRESS));
        ARM_CALL = h(emu, "emu_arm_call", callFd);
        ARM_RES_NEW = h(emu, "asmtest_emu_arm_result_new", FunctionDescriptor.of(ADDRESS));
        ARM_RES_FREE = h(emu, "asmtest_emu_arm_result_free", FunctionDescriptor.ofVoid(ADDRESS));
        ARM_REG = h(emu, "asmtest_emu_arm_reg", regFd);
    }

    // ---- helpers for marshalling Java arrays into native memory ---- //
    private static MemorySegment bytesSeg(byte[] b) {
        MemorySegment seg = ARENA.allocate(Math.max(b.length, 1));
        MemorySegment.copy(b, 0, seg, JAVA_BYTE, 0, b.length);
        return seg;
    }
    private static MemorySegment longsSeg(long[] a) {
        if (a == null || a.length == 0) return MemorySegment.NULL;
        MemorySegment seg = ARENA.allocate(JAVA_LONG.byteSize() * a.length);
        for (int i = 0; i < a.length; i++) seg.setAtIndex(JAVA_LONG, i, a[i]);
        return seg;
    }
    private static MemorySegment doublesSeg(double[] a) {
        if (a == null || a.length == 0) return MemorySegment.NULL;
        MemorySegment seg = ARENA.allocate(JAVA_DOUBLE.byteSize() * a.length);
        for (int i = 0; i < a.length; i++) seg.setAtIndex(JAVA_DOUBLE, i, a[i]);
        return seg;
    }
    private static MemorySegment vecsSeg(float[][] vectors) {
        if (vectors.length == 0) return MemorySegment.NULL;
        MemorySegment seg = ARENA.allocate(JAVA_FLOAT.byteSize() * 4 * vectors.length);
        for (int i = 0; i < vectors.length; i++)
            for (int l = 0; l < 4; l++) seg.setAtIndex(JAVA_FLOAT, (long) i * 4 + l, vectors[i][l]);
        return seg;
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
    /** Invalid-access kind reported by {@link EmuResult#faultKind()} (mirrors emu_fault_kind_t). */
    public enum FaultKind {
        NONE, READ, WRITE, FETCH;
        static FaultKind of(int v) {
            FaultKind[] vs = values();
            return (v >= 0 && v < vs.length) ? vs[v] : NONE;
        }
    }

    public static final class EmuResult implements AutoCloseable {
        private MemorySegment h;
        EmuResult() {
            try { h = (MemorySegment) EMU_RES_NEW.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        public boolean faulted() {
            try { return (int) EMU_FAULTED.invoke(h) != 0; } catch (Throwable t) { throw rethrow(t); }
        }
        /** Faulting guest address; only meaningful when {@link #faulted()}. */
        public long faultAddr() {
            try { return (long) EMU_FAULT_ADDR.invoke(h); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Why the access was invalid (a {@link FaultKind}); only meaningful when {@link #faulted()}. */
        public FaultKind faultKind() {
            try { return FaultKind.of((int) EMU_FAULT_KIND.invoke(h)); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Read an x86-64 guest register by name — GP plus "rip" / "rflags". */
        public long reg(String name) {
            try { return (long) EMU_REG.invoke(h, str(name)); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Lane (0..1) of guest XMM register {@code index} as a double (scalar return = xmmF64(0, 0)). */
        public double xmmF64(int index, int lane) {
            try { return (double) EMU_XMM_F64.invoke(h, index, lane); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Lane (0..3) of guest XMM register {@code index} as a float. */
        public float xmmF32(int index, int lane) {
            try { return (float) EMU_XMM_F32.invoke(h, index, lane); } catch (Throwable t) { throw rethrow(t); }
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
        /** Run raw x86-64 machine-code bytes with up to six integer args. */
        public EmuResult callBytes(byte[] code, long... args) {
            EmuResult res = new EmuResult();
            try { EMU_CALL.invoke(h, bytesSeg(code), (long) code.length, longsSeg(args), args.length, 0L, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /** Run raw bytes marshalling doubles into the FP arg registers (scalar
         *  return = res.xmmF64(0, 0)). */
        public EmuResult callFp(byte[] code, long[] iargs, double[] fargs) {
            EmuResult res = new EmuResult();
            int ni = iargs == null ? 0 : iargs.length, nf = fargs == null ? 0 : fargs.length;
            try { EMU_CALL_FP.invoke(h, bytesSeg(code), (long) code.length, longsSeg(iargs), ni, doublesSeg(fargs), nf, 0L, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /** Run raw bytes marshalling 128-bit vectors into xmm0..7. */
        public EmuResult callVec(byte[] code, long[] iargs, float[][] vargs) {
            EmuResult res = new EmuResult();
            int ni = iargs == null ? 0 : iargs.length;
            try { EMU_CALL_VEC.invoke(h, bytesSeg(code), (long) code.length, longsSeg(iargs), ni, vecsSeg(vargs), vargs.length, 0L, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /** Run raw bytes under the Microsoft x64 (Win64) convention. */
        public EmuResult callWin64(byte[] code, long... args) {
            EmuResult res = new EmuResult();
            try { EMU_CALL_WIN64.invoke(h, bytesSeg(code), (long) code.length, longsSeg(args), args.length, 0L, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /** Like callBytes, but record an execution trace / coverage into tr. */
        public EmuResult callTraced(byte[] code, long[] args, Trace tr) {
            EmuResult res = new EmuResult();
            int n = args == null ? 0 : args.length;
            try { EMU_CALL_TRACED.invoke(h, bytesSeg(code), (long) code.length, longsSeg(args), n, 0L, res.h, tr.h); }
            catch (Throwable t) { throw rethrow(t); }
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
    public static void assertFault(EmuResult res) {
        if (!res.faulted()) throw new AsmtestException("expected a fault, but the run completed cleanly");
    }
    public static void assertEmuReg(EmuResult res, String name, long want) {
        long got = res.reg(name);
        if (got != want) throw new AsmtestException("emu " + name + ": got " + got + ", want " + want);
    }
    public static void assertGuestReg(GuestResult res, String name, long want) {
        long got = res.reg(name);
        if (got != want) throw new AsmtestException("guest " + name + ": got " + got + ", want " + want);
    }
    public static void assertCovered(Trace tr, long off) {
        if (!tr.covered(off)) throw new AsmtestException("block " + off + ": expected covered");
    }

    // ---- Execution trace / coverage ---- //

    /** An opaque execution-trace / basic-block coverage recorder. */
    public static final class Trace implements AutoCloseable {
        MemorySegment h;
        public Trace() { this(4096, 4096); }
        public Trace(long insnsCap, long blocksCap) {
            try { h = (MemorySegment) TRACE_NEW.invoke(insnsCap, blocksCap); } catch (Throwable t) { throw rethrow(t); }
        }
        /** True if the basic block at byte-offset {@code off} was entered. */
        public boolean covered(long off) {
            try { return (int) TRACE_COVERED.invoke(h, off) != 0; } catch (Throwable t) { throw rethrow(t); }
        }
        @Override public void close() {
            try { if (h != null) TRACE_FREE.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }

    // ---- Cross-arch emulator guests (raw bytes, any host) ---- //

    /** A cross-arch run's outcome; registers are read by name. */
    public static final class GuestResult implements AutoCloseable {
        private MemorySegment h;
        private final String arch;
        GuestResult(String arch) {
            this.arch = arch;
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_RES_NEW; case "riscv" -> RISCV_RES_NEW;
                case "arm" -> ARM_RES_NEW; default -> throw new AsmtestException("unknown guest: " + arch);
            };
            try { h = (MemorySegment) mh.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        public boolean faulted() {
            try { return (int) EMU_FAULTED.invoke(h) != 0; } catch (Throwable t) { throw rethrow(t); }
        }
        /** Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0"). */
        public long reg(String name) {
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_REG; case "riscv" -> RISCV_REG; case "arm" -> ARM_REG;
                default -> throw new AsmtestException("unknown guest: " + arch);
            };
            try { return (long) mh.invoke(h, str(name)); } catch (Throwable t) { throw rethrow(t); }
        }
        @Override public void close() {
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_RES_FREE; case "riscv" -> RISCV_RES_FREE; case "arm" -> ARM_RES_FREE;
                default -> null;
            };
            try { if (h != null && mh != null) mh.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }

    /** A cross-arch Unicorn guest ("arm64"/"riscv"/"arm") running raw bytes on any
     *  host. Use try-with-resources to close. */
    public static final class Guest implements AutoCloseable {
        private MemorySegment h;
        private final String arch;
        public Guest(String arch) {
            this.arch = arch;
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_OPEN; case "riscv" -> RISCV_OPEN;
                case "arm" -> ARM_OPEN; default -> throw new AsmtestException("unknown guest: " + arch);
            };
            try { h = (MemorySegment) mh.invoke(); } catch (Throwable t) { throw rethrow(t); }
        }
        /** Run raw machine-code bytes with integer args in the guest ABI registers. */
        public GuestResult call(byte[] code, long... args) {
            GuestResult res = new GuestResult(arch);
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_CALL; case "riscv" -> RISCV_CALL; case "arm" -> ARM_CALL;
                default -> throw new AsmtestException("unknown guest: " + arch);
            };
            try { mh.invoke(h, bytesSeg(code), (long) code.length, longsSeg(args), args.length, 0L, res.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        /** Like call, but record an execution trace / coverage into tr (arm64). */
        public GuestResult callTraced(byte[] code, long[] args, Trace tr) {
            if (!arch.equals("arm64")) throw new AsmtestException("traced guest run only wired for arm64");
            GuestResult res = new GuestResult(arch);
            int n = args == null ? 0 : args.length;
            try { ARM64_CALL_TRACED.invoke(h, bytesSeg(code), (long) code.length, longsSeg(args), n, 0L, res.h, tr.h); }
            catch (Throwable t) { throw rethrow(t); }
            return res;
        }
        @Override public void close() {
            MethodHandle mh = switch (arch) {
                case "arm64" -> ARM64_CLOSE; case "riscv" -> RISCV_CLOSE; case "arm" -> ARM_CLOSE;
                default -> null;
            };
            try { if (h != null && mh != null) mh.invoke(h); } catch (Throwable t) { throw rethrow(t); } finally { h = null; }
        }
    }
}
