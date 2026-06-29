/*
 * HwTrace.java — Java binding for the optional hardware-trace native-trace tier.
 *
 * Where Asmtest.java's Emu traces isolated guest bytes through Unicorn and
 * DrTrace.java traces host-native code through DynamoRIO, this traces host-native
 * code by observing the **real CPU**: initialize a backend once, materialize
 * host-native machine code, mark a region, call into it, and read back the same
 * basic-block coverage / instruction stream the emulator and DynamoRIO tiers
 * record. It mirrors the Python wrapper (bindings/python/asmtest/hwtrace.py) and
 * drives the C API in include/asmtest_hwtrace.h.
 *
 * Four backends share one API, selected by enum. The SINGLESTEP backend
 * (EFLAGS.TF single-step) is the portable default that runs on ANY x86-64 Linux
 * (Intel, any-Zen AMD, VM, CI, container): no PMU, no perf_event, no privilege —
 * so it is the one this binding's self-test exercises live. INTEL_PT / CORESIGHT
 * / AMD_LBR self-skip off the specific bare-metal hardware they need.
 *
 * Like Asmtest.java this keeps all Foreign Function & Memory (Project Panama,
 * java.lang.foreign) plumbing inside. It loads ONE library, libasmtest_hwtrace,
 * resolved from the environment (else the repo build/), and never links a
 * decoder:
 *   ASMTEST_HWTRACE_LIB   libasmtest_hwtrace.{so,dylib}   (else <repo>/build/...)
 *
 * The library may be absent (e.g. not built), so the static initializer swallows
 * a load failure and {@link #available(int)} self-skips cleanly — callers never
 * see a throw out of {@code available()}.
 *
 * FFM is a preview API in JDK 21: compile with `--release 21 --enable-preview`,
 * run with `--enable-preview --enable-native-access=ALL-UNNAMED`.
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

public final class HwTrace {
    private HwTrace() {}

    /** ASMTEST_HW_OK — the success status returned by the lifecycle/registration calls. */
    public static final int ASMTEST_HW_OK = 0;

    // asmtest_trace_backend_t — the SINGLESTEP backend is the portable default.
    public static final int INTEL_PT = 0;
    public static final int CORESIGHT = 1;
    public static final int AMD_LBR = 2;
    public static final int SINGLESTEP = 3;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Arena ARENA = Arena.ofShared();

    // asmtest_hwtrace_options_t {int backend; size_t aux_size; size_t data_size;
    //   int snapshot; const char* object_hint;}. backend (JAVA_INT, 4) needs 4 bytes
    // of pad before the size_t fields; snapshot (JAVA_INT, 4) needs 4 bytes of pad
    // before the pointer — total 40, 8-byte aligned.
    private static final MemoryLayout OPTIONS_LAYOUT = MemoryLayout.structLayout(
        JAVA_INT.withName("backend"),
        MemoryLayout.paddingLayout(4),
        JAVA_LONG.withName("aux_size"),
        JAVA_LONG.withName("data_size"),
        JAVA_INT.withName("snapshot"),
        MemoryLayout.paddingLayout(4),
        ADDRESS.withName("object_hint"));

    // Resolved when the library loads; null when it can't (then available() == false).
    private static final MethodHandle HW_AVAILABLE, HW_SKIP_REASON, HW_INIT, HW_SHUTDOWN,
        REGISTER_REGION, HW_BEGIN, HW_END, EXEC_ALLOC, EXEC_FREE,
        TRACE_NEW, TRACE_FREE, TRACE_COVERED, TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL,
        TRACE_INSNS_LEN, TRACE_TRUNCATED, TRACE_BLOCK_AT, TRACE_INSN_AT;

    // The load error, kept for diagnostics; null on success.
    private static final Throwable LOAD_ERROR;

    // Resolve libasmtest_hwtrace: an explicit ASMTEST_HWTRACE_LIB wins (dev / custom
    // build), else <repo>/build/libasmtest_hwtrace.{so,dylib}. The test always sets
    // the env, matching how the Python wrapper resolves the repo build/ dir.
    private static String resolveHwtraceLib() {
        String env = System.getenv("ASMTEST_HWTRACE_LIB");
        if (env != null && !env.isEmpty()) return env;
        boolean mac = System.getProperty("os.name", "").toLowerCase().contains("mac");
        String name = mac ? "libasmtest_hwtrace.dylib" : "libasmtest_hwtrace.so";
        // Best-effort default: cwd-relative build/ (the test always sets the env).
        return "build/" + name;
    }

    static {
        MethodHandle hwAvailable = null, hwSkipReason = null, hwInit = null, hwShutdown = null,
            registerRegion = null, hwBegin = null, hwEnd = null, execAlloc = null, execFree = null,
            traceNew = null, traceFree = null, traceCovered = null, traceBlocksLen = null,
            traceInsnsTotal = null, traceInsnsLen = null, traceTruncated = null,
            traceBlockAt = null, traceInsnAt = null;
        Throwable loadError = null;
        try {
            SymbolLookup lib = SymbolLookup.libraryLookup(resolveHwtraceLib(), ARENA);
            hwAvailable = h(lib, "asmtest_hwtrace_available", FunctionDescriptor.of(JAVA_INT, JAVA_INT));
            hwSkipReason = h(lib, "asmtest_hwtrace_skip_reason",
                FunctionDescriptor.ofVoid(JAVA_INT, ADDRESS, JAVA_LONG));
            hwInit = h(lib, "asmtest_hwtrace_init", FunctionDescriptor.of(JAVA_INT, ADDRESS));
            hwShutdown = h(lib, "asmtest_hwtrace_shutdown", FunctionDescriptor.ofVoid());
            registerRegion = h(lib, "asmtest_hwtrace_register_region",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS));
            hwBegin = h(lib, "asmtest_hwtrace_begin", FunctionDescriptor.ofVoid(ADDRESS));
            hwEnd = h(lib, "asmtest_hwtrace_end", FunctionDescriptor.ofVoid(ADDRESS));
            execAlloc = h(lib, "asmtest_hwtrace_exec_alloc",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, ADDRESS));
            execFree = h(lib, "asmtest_hwtrace_exec_free",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
            // asmtest_trace_new(insns_cap, blocks_cap) — insns FIRST, blocks SECOND.
            traceNew = h(lib, "asmtest_trace_new",
                FunctionDescriptor.of(ADDRESS, JAVA_LONG, JAVA_LONG));
            traceFree = h(lib, "asmtest_trace_free", FunctionDescriptor.ofVoid(ADDRESS));
            traceCovered = h(lib, "asmtest_trace_covered",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
            traceBlocksLen = h(lib, "asmtest_emu_trace_blocks_len",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            traceInsnsTotal = h(lib, "asmtest_emu_trace_insns_total",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            traceInsnsLen = h(lib, "asmtest_emu_trace_insns_len",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            traceTruncated = h(lib, "asmtest_emu_trace_truncated",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            traceBlockAt = h(lib, "asmtest_emu_trace_block_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            traceInsnAt = h(lib, "asmtest_emu_trace_insn_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
        } catch (Throwable t) {
            // The library may be absent (not built) — degrade to available() == false
            // rather than failing class init.
            loadError = t;
        }
        HW_AVAILABLE = hwAvailable; HW_SKIP_REASON = hwSkipReason; HW_INIT = hwInit;
        HW_SHUTDOWN = hwShutdown; REGISTER_REGION = registerRegion; HW_BEGIN = hwBegin;
        HW_END = hwEnd; EXEC_ALLOC = execAlloc; EXEC_FREE = execFree; TRACE_NEW = traceNew;
        TRACE_FREE = traceFree; TRACE_COVERED = traceCovered; TRACE_BLOCKS_LEN = traceBlocksLen;
        TRACE_INSNS_TOTAL = traceInsnsTotal; TRACE_INSNS_LEN = traceInsnsLen;
        TRACE_TRUNCATED = traceTruncated; TRACE_BLOCK_AT = traceBlockAt; TRACE_INSN_AT = traceInsnAt;
        LOAD_ERROR = loadError;
    }

    private static MethodHandle h(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(lk.find(name).orElseThrow(
            () -> new RuntimeException("missing symbol: " + name)), fd);
    }

    private static RuntimeException rethrow(Throwable t) {
        return t instanceof RuntimeException re ? re : new RuntimeException(t);
    }

    private static MemorySegment str(String s) {
        return s == null ? MemorySegment.NULL : ARENA.allocateUtf8String(s);
    }

    // ---- process-wide lifecycle ----

    /** True if {@code backend} can run on this host. Never throws: a load failure
     *  OR {@code asmtest_hwtrace_available() == 0} → false, so callers self-skip
     *  cleanly. SINGLESTEP is the portable default that runs on any x86-64 Linux. */
    public static boolean available(int backend) {
        if (HW_AVAILABLE == null) return false;
        try { return (int) HW_AVAILABLE.invoke(backend) != 0; }
        catch (Throwable t) { return false; }
    }

    /** Convenience: availability of the portable SINGLESTEP default. */
    public static boolean available() { return available(SINGLESTEP); }

    /** Human-readable reason {@link #available(int)} is false (or "available"). */
    public static String skipReason(int backend) {
        if (HW_SKIP_REASON == null) {
            Throwable e = LOAD_ERROR;
            return "libasmtest_hwtrace not loaded" + (e != null ? ": " + e : "");
        }
        try {
            MemorySegment buf = ARENA.allocate(160);
            HW_SKIP_REASON.invoke(backend, buf, 160L);
            return buf.getUtf8String(0);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** Convenience: skip reason for the SINGLESTEP default. */
    public static String skipReason() { return skipReason(SINGLESTEP); }

    /** Diagnostic for why the library failed to load, or null if it loaded. */
    public static Throwable loadError() { return LOAD_ERROR; }

    /** Select {@code backend} and initialize the tier (asmtest_hwtrace_init);
     *  throws RuntimeException on a nonzero status. SINGLESTEP is the portable
     *  default that runs on any x86-64 Linux. */
    public static void init(int backend) {
        if (HW_INIT == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            MemorySegment opts = ARENA.allocate(OPTIONS_LAYOUT);
            opts.set(JAVA_INT, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("backend")), backend);
            int rc = (int) HW_INIT.invoke(opts);
            if (rc != ASMTEST_HW_OK) throw new RuntimeException("asmtest_hwtrace_init failed: " + rc);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Convenience: initialize the portable SINGLESTEP default. */
    public static void init() { init(SINGLESTEP); }

    /** Tear the tier down (asmtest_hwtrace_shutdown). */
    public static void shutdown() {
        if (HW_SHUTDOWN == null) return;
        try { HW_SHUTDOWN.invoke(); } catch (Throwable t) { throw rethrow(t); }
    }

    /** Allocate a trace recording up to {@code blocks} basic blocks and
     *  {@code instructions} ordered instructions (instruction recording only when
     *  {@code instructions > 0}). */
    public static NativeTrace create(int blocks, int instructions) {
        if (TRACE_NEW == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            // asmtest_trace_new(insns_cap, blocks_cap) — insns FIRST.
            MemorySegment h = (MemorySegment) TRACE_NEW.invoke((long) instructions, (long) blocks);
            if (MemorySegment.NULL.equals(h)) throw new RuntimeException("asmtest_trace_new failed");
            return new NativeTrace(h);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Host-native machine code in real executable (W^X) memory. */
    public static final class NativeCode implements AutoCloseable {
        private long base;
        private final long len;
        private boolean freed;

        private NativeCode(long base, long len) { this.base = base; this.len = len; }

        /** Map executable memory and copy {@code bytes} of host-native code into it. */
        public static NativeCode fromBytes(byte[] bytes) {
            if (EXEC_ALLOC == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
            try {
                MemorySegment in = ARENA.allocate(Math.max(bytes.length, 1));
                MemorySegment.copy(bytes, 0, in, JAVA_BYTE, 0, bytes.length);
                MemorySegment baseOut = ARENA.allocate(ADDRESS); // void**
                MemorySegment lenOut = ARENA.allocate(JAVA_LONG); // size_t*
                int rc = (int) EXEC_ALLOC.invoke(in, (long) bytes.length, baseOut, lenOut);
                if (rc != ASMTEST_HW_OK) throw new RuntimeException("asmtest_hwtrace_exec_alloc failed: " + rc);
                return new NativeCode(baseOut.get(ADDRESS, 0).address(), lenOut.get(JAVA_LONG, 0));
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Address of the executable mapping (offset 0 = entry). */
        public long base() { return base; }

        /** Number of code bytes. */
        public long length() { return len; }

        /** Invoke the code through a function pointer with two C-long args, reading
         *  the result as a C long (the SysV integer ABI). */
        public long call(long a, long b) {
            try {
                MethodHandle fn = LINKER.downcallHandle(MemorySegment.ofAddress(base),
                    FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, JAVA_LONG));
                return (long) fn.invoke(a, b);
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** Unmap the executable memory. Unregister any region over this code FIRST. */
        public void free() {
            if (freed || EXEC_FREE == null) return;
            try { EXEC_FREE.invoke(MemorySegment.ofAddress(base), len); }
            catch (Throwable t) { throw rethrow(t); }
            finally { freed = true; }
        }

        @Override public void close() { free(); }
    }

    /** A coverage recorder for a registered native region, via the hardware tier. */
    public static final class NativeTrace implements AutoCloseable {
        private MemorySegment handle; // an asmtest_trace_t*

        private NativeTrace(MemorySegment handle) { this.handle = handle; }

        /** Register a non-overlapping native code range under {@code name}, recording
         *  coverage into this trace. */
        public NativeTrace register(String name, NativeCode code) {
            try {
                int rc = (int) REGISTER_REGION.invoke(str(name),
                    MemorySegment.ofAddress(code.base()), code.length(), handle);
                if (rc != ASMTEST_HW_OK)
                    throw new RuntimeException("register_region(" + name + ") failed: " + rc);
                return this;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Run {@code body} between balanced begin/end markers for {@code name}.
         *  The markers are always balanced (end runs in a finally), so a throw from
         *  the body still closes the region. */
        public void region(String name, Runnable body) {
            MemorySegment n = str(name);
            try {
                HW_BEGIN.invoke(n);
                try { body.run(); }
                finally { HW_END.invoke(n); }
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** True if the basic block at byte-offset {@code off} was entered. */
        public boolean covered(long off) {
            try { return (int) TRACE_COVERED.invoke(handle, off) != 0; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Number of distinct basic blocks recorded. */
        public long blocksLen() {
            try { return (long) TRACE_BLOCKS_LEN.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Total instructions retired in the recorded stream (may exceed the stored
         *  insns_len when the trace's instruction capacity is reached). */
        public long insnsTotal() {
            try { return (long) TRACE_INSNS_TOTAL.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Number of instruction offsets actually stored (≤ the trace's insns capacity). */
        public long insnsLen() {
            try { return (long) TRACE_INSNS_LEN.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** True if recording hit a capacity ceiling and dropped data. */
        public boolean truncated() {
            try { return (int) TRACE_TRUNCATED.invoke(handle) != 0; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The distinct basic-block start offsets recorded, in first-seen order. */
        public long[] blockOffsets() {
            try {
                int n = (int) (long) TRACE_BLOCKS_LEN.invoke(handle);
                long[] out = new long[n];
                for (int i = 0; i < n; i++) out[i] = (long) TRACE_BLOCK_AT.invoke(handle, (long) i);
                return out;
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** The ordered instruction-offset stream actually stored — each executed
         *  instruction's offset in execution order, up to the trace's insns capacity
         *  (insns_len, not the possibly-larger insns_total). */
        public long[] insnOffsets() {
            try {
                int n = (int) (long) TRACE_INSNS_LEN.invoke(handle);
                long[] out = new long[n];
                for (int i = 0; i < n; i++) out[i] = (long) TRACE_INSN_AT.invoke(handle, (long) i);
                return out;
            } catch (Throwable t) { throw rethrow(t); }
        }

        public void free() {
            if (handle == null || TRACE_FREE == null) return;
            try { TRACE_FREE.invoke(handle); } catch (Throwable t) { throw rethrow(t); }
            finally { handle = null; }
        }

        @Override public void close() { free(); }
    }
}
