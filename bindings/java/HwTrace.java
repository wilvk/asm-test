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
 * FFM is final since JDK 22: compile with `--release 22`, run with
 * `--enable-native-access=ALL-UNNAMED`.
 */
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

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

    // asmtest_descent_level_t — call-descent policy (see the Descent class). Decides
    // what the ptrace stepper does at each call-out it would otherwise step over.
    /** DESCENT_OFF — step over, record nothing (today's default). */
    public static final int DESCENT_OFF = 0;
    /** DESCENT_RECORD_EDGES — record (call-site -> callee) edges, still step over. */
    public static final int DESCENT_RECORD_EDGES = 1;
    /** DESCENT_DESCEND_KNOWN — step INTO resolvable calls (allow-set / resolver). */
    public static final int DESCENT_DESCEND_KNOWN = 2;
    /** DESCENT_DESCEND_ALL — step INTO everything (denylist + budget + watchdog gated). */
    public static final int DESCENT_DESCEND_ALL = 3;

    // asmtest_codeimage.h — time-aware code-image recorder status codes / event kinds.
    /** ASMTEST_CI_OK — the success status returned by the code-image calls. */
    public static final int ASMTEST_CI_OK = 0;
    /** ASMTEST_CI_ENOENT — address never tracked / no version at-or-before {@code when}. */
    public static final int ASMTEST_CI_ENOENT = -7;
    /** ASMTEST_CI_KIND_MPROTECT — mprotect(...PROT_EXEC...), the common JIT edge. */
    public static final int ASMTEST_CI_KIND_MPROTECT = 1;
    /** ASMTEST_CI_KIND_MMAP — mmap(...PROT_EXEC...); addr is the real base. */
    public static final int ASMTEST_CI_KIND_MMAP = 2;
    /** ASMTEST_CI_KIND_MEMFD — memfd_create staging hint; correlate via fd. */
    public static final int ASMTEST_CI_KIND_MEMFD = 3;

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

    /** The outcome of {@link #callScoped}: the call's {@code result} (the SysV integer
     *  return), the executed body's rendered disassembly ({@code path}), the thread-scope
     *  {@code truncated} honesty bit, and the raw {@code rc}. On a clean self-skip (no
     *  single-step backend) {@code rc} is negative, {@code result} 0 and {@code path} empty;
     *  {@link #ok()} distinguishes it. Mirrors the Python {@code CallScopedResult}. */
    public record CallScopedResult(long result, String path, boolean truncated, int rc) {
        /** True when the traced call actually ran (rc == ASMTEST_HW_OK). */
        public boolean ok() { return rc == ASMTEST_HW_OK; }
    }

    /** Outcome of {@link #window}: the rendered ABSOLUTE-address disassembly ({@code path},
     *  honest-but-noisy — the FFI dispatch + JVM harness are included), the §Z4 thread-scope
     *  {@code truncated} bit (also set when the managed window overflows its buffer), the
     *  captured ABSOLUTE instruction addresses ({@code insns}), and the raw {@code rc}.
     *  Self-skip (no single-step backend) =&gt; rc negative, path empty, insns empty. */
    public record WindowResult(String path, boolean truncated, long[] insns, int rc) {
        /** True when begin_window armed (rc == ASMTEST_HW_OK). */
        public boolean armed() { return rc == ASMTEST_HW_OK; }
    }

    /** Outcome of {@link #stealthTrace}: the leaf's {@code result} (read from the caller's
     *  RAX at the {@code ret}), the executed body's region-RELATIVE instruction {@code offsets},
     *  the basic-block {@code count}, the {@code truncated} honesty bit, and the native {@code rc}
     *  ({@link #ASMTEST_HW_OK} when the reverse-attach armed; negative on a clean self-skip). */
    public record StealthResult(long result, long[] offsets, int blocks, boolean truncated, int rc) {
        /** True when the reverse-attached helper stepped the region (rc == ASMTEST_HW_OK). */
        public boolean armed() { return rc == ASMTEST_HW_OK; }
    }

    // Resolved when the library loads; null when it can't (then available() == false).
    private static final MethodHandle HW_AVAILABLE, HW_SKIP_REASON, HW_RESOLVE, HW_AUTO,
        TRACE_RESOLVE, TRACE_AUTO,
        HW_INIT, HW_SHUTDOWN,
        REGISTER_REGION, HW_BEGIN, HW_END, HW_TRY_BEGIN, HW_RENDER,
        CALL_SCOPED_EX, RENDER_SCOPE, BEGIN_WINDOW, END_WINDOW, RENDER_WINDOW, STEALTH_TRACE,
        EXEC_ALLOC, EXEC_FREE,
        TRACE_NEW, TRACE_FREE, TRACE_COVERED, TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL,
        TRACE_INSNS_LEN, TRACE_TRUNCATED, TRACE_BLOCK_AT, TRACE_INSN_AT,
        // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
        PTRACE_AVAILABLE, PTRACE_SKIP_REASON, PTRACE_TRACE_CALL, PTRACE_TRACE_ATTACHED,
        PTRACE_BLOCKSTEP_AVAILABLE, PTRACE_TRACE_CALL_BLOCKSTEP,
        PTRACE_TRACE_ATTACHED_BLOCKSTEP,
        PTRACE_TRACE_ATTACHED_VERSIONED, PTRACE_RUN_TO,
        PROC_REGION_BY_ADDR, PROC_PERFMAP_SYMBOL, JITDUMP_FIND,
        // asmtest_codeimage.h — time-aware code-image recorder (a userspace TEXT_POKE).
        CI_AVAILABLE, CI_SKIP_REASON, CI_NEW, CI_FREE, CI_TRACK, CI_REFRESH, CI_NOW,
        CI_BYTES_AT, CI_BPF_AVAILABLE, CI_BPF_SKIP_REASON, CI_WATCH_BPF, CI_POLL_BPF,
        CI_NEXT;

    // asmtest_ptrace.h — call descent (asmtest_descent_t): the (call-site -> callee)
    // edge recorder + nested-frame stepper threaded through the ptrace loop, plus the
    // three descending trace entry points (_ex). Read via the opaque-handle accessors.
    private static final MethodHandle DESCENT_NEW, DESCENT_FREE, DESCENT_SET_MAX_DEPTH,
        DESCENT_SET_INSN_BUDGET, DESCENT_SET_WATCHDOG_MS, DESCENT_ALLOW_REGION,
        DESCENT_USE_DEFAULT_DENYLIST,
        DESCENT_DENY_REGION, DESCENT_SET_RESOLVER, DESCENT_SET_DENYLIST,
        DESCENT_EDGES_LEN, DESCENT_EDGE_SITE, DESCENT_EDGE_TARGET, DESCENT_EDGE_DEPTH,
        DESCENT_FRAMES_LEN, DESCENT_FRAME_BASE, DESCENT_FRAME_LEN, DESCENT_FRAME_DEPTH,
        DESCENT_FRAME_PARENT, DESCENT_FRAME_INSN_COUNT, DESCENT_FRAME_INSN_AT,
        DESCENT_FRAME_BLOCK_COUNT, DESCENT_FRAME_BLOCK_AT, DESCENT_TRUNCATED,
        DESCENT_DEPTH_CAPPED, PTRACE_TRACE_CALL_EX, PTRACE_TRACE_ATTACHED_EX,
        PTRACE_TRACE_ATTACHED_VERSIONED_EX;

    // The load error, kept for diagnostics; null on success.
    private static final Throwable LOAD_ERROR;

    // Absolute path of the libasmtest_hwtrace actually loaded; null until it loads.
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
        java.net.URL loc = HwTrace.class.getProtectionDomain().getCodeSource().getLocation();
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

    // Resolve libasmtest_hwtrace, in order: an explicit ASMTEST_HWTRACE_LIB wins (dev /
    // custom build); then the native payload bundled in the published jar at
    // native/<os>-<arch>/ (extracted to a temp dir); then <repo>/build/... ; then a
    // bare name for the system loader. The bundled slot is tried BEFORE the dev build/
    // tree so an installed jar never prefers a leaked checkout. Each candidate is tried
    // in turn by the static initializer; the first that links wins.
    private static java.util.List<String> resolveHwtraceCandidates() {
        java.util.List<String> cands = new java.util.ArrayList<>();
        String env = System.getenv("ASMTEST_HWTRACE_LIB");
        if (env != null && !env.isEmpty()) cands.add(env);
        String bundled = bundledLib("libasmtest_hwtrace");
        if (bundled != null) cands.add(bundled);
        String name = "libasmtest_hwtrace." + libExt();
        cands.add("build/" + name); // cwd-relative dev build/ (the test usually sets the env)
        cands.add(name);            // bare name → the system loader
        return cands;
    }

    static {
        MethodHandle hwAvailable = null, hwSkipReason = null, hwResolve = null, hwAuto = null,
            traceResolve = null, traceAuto = null,
            hwInit = null, hwShutdown = null,
            registerRegion = null, hwBegin = null, hwEnd = null, hwTryBegin = null, hwRender = null,
            callScopedEx = null, renderScope = null,
            beginWindow = null, endWindow = null, renderWindow = null, stealthTrace = null,
            execAlloc = null, execFree = null,
            traceNew = null, traceFree = null, traceCovered = null, traceBlocksLen = null,
            traceInsnsTotal = null, traceInsnsLen = null, traceTruncated = null,
            traceBlockAt = null, traceInsnAt = null,
            ptraceAvailable = null, ptraceSkipReason = null, ptraceTraceCall = null,
            ptraceBlockstepAvailable = null, ptraceTraceCallBlockstep = null,
            ptraceTraceAttachedBlockstep = null,
            ptraceTraceAttached = null, ptraceTraceAttachedVersioned = null, ptraceRunTo = null,
            procRegionByAddr = null, procPerfmapSymbol = null,
            jitdumpFind = null,
            ciAvailable = null, ciSkipReason = null, ciNew = null, ciFree = null,
            ciTrack = null, ciRefresh = null, ciNow = null, ciBytesAt = null,
            ciBpfAvailable = null, ciBpfSkipReason = null, ciWatchBpf = null,
            ciPollBpf = null, ciNext = null,
            descentNew = null, descentFree = null, descentSetMaxDepth = null,
            descentSetInsnBudget = null, descentSetWatchdogMs = null, descentAllowRegion = null,
            descentUseDefaultDenylist = null,
            descentDenyRegion = null, descentSetResolver = null, descentSetDenylist = null,
            descentEdgesLen = null, descentEdgeSite = null, descentEdgeTarget = null,
            descentEdgeDepth = null, descentFramesLen = null, descentFrameBase = null,
            descentFrameLen = null, descentFrameDepth = null, descentFrameParent = null,
            descentFrameInsnCount = null, descentFrameInsnAt = null, descentFrameBlockCount = null,
            descentFrameBlockAt = null, descentTruncated = null, descentDepthCapped = null,
            ptraceTraceCallEx = null, ptraceTraceAttachedEx = null,
            ptraceTraceAttachedVersionedEx = null;
        Throwable loadError = null;
        String resolvedPath = null;
        try {
            // Try each candidate in order; the first that links wins (bundled slot
            // before the dev build/ tree). Keep the last failure for diagnostics.
            SymbolLookup lib = null;
            for (String cand : resolveHwtraceCandidates()) {
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
                    : new RuntimeException("libasmtest_hwtrace not found"));
            loadError = null;
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
            // Scoped-tracing shared core (§0/§1): error-returning begin + render-on-close.
            hwTryBegin = h(lib, "asmtest_hwtrace_try_begin", FunctionDescriptor.of(JAVA_INT, ADDRESS));
            hwRender = h(lib, "asmtest_hwtrace_render",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
            // Registry-free lazy-arm call + handle-keyed render (call_scoped path).
            // call_scoped_ex(base, len, trace, fn, args, nargs, result_out, scope_out).
            callScopedEx = h(lib, "asmtest_hwtrace_call_scoped_ex",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, ADDRESS,
                    ADDRESS, JAVA_INT, ADDRESS, ADDRESS));
            // The by-value 8-byte asmtest_hwtrace_scope_t {u32 idx, u32 gen} passes in one
            // integer register on SysV x86-64 — ABI-identical to a JAVA_LONG, so declare it
            // as a packed long (idx | gen<<32) and avoid a by-value struct descriptor.
            renderScope = h(lib, "asmtest_hwtrace_render_scope",
                FunctionDescriptor.of(JAVA_INT, JAVA_LONG, ADDRESS, JAVA_LONG));
            // §Z0/§Z1 region-free whole-window: begin(trace*, scope* out); end(scope BY
            // VALUE as packed long, trace*); render(scope BY VALUE, buf, buflen).
            beginWindow = h(lib, "asmtest_hwtrace_begin_window",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
            endWindow = h(lib, "asmtest_hwtrace_end_window",
                FunctionDescriptor.of(JAVA_INT, JAVA_LONG, ADDRESS));
            renderWindow = h(lib, "asmtest_hwtrace_render_window",
                FunctionDescriptor.of(JAVA_INT, JAVA_LONG, ADDRESS, JAVA_LONG));
            // §D3 concealed out-of-process ptrace-stealth stepper: stealth_trace(base, len,
            // trace*, result_out*, run_region fnptr, arg). The helper child steps the region
            // out of band while run_region runs it — no TF on the calling thread.
            stealthTrace = h(lib, "asmtest_hwtrace_stealth_trace",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, ADDRESS, ADDRESS, ADDRESS));
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
            // BTF block-step tier: same shapes as the per-instruction pair above.
            ptraceBlockstepAvailable = h(lib, "asmtest_ptrace_blockstep_available",
                FunctionDescriptor.of(JAVA_INT));
            ptraceTraceCallBlockstep = h(lib, "asmtest_ptrace_trace_call_blockstep",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT,
                    ADDRESS, ADDRESS));
            // asmtest_ptrace_trace_attached(pid, base, len, result, trace).
            ptraceTraceAttached = h(lib, "asmtest_ptrace_trace_attached",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    ADDRESS));
            // asmtest_ptrace_trace_attached_blockstep(pid, base, len, result, trace).
            ptraceTraceAttachedBlockstep = h(lib, "asmtest_ptrace_trace_attached_blockstep",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    ADDRESS));
            // asmtest_ptrace_trace_attached_versioned(pid, base, len, img, when, result,
            // trace) — like trace_attached but decode against a code-image timeline. img
            // is the asmtest_codeimage_t* (NULL => exactly trace_attached); when is u64.
            ptraceTraceAttachedVersioned = h(lib, "asmtest_ptrace_trace_attached_versioned",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    JAVA_LONG, ADDRESS, ADDRESS));
            // asmtest_ptrace_run_to(pid, addr).
            ptraceRunTo = h(lib, "asmtest_ptrace_run_to",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS));
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
            // asmtest_codeimage.h — time-aware code-image recorder. pid is a C int
            // (JAVA_INT); the timeline handle is an opaque asmtest_codeimage_t* (ADDRESS).
            ciAvailable = h(lib, "asmtest_codeimage_available",
                FunctionDescriptor.of(JAVA_INT));
            ciSkipReason = h(lib, "asmtest_codeimage_skip_reason",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
            // asmtest_codeimage_new(pid) -> asmtest_codeimage_t*; _free(img).
            ciNew = h(lib, "asmtest_codeimage_new",
                FunctionDescriptor.of(ADDRESS, JAVA_INT));
            ciFree = h(lib, "asmtest_codeimage_free", FunctionDescriptor.ofVoid(ADDRESS));
            // asmtest_codeimage_track(img, base, len); _refresh(img); _now(img) -> u64.
            ciTrack = h(lib, "asmtest_codeimage_track",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
            ciRefresh = h(lib, "asmtest_codeimage_refresh",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            ciNow = h(lib, "asmtest_codeimage_now",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            // asmtest_codeimage_bytes_at(img, addr, when, out, out_len) — out is a
            // const uint8_t** (ADDRESS to a pointer cell), out_len a size_t* (ADDRESS).
            ciBytesAt = h(lib, "asmtest_codeimage_bytes_at",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG, ADDRESS,
                    ADDRESS));
            // Optional eBPF emission detector (Phase C).
            ciBpfAvailable = h(lib, "asmtest_codeimage_bpf_available",
                FunctionDescriptor.of(JAVA_INT));
            ciBpfSkipReason = h(lib, "asmtest_codeimage_bpf_skip_reason",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
            ciWatchBpf = h(lib, "asmtest_codeimage_watch_bpf",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            ciPollBpf = h(lib, "asmtest_codeimage_poll_bpf",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));
            // asmtest_codeimage_next(img, out) — out is the 40-byte event struct (ADDRESS).
            ciNext = h(lib, "asmtest_codeimage_next",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS));
            // asmtest_ptrace.h — call descent (asmtest_descent_t). The handle is an opaque
            // void* (ADDRESS); uint32/int32 marshal as JAVA_INT, uint64/size_t as JAVA_LONG.
            // asmtest_descent_new(level) -> descent*; _free(d).
            descentNew = h(lib, "asmtest_descent_new",
                FunctionDescriptor.of(ADDRESS, JAVA_INT));
            descentFree = h(lib, "asmtest_descent_free", FunctionDescriptor.ofVoid(ADDRESS));
            // Configuration setters (void). max_depth/watchdog_ms are uint32; insn_budget u64.
            descentSetMaxDepth = h(lib, "asmtest_descent_set_max_depth",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_INT));
            descentSetInsnBudget = h(lib, "asmtest_descent_set_insn_budget",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
            descentSetWatchdogMs = h(lib, "asmtest_descent_set_watchdog_ms",
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_INT));
            descentUseDefaultDenylist = h(lib, "asmtest_descent_use_default_denylist",
                FunctionDescriptor.ofVoid(ADDRESS));
            // allow/deny_region(d, base, len) -> int. base is a const void* (ADDRESS).
            descentAllowRegion = h(lib, "asmtest_descent_allow_region",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
            descentDenyRegion = h(lib, "asmtest_descent_deny_region",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, ADDRESS, JAVA_LONG));
            // set_resolver/set_denylist(d, fn, user): fn is a native function pointer we
            // materialize as an upcall stub (see Descent.setResolver/setDenylist); user is
            // an opaque void*. Both are ADDRESS.
            descentSetResolver = h(lib, "asmtest_descent_set_resolver",
                FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS));
            descentSetDenylist = h(lib, "asmtest_descent_set_denylist",
                FunctionDescriptor.ofVoid(ADDRESS, ADDRESS, ADDRESS));
            // Read accessors — one scalar per call. edge_target / frame_base are ABSOLUTE
            // addresses (uint64 -> JAVA_LONG); frame_parent is int32 (-1 = root).
            descentEdgesLen = h(lib, "asmtest_descent_edges_len",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            descentEdgeSite = h(lib, "asmtest_descent_edge_site",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentEdgeTarget = h(lib, "asmtest_descent_edge_target",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentEdgeDepth = h(lib, "asmtest_descent_edge_depth",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
            descentFramesLen = h(lib, "asmtest_descent_frames_len",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
            descentFrameBase = h(lib, "asmtest_descent_frame_base",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentFrameLen = h(lib, "asmtest_descent_frame_len",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentFrameDepth = h(lib, "asmtest_descent_frame_depth",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
            descentFrameParent = h(lib, "asmtest_descent_frame_parent",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG));
            descentFrameInsnCount = h(lib, "asmtest_descent_frame_insn_count",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentFrameInsnAt = h(lib, "asmtest_descent_frame_insn_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG, JAVA_LONG));
            descentFrameBlockCount = h(lib, "asmtest_descent_frame_block_count",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG));
            descentFrameBlockAt = h(lib, "asmtest_descent_frame_block_at",
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG, JAVA_LONG));
            descentTruncated = h(lib, "asmtest_descent_truncated",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            descentDepthCapped = h(lib, "asmtest_descent_depth_capped",
                FunctionDescriptor.of(JAVA_INT, ADDRESS));
            // Descending trace entry points (_ex): each threads a descent* through the
            // existing loop. trace_call_ex(code, len, args, nargs, result, trace, descent);
            // len is the TRACED REGION length (frame 0), not necessarily the whole
            // allocation. trace / descent may be NULL.
            ptraceTraceCallEx = h(lib, "asmtest_ptrace_trace_call_ex",
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS, JAVA_INT,
                    ADDRESS, ADDRESS, ADDRESS));
            ptraceTraceAttachedEx = h(lib, "asmtest_ptrace_trace_attached_ex",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    ADDRESS, ADDRESS));
            ptraceTraceAttachedVersionedEx = h(lib, "asmtest_ptrace_trace_attached_versioned_ex",
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_LONG, ADDRESS,
                    JAVA_LONG, ADDRESS, ADDRESS, ADDRESS));
        } catch (Throwable t) {
            // The library may be absent (not built) — degrade to available() == false
            // rather than failing class init.
            loadError = t;
        }
        HW_AVAILABLE = hwAvailable; HW_SKIP_REASON = hwSkipReason; HW_RESOLVE = hwResolve;
        HW_AUTO = hwAuto; TRACE_RESOLVE = traceResolve; TRACE_AUTO = traceAuto; HW_INIT = hwInit;
        HW_SHUTDOWN = hwShutdown; REGISTER_REGION = registerRegion; HW_BEGIN = hwBegin;
        HW_END = hwEnd; HW_TRY_BEGIN = hwTryBegin; HW_RENDER = hwRender;
        CALL_SCOPED_EX = callScopedEx; RENDER_SCOPE = renderScope;
        BEGIN_WINDOW = beginWindow; END_WINDOW = endWindow; RENDER_WINDOW = renderWindow;
        STEALTH_TRACE = stealthTrace;
        EXEC_ALLOC = execAlloc; EXEC_FREE = execFree; TRACE_NEW = traceNew;
        TRACE_FREE = traceFree; TRACE_COVERED = traceCovered; TRACE_BLOCKS_LEN = traceBlocksLen;
        TRACE_INSNS_TOTAL = traceInsnsTotal; TRACE_INSNS_LEN = traceInsnsLen;
        TRACE_TRUNCATED = traceTruncated; TRACE_BLOCK_AT = traceBlockAt; TRACE_INSN_AT = traceInsnAt;
        PTRACE_AVAILABLE = ptraceAvailable; PTRACE_SKIP_REASON = ptraceSkipReason;
        PTRACE_TRACE_CALL = ptraceTraceCall; PTRACE_TRACE_ATTACHED = ptraceTraceAttached;
        PTRACE_BLOCKSTEP_AVAILABLE = ptraceBlockstepAvailable;
        PTRACE_TRACE_CALL_BLOCKSTEP = ptraceTraceCallBlockstep;
        PTRACE_TRACE_ATTACHED_BLOCKSTEP = ptraceTraceAttachedBlockstep;
        PTRACE_TRACE_ATTACHED_VERSIONED = ptraceTraceAttachedVersioned; PTRACE_RUN_TO = ptraceRunTo;
        PROC_REGION_BY_ADDR = procRegionByAddr; PROC_PERFMAP_SYMBOL = procPerfmapSymbol;
        JITDUMP_FIND = jitdumpFind;
        CI_AVAILABLE = ciAvailable; CI_SKIP_REASON = ciSkipReason; CI_NEW = ciNew;
        CI_FREE = ciFree; CI_TRACK = ciTrack; CI_REFRESH = ciRefresh; CI_NOW = ciNow;
        CI_BYTES_AT = ciBytesAt; CI_BPF_AVAILABLE = ciBpfAvailable;
        CI_BPF_SKIP_REASON = ciBpfSkipReason; CI_WATCH_BPF = ciWatchBpf;
        CI_POLL_BPF = ciPollBpf; CI_NEXT = ciNext;
        DESCENT_NEW = descentNew; DESCENT_FREE = descentFree;
        DESCENT_SET_MAX_DEPTH = descentSetMaxDepth; DESCENT_SET_INSN_BUDGET = descentSetInsnBudget;
        DESCENT_SET_WATCHDOG_MS = descentSetWatchdogMs; DESCENT_ALLOW_REGION = descentAllowRegion;
        DESCENT_USE_DEFAULT_DENYLIST = descentUseDefaultDenylist;
        DESCENT_DENY_REGION = descentDenyRegion; DESCENT_SET_RESOLVER = descentSetResolver;
        DESCENT_SET_DENYLIST = descentSetDenylist; DESCENT_EDGES_LEN = descentEdgesLen;
        DESCENT_EDGE_SITE = descentEdgeSite; DESCENT_EDGE_TARGET = descentEdgeTarget;
        DESCENT_EDGE_DEPTH = descentEdgeDepth; DESCENT_FRAMES_LEN = descentFramesLen;
        DESCENT_FRAME_BASE = descentFrameBase; DESCENT_FRAME_LEN = descentFrameLen;
        DESCENT_FRAME_DEPTH = descentFrameDepth; DESCENT_FRAME_PARENT = descentFrameParent;
        DESCENT_FRAME_INSN_COUNT = descentFrameInsnCount; DESCENT_FRAME_INSN_AT = descentFrameInsnAt;
        DESCENT_FRAME_BLOCK_COUNT = descentFrameBlockCount; DESCENT_FRAME_BLOCK_AT = descentFrameBlockAt;
        DESCENT_TRUNCATED = descentTruncated; DESCENT_DEPTH_CAPPED = descentDepthCapped;
        PTRACE_TRACE_CALL_EX = ptraceTraceCallEx; PTRACE_TRACE_ATTACHED_EX = ptraceTraceAttachedEx;
        PTRACE_TRACE_ATTACHED_VERSIONED_EX = ptraceTraceAttachedVersionedEx;
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
        return s == null ? MemorySegment.NULL : a.allocateFrom(s);
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
        try (Arena a = Arena.ofConfined()) {
            MemorySegment buf = a.allocate(160);
            HW_SKIP_REASON.invoke(backend, buf, 160L);
            return buf.getString(0);
        } catch (Throwable t) { throw rethrow(t); }
    }

    /** Convenience: skip reason for the SINGLESTEP default. */
    public static String skipReason() { return skipReason(SINGLESTEP); }

    /** This host's hardware-trace fallback cascade: the available backend enums,
     *  most-faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring
     *  {@code policy}. Empty only off x86-64 Linux (single-step is the floor there).
     *  {@code CEILING_FREE} drops the ceiling-bounded backend (AMD LBR). */
    public static int[] resolve(int policy) {
        if (HW_RESOLVE == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            // up to 4 backend ints. A sequence layout sizes by element count
            // unambiguously (a.allocate(JAVA_INT, n) sizes to one int on this
            // JDK, which only ever sufficed because this host exposes a single
            // backend — it would truncate on a multi-backend Intel-PT host).
            MemorySegment out = a.allocate(MemoryLayout.sequenceLayout(4, JAVA_INT));
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
        try (Arena a = Arena.ofConfined()) {
            final int cap = 8; // the cascade is at most 6 entries; headroom
            // 3 consecutive JAVA_INT per choice (asmtest_trace_choice_t, no padding).
            MemorySegment out = a.allocate(
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
        try (Arena a = Arena.ofConfined()) {
            MemorySegment out = a.allocate(
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

    /** Absolute path of the libasmtest_hwtrace this process resolved, or null if it
     *  failed to load. Lets a clean-room test assert the bundled tier — not a leaked
     *  build/ tree — satisfied the load. */
    public static String libraryPath() { return RESOLVED_PATH; }

    /** Select {@code backend} and initialize the tier (asmtest_hwtrace_init);
     *  throws RuntimeException on a nonzero status. SINGLESTEP is the portable
     *  default that runs on any x86-64 Linux. */
    public static void init(int backend) {
        if (HW_INIT == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment opts = a.allocate(OPTIONS_LAYOUT);
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

    /** Trace ONE native call the managed-safe way (asmtest_hwtrace_call_scoped_ex): arm the
     *  single-step window, call {@code code(args…)} through the SysV integer ABI, and disarm
     *  entirely in native code, so nothing the binding runs between arm and disarm is stepped
     *  — a tighter window than {@link AsmTrace}, where the FFI-dispatch of {@code code.call}
     *  runs. REGISTRY-FREE — consumes no MAX_REGIONS slot — so it is safe in a tight loop.
     *  {@code args} pass as C longs (0..6). Returns a {@link CallScopedResult}: {@code result}
     *  the call's return, {@code path} the executed body's disassembly, {@code truncated} the
     *  thread-scope honesty bit. Self-skips ({@code rc} negative, {@code result} 0) where no
     *  single-step backend is available. Mirrors the Python {@code HwTrace.call_scoped}. */
    public static CallScopedResult callScoped(NativeCode code, long... args) {
        if (CALL_SCOPED_EX == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            // asmtest_trace_new(insns_cap, blocks_cap) — insns FIRST. Caller-owned trace.
            MemorySegment handle = (MemorySegment) TRACE_NEW.invoke(256L, 64L);
            if (MemorySegment.NULL.equals(handle))
                throw new RuntimeException("asmtest_trace_new returned NULL");
            try {
                int n = args.length;
                // Per-call scratch: the args[] (size to element COUNT via a sequence layout,
                // NOT allocate(JAVA_LONG, n) which sizes to one element on this JDK), a long*
                // result cell, and the 8-byte {u32 idx, u32 gen} scope out-param.
                MemorySegment argSeg = a.allocate(
                    MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
                for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
                MemorySegment result = a.allocate(JAVA_LONG);
                MemorySegment scopeOut = a.allocate(MemoryLayout.sequenceLayout(2, JAVA_INT));
                // For a native leaf fn == base (offset 0 = entry).
                MemorySegment base = MemorySegment.ofAddress(code.base());
                int rc = (int) CALL_SCOPED_EX.invoke(base, code.length(), handle, base,
                    argSeg, n, result, scopeOut);
                if (rc != ASMTEST_HW_OK) return new CallScopedResult(0L, "", false, rc);
                // Render the body from the just-captured (thread-local) scope handle. The
                // by-value 8-byte scope struct is passed as a packed long (idx | gen<<32),
                // ABI-identical to the struct in one integer register on SysV x86-64.
                int idx = scopeOut.getAtIndex(JAVA_INT, 0);
                int gen = scopeOut.getAtIndex(JAVA_INT, 1);
                long packed = (idx & 0xffffffffL) | ((long) gen << 32);
                String path = "";
                int need = (int) RENDER_SCOPE.invoke(packed, MemorySegment.NULL, 0L);
                if (need > 0) {
                    MemorySegment buf = a.allocate(need + 1L);
                    RENDER_SCOPE.invoke(packed, buf, (long) (need + 1));
                    path = buf.getString(0);
                }
                boolean trunc = ((int) TRACE_TRUNCATED.invoke(handle)) != 0;
                return new CallScopedResult(result.get(JAVA_LONG, 0), path, trunc, rc);
            } finally {
                TRACE_FREE.invoke(handle);
            }
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Region-free WHOLE-WINDOW capture (§Z0/§Z1 — the Java mirror of dotnet's
     *  {@code using (new AsmTrace()) { work(); }}): arm a single-step window on THIS thread
     *  with NO registered region, run {@code body}, disarm, and render. {@code insns[]} hold
     *  ABSOLUTE addresses. Keep {@code body} TIGHT — every instruction between begin and end is
     *  stepped, and single-stepping a managed runtime records a LOT (a single call runs ~100k
     *  instructions), so the routine's own addresses appear as a SUBSET amid the JVM/FFI noise.
     *  Self-skips ({@code rc} negative) off a single-step backend / when the tier is not up.
     *
     *  <p>SAFETY: this arms EFLAGS.TF single-step on the CALLING thread, so {@code body} must be
     *  a TIGHT native leaf (an FFM downcall), not an arbitrary managed block. A body that blocks
     *  SIGTRAP or spawns threads ({@code pthread_create} masks SIGTRAP around clone) turns the
     *  #DB into a fatal blocked signal and KILLS the JVM. To trace a whole block of arbitrary
     *  managed code, use the crash-proof out-of-process path (see
     *  docs/internal/plans/managed-wholewindow-oop-plan.md), never this in-process form. */
    public static WindowResult window(Runnable body) {
        return window(body, 1 << 20);
    }

    /** {@link #window(Runnable)} with an explicit capture-buffer size (insns). If the managed
     *  window overflows {@code insnsCap}, {@code truncated} is set (an honest best-effort
     *  outcome) and the stored {@code insns} are a labelled PREFIX. */
    public static WindowResult window(Runnable body, int insnsCap) {
        if (BEGIN_WINDOW == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            // whole-window is insns-only: insns_cap FIRST, blocks_cap=0 SECOND.
            MemorySegment handle = (MemorySegment) TRACE_NEW.invoke((long) insnsCap, 0L);
            if (MemorySegment.NULL.equals(handle))
                throw new RuntimeException("asmtest_trace_new returned NULL");
            try {
                MemorySegment scopeOut = a.allocate(MemoryLayout.sequenceLayout(2, JAVA_INT));
                int rc = (int) BEGIN_WINDOW.invoke(handle, scopeOut);
                if (rc != ASMTEST_HW_OK) { // EUNAVAIL/ESTATE/EFULL/EINVAL -> clean self-skip
                    body.run();
                    return new WindowResult("", false, new long[0], rc);
                }
                int idx = scopeOut.getAtIndex(JAVA_INT, 0);
                int gen = scopeOut.getAtIndex(JAVA_INT, 1);
                long packed = (idx & 0xffffffffL) | ((long) gen << 32); // scope BY VALUE
                try {
                    body.run(); // TIGHT window: EFLAGS.TF single-step is armed across this
                } finally {
                    END_WINDOW.invoke(packed, handle); // disarm even on a body throw
                }
                String path = "";
                int need = (int) RENDER_WINDOW.invoke(packed, MemorySegment.NULL, 0L);
                if (need > 0) {
                    MemorySegment buf = a.allocate(need + 1L);
                    RENDER_WINDOW.invoke(packed, buf, (long) (need + 1));
                    path = buf.getString(0);
                }
                boolean trunc = ((int) TRACE_TRUNCATED.invoke(handle)) != 0;
                int n = (int) (long) TRACE_INSNS_LEN.invoke(handle);
                long[] insns = new long[n];
                for (int i = 0; i < n; i++) insns[i] = (long) TRACE_INSN_AT.invoke(handle, (long) i);
                return new WindowResult(path, trunc, insns, rc);
            } finally {
                TRACE_FREE.invoke(handle);
            }
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    // §D3 stealth stepper run_region upcall target: invoked out-of-band by the (helper-
    // stepped) calling thread to run the leaf so control enters [base,len). The `ran` flag +
    // leaf base + args are bound per call via MethodHandles.insertArguments; the trailing
    // MemorySegment is the C `void *arg` (unused — everything rides in the closure, like
    // dotnet's callback). `ran` records that the region actually executed, so the self-skip
    // path re-runs it exactly ONCE (never lose the call, never double-run a side effect) —
    // stealth_trace may run run_region untraced AND still return EUNAVAIL in one attach race.
    private static final MethodHandle RUN_LEAF_MH;
    static {
        try {
            RUN_LEAF_MH = MethodHandles.lookup().findStatic(HwTrace.class, "runLeaf",
                MethodType.methodType(void.class, boolean[].class, long.class, long[].class,
                    MemorySegment.class));
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    private static void runLeaf(boolean[] ran, long base, long[] args, MemorySegment ignoredArg) {
        ran[0] = true;
        invokeLeaf(base, args);
    }

    private static void invokeLeaf(long base, long[] args) {
        // The helper single-steps the region as this runs. Swallow any FFM error: the helper's
        // RAX read (result_out) is authoritative and the out-of-band stepper must never crash.
        try {
            MemoryLayout[] al = new MemoryLayout[args.length];
            for (int i = 0; i < args.length; i++) al[i] = JAVA_LONG;
            MethodHandle fn = LINKER.downcallHandle(MemorySegment.ofAddress(base),
                FunctionDescriptor.of(JAVA_LONG, al));
            Object[] boxed = new Object[args.length];
            for (int i = 0; i < args.length; i++) boxed[i] = args[i];
            fn.invokeWithArguments(boxed);
        } catch (Throwable ignored) { /* result_out is authoritative */ }
    }

    /** Trace ONE native leaf the CRASH-PROOF out-of-process way: a helper child reverse-attaches
     *  to this process and single-steps the region {@code [base,len)} while THIS thread runs the
     *  leaf — so NO EFLAGS.TF is ever armed on the calling thread
     *  ({@code asmtest_hwtrace_stealth_trace}). This is the safe counterpart to
     *  {@link #callScoped}/{@link #window} for a host with no PT/LBR (Zen 2, Docker-on-Mac): the
     *  in-process single-step tier is FORBIDDEN against a managed runtime (a body that blocks
     *  SIGTRAP or spawns a thread — {@code pthread_create} masks SIGTRAP around {@code clone} —
     *  turns the {@code #DB} into a fatal blocked signal and KILLS the JVM), whereas a
     *  ptrace-stop is not gated by the tracee's signal mask, so the body survives. Mirrors
     *  dotnet's {@code AsmTrace.Method(..., outOfProcess: true)}.
     *
     *  <p>The {@code result} is EXACT (the helper reads the caller's RAX at the {@code ret}), but
     *  the instruction STREAM is best-effort over a live runtime: single-stepping the runtime's own
     *  thread can be interrupted by its async signals, so {@code truncated} may be set with a
     *  partial {@code offsets} — the same honest-degradation posture as {@link #window} and dotnet's
     *  out-of-process {@code AsmTrace.Method}.
     *
     *  <p>{@code args} pass as C longs (0..6). Returns a {@link StealthResult}: {@code result}
     *  the leaf's return (from the helper's RAX read), {@code offsets} the executed body's
     *  region-RELATIVE instruction offsets, {@code blocks} the basic-block count,
     *  {@code truncated} the honesty bit. On a refused reverse-attach (Yama {@code ptrace_scope},
     *  no ptrace, or off-x86-64-Linux) it self-skips ({@code rc} negative, {@code armed()} false,
     *  {@code offsets} empty) — but the call STILL RUNS (never a silent miss), like dotnet's
     *  stealth path. Needs no {@link #init} (the stealth stepper is ptrace-based, independent of
     *  the single-step tier). */
    public static StealthResult stealthTrace(NativeCode code, long... args) {
        if (STEALTH_TRACE == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            // asmtest_trace_new(insns_cap, blocks_cap) — insns FIRST. Caller-owned trace.
            MemorySegment handle = (MemorySegment) TRACE_NEW.invoke(256L, 64L);
            if (MemorySegment.NULL.equals(handle))
                throw new RuntimeException("asmtest_trace_new returned NULL");
            try {
                MemorySegment resultOut = a.allocate(JAVA_LONG);
                long base = code.base();
                boolean[] ran = {false};
                // Bind ran + leaf base + args into the run_region upcall (void(void*)); the stub
                // lives in this confined arena, which outlives the synchronous stealth call.
                MethodHandle bound = MethodHandles.insertArguments(RUN_LEAF_MH, 0, ran, base, args);
                MemorySegment stub = LINKER.upcallStub(bound, FunctionDescriptor.ofVoid(ADDRESS), a);
                int rc = (int) STEALTH_TRACE.invoke(MemorySegment.ofAddress(base), code.length(),
                    handle, resultOut, stub, MemorySegment.NULL);
                if (rc != ASMTEST_HW_OK) { // EUNAVAIL/EINVAL/ENOSYS -> clean self-skip
                    if (!ran[0]) invokeLeaf(base, args); // run exactly once — never a silent miss
                    return new StealthResult(0L, new long[0], 0, false, rc);
                }
                long result = resultOut.get(JAVA_LONG, 0);
                int n = (int) (long) TRACE_INSNS_LEN.invoke(handle);
                long[] offsets = new long[n];
                for (int i = 0; i < n; i++) offsets[i] = (long) TRACE_INSN_AT.invoke(handle, (long) i);
                int blocks = (int) (long) TRACE_BLOCKS_LEN.invoke(handle);
                boolean trunc = ((int) TRACE_TRUNCATED.invoke(handle)) != 0;
                return new StealthResult(result, offsets, blocks, trunc, rc);
            } finally {
                TRACE_FREE.invoke(handle);
            }
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
            try (Arena a = Arena.ofConfined()) {
                MemorySegment in = a.allocate(Math.max(bytes.length, 1));
                MemorySegment.copy(bytes, 0, in, JAVA_BYTE, 0, bytes.length);
                MemorySegment baseOut = a.allocate(ADDRESS); // void**
                MemorySegment lenOut = a.allocate(JAVA_LONG); // size_t*
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
            try (Arena a = Arena.ofConfined()) {
                int rc = (int) REGISTER_REGION.invoke(str(a, name),
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
            try (Arena a = Arena.ofConfined()) {
                MemorySegment n = str(a, name);
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

    /**
     * The try-with-resources scope construct (§D2): an {@link AutoCloseable} over the
     * register-then-begin/close-end pair with the shared-core render-on-close. Use it as
     * {@code try (var t = HwTrace.scope(code)) { HotPath(); }}: the factory auto-names
     * the region {@code basename:line} from the call site (StackWalker), registers the
     * traced range, and brackets {@code try_begin} (a nonzero return is a clean
     * self-skip); {@link #close} ends the region, renders the executed assembly into
     * {@link #path}, and — when {@code emit} — writes it to stdout. The C core flags the
     * trace {@code truncated} on a cross-thread close (§0.2/§1). Live managed-JIT tracing
     * (the runtime's own methods) needs PT/LBR or the out-of-process ptrace stepper — a
     * forward-look managed-tier capability; this scope traces a known native leaf today.
     */
    public static final class AsmTrace implements AutoCloseable {
        private final Arena arena;
        private final MemorySegment nameSeg;
        private final MemorySegment handle;
        private final String name;
        private final boolean emit;
        private boolean armed;
        private boolean truncated;
        private String path = "";

        private AsmTrace(NativeCode code, boolean emit, String name) {
            this.arena = Arena.ofConfined();
            this.name = name;
            this.emit = emit;
            try {
                this.nameSeg = str(arena, name);
                this.handle = (MemorySegment) TRACE_NEW.invoke(256L, 64L);
                // Register-then-begin under the same generated name (Core §0.4 idempotent).
                REGISTER_REGION.invoke(nameSeg, MemorySegment.ofAddress(code.base()),
                    code.length(), handle);
                this.armed = ((int) HW_TRY_BEGIN.invoke(nameSeg)) == ASMTEST_HW_OK;
            } catch (Throwable t) { arena.close(); throw rethrow(t); }
        }

        /** Open a scope tracing {@code code}, emitting the rendered listing on close. */
        public static AsmTrace scope(NativeCode code) {
            return new AsmTrace(code, true, autoName());
        }

        /** Open a scope tracing {@code code}; {@code emit} gates the stdout write. */
        public static AsmTrace scope(NativeCode code, boolean emit) {
            return new AsmTrace(code, emit, autoName());
        }

        private static String autoName() {
            // stack: [0]=autoName, [1]=scope(...), [2]=the caller.
            var f = StackWalker.getInstance().walk(s -> s.skip(2).findFirst().orElse(null));
            if (f == null) return "asmscope";
            String file = f.getFileName();
            String n = (file == null ? "asmscope" : file) + ":" + f.getLineNumber();
            return n.length() > 63 ? n.substring(n.length() - 63) : n;
        }

        /** The rendered assembly listing (populated on close). */
        public String path() { return path; }
        /** True if the scope armed (a backend was available and try_begin succeeded). */
        public boolean armed() { return armed; }
        /** True if the close hopped OS threads / the capture overflowed (§0.2/§1). */
        public boolean truncated() { return truncated; }
        /** The auto-generated (or explicit) region name. */
        public String name() { return name; }

        @Override public void close() {
            try {
                HW_END.invoke(nameSeg);
                int need = (int) HW_RENDER.invoke(nameSeg, MemorySegment.NULL, 0L);
                if (need > 0) {
                    MemorySegment buf = arena.allocate(need + 1L);
                    HW_RENDER.invoke(nameSeg, buf, (long) (need + 1));
                    this.path = buf.getString(0);
                }
                this.truncated = ((int) TRACE_TRUNCATED.invoke(handle)) != 0;
                if (handle != null && TRACE_FREE != null) TRACE_FREE.invoke(handle);
            } catch (Throwable t) { throw rethrow(t); }
            finally { arena.close(); }
            if (emit && path != null && !path.isEmpty()) System.out.print(path);
        }
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
        try (Arena a = Arena.ofConfined()) {
            MemorySegment buf = a.allocate(160);
            PTRACE_SKIP_REASON.invoke(buf, 160L);
            return buf.getString(0);
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
        try (Arena a = Arena.ofConfined()) {
            int n = args.length;
            // Per-call scratch: the args[] (size to element COUNT via a sequence layout,
            // NOT allocate(JAVA_LONG, n) which sizes to one element on this JDK) and a
            // long* result cell.
            MemorySegment argSeg = a.allocate(
                MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
            for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_CALL.invoke(code, len, argSeg, n, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_call failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** True if the BTF block-step variant (PTRACE_SINGLEBLOCK — one #DB per TAKEN
     *  branch instead of one per instruction) can run here: x86-64 Linux with a
     *  functional PTRACE_SINGLEBLOCK and Capstone for the intra-block reconstruction.
     *  Hang-proof, cached probe; callers self-skip cleanly on false. */
    public static boolean ptraceBlockstepAvailable() {
        if (PTRACE_BLOCKSTEP_AVAILABLE == null) return false;
        try { return (int) PTRACE_BLOCKSTEP_AVAILABLE.invoke() != 0; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Block-step variant of {@link #ptraceTraceCall}: drives PTRACE_SINGLEBLOCK
     *  (DEBUGCTL.BTF), stopping once per TAKEN branch and reconstructing the
     *  intra-block instructions with Capstone — the same insns/blocks stream as
     *  ptraceTraceCall at a fraction of the stops. Probe first with
     *  {@link #ptraceBlockstepAvailable}. Complete at moderate overhead, NOT cheap:
     *  each block still costs a full ptrace round-trip. */
    public static long ptraceTraceCallBlockstep(MemorySegment code, long len, long[] args,
                                                MemorySegment trace) {
        if (PTRACE_TRACE_CALL_BLOCKSTEP == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            int n = args.length;
            MemorySegment argSeg = a.allocate(
                MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
            for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_CALL_BLOCKSTEP.invoke(code, len, argSeg, n, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_call_blockstep failed: " + rc);
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
        try (Arena a = Arena.ofConfined()) {
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED.invoke(pid, base, len, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Block-step variant of {@link #ptraceTraceAttached}: one #DB per TAKEN branch
     *  (intra-block instructions reconstructed with Capstone), same contract
     *  otherwise — the rootless managed-runtime completeness fallback at a fraction
     *  of the stops. Probe first with {@link #ptraceBlockstepAvailable}. */
    public static long ptraceTraceAttachedBlockstep(int pid, MemorySegment base, long len,
                                                    MemorySegment trace) {
        if (PTRACE_TRACE_ATTACHED_BLOCKSTEP == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED_BLOCKSTEP.invoke(pid, base, len, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached_blockstep failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Like {@link #ptraceTraceAttached} but decode the region against TIME-CORRECT
     *  bytes from a code-image recorder {@code img} (a {@link CodeImage} handle) at the
     *  logical timestamp {@code when} ({@code 0} = latest) instead of a single live
     *  snapshot — the fix for a JIT whose code at {@code base} was patched, freed, or had
     *  its address reused during the run. {@code img} must already be tracking a region
     *  covering {@code [base, base+len)}; pass {@link MemorySegment#NULL} for {@code img}
     *  to fall back to exactly {@link #ptraceTraceAttached}. Returns the target's RAX at
     *  the ret. Throws on a nonzero status. */
    public static long ptraceTraceAttachedVersioned(int pid, MemorySegment base, long len,
                                                    MemorySegment img, long when,
                                                    MemorySegment trace) {
        if (PTRACE_TRACE_ATTACHED_VERSIONED == null)
            throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED_VERSIONED.invoke(pid, base, len,
                img == null ? MemorySegment.NULL : img, when, result, trace);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached_versioned failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Descending variant of {@link #ptraceTraceCall}: fork a tracee that calls
     *  {@code code}, single-step it OUT OF PROCESS, and thread a {@link Descent}
     *  handle through the loop so the call-outs are recorded as edges and (at level
     *  &gt;= 2) descended as nested frames. Returns the routine's return value.
     *  <p>CRITICAL: {@code len} is the TRACED REGION length (frame 0), NOT necessarily
     *  the whole allocation — pass the region so an in-blob sibling the code calls stays
     *  OUTSIDE it (else the sibling falls inside the region and mis-records as recursion).
     *  {@code trace} (the flat frame-0 view) may be {@link MemorySegment#NULL} to record
     *  only into {@code descent}, and vice versa. Throws on a nonzero status. */
    public static long ptraceTraceCallEx(MemorySegment code, long len, long[] args,
                                         MemorySegment trace, MemorySegment descent) {
        if (PTRACE_TRACE_CALL_EX == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            int n = args.length;
            MemorySegment argSeg = a.allocate(
                MemoryLayout.sequenceLayout(Math.max(n, 1), JAVA_LONG));
            for (int i = 0; i < n; i++) argSeg.setAtIndex(JAVA_LONG, i, args[i]);
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_CALL_EX.invoke(code, len, argSeg, n, result,
                trace == null ? MemorySegment.NULL : trace,
                descent == null ? MemorySegment.NULL : descent);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_call_ex failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Descending variant of {@link #ptraceTraceAttached} for an externally-attached,
     *  already-ptrace-stopped process: threads a {@link Descent} handle through the loop.
     *  {@code trace} / {@code descent} may be {@link MemorySegment#NULL}. Returns the
     *  target's RAX at the ret. Throws on a nonzero status. */
    public static long ptraceTraceAttachedEx(int pid, MemorySegment base, long len,
                                             MemorySegment trace, MemorySegment descent) {
        if (PTRACE_TRACE_ATTACHED_EX == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED_EX.invoke(pid, base, len, result,
                trace == null ? MemorySegment.NULL : trace,
                descent == null ? MemorySegment.NULL : descent);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached_ex failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Descending variant of {@link #ptraceTraceAttachedVersioned} (code-image-versioned
     *  bytes) that threads a {@link Descent} handle through the loop. {@code img} may be
     *  {@link MemorySegment#NULL} (then exactly {@link #ptraceTraceAttachedEx}); {@code trace}
     *  / {@code descent} may be {@link MemorySegment#NULL}. Returns the target's RAX at the
     *  ret. Throws on a nonzero status. */
    public static long ptraceTraceAttachedVersionedEx(int pid, MemorySegment base, long len,
                                                      MemorySegment img, long when,
                                                      MemorySegment trace, MemorySegment descent) {
        if (PTRACE_TRACE_ATTACHED_VERSIONED_EX == null)
            throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment result = a.allocate(JAVA_LONG);
            int rc = (int) PTRACE_TRACE_ATTACHED_VERSIONED_EX.invoke(pid, base, len,
                img == null ? MemorySegment.NULL : img, when, result,
                trace == null ? MemorySegment.NULL : trace,
                descent == null ? MemorySegment.NULL : descent);
            if (rc != ASMTEST_PTRACE_OK)
                throw new RuntimeException("asmtest_ptrace_trace_attached_versioned_ex failed: " + rc);
            return result.get(JAVA_LONG, 0);
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** Run an already-attached, ptrace-stopped target {@code pid} forward until it
     *  reaches {@code addr} (a software breakpoint that fires when the program itself
     *  next calls in), leaving it stopped there ready for
     *  {@link #ptraceTraceAttached} — the step that makes a resolved JIT method
     *  traceable when you don't control call timing. Returns the status
     *  ({@code ASMTEST_PTRACE_OK}, or {@code ASMTEST_PTRACE_ENOENT} if the target
     *  exited first). The caller owns PTRACE_ATTACH/DETACH. */
    public static int ptraceRunTo(int pid, long addr) {
        if (PTRACE_RUN_TO == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try {
            return (int) PTRACE_RUN_TO.invoke(pid, MemorySegment.ofAddress(addr));
        } catch (RuntimeException re) { throw re; }
        catch (Throwable t) { throw rethrow(t); }
    }

    /** The executable mapping in /proc/&lt;pid&gt;/maps that CONTAINS {@code addr},
     *  as {@code {base, len}}, or {@code null} if no executable mapping contains it. */
    public static long[] procRegionByAddr(int pid, long addr) {
        if (PROC_REGION_BY_ADDR == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
        try (Arena a = Arena.ofConfined()) {
            MemorySegment baseOut = a.allocate(ADDRESS); // void**
            MemorySegment lenOut = a.allocate(JAVA_LONG); // size_t*
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
        try (Arena a = Arena.ofConfined()) {
            MemorySegment baseOut = a.allocate(ADDRESS); // void**
            MemorySegment lenOut = a.allocate(JAVA_LONG); // size_t*
            int rc = (int) PROC_PERFMAP_SYMBOL.invoke(pid, str(a, name), baseOut, lenOut);
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
        try (Arena a = Arena.ofConfined()) {
            // asmtest_jitdump_entry_t — four consecutive JAVA_LONG (uint64), 32 bytes.
            MemorySegment out = a.allocate(
                MemoryLayout.sequenceLayout(4, JAVA_LONG));
            // bytes_out: size to wantBytes (sequence layout, NOT allocate(JAVA_BYTE, n));
            // bytes_len: size_t*. Both NULL when no bytes are requested.
            MemorySegment bytesOut = wantBytes > 0
                ? a.allocate(MemoryLayout.sequenceLayout(wantBytes, JAVA_BYTE))
                : MemorySegment.NULL;
            MemorySegment bytesLen = wantBytes > 0 ? a.allocate(JAVA_LONG)
                : MemorySegment.NULL;
            int rc = (int) JITDUMP_FIND.invoke(str(a, path), pid, str(a, name), out, bytesOut,
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

    // ---- Call descent (asmtest_descent_t) ----
    //
    // Configure how the ptrace stepper handles the call-outs it would otherwise step
    // over, and read back the recorded edges + nested frames. Four levels (DESCENT_*):
    // OFF, RECORD_EDGES, DESCEND_KNOWN, DESCEND_ALL. Pass a Descent's handle() to
    // ptraceTraceCallEx and friends. Frame 0 is the root region (a superset of the flat
    // trace); descended callees are frames 1..N. Mirrors the Python wrapper's Descent.

    /** A recorded (call-site -&gt; callee) edge: the {@code site} call-site byte offset in
     *  frame 0, the ABSOLUTE {@code target} address, and the caller {@code depth}
     *  (0 = frame 0). Mirrors the Python wrapper's {@code (site, target, depth)} tuple. */
    public record Edge(long site, long target, int depth) {}

    /** Call descent (asmtest_descent_t): configure descent policy and read back the
     *  recorded edges + nested frames from a {@link #ptraceTraceCallEx} run.
     *  {@link AutoCloseable} with an idempotent {@link #close()} (the handle is NULLed
     *  after {@code asmtest_descent_free}, so a double free is a no-op). */
    public static final class Descent implements AutoCloseable {
        /** A level-2/3 resolver: return {@code {base, len}} to descend into
         *  {@code calleeAddr} (the callee's region), or {@code null} to step over it. */
        @FunctionalInterface public interface Resolver { long[] resolve(long calleeAddr); }
        /** A level-3 denylist: return {@code true} to REFUSE descent into
         *  {@code calleeAddr} (step over it), {@code false} to allow it. */
        @FunctionalInterface public interface Denylist { boolean deny(long calleeAddr); }

        // asmtest_descent_resolver_fn: int(uint64 callee, void* user, uint64* base_out,
        // uint64* len_out); asmtest_descent_denylist_fn: int(uint64 callee, void* user).
        private static final FunctionDescriptor RESOLVER_DESC =
            FunctionDescriptor.of(JAVA_INT, JAVA_LONG, ADDRESS, ADDRESS, ADDRESS);
        private static final FunctionDescriptor DENYLIST_DESC =
            FunctionDescriptor.of(JAVA_INT, JAVA_LONG, ADDRESS);
        private static final MethodHandle ON_RESOLVE_MH, ON_DENY_MH;
        static {
            try {
                MethodHandles.Lookup lk = MethodHandles.lookup();
                ON_RESOLVE_MH = lk.findVirtual(Descent.class, "onResolve",
                    MethodType.methodType(int.class, long.class, MemorySegment.class,
                        MemorySegment.class, MemorySegment.class));
                ON_DENY_MH = lk.findVirtual(Descent.class, "onDeny",
                    MethodType.methodType(int.class, long.class, MemorySegment.class));
            } catch (ReflectiveOperationException e) {
                throw new ExceptionInInitializerError(e);
            }
        }

        private MemorySegment handle; // an asmtest_descent_t*
        // The upcall trampolines are native code the callback FFI jumps to; their backing
        // Arena MUST outlive every trace_call_ex that could fire them, so it is held for
        // the handle's lifetime and closed only in free() (after descent_free). Keeping the
        // stubs + the user callbacks referenced also pins them against GC mid-single-step.
        private Arena upcallArena;
        private MemorySegment resolverStub, denylistStub;
        private Resolver resolver;
        private Denylist denylist;

        /** Allocate a descent handle at {@code level} (a {@code DESCENT_*} constant), with
         *  conservative depth/budget/watchdog defaults and an empty allow-set/denylist. */
        public Descent(int level) {
            if (DESCENT_NEW == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
            try {
                MemorySegment h = (MemorySegment) DESCENT_NEW.invoke(level);
                if (MemorySegment.NULL.equals(h)) throw new RuntimeException("asmtest_descent_new failed");
                this.handle = h;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The underlying {@code asmtest_descent_t*} handle — pass to
         *  {@link HwTrace#ptraceTraceCallEx} and friends as their {@code descent} argument. */
        public MemorySegment handle() { return handle; }

        // ---- configuration (in) ----

        /** Ceiling on nested descent depth (frame 0 is depth 0). 0 restores the default. */
        public void setMaxDepth(int maxDepth) {
            try { DESCENT_SET_MAX_DEPTH.invoke(handle, maxDepth); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Total single-step instruction budget across all descended frames; 0 = default. */
        public void setInsnBudget(long budget) {
            try { DESCENT_SET_INSN_BUDGET.invoke(handle, budget); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Real-time watchdog in milliseconds for a descended run; 0 = default. */
        public void setWatchdogMs(int ms) {
            try { DESCENT_SET_WATCHDOG_MS.invoke(handle, ms); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Arm the built-in L3 default denylist (PLT resolver / vdso / GC-JIT
         *  modules; plus blocking-libc entry points on the fork path). */
        public void useDefaultDenylist() {
            try { DESCENT_USE_DEFAULT_DENYLIST.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Add {@code [base, base+len)} to the level-2 allow-set (descend into calls landing
         *  inside). Returns 0 on success, negative on OOM. */
        public int allowRegion(long base, long len) {
            try { return (int) DESCENT_ALLOW_REGION.invoke(handle, MemorySegment.ofAddress(base), len); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Add {@code [base, base+len)} to the level-3 deny-set (never descend into it). */
        public int denyRegion(long base, long len) {
            try { return (int) DESCENT_DENY_REGION.invoke(handle, MemorySegment.ofAddress(base), len); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Install a level-2/3 {@link Resolver}. The callback fires in-process on the tracer
         *  thread mid-single-step; its upcall stub is materialized into {@link #upcallArena}
         *  (open for this handle's lifetime) and both are pinned against GC. */
        public void setResolver(Resolver r) {
            if (DESCENT_SET_RESOLVER == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
            this.resolver = r;
            if (upcallArena == null) upcallArena = Arena.ofShared();
            try {
                resolverStub = LINKER.upcallStub(ON_RESOLVE_MH.bindTo(this), RESOLVER_DESC, upcallArena);
                DESCENT_SET_RESOLVER.invoke(handle, resolverStub, MemorySegment.NULL);
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Install a level-3 {@link Denylist} (consulted in addition to the deny-region set).
         *  Same GC-pinning / arena-lifetime discipline as {@link #setResolver}. */
        public void setDenylist(Denylist d) {
            if (DESCENT_SET_DENYLIST == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
            this.denylist = d;
            if (upcallArena == null) upcallArena = Arena.ofShared();
            try {
                denylistStub = LINKER.upcallStub(ON_DENY_MH.bindTo(this), DENYLIST_DESC, upcallArena);
                DESCENT_SET_DENYLIST.invoke(handle, denylistStub, MemorySegment.NULL);
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        // Upcall targets (package-private so the same-class Lookup.findVirtual resolves
        // them). The base_out/len_out cells arrive as zero-length segments, so reinterpret
        // to a u64 before writing.
        int onResolve(long callee, MemorySegment user, MemorySegment baseOut, MemorySegment lenOut) {
            Resolver r = this.resolver;
            if (r == null) return 0;
            long[] res = r.resolve(callee);
            if (res != null && res.length == 2 && res[1] != 0) {
                baseOut.reinterpret(JAVA_LONG.byteSize()).set(JAVA_LONG, 0, res[0]);
                lenOut.reinterpret(JAVA_LONG.byteSize()).set(JAVA_LONG, 0, res[1]);
                return 1;
            }
            return 0;
        }

        int onDeny(long callee, MemorySegment user) {
            Denylist d = this.denylist;
            return (d != null && d.deny(callee)) ? 1 : 0;
        }

        // ---- results (out) ----

        /** Number of recorded (call-site -&gt; callee) edges (level &gt;= 1). */
        public long edgesLen() {
            try { return (long) DESCENT_EDGES_LEN.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Every stepped-over call as an {@link Edge} (site, ABSOLUTE target, depth). */
        public Edge[] edges() {
            try {
                int n = (int) (long) DESCENT_EDGES_LEN.invoke(handle);
                Edge[] out = new Edge[n];
                for (int i = 0; i < n; i++)
                    out[i] = new Edge((long) DESCENT_EDGE_SITE.invoke(handle, (long) i),
                        (long) DESCENT_EDGE_TARGET.invoke(handle, (long) i),
                        (int) DESCENT_EDGE_DEPTH.invoke(handle, (long) i));
                return out;
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** Number of recorded frames (1 = frame 0 only; 2+ once callees were descended). */
        public long framesLen() {
            try { return (long) DESCENT_FRAMES_LEN.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The ABSOLUTE base address of frame {@code f}. */
        public long frameBase(long f) {
            try { return (long) DESCENT_FRAME_BASE.invoke(handle, f); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The byte length of frame {@code f}. */
        public long frameLen(long f) {
            try { return (long) DESCENT_FRAME_LEN.invoke(handle, f); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The nesting depth of frame {@code f} (0 = frame 0). */
        public int frameDepth(long f) {
            try { return (int) DESCENT_FRAME_DEPTH.invoke(handle, f); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The parent frame index of {@code f}, or -1 for the root (frame 0). */
        public int frameParent(long f) {
            try { return (int) DESCENT_FRAME_PARENT.invoke(handle, f); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The ordered instruction-offset stream of frame {@code f} (offsets are relative
         *  to that frame's base). */
        public long[] frameInsns(long f) {
            try {
                int n = (int) (long) DESCENT_FRAME_INSN_COUNT.invoke(handle, f);
                long[] out = new long[n];
                for (int i = 0; i < n; i++) out[i] = (long) DESCENT_FRAME_INSN_AT.invoke(handle, f, (long) i);
                return out;
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** The distinct basic-block start offsets of frame {@code f}. */
        public long[] frameBlocks(long f) {
            try {
                int n = (int) (long) DESCENT_FRAME_BLOCK_COUNT.invoke(handle, f);
                long[] out = new long[n];
                for (int i = 0; i < n; i++) out[i] = (long) DESCENT_FRAME_BLOCK_AT.invoke(handle, f, (long) i);
                return out;
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** True if a pool overflowed / a byte failed to decode (the record is incomplete). */
        public boolean truncated() {
            try { return (int) DESCENT_TRUNCATED.invoke(handle) != 0; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** True if descent stopped at a policy limit (max_depth / budget / recursion cap),
         *  as distinct from a pool overflow. */
        public boolean depthCapped() {
            try { return (int) DESCENT_DEPTH_CAPPED.invoke(handle) != 0; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Free the descent handle (idempotent: the handle is NULLed after
         *  {@code asmtest_descent_free}, so a double free is a no-op) and close the upcall
         *  Arena that backed any resolver/denylist trampolines. */
        public void free() {
            if (handle != null && DESCENT_FREE != null) {
                try { DESCENT_FREE.invoke(handle); }
                catch (Throwable t) { throw rethrow(t); }
                finally { handle = null; }
            }
            if (upcallArena != null) {
                try { upcallArena.close(); }
                finally { upcallArena = null; resolverStub = null; denylistStub = null; }
            }
        }

        @Override public void close() { free(); }
    }

    // ---- Time-aware code-image recorder (asmtest_codeimage.h) ----
    //
    // A userspace PERF_RECORD_TEXT_POKE: a TIMESTAMPED CODE-IMAGE TIMELINE for a target
    // process. track() snapshots a region (version 0) and arms write-protect; refresh()
    // re-snapshots only the pages that changed since the last arm, appending a new version
    // stamped with the next monotonic sequence; bytesAt(addr, when) answers "what bytes
    // were live at addr as of sequence `when`" — the query the versioned W2 decoder needs
    // to reconstruct a JIT method whose address was reused mid-trace. The change detection
    // is pure userspace (soft-dirty / PAGEMAP_SCAN) and works on a FOREIGN process; pid 0
    // records THIS process. Mirrors the Ptrace wrapper above.

    /** A code-emission event from the optional eBPF detector (asmtest_codeimage_event_t):
     *  the published base {@code addr} and {@code len}, the {@code timestamp}
     *  (bpf_ktime_get_ns), the {@code pid}/{@code tid} that published, the {@code kind}
     *  ({@code ASMTEST_CI_KIND_*}), and a memfd {@code fd} (or -1). */
    public record CodeEvent(long addr, long len, long timestamp, int pid, int tid,
                            int kind, int fd) {}

    // asmtest_codeimage_event_t {u64 addr; u64 len; u64 timestamp; u32 pid; u32 tid;
    //   u32 kind; i32 fd;} — three u64 then four u32, naturally aligned, no padding: 40
    // bytes (a _Static_assert in src/codeimage.c pins the size).
    private static final MemoryLayout CI_EVENT_LAYOUT = MemoryLayout.structLayout(
        JAVA_LONG.withName("addr"),
        JAVA_LONG.withName("len"),
        JAVA_LONG.withName("timestamp"),
        JAVA_INT.withName("pid"),
        JAVA_INT.withName("tid"),
        JAVA_INT.withName("kind"),
        JAVA_INT.withName("fd"));

    /** A timestamped code-image timeline for one target process (asmtest_codeimage_t). */
    public static final class CodeImage implements AutoCloseable {
        private MemorySegment handle; // an asmtest_codeimage_t*

        /** Create a timeline recording {@code pid}'s memory ({@code pid == 0} => this
         *  process). Throws if the library is not loaded or allocation fails. */
        public CodeImage(int pid) {
            if (CI_NEW == null) throw new RuntimeException("libasmtest_hwtrace not loaded", LOAD_ERROR);
            try {
                MemorySegment h = (MemorySegment) CI_NEW.invoke(pid);
                if (MemorySegment.NULL.equals(h))
                    throw new RuntimeException("asmtest_codeimage_new failed");
                this.handle = h;
            } catch (RuntimeException re) { throw re; }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** True if the userspace recorder can detect page changes on this host
         *  (PAGEMAP_SCAN or the soft-dirty fallback). Never throws: a load failure or a
         *  zero return → false, so callers self-skip cleanly. */
        public static boolean available() {
            if (CI_AVAILABLE == null) return false;
            try { return (int) CI_AVAILABLE.invoke() != 0; }
            catch (Throwable t) { return false; }
        }

        /** Human-readable reason {@link #available()} is false (or "available"). */
        public static String skipReason() {
            if (CI_SKIP_REASON == null) {
                Throwable e = LOAD_ERROR;
                return "libasmtest_hwtrace not loaded" + (e != null ? ": " + e : "");
            }
            try (Arena a = Arena.ofConfined()) {
                MemorySegment buf = a.allocate(160);
                CI_SKIP_REASON.invoke(buf, 160L);
                return buf.getString(0);
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** Begin tracking {@code [base, base+len)}: snapshot version 0 now and arm
         *  write-protect on its pages. Returns the status ({@code ASMTEST_CI_OK} or a
         *  negative code). */
        public int track(long base, long len) {
            try { return (int) CI_TRACK.invoke(handle, MemorySegment.ofAddress(base), len); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Scan the tracked ranges for changed pages, re-snapshot each as a new version,
         *  and re-arm. Returns the number of new versions recorded ({@code >= 0}) or a
         *  negative status. */
        public int refresh() {
            try { return (int) CI_REFRESH.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The current capture sequence — a monotonic logical timestamp. 0 before
         *  anything is tracked; advances by one per recorded version. */
        public long now() {
            try { return (long) CI_NOW.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** The bytes live at {@code addr} as of capture sequence {@code when}
         *  ({@code when == 0} => the latest version), copied into a fresh {@code byte[]},
         *  or {@code null} when {@code addr} is in no tracked region / no version exists
         *  at-or-before {@code when} ({@code ASMTEST_CI_ENOENT}). */
        public byte[] bytesAt(long addr, long when) {
            try (Arena a = Arena.ofConfined()) {
                // The C call fills *out with a pointer to BORROWED bytes and *out_len with
                // how many are available; allocate two pointer-sized out cells, then copy.
                MemorySegment outPtr = a.allocate(ADDRESS);   // const uint8_t**
                MemorySegment outLen = a.allocate(JAVA_LONG); // size_t*
                int rc = (int) CI_BYTES_AT.invoke(handle, MemorySegment.ofAddress(addr),
                    when, outPtr, outLen);
                if (rc != ASMTEST_CI_OK) return null;
                long n = outLen.get(JAVA_LONG, 0);
                MemorySegment src = outPtr.get(ADDRESS, 0).reinterpret(n);
                byte[] bytes = new byte[(int) n];
                MemorySegment.copy(src, JAVA_BYTE, 0, bytes, 0, (int) n);
                return bytes;
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** True if the optional eBPF emission detector can load and attach on this host.
         *  Never throws: a load failure or a zero return → false. */
        public static boolean bpfAvailable() {
            if (CI_BPF_AVAILABLE == null) return false;
            try { return (int) CI_BPF_AVAILABLE.invoke() != 0; }
            catch (Throwable t) { return false; }
        }

        /** Human-readable reason {@link #bpfAvailable()} is false (or "available"). */
        public static String bpfSkipReason() {
            if (CI_BPF_SKIP_REASON == null) {
                Throwable e = LOAD_ERROR;
                return "libasmtest_hwtrace not loaded" + (e != null ? ": " + e : "");
            }
            try (Arena a = Arena.ofConfined()) {
                MemorySegment buf = a.allocate(160);
                CI_BPF_SKIP_REASON.invoke(buf, 160L);
                return buf.getString(0);
            } catch (Throwable t) { throw rethrow(t); }
        }

        /** Load, filter to this timeline's pid, and attach the CO-RE eBPF program;
         *  subsequent {@link #pollBpf(int)} calls drain emission events. Returns the
         *  status ({@code ASMTEST_CI_OK} or a negative code). */
        public int watchBpf() {
            try { return (int) CI_WATCH_BPF.invoke(handle); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Drain ready emission events from the BPF ring buffer into the internal queue.
         *  {@code timeoutMs == 0} is a non-blocking drain. Returns the number queued
         *  ({@code >= 0}) or a negative status. */
        public int pollBpf(int timeoutMs) {
            try { return (int) CI_POLL_BPF.invoke(handle, timeoutMs); }
            catch (Throwable t) { throw rethrow(t); }
        }

        /** Pop one queued emission event, or {@code null} when the queue is empty (or on
         *  a negative status). */
        public CodeEvent nextEvent() {
            try (Arena a = Arena.ofConfined()) {
                MemorySegment out = a.allocate(CI_EVENT_LAYOUT);
                int rc = (int) CI_NEXT.invoke(handle, out);
                if (rc != 1) return null;
                return new CodeEvent(
                    out.get(JAVA_LONG, ofs("addr")),
                    out.get(JAVA_LONG, ofs("len")),
                    out.get(JAVA_LONG, ofs("timestamp")),
                    out.get(JAVA_INT, ofs("pid")),
                    out.get(JAVA_INT, ofs("tid")),
                    out.get(JAVA_INT, ofs("kind")),
                    out.get(JAVA_INT, ofs("fd")));
            } catch (Throwable t) { throw rethrow(t); }
        }

        private static long ofs(String field) {
            return CI_EVENT_LAYOUT.byteOffset(MemoryLayout.PathElement.groupElement(field));
        }

        /** The underlying {@code asmtest_codeimage_t*} handle — pass to
         *  {@link HwTrace#ptraceTraceAttachedVersioned} as its {@code img} argument. */
        public MemorySegment handle() { return handle; }

        /** Free the timeline and all recorded versions (detaches any eBPF watch). */
        public void free() {
            if (handle == null || CI_FREE == null) return;
            try { CI_FREE.invoke(handle); } catch (Throwable t) { throw rethrow(t); }
            finally { handle = null; }
        }

        @Override public void close() { free(); }
    }
}
