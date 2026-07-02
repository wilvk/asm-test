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
        TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL, TRACE_INSNS_LEN, TRACE_BLOCK_AT, TRACE_INSN_AT,
        REGISTER_SYMBOL, SYMBOL_DEMO;

    // The load error, kept for diagnostics; null on success.
    private static final Throwable LOAD_ERROR;

    // Absolute path of the libasmtest_drapp actually loaded; null until it loads.
    // Captured for libraryPath() so a clean-room test can assert the bundled tier —
    // not a leaked build/ tree — satisfied the load.
    private static String RESOLVED_PATH;

    // The published-jar native-payload slot, e.g. native/linux-x86_64. The payload
    // dirs follow `uname -m`: macOS arm is "arm64", Linux is "aarch64" (mirrors the
    // core loader in Asmtest.java).
    private static String bundledSlotDir() {
        boolean mac = System.getProperty("os.name", "").toLowerCase().contains("mac");
        String os = mac ? "darwin" : "linux";
        String a = System.getProperty("os.arch", "").toLowerCase();
        boolean arm = a.contains("aarch64") || a.contains("arm64");
        String arch = arm ? (mac ? "arm64" : "aarch64") : "x86_64";
        return "native/" + os + "-" + arch;
    }

    private static String libExt() {
        return System.getProperty("os.name", "").toLowerCase().contains("mac") ? "dylib" : "so";
    }

    // Extract the bundled native/<os>-<arch>/ payload from the jar to a temp dir and
    // return the absolute path of <stem>.<ext> there, or null if the jar carries no
    // such slot / library. The WHOLE slot dir is co-extracted so a lib's vendored deps
    // (@loader_path/$ORIGIN rpath) resolve next to it, mirroring Asmtest.resolveEmuLib.
    private static String bundledLib(String stem) {
        String slot = bundledSlotDir();
        String name = stem + "." + libExt();
        try {
            java.nio.file.Path tmpDir = java.nio.file.Files.createTempDirectory("asmtest-native");
            tmpDir.toFile().deleteOnExit();
            int n = extractResourceDir(slot, tmpDir);
            java.nio.file.Path lib = tmpDir.resolve(name);
            if (n == 0 || !java.nio.file.Files.exists(lib)) return null;
            lib.toFile().deleteOnExit();
            return lib.toAbsolutePath().toString();
        } catch (java.io.IOException e) {
            return null;
        }
    }

    // Extract every file directly under the jar resource dir `dir` into `tmpDir`
    // (top-level files only; the THIRD-PARTY-LICENSES subdir is skipped). Works from a
    // jar (enumerate the zip) or exploded classes on disk (copy the dir). Returns the
    // count extracted. Mirrors Asmtest.extractResourceDir.
    private static int extractResourceDir(String dir, java.nio.file.Path tmpDir)
            throws java.io.IOException {
        java.net.URL loc = DrTrace.class.getProtectionDomain().getCodeSource().getLocation();
        if (loc == null) return 0;
        java.io.File src;
        try { src = new java.io.File(loc.toURI()); }
        catch (java.net.URISyntaxException e) { src = new java.io.File(loc.getPath()); }
        int n = 0;
        if (src.isFile()) { // a jar
            try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(src)) {
                java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zip.entries();
                while (en.hasMoreElements()) {
                    java.util.zip.ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (e.isDirectory() || !name.startsWith(dir + "/")) continue;
                    String base = name.substring(dir.length() + 1);
                    if (base.isEmpty() || base.contains("/")) continue; // top-level files only
                    try (java.io.InputStream in = zip.getInputStream(e)) {
                        java.nio.file.Files.copy(in, tmpDir.resolve(base),
                            java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                    }
                    tmpDir.resolve(base).toFile().deleteOnExit();
                    n++;
                }
            }
        } else { // exploded classes directory
            java.io.File[] files = new java.io.File(src, dir).listFiles();
            if (files != null) for (java.io.File f : files) {
                if (!f.isFile()) continue;
                java.nio.file.Files.copy(f.toPath(), tmpDir.resolve(f.getName()),
                    java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                tmpDir.resolve(f.getName()).toFile().deleteOnExit();
                n++;
            }
        }
        return n;
    }

    // Resolve libasmtest_drapp, in order: an explicit ASMTEST_DRAPP_LIB wins (dev /
    // custom build); then the native payload bundled in the published jar at
    // native/<os>-<arch>/ (extracted to a temp dir); then <repo>/build/... (two levels
    // up from this source dir, matching the Python wrapper's _REPO_ROOT); then a bare
    // name for the system loader. The bundled slot is tried BEFORE the dev build/ tree
    // so an installed jar never prefers a leaked checkout. Each candidate is tried in
    // turn by the static initializer; the first that links wins.
    private static java.util.List<String> resolveDrappCandidates() {
        java.util.List<String> cands = new java.util.ArrayList<>();
        String env = System.getenv("ASMTEST_DRAPP_LIB");
        if (env != null && !env.isEmpty()) cands.add(env);
        String bundled = bundledLib("libasmtest_drapp");
        if (bundled != null) cands.add(bundled);
        String name = "libasmtest_drapp." + libExt();
        cands.add("build/" + name); // cwd-relative dev build/ (the test usually sets the env)
        cands.add(name);            // bare name → the system loader
        return cands;
    }

    /** The bundled DR client shipped alongside libasmtest_drapp, if present: honor
     *  $ASMTEST_DRCLIENT first, then the bundled native/<slot>/ payload, then the dev
     *  build/ tree. Returns null when none exists (the C side then falls back to
     *  $ASMTEST_DRCLIENT / rpath), mirroring the Python wrapper's _default_client(). */
    private static String defaultClient() {
        String env = System.getenv("ASMTEST_DRCLIENT");
        if (env != null && !env.isEmpty()) return env;
        String bundled = bundledLib("libasmtest_drclient");
        if (bundled != null) return bundled;
        java.io.File dev = new java.io.File("build/libasmtest_drclient." + libExt());
        if (dev.isFile()) return dev.getAbsolutePath();
        return null;
    }

    static {
        MethodHandle drAvailable = null, drInit = null, drStart = null, drStop = null,
            drShutdown = null, registerRegion = null, unregisterRegion = null,
            traceBegin = null, traceEnd = null, markerError = null, execAlloc = null,
            execFree = null, traceNew = null, traceFree = null, traceCovered = null,
            traceBlocksLen = null, traceInsnsTotal = null, traceInsnsLen = null,
            traceBlockAt = null, traceInsnAt = null, registerSymbol = null, symbolDemo = null;
        Throwable loadError = null;
        String resolvedPath = null;
        try {
            // Try each candidate in order; the first that links wins (bundled slot
            // before the dev build/ tree). Keep the last failure for diagnostics.
            SymbolLookup lib = null;
            for (String cand : resolveDrappCandidates()) {
                try {
                    lib = SymbolLookup.libraryLookup(cand, ARENA);
                    resolvedPath = new java.io.File(cand).getAbsolutePath();
                    break;
                } catch (RuntimeException le) {
                    // IllegalArgumentException (lib not found) or any other link failure:
                    // keep it for diagnostics and fall through to the next candidate.
                    loadError = le;
                }
            }
            if (lib == null)
                throw (loadError != null ? loadError
                    : new RuntimeException("libasmtest_drapp not found"));
            loadError = null;
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
            // Symbol mode: trace a named exported function by name (no begin/end markers).
            registerSymbol = h(lib, "asmtest_dr_register_symbol",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS));
            symbolDemo = h(lib, "asmtest_symbol_demo",
                FunctionDescriptor.of(JAVA_LONG, JAVA_LONG, JAVA_LONG));
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
        TRACE_INSN_AT = traceInsnAt; REGISTER_SYMBOL = registerSymbol; SYMBOL_DEMO = symbolDemo;
        LOAD_ERROR = loadError;
        RESOLVED_PATH = loadError == null ? resolvedPath : null;
    }

    private static MethodHandle h(SymbolLookup lk, String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(lk.find(name).orElseThrow(
            () -> new RuntimeException("missing symbol: " + name)), fd);
    }

    private static RuntimeException rethrow(Throwable t) {
        return t instanceof RuntimeException re ? re : new RuntimeException(t);
    }

    // Per-call confined Arena (try-with-resources) so the transient C-string copy is
    // freed when the call returns; the shared ARENA is reserved for the process-
    // lifetime library lookup. Otherwise every call would leak its buffer forever.
    private static MemorySegment str(Arena a, String s) {
        return s == null ? MemorySegment.NULL : a.allocateUtf8String(s);
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

    /** Absolute path of the libasmtest_drapp this process resolved, or null if it
     *  failed to load. Lets a clean-room test assert the bundled tier — not a leaked
     *  build/ tree — satisfied the load. */
    public static String libraryPath() { return RESOLVED_PATH; }

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
        // Default the client to the bundled libasmtest_drclient (honoring
        // $ASMTEST_DRCLIENT first) when the caller passes none; a still-null client is
        // a NULL pointer, so the C side falls back to $ASMTEST_DRCLIENT / rpath.
        if (client == null || client.isEmpty()) client = defaultClient();
        try (Arena a = Arena.ofConfined()) {
            MemorySegment opts = a.allocate(OPTIONS_LAYOUT);
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("dynamorio_home")), str(a, dynamorioHome));
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("client_path")), str(a, client));
            opts.set(ADDRESS, OPTIONS_LAYOUT.byteOffset(
                MemoryLayout.PathElement.groupElement("client_options")), str(a, clientOptions));
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

    /** The exported fixture (a*2+b) the symbol-mode test traces by name. */
    public static long symbolDemo(long a, long b) {
        if (SYMBOL_DEMO == null) throw new RuntimeException("libasmtest_drapp not loaded", LOAD_ERROR);
        try { return (long) SYMBOL_DEMO.invoke(a, b); } catch (Throwable t) { throw rethrow(t); }
    }

    /** Host-native machine code in real executable (W^X) memory. */
    public static final class NativeCode implements AutoCloseable {
        // The exec_code_t {base, len} struct is read by base()/length() and passed to
        // exec_free, so it must outlive fromBytes(); it gets its own Arena closed in
        // free() (bounding memory to live objects), rather than leaking from the
        // shared ARENA. Shared (not confined) to keep the original cross-thread use.
        private Arena arena;
        private MemorySegment code; // an asmtest_exec_code_t {base, len}

        private NativeCode(Arena arena, MemorySegment code) { this.arena = arena; this.code = code; }

        /** Map executable memory and copy {@code bytes} of host-native code into it. */
        public static NativeCode fromBytes(byte[] bytes) {
            if (EXEC_ALLOC == null) throw new RuntimeException("libasmtest_drapp not loaded", LOAD_ERROR);
            Arena arena = Arena.ofShared();
            try (Arena tmp = Arena.ofConfined()) {
                MemorySegment in = tmp.allocate(Math.max(bytes.length, 1));
                MemorySegment.copy(bytes, 0, in, JAVA_BYTE, 0, bytes.length);
                MemorySegment ec = arena.allocate(EXEC_CODE_LAYOUT);
                int rc = (int) EXEC_ALLOC.invoke(in, (long) bytes.length, ec);
                if (rc != ASMTEST_DR_OK) throw new RuntimeException("asmtest_exec_alloc failed: " + rc);
                return new NativeCode(arena, ec);
            } catch (RuntimeException re) { arena.close(); throw re; }
            catch (Throwable t) { arena.close(); throw rethrow(t); }
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
            finally { code = null; if (arena != null) { arena.close(); arena = null; } }
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
            try (Arena a = Arena.ofConfined()) {
                int rc = (int) REGISTER_REGION.invoke(str(a, name),
                    MemorySegment.ofAddress(code.base()), code.length(), handle);
                if (rc != ASMTEST_DR_OK)
                    throw new RuntimeException("register_region(" + name + ") failed: " + rc);
                return this;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Drop the named region (the client drops its cached translation). */
        public void unregister(String name) {
            try (Arena a = Arena.ofConfined()) { UNREGISTER_REGION.invoke(str(a, name)); } catch (Throwable t) { throw rethrow(t); }
        }

        /** Symbol mode: trace a named exported function with no begin/end markers —
         *  always-on recording for [entry, entry+maxLen) of {@code symbol}, resolved
         *  by name across all loaded modules. */
        public NativeTrace registerSymbol(String symbol, long maxLen) {
            try (Arena a = Arena.ofConfined()) {
                int rc = (int) REGISTER_SYMBOL.invoke(str(a, symbol), maxLen, handle);
                if (rc != ASMTEST_DR_OK)
                    throw new RuntimeException("register_symbol(" + symbol + ") failed: " + rc);
                return this;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Run {@code body} between balanced begin/end markers for {@code name}.
         *  The markers are always balanced (end runs in a finally), so a throw from
         *  the body still closes the region. */
        public void region(String name, Runnable body) {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment n = str(a, name);
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
