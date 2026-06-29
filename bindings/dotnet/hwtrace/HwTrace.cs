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

        static IntPtr LoadLib()
        {
            foreach (var cand in Candidates())
                if (NativeLibrary.TryLoad(cand, out var h)) return h;
            return IntPtr.Zero;
        }

        // Map the logical name "asmtest_hwtrace" to the handle resolved above.
        // Registers on first access to the type (after the field initializers run).
        static HwNative()
        {
            NativeLibrary.SetDllImportResolver(typeof(HwNative).Assembly, (name, asm, paths) =>
                name == HWTRACE && LibHandle != IntPtr.Zero ? LibHandle : IntPtr.Zero);
        }

        // Candidate paths, in priority order: ASMTEST_HWTRACE_LIB, then the in-tree
        // build dir (<repo>/build/libasmtest_hwtrace.so), then the bare soname.
        static System.Collections.Generic.IEnumerable<string> Candidates()
        {
            var env = Environment.GetEnvironmentVariable("ASMTEST_HWTRACE_LIB");
            if (!string.IsNullOrEmpty(env)) yield return env;
            var repo = RepoRoot();
            if (repo != null) yield return Path.Combine(repo, "build", "libasmtest_hwtrace.so");
            yield return "libasmtest_hwtrace.so";
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
    }
}
