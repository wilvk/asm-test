/*
 * DrTrace.java — Java binding for the optional DynamoRIO native-trace tier.
 *
 * Where Asmtest.java's Emu traces isolated guest bytes through Unicorn, this
 * traces host-native code as it runs **inside this JVM process**: initialize
 * DynamoRIO once, materialize host-native machine code, mark a region, call into
 * it, and read back basic-block coverage / the instruction stream. It mirrors
 * the Python wrapper (bindings/python/asmtest/drtrace.py) and drives the C API
 * in include/asmtest_drtrace.h.
 *
 * Like Asmtest.java this keeps all Foreign Function & Memory (Project Panama,
 * java.lang.foreign) plumbing inside. It loads ONE library, libasmtest_drapp,
 * which dlopen()s libdynamorio lazily after the client is configured — so this
 * binding never links DynamoRIO. The library path is taken from the
 * environment:
 *   ASMTEST_DRAPP_LIB   libasmtest_drapp.{so,dylib}   (else <repo>/build/...)
 *
 * The tier is advanced, Linux-x86-64-only, and opt-in: the library may be absent
 * (no DynamoRIO at build time), so the static initializer swallows a load
 * failure and {@link #available()} self-skips cleanly — callers never see a
 * throw out of {@code available()}.
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

public final class DrTrace {
    private DrTrace() {}

    /** ASMTEST_DR_OK — the success status returned by the lifecycle/registration calls. */
    public static final int ASMTEST_DR_OK = 0;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Arena ARENA = Arena.ofShared();

    // asmtest_drtrace_options_t {const char* dynamorio_home; const char* client_path;
    //   const char* client_options; int mode;}. ADDRESS is 8 bytes, JAVA_INT 4 → 4 bytes
    // of trailing pad to round the struct to 8 (total 32).
    private static final MemoryLayout OPTIONS_LAYOUT = MemoryLayout.structLayout(
        ADDRESS.withName("dynamorio_home"),
        ADDRESS.withName("client_path"),
        ADDRESS.withName("client_options"),
        JAVA_INT.withName("mode"),
        MemoryLayout.paddingLayout(4));

    // asmtest_exec_code_t {void* base; size_t len;} — both 8 bytes, no padding (total 16).
    private static final MemoryLayout EXEC_CODE_LAYOUT = MemoryLayout.structLayout(
        ADDRESS.withName("base"),
        JAVA_LONG.withName("len"));

    // Resolved when the library loads; null when it can't (then available() == false).
    private static final MethodHandle DR_AVAILABLE, DR_INIT, DR_START, DR_STOP, DR_SHUTDOWN,
        REGISTER_REGION, UNREGISTER_REGION, TRACE_BEGIN, TRACE_END, MARKER_ERROR,
        EXEC_ALLOC, EXEC_FREE, TRACE_NEW, TRACE_FREE, TRACE_COVERED,
        TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL, TRACE_INSNS_LEN, TRACE_BLOCK_AT, TRACE_INSN_AT;

    // The load error, kept for diagnostics; null on success.
    private static final Throwable LOAD_ERROR;

    // Resolve libasmtest_drapp: an explicit ASMTEST_DRAPP_LIB wins (dev / custom build),
    // else <repo>/build/libasmtest_drapp.{so,dylib}. <repo> is two levels up from this
    // source dir (bindings/java/), matching how the Python wrapper resolves _REPO_ROOT.
    private static String resolveDrappLib() {
        String env = System.getenv("ASMTEST_DRAPP_LIB");
        if (env != null && !env.isEmpty()) return env;
        boolean mac = System.getProperty("os.name", "").toLowerCase().contains("mac");
        String name = mac ? "libasmtest_drapp.dylib" : "libasmtest_drapp.so";
        // Best-effort default: cwd-relative build/ (the test always sets the env).
        return "build/" + name;
    }

    static {
        MethodHandle drAvailable = null, drInit = null, drStart = null, drStop = null,
            drShutdown = null, registerRegion = null, unregisterRegion = null,
            traceBegin = null, traceEnd = null, markerError = null, execAlloc = null,
            execFree = null, traceNew = null, traceFree = null, traceCovered = null,
            traceBlocksLen = null, traceInsnsTotal = null, traceInsnsLen = null,
            traceBlockAt = null, traceInsnAt = null;
        Throwable loadError = null;
        try {
            SymbolLookup lib = SymbolLookup.libraryLookup(resolveDrappLib(), ARENA);
            drAvailable = h(lib, "asmtest_dr_available", FunctionDescriptor.of(JAVA_INT));
            drInit = h(lib, "asmtest_dr_init", FunctionDescriptor.of(JAVA_INT, ADDRESS));
            drStart = h(lib, "asmtest_dr_start", FunctionDescriptor.of(JAVA_INT));
            drStop = h(lib, "asmtest_dr_stop", FunctionDescriptor.of(JAVA_INT));
            drShutdown = h(lib, "asmtest_dr_shutdown", FunctionDescriptor.ofVoid());
            registerRegion = h(lib, "asmtest_dr_register_region",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS));
            unregisterRegion = h(lib, "asmtest_dr_unregister_region",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            traceBegin = h(lib, "asmtest_trace_begin", FunctionDescriptor.ofVoid(ADDRESS));
            traceEnd = h(lib, "asmtest_trace_end", FunctionDescriptor.ofVoid(ADDRESS));
            markerError = h(lib, "asmtest_dr_marker_error", FunctionDescriptor.of(JAVA_INT));
            execAlloc = h(lib, "asmtest_exec_alloc",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS));
            execFree = h(lib, "asmtest_exec_free", FunctionDescriptor.ofVoid(ADDRESS));
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
            traceBlockAt = h(lib, "asmtest_emu_trace_block_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            traceInsnAt = h(lib, "asmtest_emu_trace_insn_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
        } catch (Throwable t) {
            // The library requires DynamoRIO and may be absent — degrade to
            // available() == false rather than failing class init.
            loadError = t;
        }
        DR_AVAILABLE = drAvailable; DR_INIT = drInit; DR_START = drStart; DR_STOP = drStop;
        DR_SHUTDOWN = drShutdown; REGISTER_REGION = registerRegion;
        UNREGISTER_REGION = unregisterRegion; TRACE_BEGIN = traceBegin; TRACE_END = traceEnd;
        MARKER_ERROR = markerError; EXEC_ALLOC = execAlloc; EXEC_FREE = execFree;
        TRACE_NEW = traceNew; TRACE_FREE = traceFree; TRACE_COVERED = traceCovered;
        TRACE_BLOCKS_LEN = traceBlocksLen; TRACE_INSNS_TOTAL = traceInsnsTotal;
        TRACE_INSNS_LEN = traceInsnsLen; TRACE_BLOCK_AT = traceBlockAt;
        TRACE_INSN_AT = traceInsnAt;
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

    /** True if the tier can run (library loaded + libdynamorio resolvable). Never
     *  throws: a load failure OR {@code asmtest_dr_available() == 0} → false, so
     *  callers self-skip cleanly. */
    public static boolean available() {
        if (DR_AVAILABLE == null) return false;
        try { return (int) DR_AVAILABLE.invoke() != 0; }
        catch (Throwable t) { return false; }
    }

    /** Diagnostic for why the library failed to load, or null if it loaded. */
    public static Throwable loadError() { return LOAD_ERROR; }

    /**
     * Bring DynamoRIO up in-process and take over. {@code client} is the path to
     * libasmtest_drclient.so (null → NULL pointer → C falls back to
     * $ASMTEST_DRCLIENT); {@code dynamorioHome} lets the C side find libdynamorio
     * (else $ASMTEST_DR_LIB / rpath). Runs asmtest_dr_init then asmtest_dr_start;
     * throws RuntimeException on a nonzero status.
     */
    public static void initialize(String client, String dynamorioHome,
                                  String clientOptions, int mode) {
        if (DR_INIT == null) throw new RuntimeException("libasmtest_drapp not loaded", LOAD_ERROR);
        try {
            MemorySegment opts = ARENA.allocate(OPTIONS_LAYOUT);
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("dynamorio_home")), str(dynamorioHome));
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("client_path")), str(client));
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("client_options")), str(clientOptions));
            opts.set(JAVA_INT, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("mode")), mode);
            int rc = (int) DR_INIT.invoke(opts);
            if (rc != ASMTEST_DR_OK) throw new RuntimeException("asmtest_dr_init failed: " + rc);
            rc = (int) DR_START.invoke();
            if (rc != ASMTEST_DR_OK) throw new RuntimeException("asmtest_dr_start failed: " + rc);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Convenience: initialize with all defaults (client/home/options null, mode 0). */
    public static void initialize() { initialize(null, null, null, 0); }

    /** Tear DynamoRIO down (asmtest_dr_shutdown → back to UNINIT). */
    public static void shutdown() {
        if (DR_SHUTDOWN == null) return;
        try { DR_SHUTDOWN.invoke(); } catch (Throwable t) { throw rethrow(t); }
    }

    /** Count of illegal marker operations observed since init; 0 means balanced. */
    public static int markerError() {
        if (MARKER_ERROR == null) return 0;
        try { return (int) MARKER_ERROR.invoke(); } catch (Throwable t) { throw rethrow(t); }
    }

    /** Host-native machine code in real executable (W^X) memory. */
    public static final class NativeCode implements AutoCloseable {
        private MemorySegment code; // an asmtest_exec_code_t {base, len}

        private NativeCode(MemorySegment code) { this.code = code; }

        /** Map executable memory and copy {@code bytes} of host-native code into it. */
        public static NativeCode fromBytes(byte[] bytes) {
            if (EXEC_ALLOC == null) throw new RuntimeException("libasmtest_drapp not loaded", LOAD_ERROR);
            try {
                MemorySegment in = ARENA.allocate(Math.max(bytes.length, 1));
                MemorySegment.copy(bytes, 0, in, JAVA_BYTE, 0, bytes.length);
                MemorySegment ec = ARENA.allocate(EXEC_CODE_LAYOUT);
                int rc = (int) EXEC_ALLOC.invoke(in, (long) bytes.length, ec);
                if (rc != ASMTEST_DR_OK) throw new RuntimeException("asmtest_exec_alloc failed: " + rc);
                return new NativeCode(ec);
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Address of the executable mapping (offset 0 = entry). */
        public long base() {
            return code.get(ADDRESS, EXEC_CODE_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("base"))).address();
        }

        /** Number of code bytes. */
        public long length() {
            return code.get(JAVA_LONG, EXEC_CODE_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("len")));
        }

        /** Invoke the code through a function pointer with two C-long args, reading
         *  the result as a C long (the SysV integer ABI). */
        public long call(long a, long b) {
            try {
                MethodHandle fn = LINKER.downcallHandle(MemorySegment.ofAddress(base()),
                    FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, JAVA_LONG));
                return (long) fn.invoke(a, b);
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** Unmap the executable memory. Unregister any region over this code FIRST. */
        public void free() {
            if (code == null || EXEC_FREE == null) return;
            try { EXEC_FREE.invoke(code); } catch (Throwable t) { throw rethrow(t); }
            finally { code = null; }
        }

        @Override public void close() { free(); }
    }

    /** An app-owned coverage recorder for a registered native region. */
    public static final class NativeTrace implements AutoCloseable {
        private MemorySegment handle; // an asmtest_trace_t*

        private NativeTrace(MemorySegment handle) { this.handle = handle; }

        /** Allocate a trace recording up to {@code blocks} basic blocks and
         *  {@code instructions} ordered instructions (instruction recording only
         *  when {@code instructions > 0}). */
        public static NativeTrace create(int blocks, int instructions) {
            if (TRACE_NEW == null) throw new RuntimeException("libasmtest_drapp not loaded", LOAD_ERROR);
            try {
                // asmtest_trace_new(insns_cap, blocks_cap) — insns FIRST.
                MemorySegment h = (MemorySegment) TRACE_NEW.invoke((long) instructions, (long) blocks);
                if (MemorySegment.NULL.equals(h)) throw new RuntimeException("asmtest_trace_new failed");
                return new NativeTrace(h);
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Register a non-overlapping native code range under {@code name}, recording
         *  coverage into this trace. */
        public NativeTrace register(String name, NativeCode code) {
            try {
                int rc = (int) REGISTER_REGION.invoke(str(name),
                    MemorySegment.ofAddress(code.base()), code.length(), handle);
                if (rc != ASMTEST_DR_OK)
                    throw new RuntimeException("register_region(" + name + ") failed: " + rc);
                return this;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Drop the named region (the client drops its cached translation). */
        public void unregister(String name) {
            try { UNREGISTER_REGION.invoke(str(name)); } catch (Throwable t) { throw rethrow(t); }
        }

        /** Run {@code body} between balanced begin/end markers for {@code name}.
         *  The markers are always balanced (end runs in a finally), so a throw from
         *  the body still closes the region. */
        public void region(String name, Runnable body) {
            MemorySegment n = str(name);
            try {
                TRACE_BEGIN.invoke(n);
                try { body.run(); }
                finally { TRACE_END.invoke(n); }
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

        /** Total instructions in the recorded stream (0 unless instruction mode). */
        public long insnsTotal() {
            try { return (long) TRACE_INSNS_TOTAL.invoke(handle); }
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
