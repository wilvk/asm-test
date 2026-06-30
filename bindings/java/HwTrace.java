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
    /** ASMTEST_HW_EUNAVAIL — {@link #auto(int)} returns this when no backend is available. */
    public static final int ASMTEST_HW_EUNAVAIL = -3;

    // asmtest_trace_backend_t — the SINGLESTEP backend is the portable default.
    public static final int INTEL_PT = 0;
    public static final int CORESIGHT = 1;
    public static final int AMD_LBR = 2;
    public static final int SINGLESTEP = 3;

    // asmtest_hwtrace_policy_t — backend auto-selection policy. BEST is the most
    // faithful available backend; CEILING_FREE drops the one fixed-window backend
    // (AMD LBR) — re-resolve under it after a trace comes back truncated.
    public static final int BEST = 0;
    public static final int CEILING_FREE = 1;

    // asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
    // (hardware + DynamoRIO + emulator), not just the hardware backends above.
    // asmtest_trace_tier_t — the trace tiers, most-faithful to least.
    public static final int TIER_HWTRACE = 0;   // HW branch trace / single-step (real CPU)
    public static final int TIER_DYNAMORIO = 1; // in-process software DBI (real CPU)
    public static final int TIER_EMULATOR = 2;  // Unicorn virtual CPU (isolated guest)
    // asmtest_trace_fidelity_t — execution fidelity of a tier.
    public static final int FIDELITY_NATIVE = 0;  // runs the real bytes on the real CPU in-process
    public static final int FIDELITY_VIRTUAL = 1; // isolated guest on an emulated CPU
    // cross-tier policy bitmask. TRACE_BEST allows the emulator floor; TRACE_CEILING_FREE
    // drops the fixed-window backend (AMD LBR); TRACE_NATIVE_ONLY forbids the
    // native->emulator fidelity crossing.
    public static final int TRACE_BEST = 0x0;
    public static final int TRACE_CEILING_FREE = 0x1;
    public static final int TRACE_NATIVE_ONLY = 0x2;

    // asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
    /** ASMTEST_PTRACE_OK — the success status returned by the ptrace-tier calls. */
    public static final int ASMTEST_PTRACE_OK = 0;
    /** ASMTEST_PTRACE_ENOENT — region / symbol / method not found. */
    public static final int ASMTEST_PTRACE_ENOENT = -7;

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

    // asmtest_trace_choice_t {int tier; int backend; int fidelity;} — three int-sized
    // enum fields, no padding (pinned by a static_assert in the header), so a choice
    // marshals as three consecutive ints (12 bytes).
    private static final int CHOICE_INTS = 3;

    /** A resolved cross-tier trace option: which {@code tier} to use, which hardware
     *  {@code backend} within it (meaningful only when {@code tier == TIER_HWTRACE}),
     *  and the {@code fidelity} class ({@link #FIDELITY_NATIVE} vs
     *  {@link #FIDELITY_VIRTUAL}). Mirrors {@code asmtest_trace_choice_t}. */
    public record TierChoice(int tier, int backend, int fidelity) {}

    /** A JIT method resolved from a jitdump (asmtest_jitdump_entry_t): its load
     *  address ({@code codeAddr}, the base to trace), {@code codeSize}, the JIT's
     *  {@code timestamp}/{@code codeIndex}, and — unlike the text perf-map — the
     *  actual recorded native {@code code} bytes (empty when none were requested).
     *  The entry struct is four consecutive {@code JAVA_LONG} (uint64). */
    public record JitMethod(long codeAddr, long codeSize, long timestamp,
                            long codeIndex, byte[] code) {}

    // Resolved when the library loads; null when it can't (then available() == false).
    private static final MethodHandle HW_AVAILABLE, HW_SKIP_REASON, HW_RESOLVE, HW_AUTO,
        TRACE_RESOLVE, TRACE_AUTO,
        HW_INIT, HW_SHUTDOWN,
        REGISTER_REGION, HW_BEGIN, HW_END, EXEC_ALLOC, EXEC_FREE,
        TRACE_NEW, TRACE_FREE, TRACE_COVERED, TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL,
        TRACE_INSNS_LEN, TRACE_TRUNCATED, TRACE_BLOCK_AT, TRACE_INSN_AT,
        // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
        PTRACE_AVAILABLE, PTRACE_SKIP_REASON, PTRACE_TRACE_CALL, PTRACE_TRACE_ATTACHED,
        PROC_REGION_BY_ADDR, PROC_PERFMAP_SYMBOL, JITDUMP_FIND;

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
        MethodHandle hwAvailable = null, hwSkipReason = null, hwResolve = null, hwAuto = null,
            traceResolve = null, traceAuto = null,
            hwInit = null, hwShutdown = null,
            registerRegion = null, hwBegin = null, hwEnd = null, execAlloc = null, execFree = null,
            traceNew = null, traceFree = null, traceCovered = null, traceBlocksLen = null,
            traceInsnsTotal = null, traceInsnsLen = null, traceTruncated = null,
            traceBlockAt = null, traceInsnAt = null,
            ptraceAvailable = null, ptraceSkipReason = null, ptraceTraceCall = null,
            ptraceTraceAttached = null, procRegionByAddr = null, procPerfmapSymbol = null,
            jitdumpFind = null;
        Throwable loadError = null;
        try {
            SymbolLookup lib = SymbolLookup.libraryLookup(resolveHwtraceLib(), ARENA);
            hwAvailable = h(lib, "asmtest_hwtrace_available", FunctionDescriptor.of(JAVA_INT, JAVA_INT));
            hwSkipReason = h(lib, "asmtest_hwtrace_skip_reason",
                FunctionDescriptor.ofVoid(JAVA_INT, ADDRESS, JAVA_LONG));
            // asmtest_hwtrace_resolve(policy, out, cap) — writes cap backend ints into out,
            // returns the count (size_t); asmtest_hwtrace_auto(policy) — the single best int.
            hwResolve = h(lib, "asmtest_hwtrace_resolve",
                FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG));
            hwAuto = h(lib, "asmtest_hwtrace_auto", FunctionDescriptor.of(JAVA_INT, JAVA_INT));
            // Cross-tier orchestrator (asmtest_trace_auto.h): asmtest_trace_resolve(policy,
            // out, cap) writes cap choice-structs into out and returns the count (size_t);
            // asmtest_trace_auto(policy, out) writes the single best choice, returns 0/EUNAVAIL.
            traceResolve = h(lib, "asmtest_trace_resolve",
                FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG));
            traceAuto = h(lib, "asmtest_trace_auto",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS));
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
            // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit. pid is
            // a C int (JAVA_INT); the long* args/result are ADDRESS to scratch segments.
            ptraceAvailable = h(lib, "asmtest_ptrace_available",
                FunctionDescriptor.of(JAVA_INT));
            ptraceSkipReason = h(lib, "asmtest_ptrace_skip_reason",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
            // asmtest_ptrace_trace_call(code, len, args, nargs, result, trace).
            ptraceTraceCall = h(lib, "asmtest_ptrace_trace_call",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT,
                    ADDRESS, ADDRESS));
            // asmtest_ptrace_trace_attached(pid, base, len, result, trace).
            ptraceTraceAttached = h(lib, "asmtest_ptrace_trace_attached",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    ADDRESS));
            // asmtest_proc_region_by_addr(pid, addr, base_out, len_out).
            procRegionByAddr = h(lib, "asmtest_proc_region_by_addr",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, ADDRESS));
            // asmtest_proc_perfmap_symbol(pid, name, base_out, len_out).
            procPerfmapSymbol = h(lib, "asmtest_proc_perfmap_symbol",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, ADDRESS));
            // asmtest_jitdump_find(path, pid, name, out, bytes_out, bytes_cap, bytes_len).
            jitdumpFind = h(lib, "asmtest_jitdump_find",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT, ADDRESS, ADDRESS,
                    ADDRESS, JAVA_LONG, ADDRESS));
        } catch (Throwable t) {
            // The library may be absent (not built) — degrade to available() == false
            // rather than failing class init.
            loadError = t;
        }
        HW_AVAILABLE = hwAvailable; HW_SKIP_REASON = hwSkipReason; HW_RESOLVE = hwResolve;
        HW_AUTO = hwAuto; TRACE_RESOLVE = traceResolve; TRACE_AUTO = traceAuto; HW_INIT = hwInit;
        HW_SHUTDOWN = hwShutdown; REGISTER_REGION = registerRegion; HW_BEGIN = hwBegin;
        HW_END = hwEnd; EXEC_ALLOC = execAlloc; EXEC_FREE = execFree; TRACE_NEW = traceNew;
        TRACE_FREE = traceFree; TRACE_COVERED = traceCovered; TRACE_BLOCKS_LEN = traceBlocksLen;
        TRACE_INSNS_TOTAL = traceInsnsTotal; TRACE_INSNS_LEN = traceInsnsLen;
        TRACE_TRUNCATED = traceTruncated; TRACE_BLOCK_AT = traceBlockAt; TRACE_INSN_AT = traceInsnAt;
        PTRACE_AVAILABLE = ptraceAvailable; PTRACE_SKIP_REASON = ptraceSkipReason;
        PTRACE_TRACE_CALL = ptraceTraceCall; PTRACE_TRACE_ATTACHED = ptraceTraceAttached;
        PROC_REGION_BY_ADDR = procRegionByAddr; PROC_PERFMAP_SYMBOL = procPerfmapSymbol;
        JITDUMP_FIND = jitdumpFind;
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

    /** This host's hardware-trace fallback cascade: the available backend enums,
     *  most-faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring
     *  {@code policy}. Empty only off x86-64 Linux (single-step is the floor there).
     *  {@code CEILING_FREE} drops the depth-bounded backend (AMD LBR). */
    public static int[] resolve(int policy) {
        if (HW_RESOLVE == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            // up to 4 backend ints. A sequence layout sizes by element count
            // unambiguously (ARENA.allocate(JAVA_INT, n) sizes to one int on this
            // JDK, which only ever sufficed because this host exposes a single
            // backend — it would truncate on a multi-backend Intel-PT host).
            MemorySegment out = ARENA.allocate(MemoryLayout.sequenceLayout(4, JAVA_INT));
            int n = (int) (long) HW_RESOLVE.invoke(policy, out, 4L);
            int[] backends = new int[n];
            for (int i = 0; i < n; i++) backends[i] = out.getAtIndex(JAVA_INT, i);
            return backends;
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** The single most-preferred available backend enum under {@code policy} (>= 0,
     *  ready to {@link #init(int)}), or {@link #ASMTEST_HW_EUNAVAIL} (-3) when no
     *  hardware-trace backend is available on this host. */
    public static int auto(int policy) {
        if (HW_AUTO == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try { return (int) HW_AUTO.invoke(policy); }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** This host's full CROSS-TIER cascade (asmtest_trace_resolve), most-faithful
     *  first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
     *  emulator, each included only if its tier is available. {@code TRACE_NATIVE_ONLY}
     *  drops the emulator floor (no native->emulator fidelity crossing);
     *  {@code TRACE_CEILING_FREE} drops AMD LBR. */
    public static java.util.List<TierChoice> resolveTiers(int policy) {
        if (TRACE_RESOLVE == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            final int cap = 8; // the cascade is at most 6 entries; headroom
            // 3 consecutive JAVA_INT per choice (asmtest_trace_choice_t, no padding).
            MemorySegment out = ARENA.allocate(
                MemoryLayout.sequenceLayout((long) CHOICE_INTS * cap, JAVA_INT));
            int n = (int) (long) TRACE_RESOLVE.invoke(policy, out, (long) cap);
            java.util.List<TierChoice> choices = new java.util.ArrayList<>(n);
            for (int i = 0; i < n; i++) {
                int base = i * CHOICE_INTS;
                choices.add(new TierChoice(out.getAtIndex(JAVA_INT, base),
                    out.getAtIndex(JAVA_INT, base + 1), out.getAtIndex(JAVA_INT, base + 2)));
            }
            return choices;
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** The single most-preferred available cross-tier choice under {@code policy}
     *  (asmtest_trace_auto), or an empty {@link java.util.Optional} on EUNAVAIL (only
     *  off a native host under {@code TRACE_NATIVE_ONLY}). */
    public static java.util.Optional<TierChoice> autoTier(int policy) {
        if (TRACE_AUTO == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            MemorySegment out = ARENA.allocate(
                MemoryLayout.sequenceLayout(CHOICE_INTS, JAVA_INT));
            int rc = (int) TRACE_AUTO.invoke(policy, out);
            if (rc != ASMTEST_HW_OK) return java.util.Optional.empty();
            return java.util.Optional.of(new TierChoice(out.getAtIndex(JAVA_INT, 0),
                out.getAtIndex(JAVA_INT, 1), out.getAtIndex(JAVA_INT, 2)));
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

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

        /** The underlying {@code asmtest_trace_t*} handle — the recording target the
         *  ptrace-tier calls fill (mirrors the Python wrapper's {@code trace._handle}). */
        public MemorySegment handle() { return handle; }

        public void free() {
            if (handle == null || TRACE_FREE == null) return;
            try { TRACE_FREE.invoke(handle); } catch (Throwable t) { throw rethrow(t); }
            finally { handle = null; }
        }

        @Override public void close() { free(); }
    }

    // ---- Out-of-process / foreign-process tracing toolkit (asmtest_ptrace.h) ----
    //
    // Single-step a forked or externally-attached target OUT OF BAND, and resolve the
    // code region to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a binary
    // jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is
    // unavailable and in-process DynamoRIO cannot seize the runtime's threads). Linux
    // x86-64. Mirrors the Python wrapper's asmtest.hwtrace.Ptrace class.

    /** True if the out-of-process single-step tracer can run on this host (Linux
     *  x86-64). Never throws: a load failure or a zero return → false. */
    public static boolean ptraceAvailable() {
        if (PTRACE_AVAILABLE == null) return false;
        try { return (int) PTRACE_AVAILABLE.invoke() != 0; }
        catch (Throwable t) { return false; }
    }

    /** Human-readable reason {@link #ptraceAvailable()} is false (or "available"). */
    public static String ptraceSkipReason() {
        if (PTRACE_SKIP_REASON == null) {
            Throwable e = LOAD_ERROR;
            return "libasmtest_hwtrace not loaded" + (e != null ? ": " + e : "");
        }
        try {
            MemorySegment buf = ARENA.allocate(160);
            PTRACE_SKIP_REASON.invoke(buf, 160L);
            return buf.getUtf8String(0);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** Fork a tracee that calls {@code code} ({@code len} bytes of host-native code
     *  already executable at that address — e.g. {@link NativeCode#base()}) with the
     *  first {@code args.length} (0..6) integer arguments, single-step it OUT OF
     *  PROCESS, and fill {@code trace} (a {@link NativeTrace#handle()}); returns the
     *  routine's return value (the child's RAX at the ret). Throws on a nonzero status. */
    public static long ptraceTraceCall(MemorySegment code, long len, long[] args,
                                       MemorySegment trace) {
        if (PTRACE_TRACE_CALL == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            int n = args.length;
            // Per-call scratch: the args[] (size to element COUNT via a sequence layout,
            // NOT allocate(JAVA_LONG, n) which sizes to one element on this JDK) and a
            // long* result cell.
            MemorySegment argSeg = ARENA.allocate(
                MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
            for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
            MemorySegment result = ARENA.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_CALL.invoke(code, len, argSeg, n, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_call failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Trace a region {@code [base, base+len)} in a SEPARATE, already-ptrace-stopped
     *  process {@code pid} (the caller owns PTRACE_ATTACH/DETACH); reads the target's
     *  bytes via process_vm_readv. Returns the target's RAX at the ret. Throws on a
     *  nonzero status. The foreign / managed-runtime path. */
    public static long ptraceTraceAttached(int pid, MemorySegment base, long len,
                                           MemorySegment trace) {
        if (PTRACE_TRACE_ATTACHED == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            MemorySegment result = ARENA.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED.invoke(pid, base, len, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** The executable mapping in /proc/&lt;pid&gt;/maps that CONTAINS {@code addr},
     *  as {@code {base, len}}, or {@code null} if no executable mapping contains it. */
    public static long[] procRegionByAddr(int pid, long addr) {
        if (PROC_REGION_BY_ADDR == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            MemorySegment baseOut = ARENA.allocate(ADDRESS); // void**
            MemorySegment lenOut = ARENA.allocate(JAVA_LONG); // size_t*
            int rc = (int) PROC_REGION_BY_ADDR.invoke(pid, MemorySegment.ofAddress(addr),
                baseOut, lenOut);
            if (rc != ASMTEST_PTRACE_OK) return null;
            return new long[] { baseOut.get(ADDRESS, 0).address(), lenOut.get(JAVA_LONG, 0) };
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** A JIT method by {@code name} in the perf map at /tmp/perf-&lt;pid&gt;.map, as
     *  {@code {base, len}}, or {@code null} when no such symbol / no map file. */
    public static long[] procPerfmapSymbol(int pid, String name) {
        if (PROC_PERFMAP_SYMBOL == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            MemorySegment baseOut = ARENA.allocate(ADDRESS); // void**
            MemorySegment lenOut = ARENA.allocate(JAVA_LONG); // size_t*
            int rc = (int) PROC_PERFMAP_SYMBOL.invoke(pid, str(name), baseOut, lenOut);
            if (rc != ASMTEST_PTRACE_OK) return null;
            return new long[] { baseOut.get(ADDRESS, 0).address(), lenOut.get(JAVA_LONG, 0) };
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Resolve a JIT method by {@code name} from a binary jitdump ({@code path}, or
     *  /tmp/jit-&lt;pid&gt;.dump when {@code path} is null) to its load address/size,
     *  the JIT's timestamp/index, and up to {@code wantBytes} of the recorded native
     *  code, as a {@link JitMethod}; empty {@link java.util.Optional} when no such
     *  method / no file. The latest re-JIT body (highest timestamp) wins. */
    public static java.util.Optional<JitMethod> jitdumpFind(String path, String name,
                                                            int pid, int wantBytes) {
        if (JITDUMP_FIND == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            // asmtest_jitdump_entry_t — four consecutive JAVA_LONG (uint64), 32 bytes.
            MemorySegment out = ARENA.allocate(
                MemoryLayout.sequenceLayout(4, JAVA_LONG));
            // bytes_out: size to wantBytes (sequence layout, NOT allocate(JAVA_BYTE, n));
            // bytes_len: size_t*. Both NULL when no bytes are requested.
            MemorySegment bytesOut = wantBytes > 0
                ? ARENA.allocate(MemoryLayout.sequenceLayout(wantBytes, JAVA_BYTE))
                : MemorySegment.NULL;
            MemorySegment bytesLen = wantBytes > 0 ? ARENA.allocate(JAVA_LONG)
                : MemorySegment.NULL;
            int rc = (int) JITDUMP_FIND.invoke(str(path), pid, str(name), out, bytesOut,
                (long) wantBytes, bytesLen);
            if (rc != ASMTEST_PTRACE_OK) return java.util.Optional.empty();
            long codeAddr = out.getAtIndex(JAVA_LONG, 0);
            long codeSize = out.getAtIndex(JAVA_LONG, 1);
            long timestamp = out.getAtIndex(JAVA_LONG, 2);
            long codeIndex = out.getAtIndex(JAVA_LONG, 3);
            byte[] code;
            if (wantBytes > 0) {
                int blen = (int) bytesLen.get(JAVA_LONG, 0);
                code = new byte[blen];
                MemorySegment.copy(bytesOut, JAVA_BYTE, 0, code, 0, blen);
            } else {
                code = new byte[0];
            }
            return java.util.Optional.of(
                new JitMethod(codeAddr, codeSize, timestamp, codeIndex, code));
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }
}
