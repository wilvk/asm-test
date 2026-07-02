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

    // Resolved when the library loads; null when it can't (then available() == false).
    private static final MethodHandle HW_AVAILABLE, HW_SKIP_REASON, HW_RESOLVE, HW_AUTO,
        TRACE_RESOLVE, TRACE_AUTO,
        HW_INIT, HW_SHUTDOWN,
        REGISTER_REGION, HW_BEGIN, HW_END, EXEC_ALLOC, EXEC_FREE,
        TRACE_NEW, TRACE_FREE, TRACE_COVERED, TRACE_BLOCKS_LEN, TRACE_INSNS_TOTAL,
        TRACE_INSNS_LEN, TRACE_TRUNCATED, TRACE_BLOCK_AT, TRACE_INSN_AT,
        // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
        PTRACE_AVAILABLE, PTRACE_SKIP_REASON, PTRACE_TRACE_CALL, PTRACE_TRACE_ATTACHED,
        PTRACE_TRACE_ATTACHED_VERSIONED, PTRACE_RUN_TO,
        PROC_REGION_BY_ADDR, PROC_PERFMAP_SYMBOL, JITDUMP_FIND,
        // asmtest_codeimage.h — time-aware code-image recorder (a userspace TEXT_POKE).
        CI_AVAILABLE, CI_SKIP_REASON, CI_NEW, CI_FREE, CI_TRACK, CI_REFRESH, CI_NOW,
        CI_BYTES_AT, CI_BPF_AVAILABLE, CI_BPF_SKIP_REASON, CI_WATCH_BPF, CI_POLL_BPF,
        CI_NEXT;

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
            registerRegion = null, hwBegin = null, hwEnd = null, execAlloc = null, execFree = null,
            traceNew = null, traceFree = null, traceCovered = null, traceBlocksLen = null,
            traceInsnsTotal = null, traceInsnsLen = null, traceTruncated = null,
            traceBlockAt = null, traceInsnAt = null,
            ptraceAvailable = null, ptraceSkipReason = null, ptraceTraceCall = null,
            ptraceTraceAttached = null, ptraceTraceAttachedVersioned = null, ptraceRunTo = null,
            procRegionByAddr = null, procPerfmapSymbol = null,
            jitdumpFind = null,
            ciAvailable = null, ciSkipReason = null, ciNew = null, ciFree = null,
            ciTrack = null, ciRefresh = null, ciNow = null, ciBytesAt = null,
            ciBpfAvailable = null, ciBpfSkipReason = null, ciWatchBpf = null,
            ciPollBpf = null, ciNext = null;
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
        PTRACE_TRACE_ATTACHED_VERSIONED = ptraceTraceAttachedVersioned; PTRACE_RUN_TO = ptraceRunTo;
        PROC_REGION_BY_ADDR = procRegionByAddr; PROC_PERFMAP_SYMBOL = procPerfmapSymbol;
        JITDUMP_FIND = jitdumpFind;
        CI_AVAILABLE = ciAvailable; CI_SKIP_REASON = ciSkipReason; CI_NEW = ciNew;
        CI_FREE = ciFree; CI_TRACK = ciTrack; CI_REFRESH = ciRefresh; CI_NOW = ciNow;
        CI_BYTES_AT = ciBytesAt; CI_BPF_AVAILABLE = ciBpfAvailable;
        CI_BPF_SKIP_REASON = ciBpfSkipReason; CI_WATCH_BPF = ciWatchBpf;
        CI_POLL_BPF = ciPollBpf; CI_NEXT = ciNext;
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
                return buf.getUtf8String(0);
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
                return buf.getUtf8String(0);
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
