// HwTrace.cs — asm-test .NET binding: the single-step hardware-trace tier.
//
// The peer of DrTrace.cs for the optional hardware-trace tier (see
// include/asmtest_hwtrace.h and docs/native-tracing.md). It records the same
// asmtest_trace_t offsets as the emulator and DynamoRIO tiers, but by observing
// the real CPU — and unlike the DynamoRIO wrapper it needs no DynamoRIO install.
//
// Four backends share one API, selected by enum. SINGLESTEP drives the x86
// EFLAGS.TF single-step debug exception (#DB -> SIGTRAP) to record every executed
// RIP: it is exact and complete on ANY x86-64 Linux (Intel, any-Zen AMD, VM, CI,
// container) with no PMU, no perf_event, no privilege, and no decoder library, so
// it is the portable default this binding's self-test exercises live. The
// INTEL_PT / CORESIGHT / AMD_LBR backends self-skip off the bare-metal hardware
// they need. HwTrace.Available(backend) reports whether the chosen backend can
// run so callers self-skip cleanly.
//
// All P/Invoke (DllImport) stays inside the Native class, mirroring DrTrace.cs.
// The logical library "asmtest_hwtrace" is resolved by a DllImportResolver:
// ASMTEST_HWTRACE_LIB -> <repo>/build/libasmtest_hwtrace.so -> NativeLibrary.Load.
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Asmtest
{
    /// <summary>The hardware-trace backend to select (mirrors asmtest_trace_backend_t).</summary>
    public enum HwBackend { IntelPt = 0, CoreSight = 1, AmdLbr = 2, SingleStep = 3 }

    /// <summary>
    /// Backend auto-selection policy (mirrors asmtest_hwtrace_policy_t). Best is the
    /// most faithful available backend; CeilingFree additionally skips the one
    /// fixed-window backend (AMD LBR) so the pick has no completeness ceiling —
    /// re-resolve under it after a trace comes back truncated.
    /// </summary>
    public enum HwPolicy { Best = 0, CeilingFree = 1 }

    /// <summary>The cross-tier trace tier, most-faithful to least (mirrors asmtest_trace_tier_t).</summary>
    public enum TraceTier { HwTrace = 0, DynamoRio = 1, Emulator = 2 }

    /// <summary>
    /// Execution fidelity of a tier (mirrors asmtest_trace_fidelity_t). Native runs
    /// the real bytes on the real CPU in-process; Virtual runs an isolated guest on an
    /// emulated CPU. The single Native-&gt;Virtual transition is the line TracePolicy.NativeOnly gates.
    /// </summary>
    public enum TraceFidelity { Native = 0, Virtual = 1 }

    /// <summary>
    /// Cross-tier auto-selection policy bitmask (mirrors the ASMTEST_TRACE_* flags).
    /// Best is the most-faithful available choice with the emulator floor allowed;
    /// CeilingFree additionally drops the one fixed-window backend (AMD LBR); NativeOnly
    /// forbids the native-&gt;emulator fidelity crossing (drops the emulator floor).
    /// </summary>
    [Flags]
    public enum TracePolicy { Best = 0x0, CeilingFree = 0x1, NativeOnly = 0x2 }

    // The native entry points for libasmtest_hwtrace. Internal: callers use the
    // typed HwTrace / NativeCode classes below, never DllImport directly. Loading
    // is wrapped so a missing lib (tier not built) self-skips cleanly via
    // HwTrace.Available() rather than throwing out.
    internal static class HwNative
    {
        const string HWTRACE = "asmtest_hwtrace";

        public const int ASMTEST_HW_OK = 0;
        public const int ASMTEST_HW_EUNAVAIL = -3; // no hardware-trace backend available
        public const int SINGLESTEP = 3;

        // asmtest_trace_choice_t: three int-sized enum fields, no padding (pinned by a
        // static_assert in asmtest_trace_auto.h), so it marshals as three consecutive C
        // ints — matching the [Out] Choice[] the cross-tier resolve writes into.
        [StructLayout(LayoutKind.Sequential)]
        public struct Choice
        {
            public int Tier;     // asmtest_trace_tier_t
            public int Backend;  // asmtest_trace_backend_t (valid iff Tier == TIER_HWTRACE)
            public int Fidelity; // asmtest_trace_fidelity_t
        }

        // asmtest_hwtrace_options_t: backend + two ring sizes + snapshot flag + an
        // optional object-file hint. object_hint is marshalled by hand
        // (Marshal.StringToHGlobalAnsi) into the IntPtr field so a null hint maps to
        // NULL — the single-step backend ignores it entirely.
        [StructLayout(LayoutKind.Sequential)]
        public struct Options
        {
            public int Backend;       // asmtest_trace_backend_t
            public UIntPtr AuxSize;   // size_t: AUX (trace) ring bytes (0 = default)
            public UIntPtr DataSize;  // size_t: base perf ring bytes (0 = default)
            public int Snapshot;      // nonzero: circular snapshot ring
            public IntPtr ObjectHint; // const char*: optional object-file path
        }

        // The resolved libasmtest_hwtrace handle (IntPtr.Zero if it could not load).
        // Loaded eagerly here — NOT in the static ctor — because C# runs static
        // FIELD initializers (LibHandle, LibAvailable below) BEFORE the static
        // constructor. The probe must therefore load the lib itself rather than lean
        // on the DllImport resolver, which the static ctor registers later (the same
        // ordering gotcha DrTrace.cs documents). The resolver then just hands back
        // this already-loaded handle.
        public static readonly IntPtr LibHandle = LoadLib();

        // The absolute path of the libasmtest_hwtrace actually loaded (null if none
        // loaded). Lets a clean-room test assert the bundled tier — not a leaked
        // build/ tree — satisfied the load; exposed via HwTrace.LibraryPath().
        public static string ResolvedPath { get; private set; }

        static IntPtr LoadLib()
        {
            foreach (var cand in Candidates())
                if (NativeLibrary.TryLoad(cand, out var h)) { ResolvedPath = cand; return h; }
            return IntPtr.Zero;
        }

        // Map the logical name "asmtest_hwtrace" to the handle resolved above.
        // Registers on first access to the type (after the field initializers run).
        static HwNative()
        {
            NativeLibrary.SetDllImportResolver(typeof(HwNative).Assembly, (name, asm, paths) =>
                name == HWTRACE && LibHandle != IntPtr.Zero ? LibHandle : IntPtr.Zero);
        }

        // Candidate paths, in priority order: ASMTEST_HWTRACE_LIB, then the NuGet
        // bundled slot (runtimes/<rid>/native/, staged by `make dotnet-package`),
        // then the in-tree build dir (<repo>/build/libasmtest_hwtrace.so), then the
        // bare soname. The bundled slot is tried BEFORE the dev build/ tree, so an
        // installed package never prefers a leaked checkout; system search is last.
        static System.Collections.Generic.IEnumerable<string> Candidates()
        {
            var env = Environment.GetEnvironmentVariable("ASMTEST_HWTRACE_LIB");
            if (!string.IsNullOrEmpty(env)) yield return env;
            var bundled = BundledPath(LibFileName());
            if (bundled != null) yield return bundled;
            var repo = RepoRoot();
            if (repo != null) yield return Path.Combine(repo, "build", LibFileName());
            yield return LibFileName();
        }

        // The platform-specific libasmtest_hwtrace file name (mirrors the Python
        // wrapper: .dylib on macOS, .so elsewhere).
        static string LibFileName() =>
            RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? "libasmtest_hwtrace.dylib" : "libasmtest_hwtrace.so";

        // The NuGet-bundled native slot next to the loaded assembly:
        // <AppContext.BaseDirectory>/runtimes/<rid>/native/<fileName>, where <rid>
        // is this process's RuntimeIdentifier — the RID layout `make dotnet-package`
        // stages and the .NET loader restores. Returns null if the file is absent.
        static string BundledPath(string fileName)
        {
            try
            {
                var rid = RuntimeInformation.RuntimeIdentifier;
                if (string.IsNullOrEmpty(rid)) return null;
                var p = Path.Combine(AppContext.BaseDirectory, "runtimes", rid, "native", fileName);
                return File.Exists(p) ? p : null;
            }
            catch { return null; }
        }

        // Walk up from the assembly's directory looking for the repo root (the dir
        // holding "build/"). The binding's bin/ output sits several levels under the
        // repo, so this finds <repo>/build/libasmtest_hwtrace.so without an env var.
        static string RepoRoot()
        {
            try
            {
                var dir = AppContext.BaseDirectory;
                for (int i = 0; i < 12 && dir != null; i++)
                {
                    if (Directory.Exists(Path.Combine(dir, "build")) &&
                        Directory.Exists(Path.Combine(dir, "include")))
                        return dir;
                    dir = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar));
                }
            }
            catch { /* fall through */ }
            return null;
        }

        // ---- lifecycle ----
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_available(int backend);
        [DllImport(HWTRACE)] public static extern void asmtest_hwtrace_skip_reason(int backend, byte[] buf, UIntPtr buflen);
        // Auto-select: resolve writes up to cap available backend enums into out[],
        // most-faithful first, returning the count; auto returns the single best
        // backend enum (>= 0) or ASMTEST_HW_EUNAVAIL (-3) when none.
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_hwtrace_resolve(int policy, int[] @out, UIntPtr cap);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_auto(int policy);
        // Cross-tier orchestrator (asmtest_trace_auto.h), over hwtrace + DynamoRIO +
        // emulator. resolve writes up to cap Choice triples into out[], most-faithful
        // first, returning the count; auto fills one Choice and returns ASMTEST_HW_OK
        // (0) or ASMTEST_HW_EUNAVAIL (-3) when the cascade is empty.
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_trace_resolve(uint policy, [Out] Choice[] @out, UIntPtr cap);
        [DllImport(HWTRACE)] public static extern int asmtest_trace_auto(uint policy, out Choice @out);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_init(ref Options opts);
        [DllImport(HWTRACE)] public static extern void asmtest_hwtrace_shutdown();

        // ---- region registration + markers (const char* name) ----
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_register_region(
            [MarshalAs(UnmanagedType.LPStr)] string name, IntPtr @base, UIntPtr len, IntPtr trace);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern void asmtest_hwtrace_begin([MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern void asmtest_hwtrace_end([MarshalAs(UnmanagedType.LPStr)] string name);

        // ---- host-native executable code (out-param form, NOT a struct) ----
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_exec_alloc(
            byte[] bytes, UIntPtr len, out IntPtr baseOut, out UIntPtr lenOut);
        [DllImport(HWTRACE)] public static extern void asmtest_hwtrace_exec_free(IntPtr @base, UIntPtr len);

        // ---- trace handle + accessors (insns_cap FIRST, blocks_cap SECOND) ----
        [DllImport(HWTRACE)] public static extern IntPtr asmtest_trace_new(UIntPtr insnsCap, UIntPtr blocksCap);
        [DllImport(HWTRACE)] public static extern void asmtest_trace_free(IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_trace_covered(IntPtr trace, ulong off);
        [DllImport(HWTRACE)] public static extern ulong asmtest_emu_trace_blocks_len(IntPtr trace);
        [DllImport(HWTRACE)] public static extern ulong asmtest_emu_trace_insns_total(IntPtr trace);
        [DllImport(HWTRACE)] public static extern ulong asmtest_emu_trace_insns_len(IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_emu_trace_truncated(IntPtr trace);
        [DllImport(HWTRACE)] public static extern ulong asmtest_emu_trace_block_at(IntPtr trace, UIntPtr i);
        [DllImport(HWTRACE)] public static extern ulong asmtest_emu_trace_insn_at(IntPtr trace, UIntPtr i);

        // ---- out-of-process / foreign-process tracing (asmtest_ptrace.h) ----
        // The same libasmtest_hwtrace ships these. The C `long` in the trace_call /
        // trace_attached args/result is 64-bit on Linux x86-64, so it marshals as
        // long[] / out long. pid is a C int.
        public const int ASMTEST_PTRACE_OK = 0;
        public const int ASMTEST_PTRACE_ENOENT = -7; // region / symbol / method not found

        // asmtest_jitdump_entry_t: four u64 fields, no padding — a method as recorded
        // in a jitdump JIT_CODE_LOAD record (32 bytes).
        [StructLayout(LayoutKind.Sequential)]
        public struct JitEntry
        {
            public ulong CodeAddr;  // load address (the base to trace)
            public ulong CodeSize;  // code length in bytes
            public ulong Timestamp; // record timestamp (load order)
            public ulong CodeIndex; // the JIT's unique index for this code
        }

        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_available();
        [DllImport(HWTRACE)] public static extern void asmtest_ptrace_skip_reason(byte[] buf, UIntPtr buflen);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_call(
            IntPtr code, UIntPtr len, long[] args, int nargs, out long result, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached(
            int pid, IntPtr @base, UIntPtr len, out long result, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_run_to(int pid, IntPtr addr);
        [DllImport(HWTRACE)] public static extern int asmtest_proc_region_by_addr(
            int pid, IntPtr addr, out IntPtr baseOut, out UIntPtr lenOut);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)] public static extern int asmtest_proc_perfmap_symbol(
            int pid, [MarshalAs(UnmanagedType.LPStr)] string name, out IntPtr baseOut, out UIntPtr lenOut);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)] public static extern int asmtest_jitdump_find(
            [MarshalAs(UnmanagedType.LPStr)] string path, int pid,
            [MarshalAs(UnmanagedType.LPStr)] string name,
            out JitEntry @out, byte[] bytesOut, UIntPtr bytesCap, out UIntPtr bytesLen);

        // Version-aware out-of-process trace (asmtest_ptrace.h): like trace_attached, but
        // it reconstructs the region's bytes from a code-image timeline (img) as of capture
        // sequence `when` rather than from a single late snapshot — the temporally-correct
        // read for a live JIT. img/when may be NULL/0 to fall back to a fresh read. pid is a
        // C int, len a size_t, the C `long` result a 64-bit long on Linux x86-64.
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached_versioned(
            int pid, IntPtr @base, UIntPtr len, IntPtr img, ulong when, out long result, IntPtr trace);

        // ---- time-aware code-image recorder (asmtest_codeimage.h) ----
        // A userspace PERF_RECORD_TEXT_POKE: track() snapshots a region and arms
        // write-protect; refresh() re-snapshots only changed pages as new versions;
        // bytes_at(addr, when) answers what bytes were live at a logical timestamp. The
        // optional eBPF emission detector self-skips without libbpf / CAP_BPF. pid is a C
        // int (0 == self); the opaque img handle is an IntPtr; out-pointer reads marshal by
        // hand (see CodeImage.BytesAt).
        public const int ASMTEST_CI_OK = 0;
        public const int ASMTEST_CI_ENOENT = -7; // addr never tracked / no version at-or-before when

        // asmtest_codeimage_event_t: three u64 + three u32 + one i32, no padding (a
        // _Static_assert in src/codeimage.c pins the 40-byte size), so it marshals as a
        // flat sequential struct.
        [StructLayout(LayoutKind.Sequential)]
        public struct CodeImageEvent
        {
            public ulong Addr;      // published base address (0 for a memfd hint)
            public ulong Len;       // byte length (0 for a memfd hint)
            public ulong Timestamp; // bpf_ktime_get_ns() at emission
            public uint Pid;        // tgid that published
            public uint Tid;        // thread that published
            public uint Kind;       // ASMTEST_CI_KIND_*
            public int Fd;          // memfd fd, or -1
        }

        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_available();
        [DllImport(HWTRACE)] public static extern void asmtest_codeimage_skip_reason(byte[] buf, UIntPtr buflen);
        [DllImport(HWTRACE)] public static extern IntPtr asmtest_codeimage_new(int pid);
        [DllImport(HWTRACE)] public static extern void asmtest_codeimage_free(IntPtr img);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_track(IntPtr img, IntPtr @base, UIntPtr len);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_refresh(IntPtr img);
        [DllImport(HWTRACE)] public static extern ulong asmtest_codeimage_now(IntPtr img);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_bytes_at(
            IntPtr img, IntPtr addr, ulong when, out IntPtr @out, out UIntPtr outLen);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_bpf_available();
        [DllImport(HWTRACE)] public static extern void asmtest_codeimage_bpf_skip_reason(byte[] buf, UIntPtr buflen);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_watch_bpf(IntPtr img);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_poll_bpf(IntPtr img, int timeoutMs);
        [DllImport(HWTRACE)] public static extern int asmtest_codeimage_next(IntPtr img, out CodeImageEvent @out);

        // Whether libasmtest_hwtrace loaded at all. A missing lib (tier not built)
        // self-skips via HwTrace.Available() rather than crashing. Derived from the
        // handle resolved by LoadLib() above (resolver-free, so it is robust to the
        // field-initializer-before-static-ctor ordering).
        public static readonly bool LibAvailable = LibHandle != IntPtr.Zero;
    }

    /// <summary>Thrown by the hardware-trace wrappers on a nonzero native status code.</summary>
    public sealed class HwTraceException : Exception
    {
        public HwTraceException(string message) : base(message) { }
    }

    /// <summary>The signature generated code is invoked through: two longs in, a long out (SysV).</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate long HwFunc2(long a, long b);

    /// <summary>
    /// A resolved cross-tier trace option (mirrors asmtest_trace_choice_t): which
    /// <see cref="Tier"/> to use, which hardware <see cref="Backend"/> within it
    /// (meaningful only when <see cref="Tier"/> == <see cref="TraceTier.HwTrace"/>),
    /// and the <see cref="Fidelity"/> class so a caller can see at a glance whether a
    /// choice crosses the native-&gt;emulator line.
    /// </summary>
    public readonly struct TierChoice
    {
        public TraceTier Tier { get; }
        public HwBackend Backend { get; }
        public TraceFidelity Fidelity { get; }

        public TierChoice(TraceTier tier, HwBackend backend, TraceFidelity fidelity)
        {
            Tier = tier;
            Backend = backend;
            Fidelity = fidelity;
        }

        public override string ToString() =>
            $"TierChoice(tier={Tier}, backend={Backend}, fidelity={Fidelity})";
    }

    /// <summary>
    /// Host-native machine code in real executable (W^X) memory. Allocate with
    /// <see cref="FromBytes"/>, invoke through a function pointer with
    /// <see cref="Call"/>, and release with <see cref="Free"/>.
    /// </summary>
    public sealed class NativeCode
    {
        IntPtr _base;
        UIntPtr _len;
        bool _freed;

        NativeCode(IntPtr @base, UIntPtr len) { _base = @base; _len = len; }

        /// <summary>Map executable memory and copy the host-native machine-code bytes into it.</summary>
        public static NativeCode FromBytes(byte[] bytes)
        {
            int rc = HwNative.asmtest_hwtrace_exec_alloc(
                bytes, (UIntPtr)bytes.Length, out var baseOut, out var lenOut);
            if (rc != HwNative.ASMTEST_HW_OK)
                throw new HwTraceException($"asmtest_hwtrace_exec_alloc failed: {rc}");
            return new NativeCode(baseOut, lenOut);
        }

        /// <summary>The executable mapping's base address (offset 0 = entry).</summary>
        public IntPtr Base => _base;

        /// <summary>The number of code bytes.</summary>
        public long Length => (long)_len;

        /// <summary>
        /// Invoke the code through a function pointer with two integer args, reading
        /// the result as a long (the SysV integer ABI).
        /// </summary>
        public long Call(long a, long b)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<HwFunc2>(_base);
            return fn(a, b);
        }

        /// <summary>Unmap the executable memory.</summary>
        public void Free()
        {
            if (!_freed)
            {
                HwNative.asmtest_hwtrace_exec_free(_base, _len);
                _freed = true;
            }
        }
    }

    /// <summary>
    /// A coverage recorder for a registered native region, via the hardware tier.
    /// Bring the tier up once with <see cref="Init"/>, allocate per-trace recorders
    /// with <see cref="Create"/>, register a <see cref="NativeCode"/> under a name,
    /// run it inside <see cref="Region"/>, then read back coverage / the instruction
    /// stream. Tear the process-wide tier down with <see cref="Shutdown"/>.
    /// </summary>
    public sealed class HwTrace
    {
        IntPtr _handle;

        HwTrace(IntPtr handle) => _handle = handle;

        // ---- process-wide lifecycle ----

        /// <summary>
        /// True if the chosen backend can run on this host (self-skip otherwise).
        /// Never throws — a missing lib or unavailable backend returns false so
        /// callers self-skip. SINGLESTEP is available on any x86-64 Linux.
        /// </summary>
        public static bool Available(HwBackend backend = HwBackend.SingleStep)
        {
            if (!HwNative.LibAvailable) return false;
            try { return HwNative.asmtest_hwtrace_available((int)backend) != 0; }
            catch { return false; }
        }

        /// <summary>
        /// The absolute path of the libasmtest_hwtrace this process loaded (null if
        /// the lib is not available). Lets a clean-room test assert the bundled tier —
        /// not a leaked build/ tree — satisfied the load.
        /// </summary>
        public static string LibraryPath() => HwNative.ResolvedPath;

        /// <summary>Human-readable reason <see cref="Available"/> is false (or "available").</summary>
        public static string SkipReason(HwBackend backend = HwBackend.SingleStep)
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            var buf = new byte[160];
            HwNative.asmtest_hwtrace_skip_reason((int)backend, buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, z < 0 ? buf.Length : z);
        }

        /// <summary>
        /// This host's hardware-trace fallback cascade: the available backend enums,
        /// most-faithful first (INTEL_PT &gt; AMD_LBR &gt; SINGLESTEP &gt; CORESIGHT),
        /// honoring <paramref name="policy"/>. Empty only off x86-64 Linux (single-step
        /// is the floor there) or when the lib is missing. CeilingFree drops the
        /// depth-bounded backend (AMD LBR).
        /// </summary>
        public static int[] Resolve(HwPolicy policy = HwPolicy.Best)
        {
            if (!HwNative.LibAvailable) return Array.Empty<int>();
            var buf = new int[4];
            int n = (int)HwNative.asmtest_hwtrace_resolve((int)policy, buf, (UIntPtr)buf.Length);
            var cascade = new int[n];
            Array.Copy(buf, cascade, n);
            return cascade;
        }

        /// <summary>
        /// The single most-preferred available backend under <paramref name="policy"/>
        /// (a backend enum &gt;= 0, ready to <see cref="Init"/>), or
        /// <c>ASMTEST_HW_EUNAVAIL</c> (-3) when no hardware-trace backend is available
        /// on this host.
        /// </summary>
        public static int Auto(HwPolicy policy = HwPolicy.Best)
        {
            if (!HwNative.LibAvailable) return HwNative.ASMTEST_HW_EUNAVAIL;
            return HwNative.asmtest_hwtrace_auto((int)policy);
        }

        /// <summary>
        /// This host's full CROSS-TIER cascade (asmtest_trace_resolve), most-faithful
        /// first: Intel PT -&gt; AMD LBR -&gt; DynamoRIO -&gt; single-step -&gt; CoreSight
        /// -&gt; emulator, each included only if its tier is available on this host.
        /// Returns the resolved <see cref="TierChoice"/> options. Empty only off a
        /// native host under <see cref="TracePolicy.NativeOnly"/> (which drops the
        /// emulator floor) or when the lib is missing. <see cref="TracePolicy.CeilingFree"/>
        /// drops the fixed-window backend (AMD LBR).
        /// </summary>
        public static TierChoice[] ResolveTiers(TracePolicy policy = TracePolicy.Best)
        {
            if (!HwNative.LibAvailable) return Array.Empty<TierChoice>();
            var buf = new HwNative.Choice[8];
            int n = (int)HwNative.asmtest_trace_resolve((uint)policy, buf, (UIntPtr)buf.Length);
            var choices = new TierChoice[n];
            for (int i = 0; i < n; i++)
                choices[i] = new TierChoice(
                    (TraceTier)buf[i].Tier, (HwBackend)buf[i].Backend, (TraceFidelity)buf[i].Fidelity);
            return choices;
        }

        /// <summary>
        /// The single most-preferred available cross-tier choice under
        /// <paramref name="policy"/> (asmtest_trace_auto), or <c>null</c> when the
        /// cascade is empty (only off a native host under
        /// <see cref="TracePolicy.NativeOnly"/>) or the lib is missing.
        /// </summary>
        public static TierChoice? AutoTier(TracePolicy policy = TracePolicy.Best)
        {
            if (!HwNative.LibAvailable) return null;
            int rc = HwNative.asmtest_trace_auto((uint)policy, out var c);
            if (rc != HwNative.ASMTEST_HW_OK) return null;
            return new TierChoice((TraceTier)c.Tier, (HwBackend)c.Backend, (TraceFidelity)c.Fidelity);
        }

        /// <summary>
        /// Select a backend and initialize the tier. SINGLESTEP is the portable
        /// default that runs on any x86-64 Linux. Throws <see cref="HwTraceException"/>
        /// on a nonzero status.
        /// </summary>
        public static void Init(HwBackend backend = HwBackend.SingleStep)
        {
            var opts = new HwNative.Options { Backend = (int)backend };
            int rc = HwNative.asmtest_hwtrace_init(ref opts);
            if (rc != HwNative.ASMTEST_HW_OK)
                throw new HwTraceException($"asmtest_hwtrace_init failed: {rc}");
        }

        /// <summary>Tear the hardware-trace tier down.</summary>
        public static void Shutdown() => HwNative.asmtest_hwtrace_shutdown();

        // ---- per-trace ----

        /// <summary>
        /// Allocate a trace recorder. Block recording is on when blocks &gt; 0,
        /// instruction recording when instructions &gt; 0. NOTE: the native
        /// asmtest_trace_new takes insns_cap FIRST, blocks_cap SECOND.
        /// </summary>
        public static HwTrace Create(int blocks = 64, int instructions = 0)
        {
            IntPtr h = HwNative.asmtest_trace_new((UIntPtr)(nuint)instructions, (UIntPtr)(nuint)blocks);
            if (h == IntPtr.Zero) throw new HwTraceException("asmtest_trace_new failed");
            return new HwTrace(h);
        }

        /// <summary>Register a native code range under <paramref name="name"/>, recording into this trace.</summary>
        public HwTrace Register(string name, NativeCode code)
        {
            int rc = HwNative.asmtest_hwtrace_register_region(
                name, code.Base, (UIntPtr)code.Length, _handle);
            if (rc != HwNative.ASMTEST_HW_OK)
                throw new HwTraceException($"register_region(\"{name}\") failed: {rc}");
            return this;
        }

        /// <summary>
        /// Open recording for <paramref name="name"/>, run <paramref name="body"/>, and
        /// close recording — even if the body throws (markers must stay balanced).
        /// </summary>
        public void Region(string name, Action body)
        {
            HwNative.asmtest_hwtrace_begin(name);
            try { body(); }
            finally { HwNative.asmtest_hwtrace_end(name); }
        }

        /// <summary>True if the basic block at byte-offset <paramref name="off"/> was entered.</summary>
        public bool Covered(ulong off) => HwNative.asmtest_trace_covered(_handle, off) != 0;

        /// <summary>The number of distinct basic blocks recorded.</summary>
        public ulong BlocksLen() => HwNative.asmtest_emu_trace_blocks_len(_handle);

        /// <summary>The total count of instructions executed in the region.</summary>
        public ulong InsnsTotal() => HwNative.asmtest_emu_trace_insns_total(_handle);

        /// <summary>The count of instruction offsets actually stored (capped at insns_cap).</summary>
        public ulong InsnsLen() => HwNative.asmtest_emu_trace_insns_len(_handle);

        /// <summary>True if the stored instruction stream was truncated at insns_cap.</summary>
        public bool Truncated() => HwNative.asmtest_emu_trace_truncated(_handle) != 0;

        /// <summary>
        /// The distinct basic-block start offsets recorded, in storage (first-seen)
        /// order. Read one-by-one through the opaque-handle accessor.
        /// </summary>
        public ulong[] BlockOffsets()
        {
            int n = (int)BlocksLen();
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = HwNative.asmtest_emu_trace_block_at(_handle, (UIntPtr)(nuint)i);
            return offs;
        }

        /// <summary>
        /// The ordered instruction-offset stream actually stored — each executed
        /// instruction's offset in execution order, up to the trace's insns capacity
        /// (insns_len, not the possibly-larger insns_total). Read one-by-one through
        /// the scalar accessor.
        /// </summary>
        public ulong[] InsnOffsets()
        {
            int n = (int)InsnsLen();
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = HwNative.asmtest_emu_trace_insn_at(_handle, (UIntPtr)(nuint)i);
            return offs;
        }

        /// <summary>Free the trace recorder.</summary>
        public void Free()
        {
            if (_handle != IntPtr.Zero)
            {
                HwNative.asmtest_trace_free(_handle);
                _handle = IntPtr.Zero;
            }
        }

        // The opaque trace handle, for the out-of-process tracer (which records into a
        // trace it does not own the lifecycle of — the caller still Free()s it).
        internal IntPtr Handle => _handle;
    }

    /// <summary>
    /// A JIT method resolved from a jitdump (mirrors asmtest_jitdump_entry_t plus the
    /// recorded code): the load address, size, the JIT's timestamp/index, and
    /// (optionally) the recorded native code bytes.
    /// </summary>
    public readonly struct JitMethod
    {
        /// <summary>Load address — the base to hand <see cref="Ptrace.TraceAttached"/>.</summary>
        public ulong CodeAddr { get; }

        /// <summary>Code length in bytes.</summary>
        public ulong CodeSize { get; }

        /// <summary>Record timestamp (load order); the latest re-JIT body wins.</summary>
        public ulong Timestamp { get; }

        /// <summary>The JIT's unique index for this code.</summary>
        public ulong CodeIndex { get; }

        /// <summary>The recorded native code bytes (empty unless wantBytes &gt; 0 was requested).</summary>
        public byte[] Code { get; }

        public JitMethod(ulong codeAddr, ulong codeSize, ulong timestamp, ulong codeIndex, byte[] code)
        {
            CodeAddr = codeAddr;
            CodeSize = codeSize;
            Timestamp = timestamp;
            CodeIndex = codeIndex;
            Code = code;
        }

        public override string ToString() =>
            $"JitMethod(code_addr=0x{CodeAddr:x}, code_size={CodeSize}, " +
            $"timestamp={Timestamp}, code_index={CodeIndex}, code={Code.Length} bytes)";
    }

    /// <summary>
    /// Out-of-process / foreign-process tracing (asmtest_ptrace.h): single-step a
    /// forked or externally-attached target out of band, and resolve the code region
    /// to trace from the OS — /proc/&lt;pid&gt;/maps, a JIT perf-map, or a binary
    /// jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is
    /// unavailable and in-process DynamoRIO cannot seize the runtime's threads).
    /// Linux x86-64. All methods self-skip cleanly (Available()/SkipReason()) when the
    /// lib is missing or the host is unsupported.
    /// </summary>
    public static class Ptrace
    {
        /// <summary>True if the out-of-process single-step tracer can run here (Linux x86-64).</summary>
        public static bool Available()
        {
            if (!HwNative.LibAvailable) return false;
            try { return HwNative.asmtest_ptrace_available() != 0; }
            catch { return false; }
        }

        /// <summary>Human-readable reason <see cref="Available"/> is false (or "available").</summary>
        public static string SkipReason()
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            var buf = new byte[160];
            HwNative.asmtest_ptrace_skip_reason(buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, z < 0 ? buf.Length : z);
        }

        /// <summary>
        /// Fork a tracee that calls the code at <paramref name="code"/>
        /// (<paramref name="len"/> bytes, already executable in this process) with up to
        /// six integer <paramref name="args"/>, single-step it OUT OF PROCESS, and fill
        /// the trace; returns the routine's return value (the child's RAX at the ret).
        /// </summary>
        public static long TraceCall(IntPtr code, nuint len, long[] args, IntPtr trace)
        {
            // A null/empty array can't be P/Invoked as a sized pointer; pass a 1-elem
            // placeholder with nargs = 0 (the native side ignores it).
            var arr = (args == null || args.Length == 0) ? new long[1] : args;
            int n = args == null ? 0 : args.Length;
            int rc = HwNative.asmtest_ptrace_trace_call(
                code, (UIntPtr)len, arr, n, out long result, trace);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_call failed: {rc}");
            return result;
        }

        /// <summary>
        /// Trace a region <c>[base, base+len)</c> in a SEPARATE, already-ptrace-stopped
        /// process (the caller owns PTRACE_ATTACH/DETACH). Reads the target's bytes via
        /// process_vm_readv; returns the target's RAX at the region exit.
        /// </summary>
        public static long TraceAttached(int pid, IntPtr @base, nuint len, IntPtr trace)
        {
            int rc = HwNative.asmtest_ptrace_trace_attached(
                pid, @base, (UIntPtr)len, out long result, trace);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_attached failed: {rc}");
            return result;
        }

        /// <summary>
        /// Like <see cref="TraceAttached"/>, but reconstructs the region's bytes from a
        /// <see cref="CodeImage"/> timeline as of capture sequence <paramref name="when"/>
        /// (<paramref name="when"/> == 0 =&gt; latest) instead of from a single late
        /// snapshot — the temporally-correct read for a live JIT whose code is patched,
        /// freed, or has its address reused mid-trace. Pass a default
        /// <paramref name="img"/> (no timeline) to fall back to a fresh read. Returns the
        /// target's RAX at the region exit; the caller owns PTRACE_ATTACH/DETACH.
        /// </summary>
        public static long TraceAttachedVersioned(
            int pid, IntPtr @base, nuint len, CodeImage img = null, ulong when = 0, IntPtr trace = default)
        {
            IntPtr imgHandle = img != null ? img.Handle : IntPtr.Zero;
            int rc = HwNative.asmtest_ptrace_trace_attached_versioned(
                pid, @base, (UIntPtr)len, imgHandle, when, out long result, trace);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_attached_versioned failed: {rc}");
            return result;
        }

        /// <summary>
        /// Run an already-attached, ptrace-stopped target forward until it reaches
        /// <paramref name="addr"/> (a software breakpoint that fires when the program
        /// itself next calls in), leaving it stopped there ready for
        /// <see cref="TraceAttached"/> — the step that makes a resolved JIT method
        /// traceable when you don't control call timing. Returns the status code
        /// (<c>ASMTEST_PTRACE_OK</c>, or <c>ASMTEST_PTRACE_ENOENT</c> if the target
        /// exited first). The caller owns PTRACE_ATTACH/DETACH.
        /// </summary>
        public static int RunTo(int pid, IntPtr addr)
        {
            return HwNative.asmtest_ptrace_run_to(pid, addr);
        }

        /// <summary>
        /// The executable mapping in /proc/&lt;pid&gt;/maps containing
        /// <paramref name="addr"/>, as (base, len), or <c>null</c> if no executable
        /// mapping contains it.
        /// </summary>
        public static (IntPtr Base, nuint Len)? ProcRegionByAddr(int pid, IntPtr addr)
        {
            if (!HwNative.LibAvailable) return null;
            int rc = HwNative.asmtest_proc_region_by_addr(pid, addr, out var baseOut, out var lenOut);
            return rc == HwNative.ASMTEST_PTRACE_OK ? (baseOut, (nuint)lenOut) : ((IntPtr, nuint)?)null;
        }

        /// <summary>
        /// A JIT method by <paramref name="name"/> in /tmp/perf-&lt;pid&gt;.map, as
        /// (base, len), or <c>null</c>. The name is matched against the full symbol text
        /// after the size field.
        /// </summary>
        public static (IntPtr Base, nuint Len)? ProcPerfmapSymbol(int pid, string name)
        {
            if (!HwNative.LibAvailable) return null;
            int rc = HwNative.asmtest_proc_perfmap_symbol(pid, name, out var baseOut, out var lenOut);
            return rc == HwNative.ASMTEST_PTRACE_OK ? (baseOut, (nuint)lenOut) : ((IntPtr, nuint)?)null;
        }

        /// <summary>
        /// A JIT method by <paramref name="name"/> from a binary jitdump
        /// (<paramref name="path"/>, or /tmp/jit-&lt;pid&gt;.dump when <paramref name="path"/>
        /// is null) as a <see cref="JitMethod"/> carrying up to
        /// <paramref name="wantBytes"/> of recorded code, or <c>null</c>. The latest
        /// re-JIT body (highest timestamp) wins.
        /// </summary>
        public static JitMethod? JitdumpFind(string path, string name, int pid = 0, int wantBytes = 0)
        {
            if (!HwNative.LibAvailable) return null;
            byte[] buf = wantBytes > 0 ? new byte[wantBytes] : null;
            int rc = HwNative.asmtest_jitdump_find(
                path, pid, name, out var e, buf, (UIntPtr)wantBytes, out var bytesLen);
            if (rc != HwNative.ASMTEST_PTRACE_OK) return null;
            byte[] code;
            if (wantBytes > 0)
            {
                code = new byte[(int)bytesLen];
                Array.Copy(buf, code, code.Length);
            }
            else
            {
                code = Array.Empty<byte>();
            }
            return new JitMethod(e.CodeAddr, e.CodeSize, e.Timestamp, e.CodeIndex, code);
        }
    }

    /// <summary>
    /// How a code-emission event was observed (mirrors the ASMTEST_CI_KIND_* macros).
    /// MProtect is the common JIT edge (mprotect ...PROT_EXEC...); Mmap's addr is the
    /// real base; Memfd is a staging hint correlated via the fd.
    /// </summary>
    public enum CodeImageKind { MProtect = 1, Mmap = 2, Memfd = 3 }

    /// <summary>
    /// A code-emission event from the optional eBPF detector (mirrors
    /// asmtest_codeimage_event_t): where/when executable code appeared, never the
    /// instruction stream.
    /// </summary>
    public readonly struct CodeImageEvent
    {
        /// <summary>Published base address (0 for a memfd hint).</summary>
        public ulong Addr { get; }

        /// <summary>Byte length (0 for a memfd hint).</summary>
        public ulong Len { get; }

        /// <summary>bpf_ktime_get_ns() at emission.</summary>
        public ulong Timestamp { get; }

        /// <summary>The tgid that published the code.</summary>
        public uint Pid { get; }

        /// <summary>The thread that published the code.</summary>
        public uint Tid { get; }

        /// <summary>How the emission was observed.</summary>
        public CodeImageKind Kind { get; }

        /// <summary>The memfd fd, or -1.</summary>
        public int Fd { get; }

        public CodeImageEvent(ulong addr, ulong len, ulong timestamp, uint pid, uint tid, CodeImageKind kind, int fd)
        {
            Addr = addr;
            Len = len;
            Timestamp = timestamp;
            Pid = pid;
            Tid = tid;
            Kind = kind;
            Fd = fd;
        }

        public override string ToString() =>
            $"CodeImageEvent(addr=0x{Addr:x}, len={Len}, ts={Timestamp}, " +
            $"pid={Pid}, tid={Tid}, kind={Kind}, fd={Fd})";
    }

    /// <summary>
    /// A time-aware code-image recorder (asmtest_codeimage.h) — a userspace
    /// PERF_RECORD_TEXT_POKE. <see cref="Track"/> snapshots a region's bytes (version 0)
    /// and arms write-protect on its pages; <see cref="Refresh"/> re-snapshots only the
    /// pages changed since the last arm as a NEW version stamped with the next monotonic
    /// sequence; <see cref="BytesAt"/> answers what bytes were live at an address as of a
    /// logical timestamp — the query a branch-trace decoder needs to reconstruct a JIT
    /// method whose address was reused. Works on a foreign process (pid &gt; 0, needs only
    /// ptrace permission) or this one (pid 0). The optional eBPF emission detector
    /// (<see cref="WatchBpf"/>/<see cref="PollBpf"/>/<see cref="NextEvent"/>) self-skips
    /// without libbpf / CAP_BPF. Self-skips cleanly via <see cref="Available"/> /
    /// <see cref="SkipReason"/> when the lib is missing or the host is unsupported.
    /// Dispose (or <see cref="Free"/>) to release the timeline and detach any eBPF watch.
    /// </summary>
    public sealed class CodeImage : IDisposable
    {
        IntPtr _img;

        /// <summary>
        /// Create a timeline recording <paramref name="pid"/>'s memory (0 =&gt; this
        /// process). Throws <see cref="HwTraceException"/> on allocation failure.
        /// </summary>
        public CodeImage(int pid = 0)
        {
            _img = HwNative.asmtest_codeimage_new(pid);
            if (_img == IntPtr.Zero)
                throw new HwTraceException("asmtest_codeimage_new failed");
        }

        /// <summary>
        /// True if the userspace recorder can detect page changes on this host
        /// (PAGEMAP_SCAN, or the soft-dirty fallback). Never throws — a missing lib or
        /// unsupported host returns false so callers self-skip.
        /// </summary>
        public static bool Available()
        {
            if (!HwNative.LibAvailable) return false;
            try { return HwNative.asmtest_codeimage_available() != 0; }
            catch { return false; }
        }

        /// <summary>Human-readable reason <see cref="Available"/> is false (or "available").</summary>
        public static string SkipReason()
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            var buf = new byte[160];
            HwNative.asmtest_codeimage_skip_reason(buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, z < 0 ? buf.Length : z);
        }

        /// <summary>
        /// Begin tracking <c>[base, base+len)</c>: snapshot version 0 now and arm
        /// write-protect on its pages so the next <see cref="Refresh"/> sees changes. May
        /// be called for several disjoint regions. Throws on a negative native status.
        /// </summary>
        public void Track(IntPtr @base, nuint len)
        {
            int rc = HwNative.asmtest_codeimage_track(_img, @base, (UIntPtr)len);
            if (rc != HwNative.ASMTEST_CI_OK)
                throw new HwTraceException($"asmtest_codeimage_track failed: {rc}");
        }

        /// <summary>
        /// Scan the tracked ranges for changed pages, re-snapshot each as a new version,
        /// and re-arm. Returns the number of new versions recorded (&gt;= 0). Throws on a
        /// negative native status.
        /// </summary>
        public int Refresh()
        {
            int rc = HwNative.asmtest_codeimage_refresh(_img);
            if (rc < 0)
                throw new HwTraceException($"asmtest_codeimage_refresh failed: {rc}");
            return rc;
        }

        /// <summary>
        /// The current capture sequence — a monotonic logical timestamp. Advances by one
        /// for every version recorded (track + each refresh change). 0 before anything is
        /// tracked.
        /// </summary>
        public ulong Now() => HwNative.asmtest_codeimage_now(_img);

        /// <summary>
        /// The bytes live at <paramref name="addr"/> as of capture sequence
        /// <paramref name="when"/> (<paramref name="when"/> == 0 =&gt; the latest
        /// version), copied out of the timeline's borrowed buffer. Returns <c>null</c> when
        /// the address was never tracked or has no version at-or-before
        /// <paramref name="when"/> (ASMTEST_CI_ENOENT). Throws on any other negative
        /// status.
        /// </summary>
        public byte[] BytesAt(IntPtr addr, ulong when = 0)
        {
            int rc = HwNative.asmtest_codeimage_bytes_at(_img, addr, when, out IntPtr outPtr, out UIntPtr outLen);
            if (rc == HwNative.ASMTEST_CI_ENOENT) return null;
            if (rc != HwNative.ASMTEST_CI_OK)
                throw new HwTraceException($"asmtest_codeimage_bytes_at failed: {rc}");
            int n = (int)outLen;
            var bytes = new byte[n];
            if (n > 0 && outPtr != IntPtr.Zero)
                Marshal.Copy(outPtr, bytes, 0, n);
            return bytes;
        }

        // ---- optional eBPF emission detector (Phase C) ----

        /// <summary>
        /// True if the eBPF emission detector can load and attach on this host (built with
        /// libbpf, kernel BTF present, sufficient privilege). Never throws.
        /// </summary>
        public static bool BpfAvailable()
        {
            if (!HwNative.LibAvailable) return false;
            try { return HwNative.asmtest_codeimage_bpf_available() != 0; }
            catch { return false; }
        }

        /// <summary>Human-readable reason <see cref="BpfAvailable"/> is false (or "available").</summary>
        public static string BpfSkipReason()
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            var buf = new byte[160];
            HwNative.asmtest_codeimage_bpf_skip_reason(buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, z < 0 ? buf.Length : z);
        }

        /// <summary>
        /// Load the CO-RE program, filter it to this timeline's pid, and attach it.
        /// Returns the native status (<c>ASMTEST_CI_OK</c> = 0 on success; a negative
        /// status — ENOSYS/EUNAVAIL/ELOAD — when the detector cannot run).
        /// </summary>
        public int WatchBpf() => HwNative.asmtest_codeimage_watch_bpf(_img);

        /// <summary>
        /// Drain ready emission events from the BPF ring buffer into the internal queue.
        /// <paramref name="timeoutMs"/> == 0 is a non-blocking drain; &gt; 0 waits up to
        /// that long. Returns the number of events queued (&gt;= 0). Throws on a negative
        /// native status.
        /// </summary>
        public int PollBpf(int timeoutMs = 0)
        {
            int rc = HwNative.asmtest_codeimage_poll_bpf(_img, timeoutMs);
            if (rc < 0)
                throw new HwTraceException($"asmtest_codeimage_poll_bpf failed: {rc}");
            return rc;
        }

        /// <summary>
        /// Pop one queued emission event, or <c>null</c> when the queue is empty. Throws
        /// on a negative native status.
        /// </summary>
        public CodeImageEvent? NextEvent()
        {
            int rc = HwNative.asmtest_codeimage_next(_img, out HwNative.CodeImageEvent e);
            if (rc < 0)
                throw new HwTraceException($"asmtest_codeimage_next failed: {rc}");
            if (rc == 0) return null;
            return new CodeImageEvent(
                e.Addr, e.Len, e.Timestamp, e.Pid, e.Tid, (CodeImageKind)e.Kind, e.Fd);
        }

        // The opaque timeline handle, for the version-aware out-of-process tracer
        // (Ptrace.TraceAttachedVersioned), which reads but does not own it.
        internal IntPtr Handle => _img;

        /// <summary>Free the timeline and all recorded versions; detaches any eBPF watch.</summary>
        public void Free()
        {
            if (_img != IntPtr.Zero)
            {
                HwNative.asmtest_codeimage_free(_img);
                _img = IntPtr.Zero;
            }
        }

        /// <summary>Dispose pattern over <see cref="Free"/>.</summary>
        public void Dispose() => Free();
    }
}
