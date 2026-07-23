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
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Reflection; // §E6: MethodBase -> entry PC for AsmTrace.Checkpoints
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

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
    public enum TraceFidelity
    {
        Native = 0,      // runs the real bytes on the real CPU in-process
        Virtual = 1,     // isolated guest on an emulated CPU
        Statistical = 2, // sampled survey (IBS/LBR sampling): real CPU, NOT exact
    }

    /// <summary>
    /// The concrete capture MECHANISM (escalation rung) behind a resolved choice /
    /// <see cref="HwTrace.TraceCallAuto"/>'s winning rung — the F22/F26/F37
    /// discriminator (mirrors asmtest_trace_mechanism_t). Every value except
    /// Statistical is an EXACT producer; a statistical result is never mistakable
    /// for an exact one.
    /// </summary>
    public enum TraceMechanism
    {
        None = 0,        // no rung produced a trace (EUNAVAIL)
        HwBranch = 1,    // in-process HW branch record (PT / AMD LBR / CoreSight)
        TfStep = 2,      // in-process EFLAGS.TF #DB single-step
        MsrLbr = 3,      // in-process MSR-direct LBR re-run (rung 1b)
        BlockStep = 4,   // fork-isolated BTF block-step re-run
        PerInsn = 5,     // fork-isolated per-instruction ptrace re-run
        Dbi = 6,         // in-process DynamoRIO code cache
        Emulator = 7,    // Unicorn virtual CPU (isolated guest)
        Statistical = 8, // sampled survey: never exact, never parity
    }

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

        // Must match include/asmtest_hwtrace.h exactly (ASMTEST_HW_*). NOTE: ESTATE is -2,
        // NOT -4 — an earlier value was wrong but never exercised, since callers only hit
        // begin_window's ESTATE path once the empty-ctor scope started auto-initing the tier.
        public const int ASMTEST_HW_OK = 0;
        public const int ASMTEST_HW_EINVAL = -1;   // bad argument
        public const int ASMTEST_HW_ESTATE = -2;   // tier not up / wrong state
        public const int ASMTEST_HW_EUNAVAIL = -3; // no hardware-trace backend available
        public const int ASMTEST_HW_EMANAGED = -4; // safe-managed refusal: managed runtime present
                                                   // + ASMTEST_WHOLEWINDOW_SAFE_MANAGED=1 (opt-in)
        public const int ASMTEST_HW_ENOSYS = -5;   // not built / not this platform
        public const int ASMTEST_HW_EFULL = -6;    // this thread's range stack is full
        public const int ASMTEST_HW_EDECODE = -8;  // capture/decode failure
        public const int ASMTEST_HW_EPERM = -9;    // perf capture PERMISSION denied
                                                   // (substrate present) — status() only (F29)
        public const int SINGLESTEP = 3;

        // asmtest_hwtrace_status_t.stage — which gate of the available() chain failed.
        public const int STAGE_OK = 0;      // no gate failed — backend available
        public const int STAGE_DECODER = 1; // decoder library not compiled in
        public const int STAGE_CPU = 2;     // wrong CPU/ISA/vendor (or wrong OS)
        public const int STAGE_PMU = 3;     // kernel PMU sysfs node absent
        public const int STAGE_PROBE = 4;   // perf open probe failed (EPERM vs EUNAVAIL)

        // asmtest_hwtrace_scope_t: a region-free (§Z0/§Z1) scope handle — an index into
        // the calling thread's range stack tagged with a generation counter, plus the OS
        // tid that armed it (§Z4; -1 when unarmed / a sentinel). The tid is part of the
        // handle because Idx/Gen name a frame only WITHIN one thread — every thread's
        // first scope is {0, 1} — so a close from another thread would otherwise resolve
        // to that thread's own frame and close the wrong scope. Marshals as two
        // consecutive C uint32 plus an int32; at 12 bytes it is TWO INTEGER eightbytes
        // on SysV x86-64, so a by-value handle occupies two registers, not one.
        [StructLayout(LayoutKind.Sequential)]
        public struct HwScope
        {
            public uint Idx;
            public uint Gen;
            public int ArmTid;
        }

        // asmtest_trace_choice_t: four int-sized enum fields, no padding (pinned by a
        // static_assert in asmtest_trace_auto.h), so it marshals as four consecutive C
        // ints — matching the [Out] Choice[] the cross-tier resolve writes into.
        [StructLayout(LayoutKind.Sequential)]
        public struct Choice
        {
            public int Tier;      // asmtest_trace_tier_t
            public int Backend;   // asmtest_trace_backend_t (valid iff Tier == TIER_HWTRACE)
            public int Fidelity;  // asmtest_trace_fidelity_t
            public int Mechanism; // asmtest_trace_mechanism_t (F22/F26/F37)
        }

        // asmtest_hwtrace_status_t: five C ints then a 160-byte inline reason string —
        // the F29 machine-readable EPERM-vs-EUNAVAIL verdict. ByValArray keeps the
        // project unsafe-free (the interop marshaler copies the inline array back).
        [StructLayout(LayoutKind.Sequential)]
        public struct Status
        {
            public int Available;
            public int Code;  // ASMTEST_HW_OK / ASMTEST_HW_EUNAVAIL / ASMTEST_HW_EPERM
            public int Stage; // STAGE_* — which gate failed
            public int PerfEventParanoid;
            public int ProbeErrno;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 160)]
            public byte[] Reason;
        }

        // asmtest_hwtrace_options_t: backend + two ring sizes + snapshot flag + an
        // optional object-file hint + the AMD LBR period. object_hint is marshalled by
        // hand (Marshal.StringToHGlobalAnsi) into the IntPtr field so a null hint maps to
        // NULL — the single-step backend ignores it entirely. This layout MUST match the C
        // asmtest_hwtrace_options_t field-for-field (include/asmtest_hwtrace.h).
        [StructLayout(LayoutKind.Sequential)]
        public struct Options
        {
            public UIntPtr StructSize; // size_t: the F27 ABI size negotiator — ALWAYS set
            public int Backend;       // asmtest_trace_backend_t
            public UIntPtr AuxSize;   // size_t: AUX (trace) ring bytes (0 = default)
            public UIntPtr DataSize;  // size_t: base perf ring bytes (0 = default)
            public int Snapshot;      // nonzero: circular snapshot ring
            public IntPtr ObjectHint; // const char*: optional object-file path
            public int LbrPeriod;     // AMD LBR opt-in branch-retired sample period (0 = default 1)
            public int BranchFilter;  // AMD LBR opt-in reduced branch filter (0 = default BRANCH_ANY)
        }

        // asmtest_hwtrace_bucket_t: an inline char[128] label + a uint64_t count. On
        // x86-64 the u64 follows the 128-byte label with no padding (128 is 8-aligned),
        // so the struct is a flat 136 bytes. We marshal the bucket ARRAY as a raw byte
        // buffer (cap * 136) and slice each label/count out by hand — the by-hand idiom
        // this file already uses for opaque-handle reads — which sidesteps the ByValArray
        // round-trip entirely.
        public const int BucketSize = 136; // sizeof(asmtest_hwtrace_bucket_t)
        public const int BucketLabelLen = 128;

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
        // §Z1.3 whole-window WEAK/STRONG ladder: IntelPt only when the substrate is
        // present AND the decode is trusted at runtime; else SingleStep; else EUNAVAIL.
        // Plus the runtime decode-trust probe it (and DegradationNote) consult.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_window_auto();
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_window_trusted();
        // Cross-tier orchestrator (asmtest_trace_auto.h), over hwtrace + DynamoRIO +
        // emulator. resolve writes up to cap Choice triples into out[], most-faithful
        // first, returning the count; auto fills one Choice and returns ASMTEST_HW_OK
        // (0) or ASMTEST_HW_EUNAVAIL (-3) when the cascade is empty.
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_trace_resolve(uint policy, [Out] Choice[] @out, UIntPtr cap);
        [DllImport(HWTRACE)] public static extern int asmtest_trace_auto(uint policy, out Choice @out);
        // Auto-escalating CALL-OWNING cross-tier trace (asmtest_trace_call_auto): owns the
        // invocation, runs code(args…) under the fastest exact tier, and re-runs on a
        // ceiling-free tier when the trace truncates. It SELF-MANAGES the tier lifecycle
        // (init -> begin -> invoke -> end -> shutdown) internally, so it must NOT be
        // pre-armed. Fills *result (out long), the caller-allocated trace handle, and
        // *used (the {tier, backend, fidelity} Choice that produced the final trace).
        [DllImport(HWTRACE)] public static extern int asmtest_trace_call_auto(
            IntPtr code, UIntPtr len, long[] args, int nargs, uint policy,
            out long result, IntPtr trace, out Choice used);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_init(ref Options opts);
        // F29 machine-readable status (EPERM vs EUNAVAIL) + the paranoid reader.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_status(int backend, out Status status);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_perf_event_paranoid();
        [DllImport(HWTRACE)] public static extern void asmtest_hwtrace_shutdown();

        // ---- region registration + markers (const char* name) ----
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_register_region(
            [MarshalAs(UnmanagedType.LPStr)] string name, IntPtr @base, UIntPtr len, IntPtr trace);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern void asmtest_hwtrace_begin([MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern void asmtest_hwtrace_end([MarshalAs(UnmanagedType.LPStr)] string name);
        // Scoped-tracing shared core (§0/§1): error-returning begin, render-on-close.
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_try_begin([MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_render([MarshalAs(UnmanagedType.LPStr)] string name, byte[] buf, UIntPtr buflen);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_arm_tid();
        // §1 handle-keyed trio: begin_scope pushes a PER-THREAD range-stack frame and
        // returns its handle, so two threads entering the same auto-named site
        // concurrently each keep their own slice; render_scope renders by handle. On a
        // non-single-step backend begin_scope falls back to the name-keyed try_begin
        // and leaves *out a sentinel (Idx 0xffffffff) — callers then render by name.
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_begin_scope([MarshalAs(UnmanagedType.LPStr)] string name, ref HwScope @out);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_render_scope(HwScope handle, byte[] buf, UIntPtr buflen);

        // B (lazy-arm): register-region + arm + call fn(args…) + disarm as ONE native
        // step — the managed-SAFE replacement for begin_scope + DynamicInvoke + end. Only
        // fn's in-[base,len) body is stepped; nothing the caller runs is under TF.
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_call_scoped(
            [MarshalAs(UnmanagedType.LPStr)] string name, IntPtr fn, long[] args, int nargs,
            out long result, ref HwScope @out);
        // B (lazy-arm) FP sibling: a homogeneous (double…)->double body (xmm0..7 args).
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)]
        public static extern int asmtest_hwtrace_call_scoped_fp(
            [MarshalAs(UnmanagedType.LPStr)] string name, IntPtr fn, double[] args, int nargs,
            out double result, ref HwScope @out);
        // Registry-FREE lazy-arm call ([base,len) direct, no MAX_REGIONS slot) — for the
        // async-hop stitching producer, which captures many distinct one-shot bodies.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_call_scoped_ex(
            IntPtr @base, UIntPtr len, IntPtr trace, IntPtr fn, long[] args, int nargs,
            out long result, ref HwScope @out);

        // §Z0/§Z1 — the region-free (empty-ctor) whole-window scope surface.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_managed_runtime_present();
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_begin_window(IntPtr trace, ref HwScope @out);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_end_window(HwScope handle, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_render_window(HwScope handle, byte[] buf, UIntPtr buflen);

        // §Z3 — version-aware render: disassemble trace's ABSOLUTE addresses against a
        // code-image as of `when` (asmtest_codeimage_now), instead of live self memory.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_render_versioned(
            IntPtr img, ulong when, IntPtr trace, byte[] buf, UIntPtr buflen);

        // §D0.4 — async-hop stitching. asmtest_hwtrace_slice_bound_t (one per merged hop).
        [StructLayout(LayoutKind.Sequential)]
        public struct SliceBound
        {
            public UIntPtr InsnOff; // size_t offset into out->insns where this slice begins
            public ulong ScopeId;
            public uint Seq;
            public int Tid;
            public ulong Version;
        }
        // Merge N already-captured trace HANDLES (one per hop) by seq into `out`; the
        // binding-facing form of asmtest_hwtrace_stitch (the slice struct embeds an
        // asmtest_trace_t with heap pointers a binding cannot marshal by value).
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_stitch_handles(
            IntPtr[] traces, ulong[] scopeIds, uint[] seqs, int[] tids, ulong[] versions,
            UIntPtr n, IntPtr @out, [Out] SliceBound[] bounds, out UIntPtr nbounds);

        // §Z4 per-tid PT hop capture (managed-wholewindow-compose T10) — the capture
        // primitive AsmAmbientStitchedTrace opens on thread-attach and closes-with-decode
        // on thread-detach. hop_close decodes the drained AUX against `img` as of `when`
        // into `trace`. Live capture is Intel-PT-hardware-gated; off PT hop_open self-skips
        // EUNAVAIL. asmtest_ss_self_tid (src/ss_backend.c) is the OS tid of the calling
        // thread — the handler opens a per-tid hop for whichever thread the flow lands on.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_hop_open(int tid, out IntPtr ctx);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_hop_close(IntPtr ctx, IntPtr img, ulong when, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_ss_self_tid();
        // Scripted trace fill (asmtest_trace.h; not a tier-parity symbol) — the ambient
        // logic twin (T12) appends a per-thread offset pattern through the stub capture.
        [DllImport(HWTRACE, EntryPoint = "trace_append_insn")] public static extern void asmtest_trace_append_insn(IntPtr trace, ulong off);
        [DllImport(HWTRACE, EntryPoint = "trace_append_block")] public static extern void asmtest_trace_append_block(IntPtr trace, ulong off);

        // §D3 — concealed out-of-process ptrace-stealth stepper: run_region(arg) runs on
        // the CALLING thread while a helper process reverse-attaches and single-steps the
        // [base,len) region out of band. Cdecl upcall; keep the delegate alive across it.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void StealthRunFn(IntPtr arg);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_stealth_trace(
            IntPtr @base, UIntPtr len, IntPtr trace, out long result, StealthRunFn run, IntPtr arg);

        // Single-instruction classifiers (asmtest_trace.h; Capstone-gated, 0 without it).
        // IntPtr `code` so a LIVE in-process address can be classified directly.
        [DllImport(HWTRACE)] public static extern int asmtest_disas_is_call(int arch, IntPtr code, UIntPtr codeLen, ulong off);
        [DllImport(HWTRACE)] public static extern int asmtest_disas_is_branch(int arch, IntPtr code, UIntPtr codeLen, ulong off);
        [DllImport(HWTRACE)] public static extern int asmtest_disas_is_ret(int arch, IntPtr code, UIntPtr codeLen, ulong off);
        [DllImport(HWTRACE)] public static extern int asmtest_disas_call_target(
            int arch, IntPtr code, UIntPtr codeLen, ulong baseAddr, ulong off, out ulong target);

        // ---- single-instruction disassembler (Capstone-backed; §0.3 helpers) ----
        // asmtest_disas decodes ONE instruction at `code` (an absolute in-process address
        // for a live capture), formatting "<mnemonic> <operands>" into `outbuf`; base_addr
        // = the address the bytes run at, so PC-relative operands resolve to absolute
        // targets. Returns the instruction byte length (0 if undecodable / no Capstone).
        [DllImport(HWTRACE)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool asmtest_disas_available();
        [DllImport(HWTRACE, CharSet = CharSet.Ansi)] public static extern UIntPtr asmtest_disas(
            int arch, IntPtr code, UIntPtr codeLen, ulong baseAddr, ulong off, byte[] outbuf, UIntPtr outlen);

        // §3.1(c) whole-window noise attribution: bucket ips[0..n) by the containing
        // perf-map JIT symbol (preferred) or mapped-file region into buckets[0..cap),
        // returning the distinct-label count (<= cap). `buckets` is the raw byte backing
        // of an asmtest_hwtrace_bucket_t[cap] (BucketSize each); pid 0 == self. Reads
        // /proc/<pid>/maps + the perf-map, so it is post-close safe on a whole-window
        // scope's captured ABSOLUTE addresses.
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_hwtrace_symbolize_bucket(
            int pid, ulong[] ips, UIntPtr n, byte[] buckets, UIntPtr cap);

        // §3.1(c) address->name REVERSE resolver: the mapped-file pathname (or a "[...]"
        // pseudo-name) + extent containing `addr` in `pid` (0 == self). Returns 1 on a hit
        // (fills name/start/end), 0 on a miss. The counterpart of symbolize_bucket for ONE
        // address, and the only surface that yields the region EXTENT — asmtest_proc_region_by_addr
        // returns the extent but DISCARDS the pathname, and symbolize_bucket returns labels
        // with counts but no extent and needs an IP list.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_region_name(
            int pid, ulong addr, byte[] name, UIntPtr namelen, out ulong start, out ulong end);

        // asmtest_hwtrace_named_region_t: an inline char[64] name then two uint64 — a flat
        // 80 bytes on x86-64 (64 is 8-aligned, so no padding before base). Marshalled as a
        // raw byte buffer (nregions * 80) built by hand, the same idiom BucketSize uses,
        // which sidesteps the ByValArray round-trip.
        public const int NamedRegionSize = 80; // sizeof(asmtest_hwtrace_named_region_t)
        public const int NamedRegionNameLen = 64;

        // §3.1(c) whole-window attribution keyed to a live whole-window SCOPE HANDLE.
        // ABI: HwScope is 12 bytes / TWO INTEGER eightbytes on SysV x86-64, so it consumes
        // rdi+rsi and every later argument shifts down one register accordingly. It is passed
        // BY VALUE as the real 3-field struct (never hand-flattened to one ulong, which would
        // pass `regions` where the callee reads the handle's second half). `regions` is the raw
        // byte backing of an asmtest_hwtrace_named_region_t[nregions], `buckets` of an
        // asmtest_hwtrace_bucket_t[cap]. Must run BEFORE asmtest_trace_free on the scope's
        // trace: it reads the frame's recorded addresses, not a copy.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_attribute_window(
            HwScope handle, byte[] regions, UIntPtr nregions,
            byte[] buckets, UIntPtr cap, out UIntPtr nbuckets);

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
        // BTF block-step (PTRACE_SINGLEBLOCK): same contract as trace_call but ~4-10x
        // fewer tracer stops, reconstructing the identical trace. x86-64 Linux only.
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_blockstep_available();
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_call_blockstep(
            IntPtr code, UIntPtr len, long[] args, int nargs, out long result, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached(
            int pid, IntPtr @base, UIntPtr len, out long result, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached_blockstep(
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

        // §D3 whole-window capture that OWNS its tracee (fork-internal): forks a child that
        // calls code(args…), run_to's the window frame, and records the ABSOLUTE address of
        // every instruction in [code,code+len) OR any region pre-published on `chan`. The
        // crash-proof out-of-process analog of the in-process whole-window scope — a managed
        // caller cannot fork safely, so this owns the tracee. chan may be IntPtr.Zero.
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_window_call(
            IntPtr code, UIntPtr len, long[] args, int nargs, IntPtr chan, out long result, IntPtr trace);
        // §D3 cross-process JIT-address channel (asmtest_addr_channel.h) — exported FFI
        // shims over the header-only inline ring. new() allocs+inits a process-local
        // channel; publish_rec adds a (base,len,version) region; free releases it.
        [DllImport(HWTRACE)] public static extern IntPtr asmtest_addr_channel_new();
        [DllImport(HWTRACE)] public static extern void asmtest_addr_channel_publish_rec(
            IntPtr c, ulong @base, ulong len, ulong version);
        [DllImport(HWTRACE)] public static extern void asmtest_addr_channel_free(IntPtr c);
        // SHARED-memory channel: the JIT listener publishes into it live while the forked
        // helper drains it — so methods JIT'd mid-window are captured.
        [DllImport(HWTRACE)] public static extern IntPtr asmtest_addr_channel_new_shared();
        [DllImport(HWTRACE)] public static extern void asmtest_addr_channel_free_shared(IntPtr c);

        // §D3 whole-window reverse-attach stealth stepper (asmtest_hwtrace.h): a helper child
        // single-steps the caller out of band while runRegion(arg) runs the window body,
        // capturing [winBase,winLen) + every region published on the SHARED `chan` (coarse
        // ranges + methods the JIT listener publishes live) into `trace`. Crash-proof (a
        // ptrace-stop is not gated by the tracee's signal mask). runRegion is a native
        // function pointer (a marshaled managed callback).
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_stealth_trace_windowed(
            IntPtr winBase, UIntPtr winLen, IntPtr chan,
            IntPtr trace, out long result, IntPtr runRegion, IntPtr arg);

        // §D3 statistical AMD-LBR whole-window survey (asmtest_hwtrace.h): sample the branch
        // stack at `period` while runFn(arg) runs on the calling thread, filling ips[cap] with
        // absolute branch-target endpoints (a sample-weighted hot-method histogram, NOT an
        // exact trace). Crash-proof / out-of-band (no TF, no SIGTRAP). runFn is a native
        // function pointer (a marshaled managed callback).
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_sample_window_amd(
            IntPtr runFn, IntPtr arg, int period, ulong[] ips, UIntPtr cap,
            out UIntPtr nips, out int truncated);
        // §E5: the AutoFDO/BOLT block-frequency variant of the same survey — weights the
        // BLOCK spanning [to_i, from_{i+1}] (a branch target to the next branch's source) by
        // its length instead of counting one endpoint per branch, so branchy code is not
        // over-weighted vs. hot straight-line code. Identical surface + self-skip contract.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_sample_window_amd_weighted(
            IntPtr runFn, IntPtr arg, int period, ulong[] ips, UIntPtr cap,
            out UIntPtr nips, out int truncated);
        // §E6: the SAME survey plus the deterministic branchsnap TILED at up to 4 absolute
        // `checkpoints` (managed method entries). One run of runFn feeds both producers into
        // one ips[] endpoint stream: the survey's branch-stack event turns the LBR on, and
        // each checkpoint's HW breakpoint freezes + snapshots it via BPF. ips[0..ntiled) is
        // the island-sourced prefix (tiles drain first, so sampled endpoints cannot crowd
        // them out); [ntiled..nips) is sampled. islands = checkpoint hits merged; 0 means
        // tiling did not arm or the checkpoint never ran, and this degrades EXACTLY to the
        // plain survey. Same self-skip contract; needs CAP_BPF on top of the survey's needs.
        // tileTruncated and truncated are DISTINCT and must not be conflated: tileTruncated
        // means the ISLAND merge lost endpoints; truncated is the survey-wide prefix flag and
        // ALSO fires on SAMPLER-ring loss. Only the former is causally tied to island content.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_tile_window_amd(
            IntPtr runFn, IntPtr arg, int period, ulong[] checkpoints, int ncheckpoints,
            ulong[] ips, UIntPtr cap, out UIntPtr nips, out UIntPtr ntiled,
            out int islands, out int tileTruncated, out int truncated);
        // Begin/end split of the AMD-LBR survey: arm in a ctor, drain in Dispose, so the block
        // runs INLINE between them (`using (new AsmTrace(HwBackend.AmdLbr)) { block }`). _end's
        // ips may be null (a drain-less release of a leaked scope's fd+mapping).
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_sample_begin_amd(
            int period, out IntPtr ctx);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_sample_end_amd(
            IntPtr ctx, ulong[] ips, UIntPtr cap, out UIntPtr nips, out int truncated);
        // §Z1.2 native STRONG-tier PT whole-window pair driving the inline
        // `using (new AsmTrace(HwBackend.IntelPt))` ctor. begin opens a per-thread perf AUX
        // intel_pt event (self-skips EUNAVAIL off bare-metal Intel PT); end DISABLEs, drains,
        // decodes against `img` as of `when` (img==Zero: the ctx's own self image) into
        // `trace`, and frees the ctx. A Zero trace is a drain-less release (leaked-scope reclaim).
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_begin_window(out IntPtr ctx);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_pt_end_window(
            IntPtr ctx, IntPtr img, ulong when, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_stealth_window_begin(
            IntPtr chan, out IntPtr ctx);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_stealth_window_end(
            IntPtr ctx, IntPtr trace);

        // AMD-P0 deterministic boundary LBR snapshot (src/branchsnap.c). Unlike the sampled
        // survey above, this is EXACT for a tiny single-shot region the sampler is too coarse
        // to catch: enable the LBR, plant a HW execution breakpoint at base+exitOff, run
        // run(arg), and read the frozen 16-entry branch stack via bpf_get_branch_snapshot().
        // Fills `trace` (the same asmtest_trace_t the recorder/coverage APIs read). run is a
        // Cdecl void(void*) upcall (reuse StealthRunFn). _available reports the static+runtime
        // floor (returns 0 without libbpf / AMD LbrExtV2 / kernel >= 6.10) so callers self-skip;
        // _trace returns ASMTEST_HW_EUNAVAIL (needs CAP_BPF+CAP_PERFMON) or ENOSYS (no BPF build).
        [DllImport(HWTRACE)] public static extern int asmtest_amd_snapshot_available();
        [DllImport(HWTRACE)] public static extern int asmtest_amd_snapshot_trace(
            IntPtr @base, UIntPtr len, UIntPtr exitOff, StealthRunFn run, IntPtr arg, IntPtr trace);

        // ---- call descent (asmtest_descent_t) — edges + nested frames ----
        // A SEPARATE opaque handle threaded through the three _ex trace entry points
        // (below); the flat asmtest_trace_t stays the single-region frame-0 view. Every
        // getter is one scalar per call (the opaque-handle idiom), NULL-safe and
        // bounds-checked in C. size_t indices/counts marshal as UIntPtr; absolute
        // addresses (edge_target / frame_base / insn_at / block_at) as ulong. The two
        // callback installers take a managed delegate the Descent wrapper pins against
        // GC for the handle's lifetime (see Descent.SetResolver / SetDenylist).

        // Level-2/3 resolver: return 1 to descend into calleeAddr (setting *baseOut /
        // *lenOut to its extent), 0 to step over. `user` is the pointer passed to
        // set_resolver. Cdecl, matching asmtest_descent_resolver_fn.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int DescentResolverFn(ulong calleeAddr, IntPtr user, out ulong baseOut, out ulong lenOut);

        // Level-3 denylist: return 1 to REFUSE descent into calleeAddr, 0 to allow.
        // Cdecl, matching asmtest_descent_denylist_fn.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int DescentDenylistFn(ulong calleeAddr, IntPtr user);

        [DllImport(HWTRACE)] public static extern IntPtr asmtest_descent_new(int level);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_free(IntPtr d);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_set_max_depth(IntPtr d, uint maxDepth);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_set_insn_budget(IntPtr d, ulong budget);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_set_watchdog_ms(IntPtr d, uint ms);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_use_default_denylist(IntPtr d);
        [DllImport(HWTRACE)] public static extern int asmtest_descent_allow_region(IntPtr d, IntPtr @base, UIntPtr len);
        [DllImport(HWTRACE)] public static extern int asmtest_descent_deny_region(IntPtr d, IntPtr @base, UIntPtr len);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_set_resolver(IntPtr d, DescentResolverFn fn, IntPtr user);
        [DllImport(HWTRACE)] public static extern void asmtest_descent_set_denylist(IntPtr d, DescentDenylistFn fn, IntPtr user);
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_descent_edges_len(IntPtr d);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_edge_site(IntPtr d, UIntPtr i);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_edge_target(IntPtr d, UIntPtr i);
        [DllImport(HWTRACE)] public static extern uint asmtest_descent_edge_depth(IntPtr d, UIntPtr i);
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_descent_frames_len(IntPtr d);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_frame_base(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_frame_len(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern uint asmtest_descent_frame_depth(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern int asmtest_descent_frame_parent(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_descent_frame_insn_count(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_frame_insn_at(IntPtr d, UIntPtr f, UIntPtr i);
        [DllImport(HWTRACE)] public static extern UIntPtr asmtest_descent_frame_block_count(IntPtr d, UIntPtr f);
        [DllImport(HWTRACE)] public static extern ulong asmtest_descent_frame_block_at(IntPtr d, UIntPtr f, UIntPtr i);
        [DllImport(HWTRACE)] public static extern int asmtest_descent_truncated(IntPtr d);
        [DllImport(HWTRACE)] public static extern int asmtest_descent_depth_capped(IntPtr d);

        // Descending variants of the three trace entry points: each threads a descent
        // handle through the loop. trace (frame 0) may be IntPtr.Zero to record only into
        // the descent handle, and descent may be IntPtr.Zero to reproduce the non-_ex form.
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_call_ex(
            IntPtr code, UIntPtr len, long[] args, int nargs, out long result, IntPtr trace, IntPtr descent);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached_ex(
            int pid, IntPtr @base, UIntPtr len, out long result, IntPtr trace, IntPtr descent);
        [DllImport(HWTRACE)] public static extern int asmtest_ptrace_trace_attached_versioned_ex(
            int pid, IntPtr @base, UIntPtr len, IntPtr img, ulong when, out long result, IntPtr trace, IntPtr descent);

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
        /// <summary>The concrete capture mechanism — for
        /// <see cref="HwTrace.TraceCallAuto"/>'s Used, the escalation rung that
        /// actually won (F22/F26/F37).</summary>
        public TraceMechanism Mechanism { get; }

        public TierChoice(TraceTier tier, HwBackend backend, TraceFidelity fidelity,
                          TraceMechanism mechanism = TraceMechanism.None)
        {
            Tier = tier;
            Backend = backend;
            Fidelity = fidelity;
            Mechanism = mechanism;
        }

        public override string ToString() =>
            $"TierChoice(tier={Tier}, backend={Backend}, fidelity={Fidelity}, mechanism={Mechanism})";
    }

    /// <summary>
    /// The F29 machine-readable availability verdict for one backend (mirrors
    /// asmtest_hwtrace_status_t): <see cref="Code"/> distinguishes EPERM (substrate
    /// present, perf capture permission denied — lower perf_event_paranoid or grant
    /// CAP_PERFMON) from EUNAVAIL (hardware/decoder/PMU absent) — the split
    /// <see cref="HwTrace.Available"/>'s bool deliberately collapses.
    /// <see cref="Reason"/> is byte-identical to <see cref="HwTrace.SkipReason"/>.
    /// </summary>
    public readonly struct HwStatus
    {
        public bool Available { get; }
        public int Code { get; }
        public int Stage { get; }
        public int PerfEventParanoid { get; }
        public int ProbeErrno { get; }
        public string Reason { get; }

        public HwStatus(bool available, int code, int stage, int perfEventParanoid,
                        int probeErrno, string reason)
        {
            Available = available;
            Code = code;
            Stage = stage;
            PerfEventParanoid = perfEventParanoid;
            ProbeErrno = probeErrno;
            Reason = reason;
        }

        public override string ToString() =>
            $"HwStatus(available={Available}, code={Code}, stage={Stage}, " +
            $"paranoid={PerfEventParanoid}, errno={ProbeErrno}, reason=\"{Reason}\")";
    }

    /// <summary>
    /// The outcome of <see cref="HwTrace.TraceCallAuto"/> (asmtest_trace_call_auto): the
    /// call's <see cref="Result"/>, the filled <see cref="Trace"/> (a queryable
    /// <see cref="HwTrace"/> the CALLER frees via <see cref="HwTrace.Free"/>), the
    /// <see cref="TierChoice"/> that produced the final trace (<see cref="Used"/> —
    /// inspect its <see cref="TierChoice.Backend"/> to see whether escalation fired),
    /// the <see cref="Truncated"/> honesty bit, and the raw <see cref="Rc"/>. On a
    /// self-skip (no call-owning native tier) <see cref="Trace"/> is null,
    /// <see cref="Used"/> is null, and <see cref="Rc"/> is negative.
    /// </summary>
    public readonly struct TraceCallAutoResult
    {
        public long Result { get; }
        public HwTrace Trace { get; }
        public TierChoice? Used { get; }
        public bool Truncated { get; }
        public int Rc { get; }

        public TraceCallAutoResult(long result, HwTrace trace, TierChoice? used, bool truncated, int rc)
        {
            Result = result;
            Trace = trace;
            Used = used;
            Truncated = truncated;
            Rc = rc;
        }

        public override string ToString() =>
            $"TraceCallAutoResult(result={Result}, used={Used}, truncated={Truncated}, rc={Rc})";
    }

    /// <summary>
    /// Host-native machine code in real executable (W^X) memory. Allocate with
    /// <see cref="FromBytes"/>, invoke through a function pointer with
    /// <see cref="Call"/>, and release with <see cref="Free"/>.
    /// </summary>
    public sealed class NativeCode : IDisposable
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
            GC.SuppressFinalize(this);
        }

        /// <summary>Dispose pattern over <see cref="Free"/> (idempotent).</summary>
        public void Dispose() => Free();

        // Finalizer backstop (B3): a dropped (never-Free'd) NativeCode still
        // unmaps its executable memory (asmtest_hwtrace_exec_free is a munmap,
        // thread-agnostic). Guarded + try/catch to never throw on the finalizer
        // thread (mirrors the AmdSampler/PtWindowCtx finalizers below).
        ~NativeCode()
        {
            if (_freed || !HwNative.LibAvailable) return;
            _freed = true;
            try { HwNative.asmtest_hwtrace_exec_free(_base, _len); } catch { }
        }
    }

    /// <summary>
    /// One labelled bucket of a symbolized address list (mirrors asmtest_hwtrace_bucket_t):
    /// the module/JIT-symbol <see cref="Label"/> and the number of addresses that fell in
    /// it. See <see cref="HwTrace.SymbolizeBuckets"/>.
    /// </summary>
    public readonly struct HwBucket
    {
        public HwBucket(string label, ulong count) { Label = label; Count = count; }
        /// <summary>The perf-map JIT symbol, mapped-file pathname, or a "[...]" pseudo-name
        /// (<c>[anon]</c>/<c>[unknown]</c>) the addresses resolved to.</summary>
        public string Label { get; }
        /// <summary>How many addresses bucketed to this label.</summary>
        public ulong Count { get; }
        public override string ToString() => $"HwBucket(label={Label}, count={Count})";
    }

    /// <summary>
    /// The mapped region containing an address (mirrors the <c>asmtest_hwtrace_region_name</c>
    /// out-params): its <see cref="Name"/> and its <c>[Start, End)</c> extent. See
    /// <see cref="HwTrace.RegionName"/>.
    /// </summary>
    public readonly struct HwRegion
    {
        public HwRegion(string name, ulong start, ulong end) { Name = name; Start = start; End = end; }
        /// <summary>The mapped-file pathname, or a "[...]" pseudo-name (<c>[anon]</c>,
        /// <c>[stack]</c>, <c>[heap]</c>) for an unnamed mapping.</summary>
        public string Name { get; }
        /// <summary>First address of the containing mapping (inclusive).</summary>
        public ulong Start { get; }
        /// <summary>One past the last address of the containing mapping (exclusive).</summary>
        public ulong End { get; }
        public override string ToString() => $"HwRegion(name={Name}, start=0x{Start:x}, end=0x{End:x})";
    }

    /// <summary>
    /// A caller-known code region for whole-window attribution (mirrors
    /// <c>asmtest_hwtrace_named_region_t</c>): every captured address in
    /// <c>[Base, Base+Len)</c> is labelled <see cref="Name"/>.
    /// <para>This is what lets SEVERAL native leaves come back as separate, named buckets.
    /// Maps-based resolution collapses every <c>exec_alloc</c>'d blob into a single
    /// <c>[anon]</c>, and symbol/disassembly attribution cannot tell two leaves with
    /// IDENTICAL BYTES apart at all — only an exact address range can. Pass these to the
    /// whole-window <see cref="AsmTrace"/> ctor's <c>regions</c> parameter to get
    /// <see cref="AsmTrace.Buckets"/>.</para>
    /// <para><see cref="Name"/> is truncated to 63 bytes + NUL to fit the C
    /// <c>char[64]</c>.</para>
    /// </summary>
    public readonly struct AsmNamedRegion
    {
        public AsmNamedRegion(string name, ulong @base, ulong len) { Name = name; Base = @base; Len = len; }
        /// <summary>Convenience overload for a <see cref="NativeCode"/> leaf: label its
        /// whole <c>[Base, Base+Length)</c> extent.</summary>
        public AsmNamedRegion(string name, NativeCode code)
            : this(name, (ulong)(long)code.Base, (ulong)code.Length) { }
        /// <summary>The label captured addresses in this range are attributed to.</summary>
        public string Name { get; }
        /// <summary>Absolute base address of the region.</summary>
        public ulong Base { get; }
        /// <summary>Length of the region in bytes.</summary>
        public ulong Len { get; }
        public override string ToString() => $"AsmNamedRegion(name={Name}, base=0x{Base:x}, len={Len})";
    }

    /// <summary>
    /// A coverage recorder for a registered native region, via the hardware tier.
    /// Bring the tier up once with <see cref="Init"/>, allocate per-trace recorders
    /// with <see cref="Create"/>, register a <see cref="NativeCode"/> under a name,
    /// run it inside <see cref="Region"/>, then read back coverage / the instruction
    /// stream. Tear the process-wide tier down with <see cref="Shutdown"/>.
    /// </summary>
    public sealed class HwTrace : IDisposable
    {
        IntPtr _handle;

        HwTrace(IntPtr handle) => _handle = handle;

        // ---- process-wide lifecycle ----

        // Guards the process-wide tier init so the empty-ctor whole-window scope can
        // lazily bring it up (see AsmTrace's auto-init) without a race against explicit
        // Init/Shutdown. TierInited mirrors the native g_inited for the paths this binding
        // controls; the lock serializes first-use init across threads.
        internal static readonly object TierLock = new object();
        internal static bool TierInited;

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
        /// The F29 machine-readable availability verdict (asmtest_hwtrace_status):
        /// unlike <see cref="Available"/>'s bool, <c>Status().Code</c> distinguishes
        /// ASMTEST_HW_EPERM (substrate present, perf capture permission denied — lower
        /// perf_event_paranoid or grant CAP_PERFMON) from ASMTEST_HW_EUNAVAIL
        /// (hardware/decoder/PMU absent), with the failing stage (STAGE_*), the probe
        /// errno and the kernel paranoid level. Throws when the lib is missing.
        /// </summary>
        public static HwStatus Status(HwBackend backend = HwBackend.SingleStep)
        {
            if (!HwNative.LibAvailable)
                throw new HwTraceException("libasmtest_hwtrace not loaded");
            int rc = HwNative.asmtest_hwtrace_status((int)backend, out var st);
            if (rc != HwNative.ASMTEST_HW_OK)
                throw new HwTraceException($"asmtest_hwtrace_status failed: {rc}");
            var reason = st.Reason ?? Array.Empty<byte>();
            int z = Array.IndexOf(reason, (byte)0);
            return new HwStatus(st.Available != 0, st.Code, st.Stage,
                st.PerfEventParanoid, st.ProbeErrno,
                System.Text.Encoding.UTF8.GetString(reason, 0, z < 0 ? reason.Length : z));
        }

        /// <summary>
        /// The kernel's perf_event_paranoid level, or <see cref="int.MinValue"/> where
        /// the proc file is absent (non-Linux / masked /proc). Above 2 blocks
        /// unprivileged perf_event_open entirely (the EPERM case without CAP_PERFMON).
        /// </summary>
        public static int PerfEventParanoid()
        {
            if (!HwNative.LibAvailable) return int.MinValue;
            return HwNative.asmtest_hwtrace_perf_event_paranoid();
        }

        /// <summary>
        /// §Z5.2 — the composed degradation LADDER: walk the backends most-faithful
        /// first (Intel PT → AMD LBR → single-step → CoreSight), naming each
        /// unavailable tier WITH its reason, ending at the first available one ("using
        /// SingleStep") or, when none arms, at whether the out-of-process ptrace
        /// stepper can still serve as the fallback. One honest sentence a scope's
        /// self-skip (and a user's bug report) can carry.
        /// </summary>
        public static string DegradationNote()
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            var parts = new System.Collections.Generic.List<string>(5);
            foreach (var b in new[] { HwBackend.IntelPt, HwBackend.AmdLbr,
                                      HwBackend.SingleStep, HwBackend.CoreSight })
            {
                if (Available(b))
                {
                    // §Z1.3: intel_pt hardware present is necessary but NOT sufficient for
                    // the STRONG whole-window tier — the decode must also be trusted at
                    // runtime (the §Z2 fixture round-trip). If it is not, the window ladder
                    // falls through to the next tier, so name that outcome ("present but
                    // untrusted") rather than claim STRONG. (Off intel_pt the else-branch
                    // below already says "no intel_pt PMU" via SkipReason, and the trust
                    // probe is never reached — Available short-circuits.)
                    if (b == HwBackend.IntelPt
                        && HwNative.asmtest_hwtrace_pt_window_trusted() == 0)
                    {
                        parts.Add("IntelPt present but whole-window decode untrusted");
                        continue;
                    }
                    parts.Add($"using {b}");
                    break;
                }
                parts.Add($"{b} unavailable ({SkipReason(b)})");
            }
            if (!parts[parts.Count - 1].StartsWith("using", StringComparison.Ordinal))
                parts.Add(Ptrace.Available()
                    ? "out-of-process ptrace stepper available as the fallback"
                    : $"no ptrace fallback ({Ptrace.SkipReason()})");
            // D (honesty) — managed-singlestep-lazy-arm-plan. The single-step tier's
            // in-process EFLAGS.TF window is FATAL, not degraded, on a managed thread that
            // spawns a thread in-window (glibc pthread_create blocks SIGTRAP → the kernel
            // force-kills the process). AsmTrace.Method() is safe — it lazy-arms only the
            // body (Option B) — but a whole-window `new AsmTrace()` on a runtime thread is
            // not; say so where single-step is the tier in play.
            if (parts.Exists(p => p.Contains("using SingleStep")))
                parts.Add("in-process single-step over a whole managed window is fatal if "
                        + "the runtime spawns a thread in-window — use AsmTrace.Method() "
                        + "(lazy-arm, safe) or outOfProcess: true");
            return string.Join("; ", parts);
        }

        /// <summary>
        /// This host's hardware-trace fallback cascade: the available backend enums,
        /// most-faithful first (INTEL_PT &gt; AMD_LBR &gt; SINGLESTEP &gt; CORESIGHT),
        /// honoring <paramref name="policy"/>. Empty only off x86-64 Linux (single-step
        /// is the floor there) or when the lib is missing. CeilingFree drops the
        /// ceiling-bounded backend (AMD LBR).
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
                    (TraceTier)buf[i].Tier, (HwBackend)buf[i].Backend,
                    (TraceFidelity)buf[i].Fidelity, (TraceMechanism)buf[i].Mechanism);
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
            return new TierChoice((TraceTier)c.Tier, (HwBackend)c.Backend,
                (TraceFidelity)c.Fidelity, (TraceMechanism)c.Mechanism);
        }

        /// <summary>
        /// Auto-escalating CALL-OWNING cross-tier trace (asmtest_trace_call_auto): run
        /// <paramref name="code"/>(<paramref name="args"/>…) under the fastest exact tier
        /// and, when the trace comes back truncated, escalate to a ceiling-free tier and
        /// re-run — until the trace is complete or the tiers are exhausted. It OWNS the
        /// invocation, so <paramref name="code"/> must be RE-RUNNABLE (deterministic /
        /// idempotent): it is invoked once per attempted tier — the in-process fast step,
        /// then the fork-isolated ptrace escalations. Unlike
        /// <see cref="AsmTrace"/>'s call_scoped path it SELF-MANAGES the whole tier
        /// lifecycle (init -&gt; begin -&gt; invoke -&gt; end -&gt; shutdown) internally,
        /// so NO <see cref="Init"/> / pre-arm is required — pre-arming here would
        /// double-init the tier and then leave it torn down. <paramref name="args"/> pass
        /// as C longs (SysV integer ABI, 0–6; more, or FP, is rejected as EINVAL).
        /// Returns a <see cref="TraceCallAutoResult"/> whose
        /// <see cref="TraceCallAutoResult.Trace"/> is a queryable <see cref="HwTrace"/> the
        /// CALLER frees. Self-skips (Trace/Used null, negative Rc) where no call-owning
        /// native tier is available.
        /// </summary>
        public static TraceCallAutoResult TraceCallAuto(
            NativeCode code, long[] args = null, TracePolicy policy = TracePolicy.Best,
            int blocks = 64, int instructions = 512)
        {
            if (!HwNative.LibAvailable)
                return new TraceCallAutoResult(0, null, null, false, HwNative.ASMTEST_HW_EUNAVAIL);
            // No pre-arm / Init: asmtest_trace_call_auto OWNS the full tier lifecycle
            // internally, so a pre-arm here would double-init and leave the tier torn down.
            var trace = Create(blocks: blocks, instructions: instructions);
            var arr = args ?? Array.Empty<long>();
            int rc = HwNative.asmtest_trace_call_auto(
                code.Base, (UIntPtr)code.Length, arr.Length == 0 ? null : arr, arr.Length,
                (uint)policy, out long result, trace.Handle, out var used);
            if (rc != HwNative.ASMTEST_HW_OK)
            {
                trace.Free();
                return new TraceCallAutoResult(0, null, null, false, rc);
            }
            var tc = new TierChoice(
                (TraceTier)used.Tier, (HwBackend)used.Backend,
                (TraceFidelity)used.Fidelity, (TraceMechanism)used.Mechanism);
            return new TraceCallAutoResult(result, trace, tc, trace.Truncated(), rc);
        }

        /// <summary>
        /// §3.1(c) — bucket the ABSOLUTE addresses <paramref name="ips"/> by the perf-map
        /// JIT symbol / mapped-file region containing each (in process <paramref name="pid"/>,
        /// 0 = self), naming the runtime lump a whole-window scope captured
        /// (<see cref="AsmTrace.Addresses"/>). Post-close safe: it reads
        /// <c>/proc/&lt;pid&gt;/maps</c> and the perf-map, not the trace. Addresses that
        /// resolve to nothing bucket under <c>[unknown]</c>; if more than
        /// <paramref name="cap"/> distinct labels appear the surplus addresses are dropped,
        /// so size <paramref name="cap"/> to the expected module count. Empty where the lib
        /// is missing or the input is empty.
        /// </summary>
        public static HwBucket[] SymbolizeBuckets(ulong[] ips, int pid = 0, int cap = 64)
        {
            if (!HwNative.LibAvailable || ips == null || ips.Length == 0 || cap <= 0)
                return Array.Empty<HwBucket>();
            var raw = new byte[cap * HwNative.BucketSize];
            int n = (int)HwNative.asmtest_hwtrace_symbolize_bucket(
                pid, ips, (UIntPtr)(nuint)ips.Length, raw, (UIntPtr)(nuint)cap);
            if (n > cap) n = cap;
            var outb = new HwBucket[n];
            for (int i = 0; i < n; i++)
            {
                int off = i * HwNative.BucketSize;
                int z = Array.IndexOf(raw, (byte)0, off, HwNative.BucketLabelLen);
                int len = (z < 0 ? off + HwNative.BucketLabelLen : z) - off;
                string label = System.Text.Encoding.UTF8.GetString(raw, off, len);
                ulong count = BitConverter.ToUInt64(raw, off + HwNative.BucketLabelLen);
                outb[i] = new HwBucket(label, count);
            }
            return outb;
        }

        /// <summary>
        /// §3.1(c) — reverse-resolve ONE absolute <paramref name="addr"/> to the name +
        /// extent of the mapped region containing it, in process <paramref name="pid"/>
        /// (0 = self). Returns <c>null</c> on a miss (the address is in no mapping).
        /// <para>The single-address counterpart of <see cref="SymbolizeBuckets"/>, and the
        /// only surface here that yields the region's EXTENT: <c>SymbolizeBuckets</c>
        /// returns labels with counts but no bounds and needs a whole IP list, while
        /// <see cref="Ptrace.ProcRegionByAddr"/> returns the extent but DISCARDS the maps
        /// pathname. Use it to range-classify a whole-window <see cref="AsmTrace.Addresses"/>
        /// against a known mapping, or to name the module an address landed in.</para>
        /// <para>Post-close safe: it reads <c>/proc/&lt;pid&gt;/maps</c>, not the trace.</para>
        /// </summary>
        public static HwRegion? RegionName(ulong addr, int pid = 0)
        {
            if (!HwNative.LibAvailable) return null;
            var name = new byte[256];
            int rc = HwNative.asmtest_hwtrace_region_name(
                pid, addr, name, (UIntPtr)(nuint)name.Length, out ulong start, out ulong end);
            if (rc != 1) return null; // 0 = miss; the C contract returns 1 on a hit
            int z = Array.IndexOf(name, (byte)0);
            if (z < 0) z = name.Length;
            return new HwRegion(System.Text.Encoding.UTF8.GetString(name, 0, z), start, end);
        }

        /// <summary>
        /// Select a backend and initialize the tier. SINGLESTEP is the portable
        /// default that runs on any x86-64 Linux. Throws <see cref="HwTraceException"/>
        /// on a nonzero status.
        /// </summary>
        public static void Init(HwBackend backend = HwBackend.SingleStep)
        {
            lock (TierLock)
            {
                var opts = new HwNative.Options
                {
                    // F27 ABI size negotiation: self-describe the mirrored struct.
                    StructSize = (UIntPtr)Marshal.SizeOf<HwNative.Options>(),
                    Backend = (int)backend,
                };
                int rc = HwNative.asmtest_hwtrace_init(ref opts);
                if (rc != HwNative.ASMTEST_HW_OK)
                    throw new HwTraceException($"asmtest_hwtrace_init failed: {rc}");
                TierInited = true;
            }
        }

        /// <summary>Tear the hardware-trace tier down.</summary>
        public static void Shutdown()
        {
            lock (TierLock)
            {
                HwNative.asmtest_hwtrace_shutdown();
                TierInited = false;
            }
        }

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
            GC.SuppressFinalize(this);
        }

        /// <summary>Dispose pattern over <see cref="Free"/> (idempotent).</summary>
        public void Dispose() => Free();

        // Finalizer backstop (B3): a dropped recorder still frees its native
        // handle. Only the OWNER's finalizer frees — the borrowed `Handle` below
        // (the OOP tracer records into a trace it does not own) never does.
        ~HwTrace()
        {
            if (_handle == IntPtr.Zero || !HwNative.LibAvailable) return;
            IntPtr h = _handle; _handle = IntPtr.Zero;
            try { HwNative.asmtest_trace_free(h); } catch { }
        }

        // The opaque trace handle, for the out-of-process tracer (which records into a
        // trace it does not own the lifecycle of — the caller still Free()s it).
        internal IntPtr Handle => _handle;
    }

    /// <summary>
    /// AMD-P0 — the DETERMINISTIC boundary LBR snapshot (src/branchsnap.c). Where
    /// <see cref="AsmTrace.WindowHot"/> statistically SAMPLES the branch stack (and honestly
    /// truncates a routine too small/fast to be caught in-region), this captures the frozen
    /// 16-entry branch stack EXACTLY at a region boundary: enable the LBR, plant a hardware
    /// execution breakpoint at <c>base+exitOff</c>, run the region once, and read the stack
    /// via <c>bpf_get_branch_snapshot()</c> when the boundary is hit. The decoded, in-region
    /// stream fills a <see cref="HwTrace"/> the coverage/offset APIs then read. Needs the far
    /// heavier substrate the sampled path does not — <c>CAP_BPF</c> + <c>CAP_PERFMON</c> + AMD
    /// LbrExtV2 + a BPF-toolchain build + Linux &gt;= 6.10 — so it <see cref="Available"/>-gates
    /// and self-skips where any is missing. Linux x86-64 only.
    /// </summary>
    public static class AmdSnapshot
    {
        /// <summary>
        /// True if the deterministic snapshot substrate is present (BPF build + AMD LbrExtV2 +
        /// kernel floor). Never throws — returns false (self-skip) where the lib or substrate
        /// is missing.
        /// </summary>
        public static bool Available() =>
            HwNative.LibAvailable && HwNative.asmtest_amd_snapshot_available() != 0;

        /// <summary>One honest sentence for a self-skip: why <see cref="Available"/> is false.</summary>
        public static string SkipReason()
        {
            if (!HwNative.LibAvailable) return "libasmtest_hwtrace not loaded";
            return Available()
                ? "available"
                : "deterministic LBR snapshot unavailable — needs CAP_BPF + CAP_PERFMON + AMD "
                + "LbrExtV2 + a BPF-toolchain build + Linux >= 6.10";
        }

        /// <summary>
        /// Deterministically capture <paramref name="code"/> by snapshotting the frozen LBR at
        /// <paramref name="exitOff"/> (the offset of the region's exit instruction — its final
        /// <c>ret</c> / tail branch), running <paramref name="run"/> once to drive execution to
        /// that boundary. Fills <paramref name="trace"/> (allocate with
        /// <see cref="HwTrace.Create"/>); read the reconstructed stream back with
        /// <see cref="HwTrace.InsnOffsets"/> / <see cref="HwTrace.Covered"/>. Returns
        /// <c>ASMTEST_HW_OK</c> (0), <c>ASMTEST_HW_EUNAVAIL</c> (-3, substrate/privilege absent),
        /// or <c>ASMTEST_HW_ENOSYS</c> (-5, built without the BPF toolchain). Returns -3 without
        /// throwing when the lib is missing.
        /// </summary>
        public static int Trace(NativeCode code, nuint exitOff, Action run, HwTrace trace)
        {
            if (code == null) throw new ArgumentNullException(nameof(code));
            if (trace == null) throw new ArgumentNullException(nameof(trace));
            if (!HwNative.LibAvailable) return HwNative.ASMTEST_HW_EUNAVAIL;
            HwNative.StealthRunFn cb = _ => { try { run?.Invoke(); } catch { } };
            int rc = HwNative.asmtest_amd_snapshot_trace(
                code.Base, (UIntPtr)(nuint)code.Length, (UIntPtr)exitOff, cb, IntPtr.Zero, trace.Handle);
            GC.KeepAlive(cb);
            return rc;
        }
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

        /// <summary>True if BTF block-step (<c>PTRACE_SINGLEBLOCK</c>) can run here —
        /// x86-64 Linux with a functional single-block step and Capstone. Callers
        /// self-skip to <see cref="TraceCall"/> where it is false.</summary>
        public static bool BlockstepAvailable()
        {
            if (!HwNative.LibAvailable) return false;
            try { return HwNative.asmtest_ptrace_blockstep_available() != 0; }
            catch { return false; }
        }

        /// <summary>
        /// BTF block-step variant of <see cref="TraceCall"/>: drives
        /// <c>PTRACE_SINGLEBLOCK</c> (one debug exception per TAKEN branch, ~4-10x fewer
        /// tracer stops than per-instruction single-step) and reconstructs the IDENTICAL
        /// per-instruction trace by disassembling each block's straight-line run. The only
        /// exact real-CPU capture on Zen 2, and a rootless completeness fallback everywhere.
        /// Throws where block-step is unavailable — probe <see cref="BlockstepAvailable"/>.
        /// </summary>
        public static long TraceCallBlockstep(IntPtr code, nuint len, long[] args, IntPtr trace)
        {
            var arr = (args == null || args.Length == 0) ? new long[1] : args;
            int n = args == null ? 0 : args.Length;
            int rc = HwNative.asmtest_ptrace_trace_call_blockstep(
                code, (UIntPtr)len, arr, n, out long result, trace);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_call_blockstep failed: {rc}");
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
        /// Block-step variant of <see cref="TraceAttached"/>: one debug exception per
        /// TAKEN branch (intra-block instructions reconstructed with Capstone), same
        /// contract otherwise — the rootless managed-runtime completeness fallback at a
        /// fraction of the stops. Probe first with <see cref="BlockstepAvailable"/>.
        /// </summary>
        public static long TraceAttachedBlockstep(int pid, IntPtr @base, nuint len, IntPtr trace)
        {
            int rc = HwNative.asmtest_ptrace_trace_attached_blockstep(
                pid, @base, (UIntPtr)len, out long result, trace);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_attached_blockstep failed: {rc}");
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
        /// §D3 whole-window capture that OWNS its tracee — the crash-proof, out-of-process
        /// analog of the in-process <see cref="AsmTrace"/> whole-window scope. Forks a child
        /// that calls the code at <paramref name="code"/> (<paramref name="len"/> bytes,
        /// already executable in this process) with up to six integer <paramref name="args"/>,
        /// runs it to the window frame, then single-steps OUT OF PROCESS recording the
        /// ABSOLUTE address of every instruction in the window frame OR any region published
        /// on <paramref name="channel"/> (the leaves the frame calls — publish them first with
        /// <see cref="AddrChannel.Publish"/>; pass null to record just the frame). Because the
        /// stepper is a separate process, a ptrace-stop is not gated by the tracee's signal
        /// mask, so it survives code the in-process single-step tier is forbidden to step
        /// (exceptions, <c>pthread_create</c>). Fills <paramref name="trace"/> with absolute
        /// addresses (classify by region); returns the frame's return value.
        /// </summary>
        public static long TraceWindowCall(IntPtr code, nuint len, long[] args,
                                           AddrChannel channel, IntPtr trace)
        {
            var arr = (args == null || args.Length == 0) ? new long[1] : args;
            int n = args == null ? 0 : args.Length;
            IntPtr chan = channel != null ? channel.Handle : IntPtr.Zero;
            int rc = HwNative.asmtest_ptrace_trace_window_call(
                code, (UIntPtr)len, arr, n, chan, out long result, trace);
            GC.KeepAlive(channel);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_window_call failed: {rc}");
            return result;
        }

        /// <summary>
        /// Descending variant of <see cref="TraceCall"/>: thread a <see cref="Descent"/>
        /// handle through the single-step loop so the call-outs the tracer would step over
        /// are recorded as edges and (at level &gt;= 2) descended as nested frames.
        /// <paramref name="trace"/> (the flat frame-0 view) may be <see cref="IntPtr.Zero"/>
        /// to record only into the descent handle.
        /// CRITICAL: <paramref name="region"/> is the traced region's byte length — pass it
        /// when the call target is an in-blob sibling that must stay OUTSIDE the region (a
        /// sibling inside the region mis-records as recursion). Defaults to the whole
        /// <see cref="NativeCode"/> allocation only when omitted.
        /// </summary>
        public static long TraceCallEx(NativeCode code, long[] args, IntPtr trace,
                                       Descent descent, nuint? region = null)
        {
            // A null/empty array can't be P/Invoked as a sized pointer; pass a 1-elem
            // placeholder with nargs = 0 (the native side ignores it).
            var arr = (args == null || args.Length == 0) ? new long[1] : args;
            int n = args == null ? 0 : args.Length;
            UIntPtr len = region.HasValue ? (UIntPtr)region.Value : (UIntPtr)(nuint)code.Length;
            IntPtr dh = descent != null ? descent.Handle : IntPtr.Zero;
            int rc = HwNative.asmtest_ptrace_trace_call_ex(
                code.Base, len, arr, n, out long result, trace, dh);
            // Keep the descent handle (and its pinned upcall trampolines) alive across the
            // out-of-process single-step, which may call back into a resolver/denylist.
            GC.KeepAlive(descent);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_call_ex failed: {rc}");
            return result;
        }

        /// <summary>
        /// Descending variant of <see cref="TraceAttached"/> for an externally-attached,
        /// ptrace-stopped process. <paramref name="trace"/> and <paramref name="descent"/>
        /// follow the same NULL rules as <see cref="TraceCallEx"/>.
        /// </summary>
        public static long TraceAttachedEx(int pid, IntPtr @base, nuint len, IntPtr trace, Descent descent)
        {
            IntPtr dh = descent != null ? descent.Handle : IntPtr.Zero;
            int rc = HwNative.asmtest_ptrace_trace_attached_ex(
                pid, @base, (UIntPtr)len, out long result, trace, dh);
            GC.KeepAlive(descent);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_attached_ex failed: {rc}");
            return result;
        }

        /// <summary>
        /// Descending variant of <see cref="TraceAttachedVersioned"/>: reconstruct the
        /// region's bytes from a <see cref="CodeImage"/> timeline as of sequence
        /// <paramref name="when"/> (0 =&gt; latest) while threading a <see cref="Descent"/>
        /// handle through the loop. Pass <paramref name="img"/> == null for a fresh read.
        /// </summary>
        public static long TraceAttachedVersionedEx(
            int pid, IntPtr @base, nuint len, CodeImage img, ulong when, IntPtr trace, Descent descent)
        {
            IntPtr imgHandle = img != null ? img.Handle : IntPtr.Zero;
            IntPtr dh = descent != null ? descent.Handle : IntPtr.Zero;
            int rc = HwNative.asmtest_ptrace_trace_attached_versioned_ex(
                pid, @base, (UIntPtr)len, imgHandle, when, out long result, trace, dh);
            GC.KeepAlive(descent);
            if (rc != HwNative.ASMTEST_PTRACE_OK)
                throw new HwTraceException($"asmtest_ptrace_trace_attached_versioned_ex failed: {rc}");
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

    /// <summary>One published code region for the §D3 windowed stepper — matches the native
    /// <c>asmtest_addr_rec_t</c> ({base, len, version}); the stepper records instructions
    /// whose absolute address falls in any published region.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct AddrRec
    {
        public ulong Base;
        public ulong Len;
        public ulong Version;
        public AddrRec(ulong @base, ulong len, ulong version = 0) { Base = @base; Len = len; Version = version; }
    }

    /// <summary>
    /// §D3 cross-process JIT-address channel (asmtest_addr_channel.h): the set of code
    /// regions the tracer should record inside a <see cref="Ptrace.TraceWindowCall"/> window,
    /// beyond the window frame itself. Publish each leaf/method the window calls into (its
    /// base + length) BEFORE the capture; the out-of-process stepper — which cannot see the
    /// runtime's own JIT events — records those regions and steps over everything else. A
    /// process-local ring; dispose to free it.
    /// </summary>
    public sealed class AddrChannel : IDisposable
    {
        IntPtr _handle;
        /// <summary>The native channel handle (IntPtr.Zero after dispose / on a lib-less build).</summary>
        public IntPtr Handle => _handle;

        /// <summary>Allocate + init a process-local channel. Throws if the native lib is absent.</summary>
        public AddrChannel()
        {
            if (!HwNative.LibAvailable)
                throw new HwTraceException("libasmtest_hwtrace not loaded — cannot create an AddrChannel");
            _handle = HwNative.asmtest_addr_channel_new();
            if (_handle == IntPtr.Zero)
                throw new HwTraceException("asmtest_addr_channel_new failed (out of memory)");
        }

        /// <summary>Publish one code region <c>[base, base+len)</c> the window will call into,
        /// with an optional code-image <paramref name="version"/> (0 if untracked).</summary>
        public void Publish(IntPtr @base, nuint len, ulong version = 0)
        {
            if (_handle == IntPtr.Zero) return;
            HwNative.asmtest_addr_channel_publish_rec(_handle, (ulong)@base.ToInt64(), (ulong)len, version);
        }

        /// <summary>Publish a <see cref="NativeCode"/> region (its whole allocation).</summary>
        public void Publish(NativeCode code) => Publish(code.Base, (nuint)code.Length);

        /// <summary>Idempotent free.</summary>
        public void Dispose()
        {
            if (_handle == IntPtr.Zero) return;
            try { HwNative.asmtest_addr_channel_free(_handle); } catch { }
            _handle = IntPtr.Zero;
        }
    }

    /// <summary>
    /// Call-descent policy (mirrors asmtest_descent_level_t): what the ptrace stepper does
    /// at each call-out it would otherwise step over. All default OFF.
    /// </summary>
    public enum DescentLevel
    {
        /// <summary>Step over, record nothing — today's behaviour.</summary>
        Off = 0,
        /// <summary>Record each (call-site -&gt; callee) edge, still step over.</summary>
        RecordEdges = 1,
        /// <summary>Single-step INTO calls whose target resolves (allow-set / resolver), else edge + step over.</summary>
        DescendKnown = 2,
        /// <summary>Single-step INTO every call (denylist + budget + watchdog gated). Best-effort; default OFF.</summary>
        DescendAll = 3,
    }

    /// <summary>
    /// A recorded (call-site -&gt; callee) edge from a stepped-over call (mirrors the
    /// asmtest_descent_edge_* accessors): the <see cref="Site"/> call-site byte offset,
    /// the ABSOLUTE <see cref="Target"/> address, and the caller <see cref="Depth"/>.
    /// </summary>
    public readonly struct DescentEdge
    {
        /// <summary>Call-site byte offset within the recording frame.</summary>
        public ulong Site { get; }

        /// <summary>Absolute address of the callee (NOT an offset).</summary>
        public ulong Target { get; }

        /// <summary>Depth of the caller frame (0 = frame 0).</summary>
        public uint Depth { get; }

        public DescentEdge(ulong site, ulong target, uint depth)
        {
            Site = site;
            Target = target;
            Depth = depth;
        }

        public override string ToString() => $"DescentEdge(site=0x{Site:x}, target=0x{Target:x}, depth={Depth})";
    }

    /// <summary>
    /// The reference scoped-trace construct — an <see cref="IDisposable"/> over the
    /// register-then-begin/end pair with the shared-core render-on-close. Use it as
    /// <c>using (new AsmTrace(code)) { HotPath(data); }</c>: the constructor auto-names
    /// from the call site (<c>[CallerMemberName]</c> + <c>[CallerLineNumber]</c>),
    /// registers the traced range, and brackets <c>try_begin</c> (a nonzero return is a
    /// clean self-skip); <see cref="Dispose"/> ends the region, always renders to
    /// populate <see cref="Path"/>, and — when <c>emit</c> — writes that text to stdout.
    /// Requires the tier to be up (<see cref="HwTrace.Init"/>); the C core flags the
    /// trace <c>truncated</c> on a cross-thread close (§0.2/§1), surfaced via
    /// <see cref="Truncated"/>. On <c>net8.0</c> with <c>&lt;Nullable&gt;disable&lt;/Nullable&gt;</c>
    /// the caller-info parameters are unannotated <c>string</c> (a <c>?</c> would emit
    /// CS8632). A <c>[ModuleInitializer]</c> arm-on-import is the documented ergonomic
    /// optimisation on top (the real slot/SIGTRAP arming stays lazy at first scope,
    /// which is also why CA2255 is safe to suppress for a library ship).
    /// </summary>
    /// <summary>§D3: a finalizable holder for the native AMD-LBR sampler ctx, so ONLY the
    /// inline AMD scopes carry finalization overhead (not every AsmTrace). Dispose drains via
    /// <see cref="End"/>; a leaked (undisposed) scope has its perf event + mapping released by
    /// the finalizer (drain-less), so the fd is never leaked.</summary>
    internal sealed class AmdSampler
    {
        IntPtr _ctx;
        public AmdSampler(IntPtr ctx) { _ctx = ctx; }

        /// <summary>Drain + release: DISABLE + drain endpoints into <paramref name="ips"/> +
        /// free the ctx. Idempotent (a second call is a no-op).</summary>
        public int End(ulong[] ips, out UIntPtr nips, out int truncated)
        {
            nips = UIntPtr.Zero; truncated = 0;
            IntPtr c = _ctx;
            if (c == IntPtr.Zero) return 0;
            _ctx = IntPtr.Zero;
            GC.SuppressFinalize(this);
            return HwNative.asmtest_hwtrace_sample_end_amd(
                c, ips, (UIntPtr)(ips != null ? ips.Length : 0), out nips, out truncated);
        }

        ~AmdSampler()
        {
            IntPtr c = _ctx;
            if (c == IntPtr.Zero || !HwNative.LibAvailable) return;
            _ctx = IntPtr.Zero;
            // Drain-less release of a leaked scope's fd + mapping (data is irrelevant here).
            try { HwNative.asmtest_hwtrace_sample_end_amd(c, null, UIntPtr.Zero, out _, out _); } catch { }
        }
    }

    /// <summary>§Z1.2: a finalizable holder for the native STRONG-tier Intel PT window ctx, so
    /// ONLY the inline PT scopes carry finalization overhead (not every AsmTrace). Dispose
    /// drains + decodes via <see cref="End"/>; a leaked (undisposed) scope has its perf event +
    /// AUX mappings released by the finalizer (drain-less, trace==Zero), so the fd is never
    /// leaked.</summary>
    internal sealed class PtWindowCtx
    {
        IntPtr _ctx;
        public PtWindowCtx(IntPtr ctx) { _ctx = ctx; }

        /// <summary>Drain + decode into <paramref name="trace"/> against <paramref name="img"/>
        /// as of <paramref name="when"/> (img==Zero: the ctx's own self image) + free the ctx.
        /// Idempotent (a second call is a no-op).</summary>
        public int End(IntPtr img, ulong when, IntPtr trace)
        {
            IntPtr c = _ctx;
            if (c == IntPtr.Zero) return 0;
            _ctx = IntPtr.Zero;
            GC.SuppressFinalize(this);
            return HwNative.asmtest_hwtrace_pt_end_window(c, img, when, trace);
        }

        ~PtWindowCtx()
        {
            IntPtr c = _ctx;
            if (c == IntPtr.Zero || !HwNative.LibAvailable) return;
            _ctx = IntPtr.Zero;
            // Drain-less release of a leaked scope's fd + AUX mappings (trace==Zero: no decode).
            try { HwNative.asmtest_hwtrace_pt_end_window(c, IntPtr.Zero, 0, IntPtr.Zero); } catch { }
        }
    }

    /// <summary>§D3: a finalizable holder for the native out-of-process inline stepper context,
    /// so only the inline OOP window scopes carry finalization overhead. Dispose drains via
    /// <see cref="End"/>; a leaked (undisposed) scope has its helper process joined + freed by
    /// the finalizer.</summary>
    internal sealed class OopWindowCtx
    {
        IntPtr _ctx;
        public OopWindowCtx(IntPtr ctx) { _ctx = ctx; }

        public int End(IntPtr traceHandle)
        {
            IntPtr c = _ctx;
            if (c == IntPtr.Zero) return 0;
            _ctx = IntPtr.Zero;
            GC.SuppressFinalize(this);
            return HwNative.asmtest_hwtrace_stealth_window_end(c, traceHandle);
        }

        ~OopWindowCtx()
        {
            IntPtr c = _ctx;
            if (c == IntPtr.Zero || !HwNative.LibAvailable) return;
            _ctx = IntPtr.Zero;
            try { HwNative.asmtest_hwtrace_stealth_window_end(c, IntPtr.Zero); } catch { }
        }
    }

    public sealed class AsmTrace : IDisposable
    {
        readonly string _name;
        IntPtr _handle;             // ctor-assigned (R3: also by the shared ArmWholeWindow)
        readonly bool _emit;
        // R1: the single Dispose discriminator that replaces the four parallel `_*Window`
        // bools — it names which teardown Dispose runs. Region (the default) covers the
        // region form (`new AsmTrace(code)`) and the named-method scopes (name-keyed slice
        // render); each window kind gets its own Dispose branch.
        enum Kind
        {
            Region,          // region form + Method scopes (name-keyed slice render)
            WholeWindow,     // §Z0/§Z1: single-step region-free empty-ctor whole-window
            OopWindow,       // §D3: out-of-process whole-window (AsmTrace.Window factory)
            AmdWindow,       // §D3: AMD-LBR statistical inline whole-window (new AsmTrace(HwBackend.AmdLbr))
            OopInlineWindow, // §D3: out-of-process inline whole-window (new AsmTrace(outOfProcess: true))
            PtWindow,        // §Z1.2: Intel PT STRONG inline whole-window (new AsmTrace(HwBackend.IntelPt))
        }
        readonly Kind _kind;    // defaults to Kind.Region (the region / named-method form)
        AmdSampler _amdSampler; // the armed AMD sampler (finalizable; null until armed)
        PtWindowCtx _ptWinCtx;  // the armed Intel PT window ctx (finalizable; null until armed)
        OopWindowCtx _oopWinCtx;    // the armed async OOP stepper context (finalizable; null until armed)
        IntPtr _oopWinChan;         // the shared address channel for JIT regions
        ulong[] _amdIps;            // the endpoint buffer drained in Dispose
        readonly bool _renderPath;  // §Z5 opt-in: render the whole window into Path
        readonly AsmNamedRegion[] _regions; // §3.1(c) opt-in: named-region attribution -> Buckets
        bool _rundownRequested;     // §D0.2: pair DisablePerfMap with the REQUEST (R3: also set by ArmWholeWindow)
        readonly int _rundownSettleMs;   // §D0.2: opt-in bound to let the async R2R jitdump flush before Dispose reads it
        JitMethodMap _map;          // §D0.1: managed-method labelling (byMethod only; R3: also set by ArmWholeWindow)
        readonly Delegate _target;  // §D0.3: the named method (Method(...) scopes only)
        readonly IntPtr _methodBase; // §D0.3: resolved JIT'd body base
        readonly UIntPtr _methodLen; // §D0.3: resolved JIT'd body length
        readonly bool _oop;         // §D3: route Invoke through the stealth stepper
        bool _began;                // an in-process begin succeeded (Dispose must end)
        readonly int _armTid;       // §0.2/B5: managed thread that armed (complements the native OS-tid check)
        HwNative.HwScope _scope;    // region-free scope handle (whole-window only)
        bool _disposed;
        AsmTrace _survey;           // §E1: the pass-1 WindowHot survey behind a WindowHybrid scope

        /// <summary>The rendered assembly listing (populated on close).</summary>
        public string Path { get; private set; }
        /// <summary>True if the scope armed (a backend was available and begin succeeded).</summary>
        public bool Armed { get; private set; }
        /// <summary>True if the close hopped OS threads / the capture overflowed (§0.2/§1/§Z4).</summary>
        public bool Truncated { get; private set; }
        /// <summary>§Z5: when the scope did NOT arm, the honest human-readable reason
        /// (no faithful backend, tier not up, not Linux). Empty when armed.</summary>
        public string SkipReason { get; private set; } = "";
        /// <summary>managed-wholewindow-compose T7 (§Z1.1 safe-managed routing): which arm the
        /// empty-ctor whole-window chose — <c>"inproc"</c> (the convention-mitigated in-process
        /// single-step default), <c>"pt"</c> (Intel PT where the silicon exists), <c>"oop"</c>
        /// (the §D3 out-of-process stepper), or <c>"none"</c> (safe-managed policy active but
        /// neither PT nor ptrace available — an honest skip). Under the safe-managed policy this
        /// is NEVER <c>"inproc"</c>. Empty for non-whole-window scopes.</summary>
        public string Route { get; private set; } = "";
        /// <summary>§Z1: the raw ABSOLUTE addresses captured by a whole-window scope, in
        /// execution order (empty for the region-scoped form). Range-classify these
        /// against known native regions to tell multiple leaves apart.</summary>
        public ulong[] Addresses { get; private set; } = System.Array.Empty<ulong>();
        /// <summary>§3.1(c): the whole-window capture's ABSOLUTE addresses attributed to
        /// labelled buckets — each matched against the ctor's <c>regions</c> FIRST (exact,
        /// symbol-free), then falling back to the perf-map JIT symbol / mapped-file region
        /// for the runtime remainder. Empty unless <c>regions</c> was passed to a
        /// whole-window scope (and on a scope that never armed).
        /// <para>This is the attribution <see cref="HwTrace.SymbolizeBuckets"/> CANNOT do:
        /// it resolves by symbol/mapping, so several <c>exec_alloc</c>'d leaves all collapse
        /// into one <c>[anon]</c> — and two leaves with identical bytes are indistinguishable
        /// to it. An exact address range is the only thing that separates them.</para></summary>
        public HwBucket[] Buckets { get; private set; } = System.Array.Empty<HwBucket>();
        /// <summary>§D0.1: for a <c>byMethod</c> whole-window scope, the managed methods
        /// that executed in the window, sorted by attributed instruction count
        /// (descending); empty otherwise. The native-runtime remainder is not included —
        /// it is <see cref="Addresses"/>.Length − <see cref="LabelledInstructions"/>.</summary>
        public IReadOnlyList<AsmMethod> Methods { get; private set; } = System.Array.Empty<AsmMethod>();
        /// <summary>§D0.1: managed methods the JIT map observed while the scope was open.</summary>
        public int MethodsObserved { get; private set; }
        /// <summary>§D0.1: captured instructions attributed to a managed method.</summary>
        public long LabelledInstructions { get; private set; }
        /// <summary>§D0.2: true if the <c>withRundown</c> perf-map rundown was accepted by the
        /// runtime — so <see cref="Methods"/> also names WARM methods (JIT'd before the scope).
        /// False (a clean self-skip to the cold-only result) where diagnostics are off.</summary>
        public bool RundownEnabled { get; private set; }
        /// <summary>§E3: method records the live JIT publisher pushed to the out-of-process
        /// <see cref="Window"/>'s stepper while the window was open — methods JIT'd fresh
        /// MID-WINDOW that the pre-window coarse ranges could not cover. 0 for every other
        /// scope form, for the §E1 hybrid's hot-slice pass (live publish off by design), and
        /// when nothing was JIT'd in-window.</summary>
        public long LiveJitPublished { get; private set; }
        /// <summary>§D3: true for an <see cref="WindowHot"/> AMD-LBR scope — the result is a
        /// SAMPLED, STATISTICAL survey, not an exact trace. When true the instruction-framed
        /// members change meaning: <see cref="Methods"/><c>.Count</c> and
        /// <see cref="InstructionsIn"/> are a sample-weighted HOT-METHOD weight (branch-target
        /// endpoint hits), NOT an instruction count; <see cref="LabelledInstructions"/> is the
        /// resolved-sample count; <see cref="Addresses"/> are sampled branch-target PCs (not an
        /// ordered execution stream); and <see cref="Disassembly"/> is EMPTY (there is no
        /// ordered stream to render, and its per-instruction "elided native gap" has no meaning
        /// for sampled endpoints). <see cref="Truncated"/> then means "the survey is a prefix"
        /// (dropped/throttled samples), a coverage signal — not a hard error. False (an exact
        /// scope) leaves all of these with their exact-trace meaning.</summary>
        public bool IsStatistical { get; private set; }
        /// <summary>
        /// §E6: how many CHECKPOINT ISLANDS were merged into <see cref="Addresses"/> — one per
        /// time execution reached a <c>tileCheckpoints</c> address while the window was open.
        /// 0 for every non-tiled scope, and also when tiling could not arm (no <c>CAP_BPF</c> /
        /// BPF-toolchain build / AMD LbrExtV2 / free debug register) or no checkpoint ever ran
        /// — in which case the scope is EXACTLY the plain survey, not a failure. Nonzero is the
        /// only proof an island actually landed, so a caller that cares should check it rather
        /// than assume tiling happened because it asked for it.
        /// </summary>
        public int TiledIslands { get; private set; }
        /// <summary>
        /// §E6: how many leading entries of <see cref="Addresses"/> came from checkpoint
        /// islands rather than the sampler — exactly <c>Addresses.Take(TiledAddresses)</c>.
        /// Islands are merged FIRST so the sampler can never crowd them out of the bounded
        /// buffer. On a complete merge this is <see cref="TiledIslands"/> * the AMD branch-stack
        /// depth (16 on every shipping part): one checkpoint hit freezes the whole stack.
        ///
        /// WHAT THESE GUARANTEE, exactly: at each checkpoint HIT, the ~16 most recently retired
        /// branch targets, EXACTLY. WHAT THEY DO NOT: anything about the code BETWEEN hits. The
        /// merged surface is therefore SAMPLED / PARTIAL COVERAGE — exact islands in an
        /// unobserved sea — and NOT an exact whole-window trace, which remains a hardware dead
        /// end on AMD and is a documented non-goal. That is honest for this surface precisely
        /// because <see cref="WindowHot"/> is a HOT-ADDRESS histogram and makes no completeness
        /// claim; tiled endpoints must never be read as one. <see cref="IsStatistical"/> stays
        /// TRUE for a tiled scope for exactly this reason: adding exact islands to a sampled
        /// survey yields a better-covered SURVEY, not an exact trace.
        /// </summary>
        public int TiledAddresses { get; private set; }
        /// <summary>
        /// §E6: true if the ISLAND MERGE lost endpoints — a saturated BPF ringbuf dropped a
        /// checkpoint hit, or <see cref="Addresses"/> filled with islands left to merge.
        ///
        /// This is NOT <see cref="Truncated"/>, and the difference is load-bearing.
        /// <see cref="Truncated"/> is the SURVEY-wide prefix flag and also fires when the
        /// SAMPLER's ring goes near-full — which says nothing whatever about whether an island
        /// kept its endpoints. An island-content assertion written in this repo's
        /// "covered OR truncated" shape must therefore disjoin with THIS flag, never with
        /// <see cref="Truncated"/>: that rule is sound only when the truncation term is
        /// CAUSALLY TIED to the property asserted, and a fixture whose branch count grew until
        /// the sampler lost records would otherwise satisfy such an assert forever, with the
        /// island silently never checked again.
        ///
        /// Like <see cref="Truncated"/>, it does NOT mean "the coverage is partial" — tiled
        /// coverage is ALWAYS partial by construction (a ~16-branch tail per hit). A flag that
        /// is always true makes every assertion disjoined with it vacuous, so it means only:
        /// endpoints were LOST.
        /// </summary>
        public bool TiledTruncated { get; private set; }
        /// <summary>§E1: for a <see cref="WindowHybrid"/> scope, the pass-1 <see cref="WindowHot"/>
        /// AMD-LBR statistical survey used to pick the hot method set that this (pass-2, exact)
        /// scope captured — inspect its <see cref="Methods"/> for the sampled hot histogram and
        /// its <see cref="Armed"/>/<see cref="SkipReason"/> to see whether the survey drove the
        /// hot-slice restriction or the scope degraded to a full exact <see cref="Window"/>.
        /// Null for every non-hybrid scope.</summary>
        public AsmTrace Survey => _survey;
        /// <summary>§Z1: the LABELLED executed instructions of a <c>byMethod</c> whole-window
        /// scope, in execution order — each captured instruction that resolved to a managed
        /// method, disassembled from live memory and paired with its method name (and the
        /// run of native-runtime instructions elided just before it). Empty for a non-<c>byMethod</c>
        /// scope, or when this build has no Capstone (<see cref="DisassemblyAvailable"/> is
        /// then false). The unlabelled native-runtime instructions are intentionally omitted —
        /// this is the named instruction stream, not the raw million-instruction window.</summary>
        public IReadOnlyList<AsmInstruction> Disassembly { get; private set; } = System.Array.Empty<AsmInstruction>();
        /// <summary>True if this build links Capstone, so <see cref="Disassembly"/> carries
        /// real instruction text; false (empty <see cref="Disassembly"/>) without it.</summary>
        public bool DisassemblyAvailable { get; private set; }
        /// <summary>The auto-generated (or explicit) region name.</summary>
        public string Name => _name;

        /// <summary>Captured whole-window instructions whose ABSOLUTE address falls in
        /// <c>[start, start+length)</c> — e.g. count a known native leaf's executions.</summary>
        public long CountInRange(ulong start, ulong length)
        {
            ulong end = start + length;
            long n = 0;
            foreach (ulong ip in Addresses)
                if (ip >= start && ip < end) n++;
            return n;
        }

        /// <summary>§D0.1: captured instructions in managed methods whose name contains
        /// <paramref name="nameSubstring"/>; 0 unless this is a <c>byMethod</c> scope. On a
        /// STATISTICAL scope (<see cref="IsStatistical"/>) there is no instruction stream, so
        /// this returns the sample WEIGHT of the matching methods (== <see cref="WeightIn"/>) —
        /// reach for <see cref="WeightIn"/> there so the caller's intent reads honestly.</summary>
        public long InstructionsIn(string nameSubstring)
        {
            long n = 0;
            foreach (var m in Methods)
                if (m.Name.Contains(nameSubstring)) n += m.Count;
            return n;
        }

        /// <summary>§E2: the summed <see cref="AsmMethod.Weight"/> of the methods whose name
        /// contains <paramref name="nameSubstring"/> — the honest accessor on a STATISTICAL
        /// scope (<see cref="IsStatistical"/>), where the value is a sampled hot weight and
        /// "instruction count" would be a category error. Numerically identical to
        /// <see cref="InstructionsIn"/> (weight == count); the two differ only in what they
        /// claim to mean, so pick the one that matches the scope's fidelity.</summary>
        public long WeightIn(string nameSubstring)
        {
            long n = 0;
            foreach (var m in Methods)
                if (m.Name.Contains(nameSubstring)) n += m.Weight;
            return n;
        }

        /// <summary>dotnet-pt-inwindow-jit-premise T1: bounded IN-WINDOW wait for the live
        /// JIT map to observe a method whose name contains <paramref name="nameSubstring"/>.
        /// A method first-called inside a window is COMPILED inside it (the runtime emits the
        /// method-load event at JIT time, before the body runs), but the event reaches the
        /// map on the runtime's EventPipe dispatch thread asynchronously — a native-speed PT
        /// window closes sub-millisecond after the call returns, ahead of that delivery, so
        /// the close-time <see cref="MethodsObserved"/> snapshot undercounts. Call this
        /// INSIDE the scope, after the call under test, to hold the window open until the
        /// delivery lands — the property a slow single-step window gets for free. Returns
        /// true once observed; false on an unarmed or mapless scope, or when
        /// <paramref name="timeoutMs"/> expires (a genuine delivery stall — callers keep
        /// their honest self-skip for that arm). Deliberately NOT wired into Dispose:
        /// closes must never block, and "wait for ≥1 method" is wrong for a window that
        /// JITs nothing.</summary>
        public bool WaitMethodObserved(string nameSubstring, int timeoutMs = 2000)
        {
            if (!Armed || _map == null) return false;
            long deadline = Environment.TickCount64 + timeoutMs;
            while (_map.CountFor(nameSubstring) == 0)
            {
                if (Environment.TickCount64 >= deadline) return false;
                Thread.Sleep(1);
            }
            return true;
        }

        /// <summary>
        /// §Z0/§Z1 — the aspirational EMPTY-ctor form: <c>using (new AsmTrace()) { HotPath(data); }</c>.
        /// No <see cref="NativeCode"/>, no <c>[base,len)</c>; arms a region-free WHOLE-WINDOW
        /// capture on the calling thread and renders whatever executed on <see cref="Dispose"/>.
        /// Honest envelope (see the zero-config plan): the single-step WEAK tier runs on any
        /// x86-64 Linux for a NATIVE leaf; the STRONG whole-window PT / AMD LBR tiers are
        /// forward-look and this ctor <b>self-skips</b> (records <see cref="SkipReason"/>,
        /// leaves <see cref="Armed"/> false) where no faithful backend is available — never
        /// throws. No <see cref="HwTrace.Init"/> needed: when nothing is inited the ctor
        /// auto-inits the portable single-step tier itself (§Z0).
        /// </summary>
        /// <param name="byMethod">§D0.1: also label the captured window by managed method —
        /// exposes <see cref="Methods"/> / <see cref="LabelledInstructions"/> / <see
        /// cref="InstructionsIn"/> on close. Enables an in-process JIT map for the scope's
        /// lifetime (CoreCLR); leave false to skip that overhead.</param>
        /// <param name="withRundown">§D0.2: additionally resolve WARM methods (JIT'd before
        /// the scope, e.g. <c>System.Console.WriteLine</c>) by asking the runtime for a
        /// perf-map rundown over its own diagnostics socket (dependency-free, no launch
        /// knob; see <see cref="DiagnosticsIpc"/>). Implies <paramref name="byMethod"/>.
        /// Self-skips to the cold-only result where diagnostics are off. CoreCLR/Linux.</param>
        /// <param name="renderPath">§Z5 (opt-in): also render the whole window into
        /// <see cref="Path"/> on close, via the native <c>render_window</c> (live-memory
        /// disassembly, truncation banner included). Default-off because a whole window is
        /// often the raw million-instruction runtime stream; the data-only default exposes
        /// <see cref="Addresses"/> / <see cref="Disassembly"/> instead. Needs Capstone
        /// (<see cref="Path"/> stays empty without it).</param>
        /// <param name="rundownSettleMs">§D0.2 (opt-in): with <paramref name="withRundown"/>,
        /// the runtime writes <c>/tmp/jit-&lt;pid&gt;.dump</c> ASYNCHRONOUSLY and forward, but
        /// <see cref="Dispose"/> folds it in at close — so for a short scope the R2R BCL
        /// records (LINQ <c>Enumerable</c>, <c>Dictionary</c>, <c>List</c>, string formatting)
        /// may not be flushed yet and those methods go unnamed. A positive value bounds a
        /// close-time wait for the jitdump to stop growing (quiescence) before it is read,
        /// so those methods are named. Default 0 keeps the current no-wait close latency
        /// (the load is still attempted, just without waiting). Ignored without
        /// <paramref name="withRundown"/>.</param>
        /// <param name="regions">§3.1(c) (opt-in): caller-known code regions to attribute the
        /// captured window against, exposing <see cref="Buckets"/> on close (via the native
        /// <c>attribute_window</c>). Each captured address is matched to these EXACT ranges
        /// first, then falls back to perf-map / maps for the runtime remainder. This is the
        /// only way to tell several native leaves apart: maps collapses every
        /// <c>exec_alloc</c>'d blob into one <c>[anon]</c>, and two leaves with IDENTICAL
        /// BYTES defeat symbol/disassembly attribution entirely. Attribution runs while the
        /// frame is still live (before the trace is freed), so it cannot be done after the
        /// scope closes — pass the regions up front. Null/empty leaves <see cref="Buckets"/>
        /// empty.</param>
        public AsmTrace(bool emit = true, bool byMethod = false, bool withRundown = false,
                        bool renderPath = false, int rundownSettleMs = 0,
                        AsmNamedRegion[] regions = null, bool? safeManaged = null,
                        [CallerMemberName] string member = null,
                        [CallerLineNumber] int line = 0)
        {
            _name = ScopeName(member, line);
            _emit = emit;
            _renderPath = renderPath;
            _regions = regions;
            _rundownSettleMs = rundownSettleMs;
            _armTid = Environment.CurrentManagedThreadId;
            // Honor the "never throws" contract: with no native lib loaded, the first P/Invoke
            // (asmtest_trace_new) would throw DllNotFoundException. Self-skip cleanly (Armed
            // stays false, SkipReason set) so `using (new AsmTrace())` degrades, not crashes —
            // the same LibAvailable guard HwTrace.Available/Resolve/Auto already use.
            if (!HwNative.LibAvailable)
            {
                _kind = Kind.WholeWindow;
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            // managed-wholewindow-compose T7 — §Z1.1 safe-managed routing: "PT/LBR where the
            // silicon exists, else the §D3 [out-of-process] stepper" — never in-process TF
            // against a managed runtime's threads (whose SIGTRAP disposition CoreCLR's PAL owns).
            // The DECISION lives in ResolveWholeWindowRoute (unit-testable without arming); this
            // switch only ACTS on it. Inactive (the default) is byte-identical to today.
            Route = ResolveWholeWindowRoute(safeManaged);
            switch (Route)
            {
                case "pt":
                    // PT where the silicon exists — the ONE PT arm owned by the substrate
                    // (asmtest_hwtrace_pt_begin_window); this ctor only calls it.
                    _kind = Kind.PtWindow;
                    ArmPtWindow(byMethod, withRundown);
                    return;
                case "oop":
                    // else the §D3 out-of-process stepper — the arming thread is never TF-armed.
                    _kind = Kind.OopInlineWindow;
                    ArmOopWindow(byMethod, withRundown);
                    return;
                case "none":
                    // else an honest self-skip naming both misses. Nothing armed → no teardown;
                    // _kind is a Dispose no-op discriminator (WholeWindow with Armed=false,
                    // _handle Zero, _scope the invalid sentinel so end_window can't resolve it).
                    _kind = Kind.WholeWindow;
                    _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0, ArmTid = -1 };
                    SkipReason =
                        "PT unavailable (" + HwTrace.SkipReason(HwBackend.IntelPt) + "); "
                        + "ptrace unavailable (" + Ptrace.SkipReason() + ") — in-process TF "
                        + "refused under safe-managed routing";
                    return;
                default: // "inproc": R3 single-step whole-window arming, shared with the
                         // backend-keyed `new AsmTrace(HwBackend.SingleStep)` ctor (ArmWholeWindow).
                    _kind = Kind.WholeWindow;
                    ArmWholeWindow(byMethod, withRundown);
                    break;
            }
        }

        // T7 (managed-wholewindow-compose): the §Z1.1 safe-managed routing DECISION, factored out
        // of the empty ctor so it is unit-testable WITHOUT arming (arming the region-free OOP
        // window in a live managed process with active GC can abort CoreCLR — the ptrace stepper
        // collides with the runtime's thread-suspension, the T3 finding — so a suite check reads
        // the route here instead of live-arming it). Returns the route the empty ctor will take:
        //   "inproc" — policy off, or not a managed process: the in-process EFLAGS.TF whole-window
        //              (the shipping default, byte-identical to pre-T7 behavior).
        //   "pt"     — safe-managed active + managed present + the Intel PT window tier arms.
        //   "oop"    — else the §D3 out-of-process stepper is available (ptrace permitted).
        //   "none"   — safe-managed active + managed present but NEITHER PT nor ptrace: an honest
        //              skip. In-process TF is NEVER selected once the policy is active on a managed
        //              process. Opt-in: the `safeManaged` parameter wins, else the env var.
        internal static string ResolveWholeWindowRoute(bool? safeManaged)
        {
            bool policy = safeManaged ??
                (Environment.GetEnvironmentVariable("ASMTEST_WHOLEWINDOW_SAFE_MANAGED") == "1");
            if (!policy || HwNative.asmtest_hwtrace_managed_runtime_present() == 0)
                return "inproc";
            if (HwTrace.Available(HwBackend.IntelPt)) return "pt";
            if (Ptrace.Available()) return "oop";
            return "none";
        }

        // R3: the single-step whole-window arming shared by the empty `new AsmTrace()` ctor
        // and the backend-keyed `new AsmTrace(HwBackend.SingleStep)` ctor. Sets up the JIT
        // map + perf-map rundown BEFORE arming (an in-proc listener sees only methods JIT'd
        // after enable, and the socket-driven rundown must run at full speed, not under
        // single-step), allocates the retained whole-window trace, opens the region-free
        // scope, and auto-inits the portable single-step tier on ESTATE (so no explicit
        // HwTrace.Init is needed). Callers must have set the readonly ctor fields and
        // returned on the no-lib self-skip first.
        void ArmWholeWindow(bool byMethod, bool withRundown)
        {
            // §D0.1/§D0.2: enable the method map BEFORE arming, so it sees the methods JIT'd
            // inside the scope (an in-proc listener sees only methods JIT'd after enable).
            // trackBytes (§Z3): also snapshot each method's bytes into a code-image so the
            // close-time render/disassembly can decode the version live in the window.
            if (byMethod || withRundown) _map = new JitMethodMap(trackBytes: true);
            // §D0.2: trigger the perf-map rundown BEFORE arming — its socket code must run
            // at full speed, NOT under single-step (which would step the whole .NET socket
            // stack). The rundown captures the already-JIT'd WARM methods retroactively; the
            // runtime logs cold ones forward. A self-skip still gets DisablePerfMap on close,
            // so it is enabled only briefly, never left on process-wide.
            _rundownRequested = withRundown;
            if (withRundown) RundownEnabled = DiagnosticsIpc.EnablePerfMap();
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0, ArmTid = -1 };
            // Whole-window captures the runtime too (JIT/GC/marshalling), so size the
            // retained trace to the single-step window ring (SS_WINDOW_CAP = 1<<20) — a
            // tiny native leaf can otherwise fall past a small cap behind the managed
            // call machinery. Cheap: the trace buffer is only committed as it fills.
            _handle = HwNative.asmtest_trace_new((UIntPtr)(1 << 20), (UIntPtr)0);
            int rc = _handle != IntPtr.Zero
                ? HwNative.asmtest_hwtrace_begin_window(_handle, ref _scope)
                : HwNative.ASMTEST_HW_ENOSYS;
            if (rc == HwNative.ASMTEST_HW_ESTATE)
            {
                // §Z1.3: the tier was never inited — consult the WEAK/STRONG window ladder
                // to pick which backend to auto-init. window_auto() returns IntelPt only
                // when the substrate is present AND the whole-window decode is trusted at
                // runtime (else SingleStep, else EUNAVAIL); begin_window (T2) then arms the
                // PT window natively on the sentinel handle, or the portable single-step
                // window. Fires ONLY when nothing is inited (begin_window returns ESTATE);
                // an explicit Init of any backend is preserved (OK/EUNAVAIL, never ESTATE),
                // and begin_window on ESTATE is a side-effect-free no-op, so the retry is safe.
                int tier = HwNative.asmtest_hwtrace_window_auto();
                HwBackend b = tier == (int)HwBackend.IntelPt
                    ? HwBackend.IntelPt
                    : HwBackend.SingleStep;
                int initRc = AutoInitWindowBackend(b);
                rc = initRc == HwNative.ASMTEST_HW_OK
                    ? HwNative.asmtest_hwtrace_begin_window(_handle, ref _scope)
                    : initRc;
            }
            Armed = rc == HwNative.ASMTEST_HW_OK;
            if (!Armed) SkipReason = WholeWindowSkipReason(rc);
        }

        // §Z0/§Z1.3: lazily bring up the whole-window tier `b` (the ladder's pick) for the
        // empty-ctor scope, so callers need no explicit HwTrace.Init. Serialized via the
        // shared TierLock; skips the native init if the tier is already up (explicit Init or
        // a prior scope) so it can never re-init over a live capture. Returns the native
        // init status.
        internal static int AutoInitWindowBackend(HwBackend b)
        {
            lock (HwTrace.TierLock)
            {
                if (HwTrace.TierInited) return HwNative.ASMTEST_HW_OK;
                var opts = new HwNative.Options
                {
                    StructSize = (UIntPtr)Marshal.SizeOf<HwNative.Options>(), // F27
                    Backend = (int)b,
                };
                int rc = HwNative.asmtest_hwtrace_init(ref opts);
                if (rc == HwNative.ASMTEST_HW_OK) HwTrace.TierInited = true;
                return rc;
            }
        }

        // Back-compat alias: the historical empty-ctor / lazy-arm auto-init was
        // single-step-only. Callers that specifically need the WEAK tier keep this name.
        internal static int AutoInitSingleStep() =>
            AutoInitWindowBackend(HwBackend.SingleStep);

        // §Z5: map a begin_window / auto-init failure to an actionable, honest message. The
        // ctor auto-inits the single-step tier, so a failure means the host cannot run it.
        // The EUNAVAIL case appends the §Z5.2 composed LADDER (which tiers were probed and
        // why each self-skipped), so the message names the missing capability, not just "no".
        static string WholeWindowSkipReason(int rc) => rc switch
        {
            // T6 (managed-wholewindow-compose): the opt-in safe-managed refusal. Keyed on the
            // DISTINCT EMANAGED sentinel (not EUNAVAIL) so a policy refusal reads differently
            // from a genuinely absent tier — the whole reason begin_window returns its own code.
            HwNative.ASMTEST_HW_EMANAGED =>
                "managed runtime present; in-process TF window refused (safe-managed) — use "
                + "the out-of-process window or the PT tier",
            HwNative.ASMTEST_HW_EUNAVAIL =>
                "single-step whole-window tier unavailable on this host (needs x86-64 Linux; "
                + "whole-window is single-step only, so a non-single-step backend inited via "
                + "HwTrace.Init will not arm it) — " + HwTrace.DegradationNote(),
            HwNative.ASMTEST_HW_ESTATE => "hwtrace tier not up (auto-init failed)",
            HwNative.ASMTEST_HW_ENOSYS => "whole-window scope is Linux/x86-64 only",
            _ => "whole-window scope did not arm",
        };

        // T7 (managed-wholewindow-compose): the Intel PT STRONG whole-window inline arm, factored
        // out of the `AsmTrace(HwBackend.IntelPt)` ctor so the empty-ctor safe-managed PT route
        // reuses the ONE PT arm (asmtest_hwtrace_pt_begin_window) — never reimplements it. Sets up
        // the JIT map + rundown BEFORE arming (the map must see methods JIT'd inside the window;
        // the rundown's socket I/O must not run under capture), pre-allocates the retained
        // whole-window trace, arms the native pair, and on OK wraps the ctx in the finalizable
        // PtWindowCtx and pins the OS thread (the perf event is per-thread). Assumes
        // _kind == Kind.PtWindow and the caller already passed HwTrace.Available(IntelPt).
        void ArmPtWindow(bool byMethod, bool withRundown)
        {
            if (byMethod || withRundown) _map = new JitMethodMap(trackBytes: true);
            _rundownRequested = withRundown;
            if (withRundown) RundownEnabled = DiagnosticsIpc.EnablePerfMap();
            // Whole-window captures the runtime too — size the retained trace to the
            // single-step window ring (same as ArmWholeWindow); committed only as it fills.
            _handle = HwNative.asmtest_trace_new((UIntPtr)(1 << 20), (UIntPtr)0);
            if (_handle == IntPtr.Zero)
            {
                SkipReason = "Intel PT window: trace allocation failed";
                return;
            }
            int prc = HwNative.asmtest_hwtrace_pt_begin_window(out IntPtr pctx);
            if (prc != HwNative.ASMTEST_HW_OK || pctx == IntPtr.Zero)
            {
                SkipReason = $"Intel PT window did not arm (rc={prc})";
                return; // Dispose tears down _map + DisablePerfMap + frees _handle
            }
            _ptWinCtx = new PtWindowCtx(pctx);
            Thread.BeginThreadAffinity();
            Armed = true;
        }

        // T7 (managed-wholewindow-compose): the §D3 out-of-process inline-window arm, factored
        // out of the `AsmTrace(bool outOfProcess)` ctor VERBATIM so the empty-ctor safe-managed
        // OOP route reuses it (channel alloc, EnumerateManagedCodeRanges seeding,
        // stealth_window_begin, thread affinity, teardown on failure). Assumes
        // _kind == Kind.OopInlineWindow and the caller already passed the LibAvailable +
        // Ptrace.Available guards. Sets Armed + _oopWinCtx on success, or SkipReason + teardown.
        void ArmOopWindow(bool byMethod, bool withRundown)
        {
            if (byMethod || withRundown) _map = new JitMethodMap(trackBytes: true);
            if (withRundown) RundownEnabled = DiagnosticsIpc.EnablePerfMap();
            _rundownRequested = withRundown;

            _oopWinChan = HwNative.asmtest_addr_channel_new_shared();
            if (_oopWinChan == IntPtr.Zero)
            {
                SkipReason = "out-of-process window: shared channel alloc failed";
                if (_map != null) { _map.Stop(); _map.Dispose(); }
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                return;
            }
            foreach (AddrRec r in EnumerateManagedCodeRanges())
                HwNative.asmtest_addr_channel_publish_rec(_oopWinChan, r.Base, r.Len, r.Version);
            // NOTE (managed-wholewindow-compose T3 finding): the §E3 live mid-window JIT publish
            // that AsmTrace.Window wires is intentionally NOT armed here. This inline ctor's
            // region-free window_stop stepper single-steps EVERY instruction the arming thread
            // runs, so a first-call JIT INSIDE the window is stepped through the whole compiler —
            // which aborts CoreCLR (measured exit 134). The live-publish path only pays off on the
            // RANGE-based AsmTrace.Window stepper (where the JIT runs at native speed and only the
            // published region is stepped), so the unwarmed mid-window-JIT compose is proven there
            // (HwTraceProgram.WindowLiveJitChecks), not through this inline ctor — which is for a
            // body whose code is already RESIDENT (warm), matching the crashproof-showdown example.

            Thread.BeginThreadAffinity();
            int rc = HwNative.asmtest_hwtrace_stealth_window_begin(_oopWinChan, out IntPtr ctx);
            if (rc == HwNative.ASMTEST_HW_OK)
            {
                _oopWinCtx = new OopWindowCtx(ctx);
                Armed = true;
            }
            else
            {
                Thread.EndThreadAffinity();
                SkipReason = $"stealth_window_begin failed: rc={rc}";
                HwNative.asmtest_addr_channel_free_shared(_oopWinChan);
                _oopWinChan = IntPtr.Zero;
                if (_map != null) { _map.Stop(); _map.Dispose(); }
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
            }
        }

        public AsmTrace(NativeCode code, bool emit = true,
                        [CallerMemberName] string member = null,
                        [CallerLineNumber] int line = 0)
        {
            _name = ScopeName(member, line);
            _emit = emit;
            _armTid = Environment.CurrentManagedThreadId;
            // Same "never throws" self-skip as the whole-window ctor (see above).
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            _handle = HwNative.asmtest_trace_new((UIntPtr)256, (UIntPtr)64);
            if (_handle != IntPtr.Zero)
                HwNative.asmtest_hwtrace_register_region(_name, code.Base, (UIntPtr)code.Length, _handle);
            // Register-then-begin under the same generated name (Core §0.4 idempotent),
            // via the §1 HANDLE-KEYED begin: each thread entering this same auto-named
            // site gets its own range-stack frame, so concurrent same-site scopes no
            // longer alias; Dispose renders this scope's own slice by handle.
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0, ArmTid = -1 };
            int brc = HwNative.asmtest_hwtrace_begin_scope(_name, ref _scope);
            Armed = _began = brc == HwNative.ASMTEST_HW_OK;
            // §Z5: the region form has no auto-init retry (only the whole-window ctor
            // does), so say why the arm failed rather than leaving SkipReason empty.
            if (!Armed)
                SkipReason = brc == HwNative.ASMTEST_HW_ESTATE
                    ? "hwtrace tier not up — call HwTrace.Init (the region scope does not auto-init)"
                    : $"region scope did not arm (rc={brc})";
        }

        // §3.1(c) named-region attribution (opt-in via the whole-window ctor's `regions`).
        // Marshals AsmNamedRegion[] into the raw asmtest_hwtrace_named_region_t[] byte
        // backing (80 B each: char[64] name, u64 base, u64 len) and calls attribute_window
        // with the LIVE scope handle BY VALUE. Fills Buckets; on any non-OK status (a stale
        // handle, a REGION-scope handle whose insns are relative offsets, or a host without
        // the whole-window path) Buckets stays EMPTY rather than partial — the tier's
        // conservative-miss default, never a silent wrong answer.
        void AttributeNamedRegions()
        {
            if (_regions == null || _regions.Length == 0) return;
            if (!Armed || _handle == IntPtr.Zero) return;
            int n = _regions.Length;
            var regs = new byte[n * HwNative.NamedRegionSize];
            for (int i = 0; i < n; i++)
            {
                int off = i * HwNative.NamedRegionSize;
                // name[64], NUL-padded: truncate to 63 bytes so the terminator always fits.
                var nb = System.Text.Encoding.UTF8.GetBytes(_regions[i].Name ?? "");
                Array.Copy(nb, 0, regs, off, Math.Min(nb.Length, HwNative.NamedRegionNameLen - 1));
                BitConverter.TryWriteBytes(
                    new Span<byte>(regs, off + HwNative.NamedRegionNameLen, 8), _regions[i].Base);
                BitConverter.TryWriteBytes(
                    new Span<byte>(regs, off + HwNative.NamedRegionNameLen + 8, 8), _regions[i].Len);
            }
            int cap = n + 32; // the named regions plus room for the runtime remainder's labels
            var raw = new byte[cap * HwNative.BucketSize];
            int rc = HwNative.asmtest_hwtrace_attribute_window(
                _scope, regs, (UIntPtr)(nuint)n, raw, (UIntPtr)(nuint)cap, out var nbOut);
            if (rc != HwNative.ASMTEST_HW_OK) return;
            int nb2 = (int)(nuint)nbOut;
            if (nb2 > cap) nb2 = cap; // surplus labels were dropped C-side; never over-read
            var outb = new HwBucket[nb2];
            for (int i = 0; i < nb2; i++)
            {
                int off = i * HwNative.BucketSize;
                int z = Array.IndexOf(raw, (byte)0, off, HwNative.BucketLabelLen);
                int len = (z < 0 ? off + HwNative.BucketLabelLen : z) - off;
                outb[i] = new HwBucket(
                    System.Text.Encoding.UTF8.GetString(raw, off, len),
                    BitConverter.ToUInt64(raw, off + HwNative.BucketLabelLen));
            }
            Buckets = outb;
        }

        // §D0.1/§D0.2 attribution, shared by the in-process Dispose and the §D3
        // out-of-process window: fold the jitdump rundown (opt-in settle first), then resolve
        // each captured ABSOLUTE address to a managed method, building Methods /
        // LabelledInstructions / Disassembly. DATA only. `img`/`when` pin the code-image
        // version for versioned-first disassembly (both IntPtr.Zero/0 = live-memory decode).
        void AttributeAddresses(IntPtr img, ulong when)
        {
            if (_map == null) return;
            if (_rundownRequested)
            {
                string dump = DiagnosticsIpc.JitDumpPath();
                if (_rundownSettleMs > 0)
                    DiagnosticsIpc.WaitJitDumpSettled(dump, _rundownSettleMs);
                _map.LoadJitDump(dump);
            }
            _map.Freeze();
            // A STATISTICAL scope (WindowHot) has no ordered execution stream — its addresses
            // are non-consecutive SAMPLED branch-target endpoints. Building Disassembly there
            // would fabricate an ordered listing and, worse, an AsmInstruction.RuntimeBefore
            // ("elided native-runtime gap") from the count of unresolved SAMPLES — a number
            // with no executed-instruction meaning. So skip the ordered stream for statistical
            // scopes: Disassembly stays empty; Methods (sample weights) + LabelledInstructions
            // (resolved-sample count) are still built.
            bool disasOk = HwNative.asmtest_disas_available() && !IsStatistical;
            DisassemblyAvailable = disasOk;
            byte[] dbuf = disasOk ? new byte[128] : null;
            var by = new Dictionary<string, long>();
            var insns = new List<AsmInstruction>();
            long labelled = 0;
            int runtimeRun = 0; // native-runtime insns since the last labelled one
            foreach (ulong ip in Addresses)
            {
                string m = _map.Resolve(ip);
                if (m == null) { runtimeRun++; continue; }
                labelled++;
                by.TryGetValue(m, out long c);
                by[m] = c + 1;
                if (disasOk)
                    insns.Add(new AsmInstruction(ip, DisasAt(img, when, ip, dbuf), m, runtimeRun));
                runtimeRun = 0;
            }
            LabelledInstructions = labelled;
            Disassembly = insns;
            var list = new List<AsmMethod>(by.Count);
            foreach (var kv in by) list.Add(new AsmMethod(kv.Key, kv.Value));
            list.Sort((x, y) => y.Count.CompareTo(x.Count));
            Methods = list;
            _map.Dispose();
        }

        /// <summary>
        /// §D3 — the OUT-OF-PROCESS whole-window scope: trace a whole block of managed C# the
        /// way the in-process <see cref="AsmTrace()"/> whole-window form does, but CRASH-PROOF.
        /// A reverse-attached helper child single-steps THIS thread out of band, so it is never
        /// armed with EFLAGS.TF and survives code the in-process form is forbidden to step (a
        /// thrown/caught exception, <c>pthread_create</c>-adjacent tiering). The block is a
        /// delegate whose call frame delimits the window:
        /// <c>var ww = AsmTrace.Window(() =&gt; { …block… }); Report.Print(ww);</c>. Managed code
        /// the block reaches (its own JIT'd body + R2R BCL) is captured via coarse code ranges
        /// published to the stepper, PLUS — §E3 — methods JIT'd fresh MID-WINDOW (a first-call
        /// generic instantiation, a local function), published live from a sibling thread the
        /// stepper drains as the window runs (<see cref="LiveJitPublished"/>); everything is
        /// named at close through the same §D0.1/§D0.2
        /// attribution as the in-process form (see <see cref="Methods"/> / <see cref="Addresses"/>).
        /// Self-skips (runs the block uninstrumented, records <see cref="SkipReason"/>) where
        /// ptrace is denied (Yama). Returns an already-closed scope — do not wrap in <c>using</c>
        /// (the block already ran); read its properties directly. Linux x86-64/AArch64.
        /// </summary>
        public static AsmTrace Window(Action body, bool byMethod = true, bool withRundown = true,
                                      int rundownSettleMs = 300,
                                      [CallerMemberName] string member = null,
                                      [CallerLineNumber] int line = 0)
        {
            var ww = new AsmTrace(ScopeName(member, line), byMethod, withRundown, rundownSettleMs);
            ww.RunWindowOutOfProcess(body ?? (() => { }));
            return ww;
        }

        /// <summary>
        /// §E1 — the HYBRID whole-window: compose the two crash-proof forms so the exact
        /// per-instruction stepper is spent ONLY on the hot managed slice. Pass 1 runs the cheap
        /// statistical <see cref="WindowHot"/> AMD-LBR survey to find the hot methods; pass 2 runs
        /// the exact out-of-process <see cref="Window"/> but publishes ONLY the hot methods'
        /// <c>[base,len)</c> regions to the stepper — so the cold million instructions are
        /// step-overs (near-native) while the hot slice is captured exactly. No new native stepper
        /// code: it is <see cref="WindowHot"/> + <see cref="Window"/> composed. The smallest
        /// DESCENDING-weight method prefix whose running sample weight reaches
        /// <paramref name="hotFraction"/> of the total is the hot set; each is resolved to its
        /// pass-2 JIT'd region and published.
        /// <para><b>DEGRADES, never throws.</b> If AMD LBR is unavailable (the survey self-skips /
        /// <see cref="Armed"/> false / empty <see cref="Methods"/>) or no hot region resolves in
        /// pass 2, this falls back to a plain <see cref="Window"/> — publishing EVERY managed range
        /// (exact everywhere). If ptrace is unavailable, pass 2 self-skips like <see cref="Window"/>.
        /// The pass-1 survey is exposed on <see cref="Survey"/>.</para>
        /// <para><b>The body runs TWICE</b> (survey + exact), so it must be deterministic enough
        /// that pass-1's hot set still applies in pass 2 (a non-idempotent or wildly
        /// input-dependent body should use <see cref="WindowHot"/> for a one-pass survey or
        /// <see cref="Window"/> for a one-pass exact trace instead). Returns an already-closed
        /// scope — read its properties directly. Linux x86-64.</para>
        /// </summary>
        /// <param name="hotFraction">Fraction of the total sampled weight the hot set must reach
        /// (clamped to <c>[0,1]</c>); the hottest method is always kept, so a positive survey
        /// never yields an empty hot set. 0.9 captures the dominant hot slice while eliding a long
        /// cold tail.</param>
        /// <param name="byMethod">Label the exact pass by managed method (as <see cref="Window"/>).
        /// Also gates the JIT map the hot-region resolution needs; with both this and
        /// <paramref name="withRundown"/> off the scope cannot resolve the hot slice and degrades
        /// to a full exact <see cref="Window"/>.</param>
        /// <param name="withRundown">Resolve WARM/R2R methods via the perf-map rundown (as
        /// <see cref="Window"/>). Recommended on: the body's hot methods are usually already JIT'd
        /// by the survey, so the pass-2 in-proc listener alone cannot see them — the rundown
        /// jitdump is what makes them resolvable for the hot-region publish.</param>
        public static AsmTrace WindowHybrid(Action body, double hotFraction = 0.9, bool byMethod = true,
                                            bool withRundown = true, int rundownSettleMs = 300,
                                            [CallerMemberName] string member = null,
                                            [CallerLineNumber] int line = 0)
        {
            Action run = body ?? (() => { });
            // Pass 1 — the cheap, crash-proof statistical survey (the hot-method histogram).
            AsmTrace survey = WindowHot(run, 16, withRundown, rundownSettleMs, member: member, line: line);

            // Pass 2 — the exact out-of-process window (same private ctor as Window).
            var ww = new AsmTrace(ScopeName(member, line), byMethod, withRundown, rundownSettleMs);
            ww._survey = survey;

            // Pick the capture set: the hot slice when the survey drove it, else — the degrade —
            // NULL (RunWindowOutOfProcess then publishes every managed range: a full exact Window).
            // Never throws: any resolution failure falls through to the all-managed default.
            IEnumerable<AddrRec> regions = null;
            try
            {
                if (survey.Armed && survey.Methods.Count > 0 && ww._map != null)
                {
                    List<AsmMethod> hot = HotPrefix(survey.Methods, hotFraction);
                    List<AddrRec> hotRegions = ww.ResolveHotRegions(run, hot);
                    if (hotRegions != null && hotRegions.Count > 0)
                        regions = hotRegions;
                }
            }
            catch { regions = null; } // degrade to a full exact Window on any failure

            ww.RunWindowOutOfProcess(run, regions);
            return ww;
        }

        /// <summary>§E1: the smallest DESCENDING-weight method prefix whose running weight reaches
        /// <paramref name="hotFraction"/> of the total sampled weight — the "hot set". Always keeps
        /// at least the single hottest method when any weight exists (the hottest is added before
        /// the threshold test), so a positive survey never yields an empty hot set. Pure/static so
        /// the prefix rule is testable without a live AMD survey. <paramref name="methodsByWeightDesc"/>
        /// MUST be descending by <see cref="AsmMethod.Weight"/> (as <see cref="Methods"/> is).</summary>
        internal static List<AsmMethod> HotPrefix(IReadOnlyList<AsmMethod> methodsByWeightDesc, double hotFraction)
        {
            var hot = new List<AsmMethod>();
            if (methodsByWeightDesc == null || methodsByWeightDesc.Count == 0) return hot;
            long total = 0;
            foreach (AsmMethod m in methodsByWeightDesc) total += m.Weight;
            if (total <= 0) { hot.AddRange(methodsByWeightDesc); return hot; } // no weight: all hot
            double frac = hotFraction < 0 ? 0 : (hotFraction > 1 ? 1 : hotFraction);
            double target = frac * total;
            long run = 0;
            foreach (AsmMethod m in methodsByWeightDesc)
            {
                hot.Add(m);           // add BEFORE the test: the hottest is always kept
                run += m.Weight;
                if (run >= target) break;
            }
            return hot;
        }

        // §E1: resolve the hot-set method NAMES (from the pass-1 survey) to their pass-2 JIT'd
        // [base,len) regions, via THIS scope's JitMethodMap. The body's hot methods are usually
        // already JIT'd (the survey ran them), so the pass-2 in-proc listener alone cannot see
        // them — fold in the perf-map rundown (already requested by the ctor) so warm/R2R methods
        // resolve too, then match each hot name against the map. Best-effort: returns whatever
        // resolved (possibly empty → the caller degrades to a full exact Window). Never throws.
        List<AddrRec> ResolveHotRegions(Action body, IReadOnlyList<AsmMethod> hot)
        {
            var regions = new List<AddrRec>();
            if (_map == null || hot == null || hot.Count == 0) return regions;
            // Force the body's own code to exist (usually a no-op — the survey JIT'd it already),
            // so the listener/rundown can name it; mirrors RunWindowOutOfProcess's pre-JIT.
            try { RuntimeHelpers.PrepareDelegate(body); } catch { }
            // Fold in the rundown jitdump so ALREADY-JIT'd hot methods (the common case) resolve —
            // the in-proc listener only sees methods JIT'd after it started, which the survey's are
            // not. Additive + address-deduped, so the close-time AttributeAddresses reload is fine.
            if (_rundownRequested)
            {
                string dump = DiagnosticsIpc.JitDumpPath();
                if (_rundownSettleMs > 0) DiagnosticsIpc.WaitJitDumpSettled(dump, _rundownSettleMs);
                _map.LoadJitDump(dump);
            }
            var seen = new HashSet<ulong>();
            foreach (AsmMethod m in hot)
            {
                if (TryResolveHotMethod(_map, m, out ulong start, out ulong size)
                    && start != 0 && size != 0 && seen.Add(start))
                    regions.Add(new AddrRec(start, size));
                if (regions.Count >= 250) break; // the shared channel holds 256
            }
            return regions;
        }

        // §E1: resolve ONE hot method to a [base,len) via the pass-2 map, trying progressively
        // looser keys — most specific first — so a body method the survey spelled one way (in-proc
        // listener "Namespace.Method") still matches the pass-2 jitdump spelling
        // ("[asm] Type::Method(sig)[tier]"). The bare-method-token fallback bridges the "::"/"."
        // spelling gap; kept last (least specific) to prefer an exact match.
        static bool TryResolveHotMethod(JitMethodMap map, AsmMethod m, out ulong start, out ulong size)
        {
            start = 0; size = 0;
            if (!string.IsNullOrEmpty(m.Name) && map.TryResolveEntry(m.Name, out start, out size)) return true;
            string sn = m.ShortName; // "Type::Method(sig)" (jitdump) or "Namespace.Method" (listener)
            if (!string.IsNullOrEmpty(sn) && map.TryResolveEntry(sn, out start, out size)) return true;
            int paren = sn.IndexOf('(');
            string noSig = paren > 0 ? sn.Substring(0, paren) : sn; // drop the signature
            if (noSig.Length > 0 && noSig != sn && map.TryResolveEntry(noSig, out start, out size)) return true;
            // Bare method token after the last "::" or "." — matches across the "::"/"." gap.
            string token = noSig;
            int cc = token.LastIndexOf("::", System.StringComparison.Ordinal);
            if (cc >= 0) token = token.Substring(cc + 2);
            else { int dot = token.LastIndexOf('.'); if (dot >= 0) token = token.Substring(dot + 1); }
            if (token.Length >= 3 && token != noSig && map.TryResolveEntry(token, out start, out size)) return true;
            return false;
        }

        /// <summary>
        /// §D3 — the INLINE-`using` AMD-LBR statistical whole-window: the same crash-proof,
        /// out-of-band, near-native sampled survey as <see cref="WindowHot"/>, but as a bare
        /// scope — <c>using (var ww = new AsmTrace(HwBackend.AmdLbr)) { …managed block… }</c>.
        /// The ctor ARMS the branch-stack sampler; the block runs INLINE on the calling thread
        /// at native speed; <see cref="Dispose"/> DRAINS + attributes. No delegate, no
        /// marshaled callback. <see cref="IsStatistical"/> is true (see it for how
        /// <see cref="Methods"/>/<see cref="Addresses"/> change meaning). Self-skips
        /// (<see cref="Armed"/> false, <see cref="SkipReason"/> set) off Zen 3+/LBR or without
        /// <c>CAP_PERFMON</c>. R3: <see cref="HwBackend.SingleStep"/> is ALSO accepted here —
        /// it forwards to the same in-process single-step whole-window arming as the empty-ctor
        /// <c>new AsmTrace()</c> form (exact, not statistical); <see cref="HwBackend.IntelPt"/>
        /// ARMS the STRONG whole-window Intel PT capture (exact, <see cref="IsStatistical"/>
        /// false; silicon-gated — self-skips off bare-metal Intel PT); only
        /// <see cref="HwBackend.CoreSight"/> stays forward-look and self-skips. NOTE: the AMD
        /// sampled perf event is per-OS-thread, so the block
        /// must run SYNCHRONOUSLY on the calling thread (an <c>await</c>/thread hop would sample
        /// the wrong thread). MUST be disposed (a leaked scope's event is released by a
        /// finalizer). Linux x86-64 only.
        /// </summary>
        /// <param name="period">Branch-retired sample period (clamped &gt;=2); ~16 surveys well.</param>
        public AsmTrace(HwBackend backend, int period = 16, bool byMethod = true,
                        bool withRundown = true, int rundownSettleMs = 300,
                        [CallerMemberName] string member = null,
                        [CallerLineNumber] int line = 0)
        {
            _name = ScopeName(member, line);
            _emit = false;
            // R3: single-step forwards to the shared whole-window arming below, so this
            // backend-keyed ctor now covers SingleStep too — set the Dispose kind accordingly
            // (WholeWindow for the single-step forward, PtWindow for the Intel PT STRONG arm,
            // AmdWindow for the AMD-LBR survey).
            _kind = backend == HwBackend.SingleStep ? Kind.WholeWindow
                : backend == HwBackend.IntelPt ? Kind.PtWindow
                : Kind.AmdWindow;
            _rundownSettleMs = rundownSettleMs;
            _armTid = Environment.CurrentManagedThreadId;
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            if (backend == HwBackend.SingleStep)
            {
                // R3: single-step whole-window — forward to the SAME arming the empty
                // `new AsmTrace()` ctor uses (was: a self-skip pointing callers at the empty
                // ctor). Behavior matches `new AsmTrace(byMethod:, withRundown:)`, and
                // _kind == Kind.WholeWindow routes Dispose to the whole-window teardown.
                ArmWholeWindow(byMethod, withRundown);
                return;
            }
            if (backend == HwBackend.IntelPt)
            {
                // §Z1.2: the STRONG whole-window Intel PT inline capture. Exact (not
                // statistical). Gate on availability (self-skip NAMING the PT gate off
                // bare-metal Intel PT), then arm via the shared ArmPtWindow helper (T7:
                // reused by the empty-ctor safe-managed PT route so there is ONE PT arm).
                if (!HwTrace.Available(HwBackend.IntelPt))
                {
                    SkipReason = "Intel PT unavailable: " + HwTrace.SkipReason(HwBackend.IntelPt);
                    return; // Dispose's PtWindow branch tears down _map + DisablePerfMap
                }
                ArmPtWindow(byMethod, withRundown);
                return;
            }
            if (backend != HwBackend.AmdLbr)
            {
                SkipReason = $"{backend} inline whole-window is forward-look (not wired) — use "
                    + "`new AsmTrace(HwBackend.SingleStep)` / `new AsmTrace()` (single-step) or "
                    + "AsmTrace.Window (out-of-process)";
                return;
            }
            IsStatistical = true;
            if (!HwTrace.Available(HwBackend.AmdLbr))
            {
                SkipReason = "AMD LBR unavailable: " + HwTrace.SkipReason(HwBackend.AmdLbr);
                return;
            }
            // §D0.1/§D0.2: method map + rundown BEFORE arming (same as the empty ctor).
            if (byMethod || withRundown) _map = new JitMethodMap(trackBytes: true);
            _rundownRequested = withRundown;
            if (withRundown) RundownEnabled = DiagnosticsIpc.EnablePerfMap();
            // Allocate the endpoint buffer BEFORE arming, so its 512 KB allocation (+ any GC) is
            // not itself sampled into the survey as unattributed allocator weight.
            _amdIps = new ulong[65536];
            // Arm the branch-stack sampler on the calling thread; the block runs inline next.
            int rc = HwNative.asmtest_hwtrace_sample_begin_amd(period, out IntPtr ctx);
            if (rc != HwNative.ASMTEST_HW_OK || ctx == IntPtr.Zero)
            {
                SkipReason = $"AMD LBR survey did not arm (rc={rc})";
                return; // Dispose's AMD branch tears down _map + DisablePerfMap
            }
            _amdSampler = new AmdSampler(ctx);
            // Pin the OS thread for the window's life — the perf event is per-thread; a
            // migration would sample the wrong thread. Paired with EndThreadAffinity in Dispose
            // (which MUST run on this thread — the documented must-dispose contract).
            Thread.BeginThreadAffinity();
            Armed = true;
        }

        /// <summary>
        /// §D3 — the out-of-process inline-`using` whole-window: the same crash-proof,
        /// out-of-band single-step survey as <see cref="Window"/>, but as a bare
        /// scope — <c>using (var ww = new AsmTrace(outOfProcess: true)) { …managed block… }</c>.
        /// The ctor forks the helper process and arms tracing; the block runs inline on the calling thread
        /// single-stepped out of band; <see cref="Dispose"/> joins the helper and attributes.
        /// Self-skips (<see cref="Armed"/> false, <see cref="SkipReason"/> set) where ptrace is refused (Yama).
        /// </summary>
        public AsmTrace(bool outOfProcess, bool byMethod = true, bool withRundown = true,
                        int rundownSettleMs = 300,
                        [CallerMemberName] string member = null,
                        [CallerLineNumber] int line = 0)
        {
            _name = ScopeName(member, line);
            _emit = false;
            _kind = Kind.OopInlineWindow;
            _rundownSettleMs = rundownSettleMs;
            _armTid = Environment.CurrentManagedThreadId;
            if (outOfProcess == false)
            {
                SkipReason = "outOfProcess must be true; for in-process single-step whole-window use the empty constructor `new AsmTrace()`";
                return;
            }
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            if (!Ptrace.Available())
            {
                SkipReason = "out-of-process stepper unavailable: " + Ptrace.SkipReason();
                return;
            }
            // T7: the arm body is now the shared ArmOopWindow helper (reused by the empty-ctor
            // safe-managed OOP route). _kind == Kind.OopInlineWindow is already set above.
            ArmOopWindow(byMethod, withRundown);
        }

        /// <summary>
        /// §D3 — the STATISTICAL AMD-LBR whole-window survey. Runs <paramref name="body"/> at
        /// NATIVE speed while AMD LBR SAMPLES the branch stack out of band, then buckets the
        /// sampled branch-target endpoints by managed method into a HOT-METHOD histogram.
        /// Crash-proof and cheap: no <c>EFLAGS.TF</c>, no SIGTRAP (survives code the in-process
        /// whole-window form is forbidden to step), a handful of PMIs (near-native), unlike the
        /// exact-but-slow out-of-process <see cref="Window"/>. This is a SAMPLED survey, NOT an
        /// exact trace — <see cref="IsStatistical"/> is true, <see cref="Methods"/>.Count is a
        /// sample-weighted hot weight (endpoint hits, not instructions), and
        /// <see cref="Addresses"/> are sampled branch-target PCs (not an ordered stream). Exact
        /// whole-window is a hardware dead end on AMD, so this is the honest AMD whole-window
        /// shape (the AutoFDO/BOLT model). Self-skips (runs <paramref name="body"/>
        /// uninstrumented, records <see cref="SkipReason"/>) off Zen 3+/LBR or without
        /// <c>CAP_PERFMON</c> / lowered <c>perf_event_paranoid</c>. Returns an already-closed
        /// scope. Linux x86-64 only.
        /// </summary>
        /// <param name="period">Branch-retired sample period (clamped &gt;=2). Larger = fewer
        /// PMIs / less throttling but coarser; ~16 is a good survey default.</param>
        /// <param name="blockWeighted">§E5: weight each sampled straight-line BLOCK
        /// <c>[to_i, from_{i+1}]</c> (a branch target to the next branch's source) by its length
        /// — the AutoFDO/BOLT MCF block-frequency model — instead of counting one endpoint per
        /// branch, so a method's <see cref="Methods"/> weight tracks the instructions it retired
        /// and branchy code is not over-weighted vs. hot straight-line code. Additive fidelity
        /// behind the same survey surface; degrades to the plain endpoint model on the Zen-2
        /// IBS-Op fallback. Default keeps the E2 one-endpoint-per-branch weighting.</param>
        /// <param name="tileCheckpoints">§E6: up to 4 absolute code addresses (managed method
        /// entries — see <see cref="Checkpoints"/>) to TILE the deterministic branch snapshot
        /// at. Each hit freezes the ~16-deep LBR and merges that exact ISLAND into
        /// <see cref="Addresses"/>, so a routine too small or too rare for the sampler to catch
        /// still gets covered — the gap a one-shot method falls into at any realistic
        /// <paramref name="period"/>. An ADDITIVE producer into the same surface, not a mode:
        /// the sampler keeps running, one pass of <paramref name="body"/> feeds both, and
        /// <see cref="IsStatistical"/> stays true because the result is a better-covered
        /// SURVEY, never an exact trace. The coverage it adds is exact AT the checkpoints and
        /// blind between them. Needs <c>CAP_BPF</c> on top of the survey's requirements; where
        /// tiling cannot arm (or a checkpoint never runs) the scope degrades EXACTLY to the
        /// plain survey and reports <see cref="TiledIslands"/> == 0 rather than failing. More
        /// than 4 checkpoints throws — the limit is the 4 x86 debug registers, hardware, not
        /// policy. Null/empty = no tiling (byte-identical to the pre-E6 survey).</param>
        public static AsmTrace WindowHot(Action body, int period = 16, bool withRundown = true,
                                         int rundownSettleMs = 300, bool blockWeighted = false,
                                         ulong[] tileCheckpoints = null,
                                         [CallerMemberName] string member = null,
                                         [CallerLineNumber] int line = 0)
        {
            if (tileCheckpoints != null && tileCheckpoints.Length > MaxTileCheckpoints)
                throw new ArgumentException(
                    $"at most {MaxTileCheckpoints} tile checkpoints (one x86 debug register each), got "
                    + tileCheckpoints.Length, nameof(tileCheckpoints));
            // Refuse rather than silently drop one of them: E5 weights the span between
            // adjacent sampled branches, but islands are disjoint 16-branch windows, so the
            // gap across an island boundary is not a straight-line run and weighting it would
            // invent frequency the hardware never reported.
            if (blockWeighted && tileCheckpoints != null && tileCheckpoints.Length > 0)
                throw new ArgumentException(
                    "blockWeighted (E5 block-frequency weighting) and tileCheckpoints (E6 island "
                    + "tiling) are not combinable: E5 weights the straight-line span between "
                    + "adjacent sampled branches, which is undefined across a checkpoint island's "
                    + "boundary. Pick one producer model.", nameof(blockWeighted));
            var ww = new AsmTrace(ScopeName(member, line), byMethod: true, withRundown, rundownSettleMs);
            ww.RunWindowHotAmd(body ?? (() => { }), period, blockWeighted, tileCheckpoints);
            return ww;
        }

        /// <summary>
        /// §E6: the number of checkpoints <see cref="WindowHot"/> can tile at once — one x86
        /// debug register each (mirrors the native <c>ASMTEST_AMD_MAX_EXITS</c>). A hardware
        /// ceiling, not a policy one.
        /// </summary>
        public const int MaxTileCheckpoints = 4;

        /// <summary>
        /// §E6: one line per method the last <see cref="Checkpoints(MethodBase[], int)"/> call
        /// could NOT resolve to an unambiguous JIT'd body, and why — an AMBIGUOUS match
        /// (overloads / generic instantiations / a name that is a prefix of another) or no
        /// match at all. Empty when every method resolved.
        ///
        /// Read it. A returned array that is quietly SHORT is indistinguishable from success
        /// with fewer islands, and the alternative — resolving a bare name to whichever body
        /// JIT'd last — is worse: the breakpoint arms cleanly on the WRONG method and reports
        /// a perfect 16-entry island for code you did not ask about.
        /// </summary>
        public static string[] LastCheckpointSkips { get; private set; } = System.Array.Empty<string>();

        /// <summary>
        /// §E6: resolve managed methods to the JIT'd-body entry PCs <see cref="WindowHot"/>'s
        /// <c>tileCheckpoints</c> wants. Each method is JIT-PREPARED
        /// (<see cref="RuntimeHelpers.PrepareMethod"/>) so a body EXISTS, and then resolved
        /// through the RUNDOWN JIT MAP — the same jitdump path <see cref="WindowHybrid"/> uses
        /// to turn a hot method into its <c>[base,len)</c>.
        ///
        /// It does NOT use <c>MethodHandle.GetFunctionPointer()</c>, and that is the whole
        /// point of this helper. That pointer is the method's STABLE ENTRY — a precode stub —
        /// not the JIT'd body. A breakpoint planted there never fires, because the runtime
        /// backpatches call sites to the real body and the stub stops executing. MEASURED, not
        /// assumed: with a GetFunctionPointer checkpoint the snapshot armed cleanly
        /// (<c>tile_begin: armed ncp=1</c>) and reported <c>islands=0</c> — a silent, total
        /// miss that looks exactly like "the method never ran". The jitdump address fires.
        ///
        /// Needs the diagnostics rundown (it enables the perf-map/jitdump briefly and waits
        /// <paramref name="settleMs"/> for it to quiesce). Methods that cannot be prepared or
        /// resolved (abstract, open generics, or a runtime with diagnostics off) are SKIPPED,
        /// not thrown — so a caller gets a best-effort set and an honestly shorter array. Check
        /// <see cref="TiledIslands"/> afterwards to see whether a checkpoint actually landed.
        ///
        /// NB (tiered compilation): a method re-JIT'd at a higher tier MOVES to a new body and
        /// the checkpoint stays on the old one — it simply stops firing, which shows up
        /// honestly as fewer <see cref="TiledIslands"/>, never as wrong addresses. Pin
        /// <c>TieredCompilation=false</c> (as the amd-tile example does), or resolve after the
        /// methods have reached their final tier, when a checkpoint must hold for a long window.
        /// </summary>
        public static ulong[] Checkpoints(MethodBase[] methods, int settleMs = 300)
        {
            if (methods == null || methods.Length == 0) return System.Array.Empty<ulong>();
            // Force each body to EXIST before the map is read — an un-JIT'd method has no
            // jitdump record to resolve, and PrepareMethod is the supported way to demand one.
            foreach (var m in methods)
            {
                if (m == null) continue;
                try { RuntimeHelpers.PrepareMethod(m.MethodHandle); }
                catch { /* abstract / open generic: nothing to plant a checkpoint on */ }
            }
            bool rundown = false;
            var map = new JitMethodMap();
            try
            {
                rundown = DiagnosticsIpc.EnablePerfMap();
                if (!rundown) return System.Array.Empty<ulong>(); // diagnostics off: honest empty
                string dump = DiagnosticsIpc.JitDumpPath();
                if (settleMs > 0) DiagnosticsIpc.WaitJitDumpSettled(dump, settleMs);
                map.LoadJitDump(dump);
                var addrs = new List<ulong>(methods.Length);
                var seen = new HashSet<ulong>();
                var unresolved = new List<string>();
                foreach (var m in methods)
                {
                    if (m == null) continue;
                    if (TryResolveMethodEntry(map, m, out ulong start, out string why)
                        && start != 0)
                    {
                        if (seen.Add(start)) addrs.Add(start);
                    }
                    else unresolved.Add(m.Name + ": " + why);
                }
                // A method we could not resolve UNAMBIGUOUSLY is not a checkpoint we may
                // silently omit — the caller asked for coverage of it, and an array that is
                // quietly short looks exactly like success with fewer islands. Record the
                // reasons so a caller (and the demo) can print them.
                LastCheckpointSkips = unresolved.ToArray();
                return addrs.ToArray();
            }
            catch { return System.Array.Empty<ulong>(); }
            finally
            {
                if (rundown) DiagnosticsIpc.DisablePerfMap();
                map.Stop();
                map.Dispose();
            }
        }

        /// <summary>§E6 convenience: <see cref="Checkpoints(MethodBase[], int)"/> with the
        /// default settle budget, in <c>params</c> shape.</summary>
        public static ulong[] Checkpoints(params MethodBase[] methods) => Checkpoints(methods, 300);

        // §E6: one method -> its JIT'd body entry, against the jitdump's real spelling
        // (observed: "void [amd-tile] Program::ColdLeaf()[MinOptJitted]").
        //
        // This deliberately does NOT reuse TryResolveHotMethod's loosening ladder, and the
        // difference matters. That ladder ends in a BARE-NAME substring match and takes the
        // NEWEST hit — correct there (a wrong guess mislabels one sampled address) and unsafe
        // here for two reasons a hardware breakpoint makes severe:
        //
        //   (a) PREFIX COLLISION. "Program::ColdLeaf" is a substring of
        //       "...Program::ColdLeafExtra()...", and if ColdLeafExtra JIT'd later it WINS.
        //   (b) OVERLOADS / generic instantiations. "Program::Over" matches both Over(int32)
        //       and Over(System.String) — whichever JIT'd last wins.
        //
        // Either way the breakpoint arms cleanly, fires on the WRONG body, and merges a real
        // 16-entry island: TiledIslands==1, TiledAddresses==16 — INDISTINGUISHABLE from
        // success, with the caller's requested coverage silently absent. That is the same
        // class as the precode-stub bug (an address that is real but not the requested
        // method's), and the same trap the DR taint tier records: "methodscan substring must
        // be UNIQUE to the sink method".
        //
        // So: anchor on '(' (which alone kills (a) — "ColdLeafExtra(" does not contain
        // "ColdLeaf("), prefer the full signature (which kills (b)), and REFUSE rather than
        // guess when more than one distinct method still matches.
        static bool TryResolveMethodEntry(JitMethodMap map, MethodBase m, out ulong start,
                                          out string why)
        {
            start = 0; why = null;
            string type = m.DeclaringType != null ? m.DeclaringType.Name : null;
            if (string.IsNullOrEmpty(type)) { why = "method has no declaring type"; return false; }

            // "Type::Method(p1,p2)" — the jitdump's own signature spelling.
            var ps = m.GetParameters();
            var spelled = new string[ps.Length];
            for (int i = 0; i < ps.Length; i++) spelled[i] = SpellType(ps[i].ParameterType);
            string sigKey = type + "::" + m.Name + "(" + string.Join(",", spelled) + ")";
            // "Type::Method(" — the paren-anchored fallback for when our spelling misses the
            // runtime's (an exotic type). Still collision-proof; not overload-proof, so the
            // uniqueness check below is what keeps it honest.
            string anchored = type + "::" + m.Name + "(";

            List<string> names = map.MatchingNames(sigKey);
            string key = sigKey;
            if (names.Count == 0) { names = map.MatchingNames(anchored); key = anchored; }
            if (names.Count == 0) { why = "no JIT'd body matches \"" + anchored + "\""; return false; }

            // Distinct METHODS matching, ignoring the trailing "[tier]": the SAME method
            // re-JIT'd at a higher tier is not an ambiguity — it is one method with two
            // bodies, and the newest is the one that runs. Two DIFFERENT methods is an
            // ambiguity, and there is no honest way to pick.
            var distinct = new HashSet<string>();
            foreach (string n in names) distinct.Add(StripTier(n));
            if (distinct.Count > 1)
            {
                why = "AMBIGUOUS: " + distinct.Count + " different JIT'd methods match \"" + key
                    + "\" (overloads, generic instantiations, or a name that is a prefix of "
                    + "another). Refusing to guess — a checkpoint on the wrong body arms "
                    + "cleanly and reports a perfect island for code you did not ask about.";
                return false;
            }
            ulong size;
            if (!map.TryResolveEntry(key, out start, out size)) { why = "resolve failed"; return false; }
            return true;
        }

        // §E6: the CoreCLR jitdump's type spelling (observed: "int64 [amd-tile]
        // Program::HotWork(int64)[MinOptJitted]"). Primitives use the runtime's short names;
        // everything else falls back to the full name. A miss is SAFE, not silent: the
        // signature key simply finds nothing and resolution falls back to the paren-anchored
        // key, which still refuses an ambiguous match.
        static string SpellType(Type t)
        {
            if (t == typeof(void)) return "void";
            if (t == typeof(bool)) return "bool";
            if (t == typeof(char)) return "char";
            if (t == typeof(sbyte)) return "int8";
            if (t == typeof(byte)) return "uint8";
            if (t == typeof(short)) return "int16";
            if (t == typeof(ushort)) return "uint16";
            if (t == typeof(int)) return "int32";
            if (t == typeof(uint)) return "uint32";
            if (t == typeof(long)) return "int64";
            if (t == typeof(ulong)) return "uint64";
            if (t == typeof(float)) return "float32";
            if (t == typeof(double)) return "float64";
            if (t == typeof(IntPtr)) return "native int";
            if (t == typeof(UIntPtr)) return "native uint";
            // OBSERVED, not assumed: the runtime writes "void [amd-tile] Program::Over(string)",
            // not "System.String" — it uses the ILDASM short names for these two as well.
            if (t == typeof(string)) return "string";
            if (t == typeof(object)) return "object";
            return t.FullName ?? t.Name;
        }

        // §E6: drop the jitdump name's trailing "[tier]" ("...ColdLeaf()[MinOptJitted]" ->
        // "...ColdLeaf()"), so one method's tier bodies collapse to one identity while two
        // different methods stay two.
        static string StripTier(string jitName)
        {
            if (string.IsNullOrEmpty(jitName)) return jitName;
            int close = jitName.LastIndexOf(']');
            if (close != jitName.Length - 1) return jitName;
            int open = jitName.LastIndexOf('[');
            return open > 0 ? jitName.Substring(0, open) : jitName;
        }

        void RunWindowHotAmd(Action body, int period, bool blockWeighted = false,
                             ulong[] tileCheckpoints = null)
        {
            _disposed = true;       // this factory already closed the scope
            IsStatistical = true;   // the result is a sampled survey, always
            if (!HwNative.LibAvailable) { SafeRun(body); return; }
            if (!HwTrace.Available(HwBackend.AmdLbr))
            {
                SkipReason = "AMD LBR unavailable: " + HwTrace.SkipReason(HwBackend.AmdLbr);
                if (_map != null) { _map.Stop(); _map.Dispose(); }
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                SafeRun(body);
                return;
            }
            // The body as a native callback; AMD LBR samples the calling thread while it runs
            // at native speed (no stepping, so the JIT listener catches mid-window JITs normally).
            var cb = new RunRegionFn(_ => { try { body(); } catch { } });
            IntPtr fn = Marshal.GetFunctionPointerForDelegate(cb);
            ulong[] ips = new ulong[65536];
            int rc;
            UIntPtr nips;
            int trunc;
            UIntPtr ntiled = UIntPtr.Zero;
            int islands = 0;
            int tileTrunc = 0;
            bool tiling = tileCheckpoints != null && tileCheckpoints.Length > 0;
            Thread.BeginThreadAffinity(); // the sampled event is on this OS thread
            try
            {
                // §E6 tiling composes with the plain endpoint drain (the tiled producer emits
                // one endpoint per retired branch, matching that model exactly); it is NOT
                // offered with §E5 block weighting, whose weights come from measuring spans
                // BETWEEN adjacent sampled branches. Islands are disjoint 16-branch windows,
                // so the "block" between an island's last entry and the next sample's first is
                // not a real straight-line run — weighting it would invent frequency the
                // hardware never reported. Refusing that combination is the honest choice.
                rc = tiling
                    ? HwNative.asmtest_hwtrace_tile_window_amd(
                        fn, IntPtr.Zero, period, tileCheckpoints, tileCheckpoints.Length,
                        ips, (UIntPtr)ips.Length, out nips, out ntiled, out islands,
                        out tileTrunc, out trunc)
                    : blockWeighted
                        ? HwNative.asmtest_hwtrace_sample_window_amd_weighted(
                            fn, IntPtr.Zero, period, ips, (UIntPtr)ips.Length, out nips, out trunc)
                        : HwNative.asmtest_hwtrace_sample_window_amd(
                            fn, IntPtr.Zero, period, ips, (UIntPtr)ips.Length, out nips, out trunc);
            }
            finally { Thread.EndThreadAffinity(); GC.KeepAlive(cb); }

            if (_map != null) _map.Stop();
            Armed = rc == HwNative.ASMTEST_HW_OK;
            if (!Armed)
            {
                SkipReason = $"AMD LBR survey did not arm (rc={rc})";
                if (_map != null) _map.Dispose();
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                return;
            }
            ulong n = (ulong)nips;
            var addrs = new ulong[n];
            Array.Copy(ips, addrs, (long)n);
            Addresses = addrs;                 // sampled branch-target PCs (not ordered)
            // §E6: the island-sourced prefix. Recorded even when 0 (tiling asked for but not
            // armed / checkpoint never reached) — that IS the honest answer, and the caller
            // reads TiledIslands to tell "tiling added nothing" from "tiling never ran".
            TiledIslands = islands;
            TiledAddresses = (int)(ulong)ntiled;
            TiledTruncated = tileTrunc != 0;
            Truncated = trunc != 0;            // a prefix (dropped/throttled samples)
            // Pin the code-image version AFTER Stop() (mirrors the in-process Dispose order),
            // so a method JIT'd mid-window disassembles against the version that actually ran.
            IntPtr img = _map != null ? _map.ImageHandle : IntPtr.Zero;
            ulong when = _map != null ? _map.ImageNow : 0;
            AttributeAddresses(img, when);     // reuse: endpoint PCs -> methods; Count = weight
            if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
        }

        // Private ctor for the §D3 out-of-process window (AsmTrace.Window). Sets up the method
        // map + rundown BEFORE the window (same as the in-process whole-window ctor), but arms
        // nothing in-process — RunWindowOutOfProcess drives the reverse-attach capture.
        AsmTrace(string name, bool byMethod, bool withRundown, int rundownSettleMs)
        {
            _name = name;
            _emit = false;
            _kind = Kind.OopWindow;
            _rundownSettleMs = rundownSettleMs;
            _armTid = Environment.CurrentManagedThreadId;
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            if (byMethod || withRundown) _map = new JitMethodMap(trackBytes: true);
            _rundownRequested = withRundown;
            if (withRundown) RundownEnabled = DiagnosticsIpc.EnablePerfMap();
        }

        // The window body, invoked from native as a reverse-P/Invoke callback (win_base ==
        // this delegate's entry). Kept alive across the native call via GC.KeepAlive.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        delegate void RunRegionFn(IntPtr arg);

        // §E1: <paramref name="regionsOrNull"/> is the injectable capture set. NULL keeps the
        // plain-<see cref="Window"/> behavior byte-identical — publish EVERY managed code range
        // (EnumerateManagedCodeRanges), exact everywhere. A non-null set (from WindowHybrid)
        // publishes ONLY those hot [base,len) regions, so the stepper captures the hot managed
        // slice exactly and step-overs the cold remainder (in_region_set, ptrace_backend.c).
        void RunWindowOutOfProcess(Action body, IEnumerable<AddrRec> regionsOrNull = null)
        {
            _disposed = true; // this factory already closed the scope; no Dispose teardown
            if (!HwNative.LibAvailable) { SafeRun(body); return; }
            if (!Ptrace.Available())
            {
                SkipReason = "out-of-process stepper unavailable: " + Ptrace.SkipReason();
                if (_map != null) { _map.Stop(); _map.Dispose(); }
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                SafeRun(body);
                return;
            }

            // Pre-JIT the body so its own code exists before the (single-stepped) window —
            // the window then steps the compiled body, not the JIT compiling it.
            try { System.Runtime.CompilerServices.RuntimeHelpers.PrepareDelegate(body); } catch { }

            // §D3 capture channel: a SHARED ring the forked stepper drains. Seed it with the
            // coarse managed code ranges (JIT heap + R2R BCL .dll images) so all currently-
            // mapped managed code is captured. Native runtime (*.so) stays unpublished and is
            // stepped over (the elided noise). Methods JIT'd fresh MID-WINDOW land outside
            // these pre-window ranges — the §E3 sibling publisher armed below catches them.
            IntPtr chan = HwNative.asmtest_addr_channel_new_shared();
            if (chan == IntPtr.Zero)
            {
                SkipReason = "out-of-process window: shared channel alloc failed";
                if (_map != null) { _map.Stop(); _map.Dispose(); }
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                SafeRun(body);
                return;
            }
            foreach (AddrRec r in regionsOrNull ?? EnumerateManagedCodeRanges())
                HwNative.asmtest_addr_channel_publish_rec(chan, r.Base, r.Len, r.Version);
            // §E3 (extensions plan) — the LIVE mid-window publish, ON for the plain full
            // window. Arm the JIT listener's SIBLING publisher thread on the same shared
            // channel: a method JIT'd fresh MID-WINDOW (a first-call generic instantiation,
            // a local function) is published while the window runs, the stepper drains it
            // live and records it — closing the deep-BCL gap that made this scope
            // honest-partial. The publish must NOT happen inline in the EventPipe callback
            // (it can fire on the single-stepped thread, where re-entering the runtime under
            // step SIGABRTs — observed), hence the sibling thread, started HERE, before the
            // window arms, and joined in the finally below before the channel is freed.
            // The §E1 hybrid (regionsOrNull != null) keeps the live publish OFF by design:
            // its contract is to capture ONLY the surveyed hot slice, so publishing every
            // fresh JIT would reintroduce exactly the cold code it elides. Requires the JIT
            // map (byMethod/withRundown — the Window defaults); with neither there is no
            // listener to drain, and the pre-E3 coarse-ranges-only behavior remains.
            if (_map != null && regionsOrNull == null)
                _map.SetPublishChannel(chan);

            IntPtr img = _map != null ? _map.ImageHandle : IntPtr.Zero;
            ulong when = _map != null ? _map.ImageNow : 0;

            var cb = new RunRegionFn(_ => { try { body(); } catch { } });
            IntPtr fn = Marshal.GetFunctionPointerForDelegate(cb);
            var tr = HwTrace.Create(blocks: 4096, instructions: 65536);
            long result = 0;
            int rc;
            Thread.BeginThreadAffinity(); // pin the OS thread the stepper targets for the window
            try
            {
                rc = HwNative.asmtest_hwtrace_stealth_trace_windowed(
                    fn, (UIntPtr)4096, chan, tr.Handle, out result, fn, IntPtr.Zero);
            }
            finally
            {
                Thread.EndThreadAffinity();
                GC.KeepAlive(cb);
                if (_map != null)
                {
                    // §E3: SetPublishChannel(Zero) stops AND JOINS the sibling publisher,
                    // so freeing the shared ring below cannot race an in-flight publish.
                    _map.SetPublishChannel(IntPtr.Zero);
                    LiveJitPublished = _map.LivePublished;
                }
                HwNative.asmtest_addr_channel_free_shared(chan);
            }

            if (_map != null) _map.Stop();
            Armed = rc == HwNative.ASMTEST_HW_OK;
            if (!Armed)
            {
                SkipReason = rc == HwNative.ASMTEST_HW_EUNAVAIL
                    ? "reverse-attach refused (Yama ptrace_scope / no ptrace) — block ran uninstrumented"
                    : $"out-of-process window did not arm (rc={rc})";
                if (_map != null) _map.Dispose();
                if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                tr.Free();
                return;
            }

            // Read the captured ABSOLUTE addresses and attribute them (reuses the seam).
            ulong n = HwNative.asmtest_emu_trace_insns_len(tr.Handle);
            var addrs = new ulong[n];
            for (ulong i = 0; i < n; i++)
                addrs[i] = HwNative.asmtest_emu_trace_insn_at(tr.Handle, (UIntPtr)i);
            Addresses = addrs;
            Truncated = tr.Truncated();
            AttributeAddresses(img, when);
            if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
            tr.Free();
        }

        static void SafeRun(Action a) { try { a(); } catch { } }

        // §D3 coarse code-range enumeration: the managed code segments from /proc/self/maps —
        // executable file-backed *.dll images (R2R/PreJIT BCL) plus anonymous executable
        // mappings (the JIT code heap). A few dozen big (base,len) records that fit the 256-slot
        // channel and admit ALL managed code the window reaches, while leaving native runtime
        // (*.so) unpublished so it is stepped over. Best-effort; returns an empty array on any
        // read error (the window then records only its own frame).
        static AddrRec[] EnumerateManagedCodeRanges()
        {
            var ranges = new List<AddrRec>();
            try
            {
                foreach (string line in File.ReadLines("/proc/self/maps"))
                {
                    int dash = line.IndexOf('-');
                    int sp = line.IndexOf(' ');
                    if (dash <= 0 || sp <= dash) continue;
                    string perms = sp + 5 <= line.Length ? line.Substring(sp + 1, 4) : "";
                    if (perms.Length < 3 || perms[2] != 'x') continue; // executable only
                    // path is the tail after the 5th field. Managed code = a .dll image (R2R
                    // BCL) OR an anonymous / named-anon executable mapping (the JIT code heap,
                    // which Linux .NET may label e.g. [anon:...]). EXCLUDE native runtime — any
                    // *.so and the [vdso]/[vsyscall]/[vvar] specials — so it stays unpublished
                    // and is stepped over (the runtime noise the capture elides).
                    int lastSp = line.LastIndexOf(' ');
                    string path = lastSp >= 0 && lastSp + 1 < line.Length ? line.Substring(lastSp + 1).Trim() : "";
                    bool native = path.Contains(".so") || path.StartsWith("[vdso") ||
                                  path.StartsWith("[vsyscall") || path.StartsWith("[vvar");
                    if (native) continue;
                    ulong start = Convert.ToUInt64(line.Substring(0, dash), 16);
                    ulong end = Convert.ToUInt64(line.Substring(dash + 1, sp - dash - 1), 16);
                    if (end > start) ranges.Add(new AddrRec(start, end - start));
                    if (ranges.Count >= 250) break; // channel holds 256
                }
            }
            catch { }
            return ranges.ToArray();
        }

        /// <summary>
        /// §D0.3 — the NAMED-METHOD form: trace one managed method's own JIT'd body.
        /// <c>using var t = AsmTrace.Method(HotPath); var r = t.Invoke(args…); … t.Path;</c>
        /// Resolves the standalone body via a fresh <see cref="JitMethodMap"/> around
        /// <c>RuntimeHelpers.PrepareMethod</c> (a WARM method the listener cannot see is
        /// found via the §D0.2 jitdump-rundown fallback), registers it as a region, and
        /// arms — auto-initing the single-step tier like the empty ctor (§Z0). Reliable
        /// where the whole-window form is best-effort: region + step-over, exact body
        /// offsets in <see cref="Path"/> on close. Never throws on this path: resolution
        /// or arm failure self-skips (<see cref="Armed"/> false, <see cref="SkipReason"/>
        /// says why). The resolved body is the version live at arm time — a mid-scope
        /// tier-up is not followed (§Z3 forward-look).
        /// </summary>
        /// <param name="target">The method to trace, as a delegate (e.g.
        /// <c>(Func&lt;long,long,long&gt;)HotPath</c>). Delegate invocation is indirect, so
        /// the standalone JIT'd body is what runs — the caller cannot inline it away.</param>
        /// <param name="outOfProcess">§D3: run <see cref="Invoke"/> under the concealed
        /// out-of-process ptrace-stealth stepper instead of in-process single-step — the
        /// calling thread is never armed with EFLAGS.TF (a bundled helper reverse-attaches
        /// and steps the body out of band). Self-skips (plain uninstrumented call, reason
        /// recorded) where the attach is denied (Yama) or the helper is unavailable.</param>
        public static AsmTrace Method(Delegate target, bool emit = true, bool outOfProcess = false,
                                      [CallerMemberName] string member = null,
                                      [CallerLineNumber] int line = 0)
        {
            if (target == null) throw new ArgumentNullException(nameof(target));
            bool found = ResolveJitBody(target, out ulong start, out ulong size);
            return new AsmTrace(target, start, size, found, emit, outOfProcess, member, line);
        }

        /// <summary>
        /// §D0.1/§D0.2 — resolve a managed delegate's own standalone JIT'd body to
        /// (start, size): <c>PrepareMethod</c> forces the JIT, a fresh
        /// <see cref="JitMethodMap"/> (MethodLoadVerbose) reads the address, and a WARM /
        /// ReadyToRun body falls back to the §D0.2 jitdump rundown. Shared by
        /// <see cref="Method"/> and <see cref="AsmStitchedTrace"/>. Returns false
        /// (start=size=0) when the lib is absent or no body resolves.
        /// </summary>
        internal static bool ResolveJitBody(Delegate target, out ulong start, out ulong size)
        {
            start = 0; size = 0;
            if (!HwNative.LibAvailable) return false;
            // Map BEFORE PrepareMethod: an in-proc listener only sees methods JIT'd after
            // it enables the keyword (§D0.1 caveat i).
            using (var map = new JitMethodMap())
            {
                try { RuntimeHelpers.PrepareMethod(target.Method.MethodHandle); }
                catch { /* generic/abstract handles: fall through to the rundown */ }
                string typed = (target.Method.DeclaringType?.Name ?? "") + "." + target.Method.Name;
                bool found = map.TryResolveEntry(typed, out start, out size)
                          || map.TryResolveEntry("." + target.Method.Name, out start, out size);
                if (!found && DiagnosticsIpc.EnablePerfMap())
                {
                    // WARM body (JIT'd before this call): PrepareMethod was a no-op and no
                    // event fired — the jitdump rundown names it ("Type::Method(sig)").
                    map.LoadJitDump(DiagnosticsIpc.JitDumpPath());
                    DiagnosticsIpc.DisablePerfMap();
                    found = map.TryResolveEntry("::" + target.Method.Name + "(", out start, out size)
                         || map.TryResolveEntry(typed, out start, out size);
                }
                return found;
            }
        }

        // §D0.3/§D3 private ctor: region scope over a resolved managed-method body.
        AsmTrace(Delegate target, ulong start, ulong size, bool found, bool emit, bool oop,
                 string member, int line)
        {
            _name = ScopeName(member, line);
            _emit = emit;
            _armTid = Environment.CurrentManagedThreadId;
            _target = target;
            _oop = oop;
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
            if (!found || size == 0)
            {
                SkipReason = $"could not resolve a JIT'd body for {target.Method.Name} "
                           + "(no MethodLoadVerbose event and the jitdump-rundown fallback missed)";
                return;
            }
            _methodBase = new IntPtr(unchecked((long)start));
            _methodLen = (UIntPtr)size;
            // Managed bodies run hot loops: size the instruction buffer well past a
            // native-leaf's (region filtering keeps only in-body offsets, so this is
            // the retained stream, committed as it fills).
            _handle = HwNative.asmtest_trace_new((UIntPtr)(1 << 16), (UIntPtr)256);
            if (_handle == IntPtr.Zero)
            {
                SkipReason = "trace allocation failed";
                return;
            }
            // B (lazy-arm) — managed-singlestep-lazy-arm-plan. Ensure the portable tier is
            // up (register_region needs it) and register the body region, but DO NOT arm
            // here. Both the in-process (call_scoped) and out-of-process (stealth) paths
            // arm lazily inside Invoke, so the armed EFLAGS.TF window spans ONLY the call —
            // never the caller's setup or a managed thread-spawn that would force-kill an
            // in-process TF window. Armed flips true when Invoke actually steps the body.
            int initRc = AutoInitSingleStep();
            if (initRc != HwNative.ASMTEST_HW_OK && !_oop)
            {
                SkipReason = $"single-step tier did not initialise (rc={initRc}) — "
                           + HwTrace.DegradationNote();
                return;
            }
            int reg = HwNative.asmtest_hwtrace_register_region(_name, _methodBase, _methodLen, _handle);
            if (reg != HwNative.ASMTEST_HW_OK && !_oop)
            {
                SkipReason = $"could not register the method-body region (rc={reg})";
                return;
            }
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0, ArmTid = -1 };
        }

        // B (lazy-arm) shim table — the concrete (long…)->long reverse-P/Invoke delegates.
        // A generic Func<> cannot be marshaled to a function pointer, so the arities are
        // spelled out; Cdecl == SysV on Linux x86-64, matching the native ss_dispatch_call.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody0();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody1(long a);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody2(long a, long b);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody3(long a, long b, long c);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody4(long a, long b, long c, long d);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody5(long a, long b, long c, long d, long e);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate long AsmBody6(long a, long b, long c, long d, long e, long f);
        static readonly Type[] ShimTypes = {
            typeof(AsmBody0), typeof(AsmBody1), typeof(AsmBody2), typeof(AsmBody3),
            typeof(AsmBody4), typeof(AsmBody5), typeof(AsmBody6),
        };
        Delegate _shim; // the reverse-P/Invoke shim, kept alive across the native call

        // Build the reverse-P/Invoke shim for _target's signature and marshal the args to
        // long[]. shimmable=false when the arity is > 6, the signature is not (long…)->long
        // (CreateDelegate throws), or an arg is not integer-convertible — the caller then
        // routes OUT-OF-PROCESS rather than stepping DynamicInvoke in a managed window.
        long[] MarshalIntArgs(object[] args, out bool shimmable)
        {
            shimmable = TryBuildIntShim(_target, args, out _shim, out long[] iargs);
            return iargs ?? System.Array.Empty<long>();
        }

        // Shared with AsmStitchedTrace: build an integer (long…)->long reverse-P/Invoke
        // shim + long[] args for `target`, or return false (arity > 6, signature not
        // (long…)->long, or a non-integer-convertible arg). The caller keeps `shim` alive
        // across the native call.
        internal static bool TryBuildIntShim(Delegate target, object[] args, out Delegate shim, out long[] iargs)
        {
            shim = null; iargs = null;
            int n = args?.Length ?? 0;
            if (n > 6) return false;
            try { shim = Delegate.CreateDelegate(ShimTypes[n], target.Target, target.Method); }
            catch { return false; }
            var a = new long[n];
            try { for (int i = 0; i < n; i++) a[i] = ToLong(args[i]); }
            catch { shim = null; return false; }
            iargs = a;
            return true;
        }

        static long ToLong(object o) => o switch
        {
            null => 0L,
            long l => l,
            IntPtr p => (long)p,
            bool b => b ? 1L : 0L,
            _ => Convert.ToInt64(o),
        };

        // B (lazy-arm) FP shim table — the homogeneous (double…)->double delegates, args
        // in xmm0..7 (8 register-resident slots, so 0-8 arities), used when the integer
        // family cannot express the signature. Cdecl == SysV on Linux x86-64.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD0();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD1(double a);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD2(double a, double b);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD3(double a, double b, double c);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD4(double a, double b, double c, double d);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD5(double a, double b, double c, double d, double e);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD6(double a, double b, double c, double d, double e, double f);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD7(double a, double b, double c, double d, double e, double f, double g);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate double AsmBodyD8(double a, double b, double c, double d, double e, double f, double g, double h);
        static readonly Type[] ShimTypesFp = {
            typeof(AsmBodyD0), typeof(AsmBodyD1), typeof(AsmBodyD2), typeof(AsmBodyD3),
            typeof(AsmBodyD4), typeof(AsmBodyD5), typeof(AsmBodyD6), typeof(AsmBodyD7),
            typeof(AsmBodyD8),
        };

        // Build the FP reverse-P/Invoke shim for _target and marshal args to double[].
        // shimmable=false when the arity is > 8 or the signature is not (double…)->double.
        double[] MarshalFpArgs(object[] args, out bool shimmable)
        {
            shimmable = false;
            int n = args?.Length ?? 0;
            if (n > 8) return System.Array.Empty<double>();
            try { _shim = Delegate.CreateDelegate(ShimTypesFp[n], _target.Target, _target.Method); }
            catch { return System.Array.Empty<double>(); }
            var a = new double[n];
            try { for (int i = 0; i < n; i++) a[i] = ToDouble(args[i]); }
            catch { _shim = null; return System.Array.Empty<double>(); }
            shimmable = true;
            return a;
        }

        static double ToDouble(object o) => o switch
        {
            null => 0.0,
            double d => d,
            float f => f,
            _ => Convert.ToDouble(o),
        };

        /// <summary>
        /// §D0.3 / Option B: invoke the named method inside this scope. In-process, this
        /// is the LAZY-ARM path — a native function pointer to the body is minted pre-arm
        /// (a per-arity reverse-P/Invoke shim) and then <c>arm → call → disarm</c> happens
        /// entirely in native code (<c>asmtest_hwtrace_call_scoped</c>), so the runtime's
        /// machinery (and any in-window <c>pthread_create</c>) is NEVER stepped — the
        /// window holds only the body's executed offsets. A signature the shim set cannot
        /// express (ref/out, structs, &gt;6 args), or <c>outOfProcess: true</c>, routes
        /// through the stealth stepper instead; on a refused attach the method still runs
        /// (uninstrumented) and <see cref="SkipReason"/> says why — never a silent miss.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.NoOptimization)]
        public object Invoke(params object[] args)
        {
            if (_target == null)
                throw new HwTraceException("Invoke is only valid on an AsmTrace.Method(...) scope");
            if (_disposed)
                throw new HwTraceException("Invoke after Dispose");
            if (!HwNative.LibAvailable || _handle == IntPtr.Zero)
                return _target.DynamicInvoke(args); // ctor self-skipped: never lose the call

            // B (lazy-arm), in-process. Only for a signature the (long…)->long shim set
            // covers; anything else falls through to the out-of-process path (B3) rather
            // than stepping DynamicInvoke in a managed window (the crash surface).
            if (!_oop)
            {
                long[] iargs = MarshalIntArgs(args, out bool intShim);
                if (intShim)
                {
                    IntPtr fn = Marshal.GetFunctionPointerForDelegate(_shim);
                    int rc = HwNative.asmtest_hwtrace_call_scoped(
                        _name, fn, iargs, iargs.Length, out long r, ref _scope);
                    GC.KeepAlive(_shim);
                    if (rc == HwNative.ASMTEST_HW_OK) { Armed = true; return r; }
                    // B3: in-process arm failed — degrade OUT-OF-PROCESS, loud reason.
                    SkipReason = $"in-process lazy-arm did not run (rc={rc}); falling back out-of-process";
                }
                else
                {
                    // Integer shims don't fit — try the (double…)->double FP family before
                    // giving up to out-of-process.
                    double[] fargs = MarshalFpArgs(args, out bool fpShim);
                    if (fpShim)
                    {
                        IntPtr fn = Marshal.GetFunctionPointerForDelegate(_shim);
                        int rc = HwNative.asmtest_hwtrace_call_scoped_fp(
                            _name, fn, fargs, fargs.Length, out double dr, ref _scope);
                        GC.KeepAlive(_shim);
                        if (rc == HwNative.ASMTEST_HW_OK) { Armed = true; return dr; }
                        SkipReason = $"in-process FP lazy-arm did not run (rc={rc}); falling back out-of-process";
                    }
                    else
                        // B3: signature outside BOTH shim sets — route out-of-process rather
                        // than stepping the managed reflection machinery under EFLAGS.TF.
                        SkipReason = "signature outside the (long…)->long / (double…)->double shim sets; routing out-of-process";
                }
            }

            // Out-of-process (explicit outOfProcess, or the B3 auto-fallback above). The
            // calling thread is never TF-armed; the helper steps the body out of band. On
            // a refused attach the call still runs uninstrumented — never a silent miss.
            object ret = null;
            Exception thrown = null;
            bool ran = false;
            HwNative.StealthRunFn cb = _ =>
            {
                ran = true;
                try { ret = _target.DynamicInvoke(args); }
                catch (Exception e) { thrown = e; }
            };
            int src = HwNative.asmtest_hwtrace_stealth_trace(
                _methodBase, _methodLen, _handle, out long _, cb, IntPtr.Zero);
            GC.KeepAlive(cb);
            if (src == HwNative.ASMTEST_HW_OK) Armed = true;
            else SkipReason = (string.IsNullOrEmpty(SkipReason) ? "" : SkipReason + " — ")
                            + $"stealth stepper did not run (rc={src}) — Yama ptrace_scope, "
                            + "missing asmtest-stealth-helper, or non-x86-64-Linux";
            if (!ran) ret = _target.DynamicInvoke(args); // never lose the call itself
            if (thrown != null) throw thrown;
            return ret;
        }

        static string ScopeName(string member, int line)
        {
            string n = $"{member}:{line}";
            return n.Length > 63 ? n.Substring(n.Length - 63) : n;
        }

        public void Dispose()
        {
            // §D3: an out-of-process window (AsmTrace.Window) is already closed by its factory
            // (nothing armed in-process); a stray using() on it is a no-op.
            if (_kind == Kind.OopWindow || _disposed) return;
            _disposed = true;
            // With no native lib the ctor self-skipped and allocated no native resources; skip
            // the (P/Invoking) teardown so Dispose also honors "never throws".
            if (!HwNative.LibAvailable) { Path = ""; return; }
            // R1: dispatch the close by scope kind. OopWindow already returned above (its
            // factory closed it); the AMD / OOP-inline windows tear down and return, while
            // WholeWindow and Region (the region form + named-method scopes) fall through
            // to the shared trace-free / thread-hop / emit tail below.
            switch (_kind)
            {
                case Kind.AmdWindow:
                {
                    // §D3: the INLINE AMD-LBR statistical window (new AsmTrace(HwBackend.AmdLbr)).
                    // The block just ran inline, sampled out of band — DRAIN the sampler, attribute
                    // the endpoints (reused seam; Count = weight), release. Handle a self-skipped
                    // (unarmed) scope too: no sampler, just tear down the map + rundown.
                    if (_amdSampler != null)
                    {
                        _amdSampler.End(_amdIps, out UIntPtr nips, out int trunc);
                        _amdSampler = null;
                        ulong n = (ulong)nips;
                        var addrs = new ulong[n];
                        if (n > 0) Array.Copy(_amdIps, addrs, (long)n);
                        Addresses = addrs;
                        Truncated = trunc != 0;
                        if (_map != null) _map.Stop();
                        IntPtr aimg = _map != null ? _map.ImageHandle : IntPtr.Zero;
                        ulong awhen = _map != null ? _map.ImageNow : 0;
                        AttributeAddresses(aimg, awhen); // disposes _map
                        Thread.EndThreadAffinity();       // paired with the ctor's pin (armed path)
                    }
                    else if (_map != null) { _map.Stop(); _map.Dispose(); }
                    if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                    return;
                }
                case Kind.PtWindow:
                {
                    // §Z1.2: the INLINE Intel PT STRONG window (new AsmTrace(HwBackend.IntelPt)).
                    // The block just ran inline, captured out of band via the perf AUX intel_pt
                    // ring — DRAIN + decode the native pair against the JIT map's code-image
                    // (falls back to the ctx's own self image when there is no map), read the
                    // ABSOLUTE addresses + honest truncation, attribute, release. IsStatistical
                    // stays false (exact). A self-skipped (unarmed) scope has no ctx — just tear
                    // down the map + rundown + any allocated trace.
                    if (_ptWinCtx != null)
                    {
                        // Freeze the map BEFORE reading its image, so the decode sees exactly the
                        // methods JIT'd while the window was open; img stays valid across End
                        // because AttributeAddresses (which disposes _map) runs only afterward.
                        if (_map != null) _map.Stop();
                        IntPtr img = _map != null ? _map.ImageHandle : IntPtr.Zero;
                        ulong when = _map != null ? _map.ImageNow : 0;
                        int rc = _ptWinCtx.End(img, when, _handle);
                        _ptWinCtx = null;
                        Thread.EndThreadAffinity();       // paired with the ctor's pin (armed path)
                        if (rc == HwNative.ASMTEST_HW_OK && _handle != IntPtr.Zero)
                        {
                            if (_map != null) MethodsObserved = _map.Count;
                            ulong n = HwNative.asmtest_emu_trace_insns_len(_handle);
                            var addrs = new ulong[n];
                            for (ulong i = 0; i < n; i++)
                                addrs[i] = HwNative.asmtest_emu_trace_insn_at(_handle, (UIntPtr)i);
                            Addresses = addrs;
                            Truncated = HwNative.asmtest_emu_trace_truncated(_handle) != 0;
                        }
                        AttributeAddresses(img, when); // endpoint PCs -> methods; disposes _map
                    }
                    else if (_map != null) { _map.Stop(); _map.Dispose(); }
                    if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                    if (_handle != IntPtr.Zero)
                    {
                        HwNative.asmtest_trace_free(_handle);
                        _handle = IntPtr.Zero;
                    }
                    return;
                }
                case Kind.OopInlineWindow:
                {
                    if (_oopWinCtx != null)
                    {
                        var tr = HwTrace.Create(blocks: 4096, instructions: 65536);
                        int rc = _oopWinCtx.End(tr.Handle);
                        _oopWinCtx = null;
                        Thread.EndThreadAffinity();
                        if (rc == HwNative.ASMTEST_HW_OK)
                        {
                            if (_map != null)
                            {
                                _map.Stop();
                                MethodsObserved = _map.Count;
                            }
                            ulong n = HwNative.asmtest_emu_trace_insns_len(tr.Handle);
                            var addrs = new ulong[n];
                            for (ulong i = 0; i < n; i++)
                                addrs[i] = HwNative.asmtest_emu_trace_insn_at(tr.Handle, (UIntPtr)i);
                            Addresses = addrs;
                            Truncated = HwNative.asmtest_emu_trace_truncated(tr.Handle) != 0;
                            IntPtr img = _map != null ? _map.ImageHandle : IntPtr.Zero;
                            ulong when = _map != null ? _map.ImageNow : 0;
                            AttributeAddresses(img, when);
                        }
                        tr.Free();
                    }
                    else if (_map != null) { _map.Stop(); _map.Dispose(); }
                    if (_oopWinChan != IntPtr.Zero)
                    {
                        HwNative.asmtest_addr_channel_free_shared(_oopWinChan);
                        _oopWinChan = IntPtr.Zero;
                    }
                    if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                    return;
                }
                case Kind.WholeWindow:
                {
                    // Always render (size-then-allocate) to populate Path — emit gates only the
                    // sink write, not producing Path. The whole-window (§Z1) path is handle-keyed
                    // and renders the recorded ABSOLUTE addresses from live self memory; the
                    // region path is name-keyed and renders base-relative offsets.
                    // (T7 safe-managed "none" route arms nothing: _handle == Zero and no live
                    // frame — guard so an unarmed WholeWindow scope closes as a clean no-op.)
                    if (_handle != IntPtr.Zero)
                        HwNative.asmtest_hwtrace_end_window(_scope, _handle);
                    // §D0.1: freeze the JIT map to exactly the methods seen while the scope
                    // was open (before the readback/classification below JITs anything more).
                    // §Z3: also pin the code-image version AT CLOSE — the labelled
                    // disassembly below decodes against it, so a method that re-tiers/moves
                    // AFTER the window still renders the bytes that actually ran.
                    // (T7 safe-managed "none" route: an unarmed scope has _handle == Zero and
                    // no live frame — end_window above no-op'd, and the readbacks below are all
                    // _handle-guarded, so this branch is a clean no-op for it.)
                    IntPtr img = IntPtr.Zero;
                    ulong when = 0;
                    if (_map != null)
                    {
                        _map.Stop();
                        MethodsObserved = _map.Count;
                        img = _map.ImageHandle;
                        when = _map.ImageNow;
                    }
                    // Capture the raw ABSOLUTE addresses (before the trace is freed below).
                    // A whole-window trace can be a million runtime instructions, so we do
                    // NOT auto-render its disassembly (Path) — the caller ATTRIBUTES the
                    // Addresses instead (classify by known native regions to tell leaves
                    // apart; §Z1). Path stays empty for the whole-window form.
                    if (_handle != IntPtr.Zero)
                    {
                        ulong n = HwNative.asmtest_emu_trace_insns_len(_handle);
                        var addrs = new ulong[n];
                        for (ulong i = 0; i < n; i++)
                            addrs[i] = HwNative.asmtest_emu_trace_insn_at(_handle, (UIntPtr)i);
                        Addresses = addrs;
                    }
                    // §D0.1/§D0.2: attribute the captured addresses to managed methods (shared
                    // with the §D3 out-of-process window path). DATA only; the caller presents it.
                    AttributeAddresses(img, when);
                    // §3.1(c) (opt-in): attribute the captured ABSOLUTE addresses to the
                    // caller's NAMED regions first, then perf-map / maps. Must run HERE —
                    // after end_window (the frame is still resolvable) but BEFORE the
                    // trace_free below, which the frame's trace points into — so it cannot
                    // be offered as a post-Dispose method. This is the named-region split
                    // that SymbolizeBuckets(Addresses) cannot reproduce: identical-byte
                    // leaves are one [anon] to symbol/maps resolution.
                    AttributeNamedRegions();
                    // §D0.2: turn perf-map generation back off so it is not left on
                    // process-wide (unbounded jitdump growth / per-JIT overhead). Keyed on
                    // the REQUEST, not RundownEnabled — EnablePerfMap can succeed runtime-side
                    // while its response read times out (reported false), and Disable is a
                    // harmless no-op when it never took effect.
                    if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
                    // §Z5 (opt-in): render the whole window into Path via the native
                    // render_window — live-memory disassembly with the truncation banner.
                    // The frame stays resolvable after end_window (same order the C
                    // harness uses), and the code it decodes is still mapped; must run
                    // BEFORE the trace_free below, which the frame's trace points into.
                    Path = "";
                    if (_renderPath && _handle != IntPtr.Zero)
                    {
                        int wneed = HwNative.asmtest_hwtrace_render_window(_scope, null, UIntPtr.Zero);
                        if (wneed > 0)
                        {
                            byte[] wbuf = new byte[wneed + 1];
                            HwNative.asmtest_hwtrace_render_window(_scope, wbuf, (UIntPtr)(wneed + 1));
                            Path = System.Text.Encoding.ASCII.GetString(wbuf, 0, wneed);
                        }
                    }
                    break;
                }
                default: // Kind.Region — the region form (new AsmTrace(code)) + named-method scopes
                {
                    int need = 0;
                    // §D3: an out-of-process Method scope never began in-process — the
                    // helper filled the trace out of band, so there is nothing to end.
                    if (_began) HwNative.asmtest_hwtrace_end(_name);
                    // §1: render THIS scope's slice by handle where begin_scope produced
                    // one (concurrent same-site scopes each render their own); fall back
                    // to the name-keyed render (oop scopes, non-single-step fallback).
                    bool byHandle = _began && _scope.Idx != 0xffffffffu;
                    need = byHandle
                        ? HwNative.asmtest_hwtrace_render_scope(_scope, null, UIntPtr.Zero)
                        : HwNative.asmtest_hwtrace_render(_name, null, UIntPtr.Zero);
                    if (need <= 0 && byHandle)
                    {
                        byHandle = false;
                        need = HwNative.asmtest_hwtrace_render(_name, null, UIntPtr.Zero);
                    }
                    if (need > 0)
                    {
                        byte[] buf = new byte[need + 1];
                        if (byHandle) HwNative.asmtest_hwtrace_render_scope(_scope, buf, (UIntPtr)(need + 1));
                        else HwNative.asmtest_hwtrace_render(_name, buf, (UIntPtr)(need + 1));
                        Path = System.Text.Encoding.ASCII.GetString(buf, 0, need);
                    }
                    else Path = "";
                    break;
                }
            }
            if (_handle != IntPtr.Zero)
            {
                Truncated = HwNative.asmtest_emu_trace_truncated(_handle) != 0;
                HwNative.asmtest_trace_free(_handle);
            }
            // §0.2/B5: complementary managed-thread guard. The native side is authoritative
            // (it flags a cross-OS-thread close via the arm-tid check), but a managed thread
            // can migrate OS threads under the CLR scheduler, so also flag when Dispose runs
            // on a different MANAGED thread than the ctor — never present a hopped scope as a
            // complete single-thread window. An out-of-process (§D3) scope is exempt: its
            // body ran in the helper, not on this thread, so a Dispose hop is not a capture
            // hop. Honest-conservative: this only ever SETS Truncated, never clears it.
            if (!_oop && _armTid != 0 && Environment.CurrentManagedThreadId != _armTid)
                Truncated = true;
            if (_emit && !string.IsNullOrEmpty(Path)) Console.Write(Path);
        }

        // Disassemble the single instruction at absolute address `addr` into
        // "<mnemonic> <operands>" (or "(undecodable)"). §Z3 versioned-first: when a
        // code-image `img` is tracking, decode the bytes live AT `when` (the window
        // close) so a body that re-tiers/moves after the window renders what actually
        // ran; addresses with no tracked version (native runtime) fall back to LIVE
        // memory — safe for a captured (executed) address, its page is mapped.
        // `scratch` is a reusable ≥16-byte buffer; asmtest_disas NUL-terminates it.
        static string DisasAt(IntPtr img, ulong when, ulong addr, byte[] scratch)
        {
            scratch[0] = 0;
            if (img != IntPtr.Zero &&
                HwNative.asmtest_codeimage_bytes_at(img, new IntPtr(unchecked((long)addr)), when,
                                                    out IntPtr vb, out UIntPtr vl) == HwNative.ASMTEST_CI_OK &&
                vb != IntPtr.Zero && (ulong)vl > 0)
            {
                ulong take = (ulong)vl < 16UL ? (ulong)vl : 16UL;
                HwNative.asmtest_disas(0 /* ASMTEST_ARCH_X86_64 */, vb, (UIntPtr)take,
                                       addr, 0, scratch, (UIntPtr)scratch.Length);
                int zv = System.Array.IndexOf<byte>(scratch, 0);
                if (zv > 0) return System.Text.Encoding.ASCII.GetString(scratch, 0, zv);
                scratch[0] = 0; // undecodable at-version: fall through to the live read
            }
            HwNative.asmtest_disas(0 /* ASMTEST_ARCH_X86_64 */, new IntPtr(unchecked((long)addr)),
                                   (UIntPtr)16, addr, 0, scratch, (UIntPtr)scratch.Length);
            int z = System.Array.IndexOf<byte>(scratch, 0);
            if (z < 0) z = scratch.Length;
            return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(scratch, 0, z);
        }
    }

    /// <summary>One LABELLED executed instruction of a whole-window capture: its absolute
    /// address, disassembled text, the managed method it belongs to, and how many
    /// native-runtime (unlabelled) instructions ran immediately before it. See
    /// <see cref="AsmTrace.Disassembly"/>.</summary>
    public readonly struct AsmInstruction
    {
        public AsmInstruction(ulong address, string text, string method, int runtimeBefore)
        { Address = address; Text = text; Method = method; RuntimeBefore = runtimeBefore; }
        /// <summary>The instruction's absolute in-process address.</summary>
        public ulong Address { get; }
        /// <summary>The disassembled <c>&lt;mnemonic&gt; &lt;operands&gt;</c> (or <c>(undecodable)</c>).</summary>
        public string Text { get; }
        /// <summary>The full method name this instruction belongs to (jitdump/perf-map or
        /// listener spelling); split with <see cref="ShortMethod"/> / <see cref="Assembly"/>.</summary>
        public string Method { get; }
        /// <summary>Native-runtime (unlabelled) instructions executed immediately before this
        /// one — the gap elided from this named instruction stream.</summary>
        public int RuntimeBefore { get; }
        /// <summary>The bare <c>Type::Method(sig)</c> of <see cref="Method"/> (reuses the
        /// <see cref="AsmMethod"/> name parser).</summary>
        public string ShortMethod => new AsmMethod(Method, 0).ShortName;
        /// <summary>The declaring assembly of <see cref="Method"/> (empty if untagged).</summary>
        public string Assembly => new AsmMethod(Method, 0).Assembly;
    }

    /// <summary>One managed method's share of a whole-window capture: its fully-qualified
    /// name and the number of captured instructions attributed to it. See
    /// <see cref="AsmTrace.Methods"/>.</summary>
    public readonly struct AsmMethod
    {
        public AsmMethod(string name, long count) { Name = name; Count = count; }
        /// <summary>The full name as recorded (jitdump/perf-map spelling
        /// <c>&lt;ret&gt; [&lt;assembly&gt;] Type::Method(sig)[tier]</c>, or the listener's dotted
        /// <c>Namespace.Method</c>).</summary>
        public string Name { get; }
        /// <summary>The method's share of the capture. For an EXACT scope
        /// (<see cref="AsmTrace.IsStatistical"/> false) this is the number of captured
        /// instructions attributed to the method; for a STATISTICAL scope
        /// (<see cref="AsmTrace.WindowHot"/>) it is the sample-weighted hot weight
        /// (branch-target endpoint hits), NOT an instruction count. See <see cref="Weight"/>,
        /// which carries the same value with that meaning made explicit in the name.</summary>
        public long Count { get; }
        /// <summary>§E2: the method's attributed WEIGHT — the same value as <see cref="Count"/>,
        /// with the statistical/exact meaning promoted into the type instead of living only in
        /// the docs. For a STATISTICAL scope (<see cref="AsmTrace.IsStatistical"/>) this is the
        /// endpoint-hit sample weight; for an EXACT scope it is the captured-instruction count.
        /// <c>Weight == Count</c> in both cases — <see cref="Count"/> is retained for source
        /// compatibility, and <see cref="Weight"/> is the honest name to reach for on a
        /// statistical survey (where "instruction count" would be a category error). See
        /// <see cref="AsmTrace.WeightIn"/> for a name-substring sum.</summary>
        public long Weight => Count;

        /// <summary>The declaring assembly/module in the jitdump/perf-map spelling
        /// <c>&lt;ret&gt; [&lt;assembly&gt;] Type::Method(sig)[tier]</c> — e.g.
        /// <c>System.Private.CoreLib</c>, <c>System.Console</c>. Empty for a listener-spelled
        /// name that carries no assembly tag. Anchored on the <c>[...]</c> immediately before
        /// the <c>Type::Method</c> separator — NOT the first <c>[</c>, because the return type
        /// can itself contain brackets (e.g. an array return <c>!0[]</c> or a generic).</summary>
        public string Assembly => Bracketed(out string asm, out _) ? asm : "";

        /// <summary>The method itself — <c>Type::Method(sig)</c> — without the return type,
        /// <c>[assembly]</c>, or trailing <c>[tier]</c>. Falls back to <see cref="Name"/> for a
        /// listener-spelled name.</summary>
        public string ShortName
        {
            get
            {
                string s = Bracketed(out _, out int after) ? Name.Substring(after) : Name;
                // Strip a trailing "[tier]" (e.g. "[PreJIT]") — the tag after the signature's ')'.
                int rp = s.LastIndexOf(')');
                int lt = s.LastIndexOf('[');
                if (lt > rp && s.EndsWith("]")) s = s.Substring(0, lt);
                return s.Trim();
            }
        }

        /// <summary>The JIT/compilation tier — the trailing <c>[tier]</c> tag: <c>PreJIT</c>
        /// (ReadyToRun, precompiled), <c>MinOptJitted</c>/<c>Tier0</c> (cold JIT),
        /// <c>OptimizedTier1</c> (hot), etc. Empty for a listener-spelled dotted name (which
        /// carries no tag). Anchored on the last <c>[..]</c> AFTER the signature's <c>)</c> — so
        /// a generic-arg bracket in the type/return is never mistaken for a tier.</summary>
        public string Tier
        {
            get
            {
                if (Name.Length == 0 || Name[Name.Length - 1] != ']') return "";
                int lt = Name.LastIndexOf('[');
                int rp = Name.LastIndexOf(')');
                if (lt <= rp || lt < 0) return "";
                string t = Name.Substring(lt + 1, Name.Length - lt - 2);
                // Tiers are bare identifiers (PreJIT, Tier0, OptimizedTier1, MinOptJitted); a
                // dotted token is a generic-arg bracket (e.g. [System.Char]), not a tier.
                return t.IndexOf('.') < 0 ? t : "";
            }
        }

        // Locate the "[<assembly>] " tag: the last "] " that precedes the "Type::Method"
        // separator ("::"). The return type may contain "[]" (array) or generic brackets, so
        // the first "[" is unsafe; the arg list may contain "[Assembly]" too, so we look only
        // left of "::". Returns false for a listener-spelled name (no such tag).
        private bool Bracketed(out string assembly, out int afterTag)
        {
            assembly = ""; afterTag = 0;
            int sep = Name.IndexOf("::");
            int scan = sep < 0 ? Name.Length : sep;
            if (scan < 2) return false;
            int rb = Name.LastIndexOf("] ", scan - 1, System.StringComparison.Ordinal);
            if (rb < 0) return false;
            int lb = Name.LastIndexOf('[', rb);
            if (lb < 0) return false;
            assembly = Name.Substring(lb + 1, rb - lb - 1);
            afterTag = rb + 2;
            return true;
        }
    }

    /// <summary>
    /// Single-instruction classifiers over LIVE in-process addresses (Capstone-gated —
    /// every predicate is false without it, mirroring the native contract). Lets a
    /// caller walking <see cref="AsmTrace.Disassembly"/> classify control flow
    /// structurally (call/branch/ret + direct-call target) instead of string-parsing
    /// <see cref="AsmInstruction.Text"/> mnemonics.
    /// </summary>
    public static class Disas
    {
        const int X86_64 = 0;   // ASMTEST_ARCH_X86_64
        const int MAXLEN = 16;  // one x86 instruction is at most 15 bytes

        /// <summary>True if this build links Capstone (classifiers work).</summary>
        public static bool Available =>
            HwNative.LibAvailable && HwNative.asmtest_disas_available();

        /// <summary>The instruction at live <paramref name="addr"/> is a CALL.</summary>
        public static bool IsCall(ulong addr) => HwNative.LibAvailable &&
            HwNative.asmtest_disas_is_call(X86_64, Ptr(addr), (UIntPtr)MAXLEN, 0) != 0;

        /// <summary>The instruction at live <paramref name="addr"/> is any branch-class
        /// control transfer (jump, call, or return).</summary>
        public static bool IsBranch(ulong addr) => HwNative.LibAvailable &&
            HwNative.asmtest_disas_is_branch(X86_64, Ptr(addr), (UIntPtr)MAXLEN, 0) != 0;

        /// <summary>The instruction at live <paramref name="addr"/> is a RETURN.</summary>
        public static bool IsRet(ulong addr) => HwNative.LibAvailable &&
            HwNative.asmtest_disas_is_ret(X86_64, Ptr(addr), (UIntPtr)MAXLEN, 0) != 0;

        /// <summary>Resolve the DIRECT-call target of the instruction at live
        /// <paramref name="addr"/> (absolute). False for indirect calls, non-calls,
        /// undecodable bytes, or a Capstone-free build.</summary>
        public static bool TryCallTarget(ulong addr, out ulong target)
        {
            target = 0;
            return HwNative.LibAvailable && HwNative.asmtest_disas_call_target(
                X86_64, Ptr(addr), (UIntPtr)MAXLEN, addr, 0, out target) != 0;
        }

        static IntPtr Ptr(ulong addr) => new IntPtr(unchecked((long)addr));
    }

    /// <summary>
    /// §D0.1 — an in-process address→managed-method map, built from the CoreCLR
    /// <c>MethodLoadVerbose</c> events, so a whole-window capture's ABSOLUTE addresses
    /// (see <see cref="AsmTrace.Addresses"/>) can be labelled by managed method name —
    /// turning "N runtime instructions" into a per-method breakdown, WITHOUT a launch
    /// knob (no <c>DOTNET_PerfMapEnabled</c>) and without Intel PT.
    ///
    /// Construct it BEFORE the code under trace is JIT'd: an in-proc listener sees only
    /// methods JIT'd AFTER it enables the keyword (there is no in-proc rundown of
    /// already-jitted methods). Call <see cref="Freeze"/> after the scope closes, then
    /// <see cref="Resolve"/> each captured address. CoreCLR only; a no-op elsewhere.
    /// </summary>
    public sealed class JitMethodMap : EventListener
    {
        // The CoreCLR runtime EventSource (the ETW provider name) + its JIT keyword.
        const string RuntimeProvider = "Microsoft-Windows-DotNETRuntime";
        const EventKeywords JitKeyword = (EventKeywords)0x10;

        readonly object _lock = new object();
        readonly List<Entry> _methods = new List<Entry>();
        IntPtr _img; // §Z3: optional self code-image tracking JIT'd bytes (zeroed on Dispose)
        Entry[] _frozen; // sorted-by-start snapshot for binary search
        volatile bool _stopped; // stop ingesting once the traced scope has closed

        // §E3 (asmtrace-extensions-plan) — the SIBLING-THREAD live-publish state. The
        // listener callback must never P/Invoke into the shared channel itself: for the
        // out-of-process window that callback can fire on the very thread being
        // single-stepped, and re-entering the runtime/native boundary under step ABORTS
        // the runtime (SIGABRT, observed — managed-wholewindow-oop-plan.md). So
        // OnEventWritten only ENQUEUES the (base,len) onto a lock-free queue, and a
        // dedicated, never-stepped publisher thread drains it and does the P/Invoke.
        volatile IntPtr _publishChannel;            // §D3/§E3: shared channel (IntPtr.Zero = off)
        ConcurrentQueue<PublishRec> _publishQueue;  // listener -> sibling handoff (lock-free enqueue)
        SemaphoreSlim _publishWake;                 // signaled once per enqueue (+ a poll backstop)
        Thread _publishThread;                      // the §E3 sibling publisher (background)
        volatile bool _publishStop;                 // publisher exit flag (set by SetPublishChannel(0))
        long _livePublished;                        // records actually pushed to the ring (Interlocked)

        struct Entry { public ulong Start, End; public string Name; }
        struct PublishRec { public ulong Start, Size; }

        /// <summary>§D3/§E3: while set, each JIT'd method is also published to this shared
        /// address channel so the out-of-process window's stepper records it live — the
        /// mid-window publish that closes the deep-BCL gap (extensions plan E3). A non-zero
        /// channel STARTS the sibling publisher thread (the EventPipe callback itself never
        /// touches the channel: it can fire on the single-stepped thread, where the P/Invoke
        /// re-enters the runtime under step and aborts it). IntPtr.Zero STOPS the publisher
        /// and JOINS its thread — on return the caller may free the channel with no
        /// use-after-free window. Not reentrant; call from the scope-owning thread only.</summary>
        public void SetPublishChannel(IntPtr chan)
        {
            if (chan != IntPtr.Zero)
            {
                if (_publishThread != null) { _publishChannel = chan; return; } // re-point, keep thread
                _publishQueue = new ConcurrentQueue<PublishRec>();
                _publishWake = new SemaphoreSlim(0);
                _publishStop = false;
                _publishChannel = chan;
                // Started BEFORE the window arms, on a plain managed thread the stealth
                // stepper never targets (it steps only the arming thread) — so the
                // publisher's P/Invokes run at native speed while the window crawls.
                _publishThread = new Thread(PublishLoop)
                {
                    IsBackground = true,
                    Name = "asmtest-e3-jit-publish",
                };
                _publishThread.Start();
                return;
            }
            Thread t = _publishThread;
            if (t == null) { _publishChannel = IntPtr.Zero; return; } // never started: no-op
            _publishStop = true;
            try { _publishWake.Release(); } catch (SemaphoreFullException) { }
            // The loop re-checks _publishStop at least every poll interval and publish_rec
            // never blocks (a full ring overwrites-oldest + flags overrun), so this join is
            // prompt — and it is what makes the caller's channel free safe.
            t.Join();
            _publishThread = null;
            _publishChannel = IntPtr.Zero;
            _publishWake.Dispose();
            _publishWake = null;
            _publishQueue = null;
        }

        /// <summary>§E3: how many method records the sibling publisher has actually pushed
        /// to the shared channel (0 while the live publish is off / never armed).</summary>
        public long LivePublished => Interlocked.Read(ref _livePublished);

        // §E3 — the sibling publisher body. Once the window arms, THIS thread is the ring's
        // only producer (the coarse-range seeding on the scope thread happens-before
        // Thread.Start), preserving the channel's single-producer/single-consumer contract
        // (asmtest_addr_channel.h); the helper process is the one consumer, draining between
        // steps. Simplest thread-safe shape per the plan: semaphore-woken drain loop with a
        // 50ms poll backstop against a lost wakeup.
        void PublishLoop()
        {
            try
            {
                while (true)
                {
                    try { _publishWake.Wait(50); }
                    catch (ObjectDisposedException) { return; } // defensive: stop disposed it
                    while (_publishQueue.TryDequeue(out PublishRec r))
                    {
                        IntPtr chan = _publishChannel;
                        if (chan == IntPtr.Zero) break; // stopping — drop stale records
                        HwNative.asmtest_addr_channel_publish_rec(chan, r.Start, r.Size, 0);
                        Interlocked.Increment(ref _livePublished);
                    }
                    if (_publishStop) return; // window closed: further publishes are useless
                }
            }
            catch
            {
                // The publisher must never take the process down; a dead publisher only
                // reopens the (documented, honest-partial) mid-window gap for this scope.
            }
        }

        public JitMethodMap() : this(false) { }

        /// <summary>§Z3: with <paramref name="trackBytes"/>, each observed method's bytes
        /// are also snapshotted into a self code-image (<c>asmtest_codeimage_track</c>),
        /// so a close-time render can decode against the version live in the window
        /// (<see cref="ImageHandle"/>/<see cref="ImageNow"/>) instead of live memory.</summary>
        public JitMethodMap(bool trackBytes)
        {
            if (trackBytes && HwNative.LibAvailable)
                _img = HwNative.asmtest_codeimage_new(0); // 0 == self; IntPtr.Zero on failure
        }

        /// <summary>§Z3: the optional code-image handle (IntPtr.Zero when not tracking).</summary>
        public IntPtr ImageHandle => _img;

        /// <summary>§Z3: the image's current version sequence (0 when not tracking).</summary>
        public ulong ImageNow => _img != IntPtr.Zero ? HwNative.asmtest_codeimage_now(_img) : 0;

        /// <summary>The number of JIT'd methods observed so far.</summary>
        public int Count { get { lock (_lock) return _methods.Count; } }

        /// <summary>Observed entries whose name contains <paramref name="nameSubstring"/> —
        /// ≥ 2 for one method means a re-JIT (tier-up) was observed at a new address.</summary>
        public int CountFor(string nameSubstring)
        {
            int n = 0;
            lock (_lock)
                foreach (var m in _methods)
                    if (m.Name.Contains(nameSubstring)) n++;
            return n;
        }

        /// <summary>§ managed-wholewindow-compose T2: how many DISTINCT JIT'd bodies (unique
        /// start addresses) were observed for methods whose name contains
        /// <paramref name="nameSubstring"/> — ≥ 2 means a tier-up / OSR re-JIT moved the method
        /// to a fresh address. Unlike <see cref="CountFor"/> (which counts events), this counts
        /// unique start PCs, so a repeated <c>MethodLoadVerbose</c> for the same body is not
        /// double-counted — the honest "how many versions did the runtime produce" signal.</summary>
        public int ObservedVersions(string nameSubstring)
        {
            if (string.IsNullOrEmpty(nameSubstring)) return 0;
            var starts = new HashSet<ulong>();
            lock (_lock)
                foreach (var m in _methods)
                    if (m.Name.Contains(nameSubstring)) starts.Add(m.Start);
            return starts.Count;
        }

        /// <summary>§D0.3: the LATEST observed entry whose name contains
        /// <paramref name="nameSubstring"/> (later entry wins — a re-JIT'd body
        /// supersedes its tier-0 predecessor). False if none seen.</summary>
        public bool TryResolveEntry(string nameSubstring, out ulong start, out ulong size)
        {
            start = 0; size = 0;
            lock (_lock)
            {
                for (int i = _methods.Count - 1; i >= 0; i--)
                    if (_methods[i].Name.Contains(nameSubstring))
                    {
                        start = _methods[i].Start;
                        size = _methods[i].End - _methods[i].Start;
                        return true;
                    }
            }
            return false;
        }

        /// <summary>
        /// §E6: every recorded method name CONTAINING <paramref name="nameSubstring"/>, newest
        /// first. <see cref="TryResolveEntry"/> answers "give me the newest match", which is
        /// right for labelling a sampled address (a wrong guess costs a mis-attributed sample)
        /// and WRONG for planting a hardware breakpoint (a wrong guess arms on the wrong method
        /// and reports a perfect, indistinguishable success). A caller that must not guess uses
        /// this to SEE the ambiguity and refuse it.
        /// </summary>
        internal List<string> MatchingNames(string nameSubstring)
        {
            var hits = new List<string>();
            if (string.IsNullOrEmpty(nameSubstring)) return hits;
            lock (_lock)
            {
                for (int i = _methods.Count - 1; i >= 0; i--)
                    if (_methods[i].Name.Contains(nameSubstring))
                        hits.Add(_methods[i].Name);
            }
            return hits;
        }

        protected override void OnEventSourceCreated(EventSource src)
        {
            if (src.Name == RuntimeProvider)
                EnableEvents(src, EventLevel.Verbose, JitKeyword);
        }

        protected override void OnEventWritten(EventWrittenEventArgs e)
        {
            // EventListener's base ctor can dispatch callbacks BEFORE this instance's
            // field initializers run, and a listener callback must never throw — so
            // guard the not-yet-initialized state and wrap the whole body. `_stopped`
            // freezes the map to exactly the methods seen while the scope was open.
            if (_stopped || _lock == null || _methods == null) return;
            try
            {
                // MethodLoadVerbose / _V1 / _V2 all carry MethodStartAddress + MethodSize
                // + MethodNamespace/Name. Keep this allocation-light: handling the event
                // can itself re-enter the JIT (reentrancy), so do only the record.
                if (e.EventName == null || !e.EventName.StartsWith("MethodLoadVerbose"))
                    return;
                ulong start = 0, size = 0;
                string ns = null, name = null;
                var names = e.PayloadNames;
                if (names == null) return;
                for (int i = 0; i < names.Count; i++)
                {
                    object v = e.Payload[i];
                    switch (names[i])
                    {
                        case "MethodStartAddress": start = Convert.ToUInt64(v); break;
                        case "MethodSize": size = Convert.ToUInt64(v); break;
                        case "MethodNamespace": ns = v as string; break;
                        case "MethodName": name = v as string; break;
                    }
                }
                if (start == 0 || size == 0) return;
                string full = string.IsNullOrEmpty(ns) ? (name ?? "?") : ns + "." + name;
                lock (_lock)
                {
                    _methods.Add(new Entry { Start = start, End = start + size, Name = full });
                    _frozen = null; // invalidate the snapshot
                }
                // §Z3: snapshot the freshly JIT'd bytes into the code-image (native call,
                // no managed allocation — safe for the reentrancy-light contract above).
                if (_img != IntPtr.Zero)
                    HwNative.asmtest_codeimage_track(_img, new IntPtr(unchecked((long)start)), (UIntPtr)size);
                // §D3/§E3: hand the freshly JIT'd method to the SIBLING publisher thread,
                // which P/Invokes it into the out-of-process window's SHARED channel — the
                // reverse-attach stepper drains that live, so a method JIT'd MID-WINDOW (a
                // first-call generic instantiation, a local function) is recorded from the
                // moment it is published, which a pre-window code-range scan cannot see.
                // The enqueue is lock-free and calls no native code: THIS callback can fire
                // on the single-stepped thread, where a channel P/Invoke re-enters the
                // runtime under step and aborts it (SIGABRT, observed — the reason the
                // inline publish was left OFF before E3; managed-wholewindow-oop-plan.md).
                ConcurrentQueue<PublishRec> q = _publishQueue;
                if (q != null && _publishChannel != IntPtr.Zero)
                {
                    q.Enqueue(new PublishRec { Start = start, Size = size });
                    SemaphoreSlim wake = _publishWake; // may be torn down concurrently
                    if (wake != null)
                    {
                        try { wake.Release(); }
                        catch (ObjectDisposedException) { } // stop won the race: publisher gone
                        catch (SemaphoreFullException) { }  // already saturated with wakeups
                    }
                }
            }
            catch
            {
                // A malformed/unexpected event payload must not tear down the listener.
            }
        }

        /// <summary>Stop ingesting method events — call right after the traced scope
        /// closes so the map holds exactly the methods seen while it was open (the
        /// listener otherwise keeps recording the caller's own post-scope JIT).</summary>
        public void Stop() { _stopped = true; }

        public override void Dispose()
        {
            _stopped = true;
            // §E3 defensive: a map disposed with the publisher still live joins it first
            // (idempotent no-op on the normal path, where the window teardown already
            // called SetPublishChannel(IntPtr.Zero) before freeing the channel).
            try { SetPublishChannel(IntPtr.Zero); } catch { }
            if (_img != IntPtr.Zero) { HwNative.asmtest_codeimage_free(_img); _img = IntPtr.Zero; }
            base.Dispose();
        }

        /// <summary>Snapshot + sort the observed methods for O(log n) <see cref="Resolve"/>.
        /// Call once after <see cref="Stop"/>.</summary>
        public void Freeze()
        {
            lock (_lock)
            {
                // Sort a LOCAL, then publish the reference last — a lock-free Resolve
                // reader must never observe a half-sorted (or torn) array.
                var a = _methods.ToArray();
                Array.Sort(a, (x, y) => x.Start.CompareTo(y.Start));
                _frozen = a;
            }
        }

        /// <summary>The managed method containing <paramref name="addr"/>, or null. Binary
        /// search over the frozen snapshot (falls back to a linear scan if not frozen).</summary>
        public string Resolve(ulong addr)
        {
            Entry[] s = _frozen;
            if (s == null)
            {
                lock (_lock)
                    foreach (var m in _methods)
                        if (addr >= m.Start && addr < m.End) return m.Name;
                return null;
            }
            int lo = 0, hi = s.Length - 1;
            while (lo <= hi)
            {
                int mid = (int)(((uint)lo + (uint)hi) >> 1);
                if (addr < s[mid].Start) hi = mid - 1;
                else if (addr >= s[mid].End) lo = mid + 1;
                else return s[mid].Name;
            }
            return null;
        }

        /// <summary>§D0.2: fold in a perf **jitdump** (<c>/tmp/jit-&lt;pid&gt;.dump</c>, the binary
        /// perf JIT_CODE_LOAD format) — which, unlike the text perf-map, ALSO carries
        /// ReadyToRun (R2R, precompiled) methods (e.g. <c>System.Console::WriteLine</c>), so a
        /// whole-window capture can be labelled by warm AND R2R BCL methods. Additive; call
        /// before <see cref="Freeze"/>. Dedupes by start address so a cold in-scope method
        /// the listener already recorded keeps its spelling. Returns entries loaded (0 on a
        /// missing/unreadable file); never throws. x86-64 little-endian.</summary>
        public int LoadJitDump(string path)
        {
            int n = 0;
            try
            {
                if (!File.Exists(path)) return 0;
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                using var br = new BinaryReader(fs);
                if (fs.Length < 40 || br.ReadUInt32() != 0x4A695444u) return 0; // magic "JiTD" (LE)
                br.ReadUInt32();                       // version
                uint headerSize = br.ReadUInt32();     // total header size
                if (headerSize < 40 || headerSize > fs.Length) return 0;
                fs.Position = headerSize;              // skip to the first record

                HashSet<ulong> seen;
                lock (_lock)
                {
                    seen = new HashSet<ulong>(_methods.Count);
                    foreach (var m in _methods) seen.Add(m.Start);
                }
                // Records: [u32 id][u32 total_size][u64 timestamp] + body. id 0 = JIT_CODE_LOAD:
                // [u32 pid][u32 tid][u64 vma][u64 code_addr][u64 code_size][u64 code_index]
                // [name\0][code bytes]. total_size advances to the next record.
                while (fs.Position + 16 <= fs.Length)
                {
                    long rec = fs.Position;
                    uint id = br.ReadUInt32();
                    uint recSize = br.ReadUInt32();
                    br.ReadUInt64();                   // timestamp
                    if (recSize < 16 || rec + recSize > fs.Length) break;
                    // JIT_CODE_LOAD body is 40B fixed + name(>=1) + code; a shorter record
                    // would read code_addr/size from the wrong offsets — skip it.
                    if (id == 0 && recSize >= 57)       // JIT_CODE_LOAD
                    {
                        br.ReadUInt32(); br.ReadUInt32(); br.ReadUInt64(); // pid, tid, vma
                        ulong codeAddr = br.ReadUInt64();
                        ulong codeSize = br.ReadUInt64();
                        br.ReadUInt64();               // code_index
                        var nb = new List<byte>(64);
                        int b;
                        while ((b = fs.ReadByte()) > 0) nb.Add((byte)b);
                        if (codeAddr != 0 && codeSize != 0 && seen.Add(codeAddr))
                        {
                            string name = System.Text.Encoding.UTF8.GetString(nb.ToArray());
                            lock (_lock)
                            {
                                _methods.Add(new Entry { Start = codeAddr, End = codeAddr + codeSize, Name = name });
                                _frozen = null;
                            }
                            n++;
                        }
                    }
                    fs.Position = rec + recSize;        // next record
                }
            }
            catch { /* partial/unreadable jitdump: keep whatever loaded */ }
            return n;
        }
    }

    /// <summary>
    /// Native-IL attribution: subscribe CoreCLR's own <c>MethodILToNativeMap</c> JIT events
    /// (keyword <c>JittedMethodILToNativeMapKeyword</c> = 0x20000, Verbose) so a captured
    /// ABSOLUTE address resolves to <c>(method, nativeOffset, ilOffset)</c> — the IL-offset
    /// counterpart of <see cref="JitMethodMap"/> (which names the method only). No launch knob,
    /// no Intel PT; CoreCLR only, a no-op elsewhere.
    ///
    /// Construct it BEFORE the code under trace is JIT'd: an in-proc listener sees only methods
    /// JIT'd AFTER it enables the keyword (there is no in-proc rundown of already-jitted
    /// methods). Call <see cref="Freeze"/> after the scope closes, then <see cref="TryResolve"/>.
    ///
    /// CAVEAT (verified): <b>ReJITID does not distinguish tiers</b> — Tier0/Tier1/OSR bodies of
    /// one method share ReJITID 0 at DIFFERENT addresses. Identity is therefore the code RANGE,
    /// which is exactly what the join produces: each <c>MethodLoadVerbose</c> record gets its own
    /// address range, and its IL map is the <c>MethodILToNativeMap</c> with equal
    /// <c>(MethodID, ReJITID)</c> paired in emission order.
    ///
    /// CAVEAT (verified on .NET 8): an in-proc <see cref="EventListener"/> receives the
    /// <c>MethodILToNativeMap_V1</c> event and its <c>CountOfMapEntries</c>, but the runtime
    /// TRUNCATES the trailing <c>ILOffsets</c>/<c>NativeOffsets</c> arrays — they surface as
    /// neither typed arrays nor a byte[] blob (only an out-of-proc EventPipe trace parser sees
    /// them). So <see cref="MapCount"/>/<see cref="TotalMapEntries"/> prove the 0x20000 keyword
    /// is wired and a real IL map exists, while <see cref="TryResolve"/> returns the method +
    /// native offset with <c>ilOffset = NO_MAPPING</c> until a build surfaces the arrays (the
    /// <see cref="PointsDecoded"/> path). Feeding an <c>asmtest_srcreg</c> from a decoded map is
    /// follow-on work once a build/route delivers the offsets.
    /// </summary>
    public sealed class IlToNativeMap : EventListener
    {
        const string RuntimeProvider = "Microsoft-Windows-DotNETRuntime";
        // JitKeyword 0x10 (MethodLoadVerbose, Informational) | JittedMethodILToNativeMapKeyword
        // 0x20000 (MethodILToNativeMap, Verbose). One Verbose subscription carries both.
        const EventKeywords Keywords = (EventKeywords)(0x10 | 0x20000);

        // ICorDebugInfo pseudo-IL-offsets, mirrored from ASMTEST_SRC_IL_* (asmtest_trace.h).
        public const int IlNoMapping = -1;
        public const int IlProlog = -2;
        public const int IlEpilog = -3;

        readonly object _lock = new object();
        readonly List<Load> _loads = new List<Load>();
        readonly List<IlMap> _maps = new List<IlMap>();
        volatile bool _stopped;
        Method[] _frozen; // sorted by Start for binary search

        struct Load { public ulong MethodId, ReJITID, Start, Size; public string Name; }
        struct IlMap { public ulong MethodId, ReJITID; public int Entries; public IlPoint[] Points; }
        struct IlPoint { public uint NativeOffset; public int IlOffset; }
        struct Method { public ulong Start, End; public string Name; public int MapEntries; public IlPoint[] Points; }

        /// <summary>MethodLoadVerbose records observed.</summary>
        public int Count { get { lock (_lock) return _loads.Count; } }
        /// <summary>MethodILToNativeMap records observed (the 0x20000 keyword fired).</summary>
        public int MapCount { get { lock (_lock) return _maps.Count; } }
        /// <summary>Total IL->native map entries the runtime reported (the sum of each event's
        /// <c>CountOfMapEntries</c>) — non-zero proves a real IL map was delivered, even on a
        /// runtime that does not surface the offset ARRAYS to an in-proc listener.</summary>
        public int TotalMapEntries { get { lock (_lock) { int t = 0; foreach (var m in _maps) t += m.Entries; return t; } } }
        /// <summary>Concrete (native_off, il_off) points decoded from the offset arrays (0 when
        /// the in-proc listener truncated them at CountOfMapEntries — the common .NET 8 case).</summary>
        public int PointsDecoded { get { lock (_lock) { int t = 0; foreach (var m in _maps) t += m.Points.Length; return t; } } }

        /// <summary>The latest observed method whose name contains
        /// <paramref name="nameSubstring"/> — its native (start,size). False if none.</summary>
        public bool TryFindByName(string nameSubstring, out ulong start, out ulong size)
        {
            start = 0; size = 0;
            if (string.IsNullOrEmpty(nameSubstring)) return false;
            lock (_lock)
                for (int i = _loads.Count - 1; i >= 0; i--)
                    if (_loads[i].Name.Contains(nameSubstring))
                    { start = _loads[i].Start; size = _loads[i].Size; return true; }
            return false;
        }

        protected override void OnEventSourceCreated(EventSource src)
        {
            if (src != null && src.Name == RuntimeProvider)
                EnableEvents(src, EventLevel.Verbose, Keywords);
        }

        protected override void OnEventWritten(EventWrittenEventArgs e)
        {
            // The base ctor can dispatch before this instance's fields init; a listener
            // callback must never throw. Guard + wrap. `_stopped` freezes ingestion.
            if (_stopped || _lock == null || _loads == null || _maps == null) return;
            try
            {
                string name = e.EventName;
                if (name == null) return;
                // Runtime spells these MethodLoadVerbose_V1/_V2 and MethodILToNativeMap_V1.
                if (name.StartsWith("MethodILToNativeMap")) ParseIlMap(e);
                else if (name.StartsWith("MethodLoadVerbose")) ParseLoad(e);
            }
            catch { /* a malformed payload must not tear down the listener */ }
        }

        void ParseLoad(EventWrittenEventArgs e)
        {
            ulong mid = 0, rejit = 0, start = 0, size = 0;
            string ns = null, mn = null;
            var names = e.PayloadNames;
            if (names == null) return;
            for (int i = 0; i < names.Count; i++)
            {
                object v = e.Payload[i];
                switch (names[i])
                {
                    case "MethodID": mid = Convert.ToUInt64(v); break;
                    case "ReJITID": rejit = Convert.ToUInt64(v); break;
                    case "MethodStartAddress": start = Convert.ToUInt64(v); break;
                    case "MethodSize": size = Convert.ToUInt64(v); break;
                    case "MethodNamespace": ns = v as string; break;
                    case "MethodName": mn = v as string; break;
                }
            }
            if (start == 0 || size == 0) return;
            string full = string.IsNullOrEmpty(ns) ? (mn ?? "?") : ns + "." + mn;
            lock (_lock)
            {
                _loads.Add(new Load { MethodId = mid, ReJITID = rejit, Start = start, Size = size, Name = full });
                _frozen = null;
            }
        }

        void ParseIlMap(EventWrittenEventArgs e)
        {
            // Payload: MethodID u64, ReJITID u64, MethodExtent u8, CountOfMapEntries u16,
            // ILOffsets u32[], NativeOffsets u32[], ClrInstanceID u16. In-proc array payloads
            // are runtime-inconsistent (int[]/uint[]/IEnumerable, or a byte[] blob), so decode
            // both — following the GcMoveMap byte[]-blob precedent.
            ulong mid = 0, rejit = 0;
            int count = -1;
            object ilObj = null, natObj = null;
            var names = e.PayloadNames;
            if (names == null) return;
            for (int i = 0; i < names.Count; i++)
            {
                object v = e.Payload[i];
                switch (names[i])
                {
                    case "MethodID": mid = Convert.ToUInt64(v); break;
                    case "ReJITID": rejit = Convert.ToUInt64(v); break;
                    case "CountOfMapEntries": count = Convert.ToInt32(v); break;
                    case "ILOffsets": ilObj = v; break;
                    case "NativeOffsets": natObj = v; break;
                }
            }
            int[] il = DecodeI32Array(ilObj);
            int[] nat = DecodeI32Array(natObj);
            IlPoint[] pts;
            if (il != null && nat != null)
            {
                int n = Math.Min(il.Length, nat.Length);
                pts = new IlPoint[n];
                for (int i = 0; i < n; i++)
                    pts[i] = new IlPoint { NativeOffset = unchecked((uint)nat[i]), IlOffset = il[i] };
                Array.Sort(pts, (x, y) => x.NativeOffset.CompareTo(y.NativeOffset)); // enclosing-point
            }
            else
            {
                // The in-proc EventListener truncated the ILOffsets/NativeOffsets arrays after
                // CountOfMapEntries (the common .NET 8 case — they surface neither as typed
                // arrays nor as a byte[] blob). Record the entry COUNT so the map's presence is
                // still provable; the concrete points stay empty (ilOffset resolves to
                // NO_MAPPING). A future runtime/build that surfaces the arrays decodes above.
                pts = Array.Empty<IlPoint>();
            }
            if (count < 0) count = pts.Length;
            lock (_lock)
            {
                _maps.Add(new IlMap { MethodId = mid, ReJITID = rejit, Entries = count, Points = pts });
                _frozen = null;
            }
        }

        // Decode a u32-array event field to int[], handling every shape an in-proc
        // EventListener may hand us: int[]/uint[] (typed), IEnumerable, or a raw byte[] blob
        // (each u32 little-endian). Returns null if the field is absent/unhandled.
        static int[] DecodeI32Array(object v)
        {
            switch (v)
            {
                case null: return null;
                case int[] ia: return ia;
                case uint[] ua:
                {
                    var r = new int[ua.Length];
                    for (int i = 0; i < ua.Length; i++) r[i] = unchecked((int)ua[i]);
                    return r;
                }
                case byte[] blob:
                {
                    var r = new int[blob.Length / 4];
                    for (int i = 0; i < r.Length; i++) r[i] = BitConverter.ToInt32(blob, i * 4);
                    return r;
                }
                case System.Collections.IEnumerable en:
                {
                    var list = new List<int>();
                    foreach (var o in en) list.Add(Convert.ToInt32(o));
                    return list.ToArray();
                }
                default: return null;
            }
        }

        /// <summary>Stop ingesting events — call right after the traced scope closes.</summary>
        public void Stop() { _stopped = true; }

        public override void Dispose() { _stopped = true; base.Dispose(); }

        /// <summary>Join loads to their IL maps by equal <c>(MethodID, ReJITID)</c> and snapshot
        /// the method ranges sorted by start for O(log n) <see cref="TryResolve"/>. Callback
        /// ordering across threads is not guaranteed, so join HERE (not per-event). Call once
        /// after <see cref="Stop"/>.</summary>
        public void Freeze()
        {
            lock (_lock)
            {
                // ReJITID does not distinguish tiers, so a method may have several loads
                // (Tier0, OSR, Tier1) sharing (MethodID, ReJITID) at DIFFERENT addresses,
                // each with its OWN MethodILToNativeMap (which carries no address). Pair the
                // i-th load with the i-th UNUSED matching map (emission order: each
                // compilation emits load-then-map), so each address range gets its own map.
                var mapUsed = new bool[_maps.Count];
                var a = new Method[_loads.Count];
                for (int i = 0; i < _loads.Count; i++)
                {
                    var ld = _loads[i];
                    IlPoint[] pts = null; int entries = 0; bool found = false;
                    for (int j = 0; j < _maps.Count; j++)
                        if (!mapUsed[j] && _maps[j].MethodId == ld.MethodId && _maps[j].ReJITID == ld.ReJITID)
                        { pts = _maps[j].Points; entries = _maps[j].Entries; mapUsed[j] = true; found = true; break; }
                    if (!found) // fewer maps than loads: reuse the newest matching
                        for (int j = _maps.Count - 1; j >= 0; j--)
                            if (_maps[j].MethodId == ld.MethodId && _maps[j].ReJITID == ld.ReJITID)
                            { pts = _maps[j].Points; entries = _maps[j].Entries; break; }
                    a[i] = new Method { Start = ld.Start, End = ld.Start + ld.Size, Name = ld.Name, MapEntries = entries, Points = pts };
                }
                Array.Sort(a, (x, y) => x.Start.CompareTo(y.Start));
                _frozen = a;
            }
        }

        /// <summary>Resolve absolute <paramref name="addr"/> to its managed
        /// <paramref name="method"/>, <paramref name="nativeOff"/> from the method start, and
        /// enclosing <paramref name="ilOffset"/> (the map entry with the largest
        /// <c>NativeOffset &lt;= nativeOff</c> — the same enclosing-point semantics as the T3
        /// srcmap; pseudo-offsets -1/-2/-3 pass through). Returns false for an address outside
        /// every observed method. Call after <see cref="Freeze"/>.</summary>
        public bool TryResolve(ulong addr, out string method, out uint nativeOff, out int ilOffset)
        {
            method = null; nativeOff = 0; ilOffset = IlNoMapping;
            Method[] s = _frozen;
            if (s == null) { Freeze(); s = _frozen; }
            if (s == null || s.Length == 0) return false;
            int lo = 0, hi = s.Length - 1, hit = -1;
            while (lo <= hi)
            {
                int mid = (int)(((uint)lo + (uint)hi) >> 1);
                if (addr < s[mid].Start) hi = mid - 1;
                else if (addr >= s[mid].End) lo = mid + 1;
                else { hit = mid; break; }
            }
            if (hit < 0) return false;
            method = s[hit].Name;
            nativeOff = (uint)(addr - s[hit].Start);
            var pts = s[hit].Points;
            if (pts != null)
                for (int i = 0; i < pts.Length; i++)
                    if (pts[i].NativeOffset <= nativeOff) ilOffset = pts[i].IlOffset;
                    else break;
            return true;
        }

        /// <summary>Kind-labelled name of an IL offset: the ICorDebugInfo pseudo-offsets
        /// (<c>NO_MAPPING</c>/<c>PROLOG</c>/<c>EPILOG</c>) or <c>IL_0xNN</c> — matching the T3
        /// srcmap report spelling.</summary>
        public static string IlName(int ilOffset)
        {
            switch (ilOffset)
            {
                case IlNoMapping: return "NO_MAPPING";
                case IlProlog: return "PROLOG";
                case IlEpilog: return "EPILOG";
                default: return ilOffset >= 0 ? $"IL_0x{ilOffset:x}" : $"IL({ilOffset})";
            }
        }
    }

    /// <summary>
    /// Data-flow Phase 4 — the LIVE feed for the GC-move canonicalizer (the pure C side
    /// <c>asmtest_gcmove_canonicalize</c> in <c>src/dataflow_gcmove.c</c>). An in-proc
    /// <see cref="EventListener"/> on the CoreCLR runtime provider that captures
    /// <c>GCBulkMovedObjectRanges</c> (emitted only for a COMPACTING GC, under the
    /// GCHeapSurvivalAndMovement keyword) — each event reports how many object ranges the
    /// collector relocated, and (where the runtime surfaces the struct-array payload) the
    /// concrete {old_base → new_base, len} triples. Those triples are exactly the
    /// <c>asmtest_gcmove_t</c> the C canonicalizer consumes to remap a value trace's memory
    /// addresses across a compaction so a managed value's def-use survives the move.
    ///
    /// Construct it BEFORE the traced window so it sees compactions during the window.
    /// Follows the §E3 never-throw / enqueue-light callback discipline (the GC callback can
    /// fire on ANY runtime thread, including a single-stepped one — it only records, it never
    /// P/Invokes). CoreCLR only; a no-op elsewhere.
    /// </summary>
    public sealed class GcMoveMap : EventListener
    {
        const string RuntimeProvider = "Microsoft-Windows-DotNETRuntime";
        // GCKeyword (0x1) tracks GCs; GCHeapSurvivalAndMovement (0x400000) is the one that
        // actually emits GCBulkMovedObjectRanges/GCBulkSurvivingObjectRanges. Enable both.
        const EventKeywords GcMoveKeywords = (EventKeywords)(0x1 | 0x400000);

        /// <summary>One relocated object range: [OldBase, OldBase+Len) moved to
        /// [NewBase, NewBase+Len). Feeds <c>asmtest_gcmove_t</c>.</summary>
        public readonly struct MoveRange
        {
            public readonly ulong OldBase, NewBase, Len;
            public MoveRange(ulong o, ulong nw, ulong l) { OldBase = o; NewBase = nw; Len = l; }
        }

        readonly object _lock = new object();
        readonly List<MoveRange> _ranges = new List<MoveRange>();
        long _eventCount;   // GCBulkMovedObjectRanges events observed (Interlocked)
        long _rangeCount;   // sum of each event's Count field — reliably surfaced even when
                            // the Values struct-array is not (Interlocked)
        volatile bool _stopped;

        /// <summary>GCBulkMovedObjectRanges events observed since construction.</summary>
        public long EventCount => Interlocked.Read(ref _eventCount);
        /// <summary>Total object ranges the collector reported moving (the event Count sum) —
        /// non-zero proves a real compaction was fed, independent of struct-array decode.</summary>
        public long RangeCount => Interlocked.Read(ref _rangeCount);
        /// <summary>The concrete {old,new,len} triples decoded from the Values payload (may be
        /// fewer than <see cref="RangeCount"/> if the runtime does not surface the struct array
        /// to an in-proc listener — see <see cref="TriplesDecoded"/>).</summary>
        public MoveRange[] Ranges { get { lock (_lock) return _ranges.ToArray(); } }
        /// <summary>How many concrete triples were decoded (vs only counted).</summary>
        public int TriplesDecoded { get { lock (_lock) return _ranges.Count; } }

        /// <summary>Freeze the map after the traced window closes.</summary>
        public void Freeze() { _stopped = true; }

        protected override void OnEventSourceCreated(EventSource src)
        {
            if (src != null && src.Name == RuntimeProvider)
                EnableEvents(src, EventLevel.Verbose, GcMoveKeywords);
        }

        protected override void OnEventWritten(EventWrittenEventArgs e)
        {
            // The base ctor can dispatch before this instance's fields init; a listener
            // callback must never throw. Guard + wrap.
            if (_stopped || _lock == null || _ranges == null) return;
            try
            {
                if (e.EventName == null || e.EventName != "GCBulkMovedObjectRanges") return;
                Interlocked.Increment(ref _eventCount);
                var names = e.PayloadNames;
                if (names == null) return;
                object valuesObj = null;
                for (int i = 0; i < names.Count; i++)
                {
                    switch (names[i])
                    {
                        case "Count": Interlocked.Add(ref _rangeCount, Convert.ToInt64(e.Payload[i])); break;
                        case "Values": valuesObj = e.Payload[i]; break;
                    }
                }
                // Best-effort decode of the per-range struct array. In-proc EventListener
                // surfaces a manifest struct-array inconsistently across runtimes: it may be a
                // byte[] blob (each entry {Address:u64, NewAddress:u64, Length:u64} = 24B on
                // x64), an IEnumerable of field maps, or absent. Handle the byte[] form (the
                // common one) and skip silently otherwise — RangeCount still records the move.
                if (valuesObj is byte[] blob)
                {
                    const int ENTRY = 24; // 3 x u64, little-endian x64
                    lock (_lock)
                    {
                        for (int off = 0; off + ENTRY <= blob.Length; off += ENTRY)
                        {
                            ulong oldB = BitConverter.ToUInt64(blob, off);
                            ulong newB = BitConverter.ToUInt64(blob, off + 8);
                            ulong len = BitConverter.ToUInt64(blob, off + 16);
                            if (len != 0)
                                _ranges.Add(new MoveRange(oldB, newB, len));
                        }
                    }
                }
            }
            catch { /* never let a diagnostics callback take the process down */ }
        }
    }

    /// <summary>
    /// §D0.2 — a dependency-free client for the ONE CoreCLR diagnostics-IPC command we
    /// need: <c>EnablePerfMap(JitDump)</c>. It asks the runtime (over the Unix domain socket
    /// the runtime opens at startup) to run down ALL already-loaded methods — JIT'd AND
    /// <b>ReadyToRun</b> (R2R, precompiled BCL like <c>System.Console::WriteLine</c>) — into
    /// <c>/tmp/jit-&lt;pid&gt;.dump</c> and log new ones forward, so a whole-window capture can be
    /// labelled by warm + R2R methods too, with no NuGet package and no launch knob (no
    /// <c>DOTNET_PerfMapEnabled</c>). It hand-rolls the documented <c>DOTNET_IPC_V1</c> wire
    /// format (pinned to .NET 8+) using only BCL sockets, and self-skips (returns false,
    /// never throws) where diagnostics are off (<c>DOTNET_EnableDiagnostics=0</c>) or the
    /// protocol errors — the caller then falls back to the cold-only <see cref="JitMethodMap"/>
    /// result. NB: R2R names go ONLY to the binary jitdump, never the text perf-map, and only
    /// <c>PerfMapType.JitDump</c>/<c>All</c> (not <c>PerfMap</c>) run the R2R rundown.
    /// </summary>
    public static class DiagnosticsIpc
    {
        // DiagnosticsServerCommandSet.Process / ProcessCommandId.{Enable,Disable}PerfMap.
        const byte CommandSet_Process = 0x04;
        const byte CommandId_EnablePerfMap = 0x05;
        const byte CommandId_DisablePerfMap = 0x06;
        // PerfMapType.JitDump(2) — NOT PerfMap(3): only JitDump/All run the ReadyToRun
        // rundown (ReadyToRunInfo::MethodIterator in coreclr PerfMap::Enable), so it names
        // R2R/precompiled BCL methods too; PerfMap(3) writes the text perf-map, which is
        // JIT-only and never contains R2R. JitDump goes to /tmp/jit-<pid>.dump (binary).
        const uint PerfMapType_JitDump = 2;
        const byte CommandSet_Server = 0xFF; // response command set
        const byte CommandId_OK = 0x00;      // response OK
        const int IpcTimeoutMs = 1000;       // never hang the traced scope on a stalled peer
        static readonly byte[] Magic = System.Text.Encoding.ASCII.GetBytes("DOTNET_IPC_V1\0"); // 14 bytes

        /// <summary>Ask the runtime to (rundown +) write <c>/tmp/jit-&lt;pid&gt;.dump</c> — the
        /// jitdump, which includes JIT + R2R methods. Returns true on an OK response; false
        /// on any failure (clean self-skip).</summary>
        public static bool EnablePerfMap() => Send(CommandId_EnablePerfMap, PerfMapType_JitDump);

        /// <summary>Turn perf-map generation back off (paired with <see cref="EnablePerfMap"/>
        /// so it is not left enabled process-wide after the scope). Best-effort.</summary>
        public static bool DisablePerfMap() => Send(CommandId_DisablePerfMap, null);

        static bool Send(byte commandId, uint? payload)
        {
            try
            {
                string sock = FindSocket();
                if (sock == null) return false;
                using var s = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
                s.SendTimeout = IpcTimeoutMs;
                s.ReceiveTimeout = IpcTimeoutMs;
                s.Connect(new UnixDomainSocketEndPoint(sock));

                // IpcMessage: header(20) = magic(14) + size(u16 LE) + cmdSet + cmdId +
                // reserved(u16); optional payload = u32 LE (EnablePerfMap: PerfMapType).
                int total = payload.HasValue ? 24 : 20;
                var msg = new byte[total];
                Array.Copy(Magic, 0, msg, 0, 14);
                msg[14] = (byte)(total & 0xff); msg[15] = (byte)(total >> 8);
                msg[16] = CommandSet_Process;
                msg[17] = commandId;
                // reserved (18,19) stays 0
                if (payload.HasValue)
                {
                    uint v = payload.Value;
                    msg[20] = (byte)v; msg[21] = (byte)(v >> 8);
                    msg[22] = (byte)(v >> 16); msg[23] = (byte)(v >> 24);
                }
                s.Send(msg);

                var hdr = new byte[20];
                if (!ReadExact(s, hdr, 20)) return false;
                int size = hdr[14] | (hdr[15] << 8);
                if (size > 20) { var pl = new byte[size - 20]; ReadExact(s, pl, pl.Length); }
                return hdr[16] == CommandSet_Server && hdr[17] == CommandId_OK;
            }
            catch { return false; }
        }

        /// <summary>The jitdump path the runtime writes for this process.</summary>
        public static string JitDumpPath()
            => Path.Combine(TempDir(), $"jit-{Environment.ProcessId}.dump");

        /// <summary>§D0.2: the runtime writes the jitdump ASYNCHRONOUSLY after an
        /// <see cref="EnablePerfMap"/> ACK, so a close that reads it immediately misses the
        /// R2R rundown still being flushed. Poll the file length until it stops growing across
        /// two short intervals (quiescence) or <paramref name="budgetMs"/> is exhausted — a
        /// bounded, best-effort settle so the reader sees a complete dump. Returns the final
        /// observed length. Never throws (a missing/locked file just returns quickly).</summary>
        public static long WaitJitDumpSettled(string path, int budgetMs)
        {
            const int stepMs = 15; // poll cadence: short enough to add little latency
            long last = -1;
            int stableHits = 0;
            for (int waited = 0; waited < budgetMs; waited += stepMs)
            {
                long len;
                try { len = File.Exists(path) ? new FileInfo(path).Length : 0; }
                catch { return last < 0 ? 0 : last; }
                // Settled = a non-empty file whose size held steady across two polls. The
                // runtime writes the header + records forward; a steady length means the
                // rundown has drained (no new JIT_CODE_LOAD records arriving this window).
                if (len > 0 && len == last)
                {
                    if (++stableHits >= 2) return len;
                }
                else
                {
                    stableHits = 0;
                }
                last = len;
                try { System.Threading.Thread.Sleep(stepMs); } catch { }
            }
            return last < 0 ? 0 : last;
        }

        static string TempDir()
        {
            string t = Environment.GetEnvironmentVariable("TMPDIR");
            return string.IsNullOrEmpty(t) ? "/tmp" : t;
        }

        static string FindSocket()
        {
            int pid = Environment.ProcessId;
            foreach (string dir in new[] { TempDir(), "/tmp" })
            {
                if (!Directory.Exists(dir)) continue;
                string[] hits;
                try { hits = Directory.GetFiles(dir, $"dotnet-diagnostic-{pid}-*-socket"); }
                catch { continue; }
                if (hits.Length == 0) continue;
                // Prefer the NEWEST socket — after PID reuse a stale one from a crashed
                // prior process can share our pid, and its name may sort last.
                string best = hits[0];
                DateTime bestT = DateTime.MinValue;
                foreach (string h in hits)
                {
                    try { DateTime t = File.GetLastWriteTimeUtc(h); if (t >= bestT) { bestT = t; best = h; } }
                    catch { }
                }
                return best;
            }
            return null;
        }

        static bool ReadExact(Socket s, byte[] buf, int n)
        {
            int off = 0;
            while (off < n)
            {
                int r = s.Receive(buf, off, n - off, SocketFlags.None);
                if (r <= 0) return false;
                off += r;
            }
            return true;
        }
    }

    /// <summary>
    /// Call descent (asmtest_descent_t): configure how <see cref="Ptrace.TraceCallEx"/> and
    /// its siblings handle the call-outs the tracer would otherwise step over, and read back
    /// the recorded edges (level &gt;= 1) and nested frames (level &gt;= 2). Frame 0 is the
    /// root region (a superset of the flat trace); descended callees are frames 1..N.
    ///
    /// A resolver/denylist upcall is a MANAGED delegate the native single-step loop calls
    /// back into. The delegate (and a <see cref="GCHandle"/> pinning it) is kept alive as a
    /// field for the handle's lifetime so the GC cannot collect or move the trampoline
    /// mid-single-step; the _ex trace wrappers additionally <c>GC.KeepAlive</c> the Descent
    /// across the native call. <see cref="Dispose"/> is idempotent (NULLs the handle after
    /// asmtest_descent_free, so a double free is a no-op) and releases the pins.
    /// </summary>
    public sealed class Descent : IDisposable
    {
        IntPtr _handle;

        // Registered upcall trampolines + their GC pins, kept for the handle's lifetime.
        HwNative.DescentResolverFn _resolver;
        HwNative.DescentDenylistFn _denylist;
        GCHandle _resolverPin;
        GCHandle _denylistPin;

        /// <summary>
        /// Allocate a descent handle at <paramref name="level"/> (conservative defaults for
        /// depth/budget/watchdog; empty allow-set/denylist). Throws on allocation failure.
        /// </summary>
        public Descent(DescentLevel level = DescentLevel.Off)
        {
            _handle = HwNative.asmtest_descent_new((int)level);
            if (_handle == IntPtr.Zero)
                throw new HwTraceException("asmtest_descent_new failed");
        }

        // The opaque handle, for the descending ptrace entry points.
        internal IntPtr Handle => _handle;

        // ---- configuration (in) ----

        /// <summary>Ceiling on nested descent depth (frame 0 is depth 0). 0 restores the default.</summary>
        public void SetMaxDepth(uint maxDepth) => HwNative.asmtest_descent_set_max_depth(_handle, maxDepth);

        /// <summary>Total single-step instruction budget across all descended frames; 0 = default.</summary>
        public void SetInsnBudget(ulong budget) => HwNative.asmtest_descent_set_insn_budget(_handle, budget);

        /// <summary>Real-time watchdog in milliseconds for a descended run; 0 = default.</summary>
        public void SetWatchdogMs(uint ms) => HwNative.asmtest_descent_set_watchdog_ms(_handle, ms);

        /// <summary>Arm the built-in L3 default denylist (PLT resolver / vdso / GC-JIT
        /// modules; plus blocking-libc entry points on the fork path).</summary>
        public void UseDefaultDenylist() => HwNative.asmtest_descent_use_default_denylist(_handle);

        /// <summary>Add <c>[base, base+len)</c> to the level-2 allow-set. Returns 0 on success, negative on OOM.</summary>
        public int AllowRegion(IntPtr @base, nuint len) =>
            HwNative.asmtest_descent_allow_region(_handle, @base, (UIntPtr)len);

        /// <summary>Add <c>[base, base+len)</c> to the level-3 deny-set. Returns 0 on success, negative on OOM.</summary>
        public int DenyRegion(IntPtr @base, nuint len) =>
            HwNative.asmtest_descent_deny_region(_handle, @base, (UIntPtr)len);

        /// <summary>
        /// Install the level-2/3 resolver: <paramref name="fn"/> maps a callee address to
        /// <c>(descend, base, length)</c> — return <c>(true, base, length)</c> to descend into
        /// the callee region, <c>(false, 0, 0)</c> to step over. The delegate is pinned against
        /// GC for the handle's lifetime.
        /// </summary>
        public void SetResolver(Func<ulong, (bool descend, ulong @base, ulong length)> fn, IntPtr user = default)
        {
            // Wrap the user function in a native-callable trampoline. Keep both the delegate
            // and a GC pin as fields so the runtime-generated thunk survives single-stepping.
            HwNative.DescentResolverFn thunk = (ulong callee, IntPtr u, out ulong baseOut, out ulong lenOut) =>
            {
                var (descend, b, ln) = fn(callee);
                if (descend && ln != 0)
                {
                    baseOut = b;
                    lenOut = ln;
                    return 1;
                }
                baseOut = 0;
                lenOut = 0;
                return 0;
            };
            if (_resolverPin.IsAllocated) _resolverPin.Free();
            _resolver = thunk;
            _resolverPin = GCHandle.Alloc(thunk);
            HwNative.asmtest_descent_set_resolver(_handle, thunk, user);
        }

        /// <summary>
        /// Install the level-3 denylist: <paramref name="fn"/> returns <c>true</c> to REFUSE
        /// descent into a callee, <c>false</c> to allow it. The delegate is pinned against GC
        /// for the handle's lifetime.
        /// </summary>
        public void SetDenylist(Func<ulong, bool> fn, IntPtr user = default)
        {
            HwNative.DescentDenylistFn thunk = (ulong callee, IntPtr u) => fn(callee) ? 1 : 0;
            if (_denylistPin.IsAllocated) _denylistPin.Free();
            _denylist = thunk;
            _denylistPin = GCHandle.Alloc(thunk);
            HwNative.asmtest_descent_set_denylist(_handle, thunk, user);
        }

        // ---- results (out) ----

        /// <summary>Every stepped-over call as a <see cref="DescentEdge"/> (level &gt;= 1).</summary>
        public DescentEdge[] Edges()
        {
            int n = (int)HwNative.asmtest_descent_edges_len(_handle);
            var edges = new DescentEdge[n];
            for (int i = 0; i < n; i++)
                edges[i] = new DescentEdge(
                    HwNative.asmtest_descent_edge_site(_handle, (UIntPtr)(nuint)i),
                    HwNative.asmtest_descent_edge_target(_handle, (UIntPtr)(nuint)i),
                    HwNative.asmtest_descent_edge_depth(_handle, (UIntPtr)(nuint)i));
            return edges;
        }

        /// <summary>The number of recorded frames (1 = frame 0 only; 2+ = descended callees).</summary>
        public int FramesLen() => (int)HwNative.asmtest_descent_frames_len(_handle);

        /// <summary>Absolute base address of frame <paramref name="f"/>.</summary>
        public ulong FrameBase(int f) => HwNative.asmtest_descent_frame_base(_handle, (UIntPtr)(nuint)f);

        /// <summary>Byte length of frame <paramref name="f"/>.</summary>
        public ulong FrameLen(int f) => HwNative.asmtest_descent_frame_len(_handle, (UIntPtr)(nuint)f);

        /// <summary>Nesting depth of frame <paramref name="f"/> (0 = frame 0).</summary>
        public uint FrameDepth(int f) => HwNative.asmtest_descent_frame_depth(_handle, (UIntPtr)(nuint)f);

        /// <summary>Parent frame index of frame <paramref name="f"/> (-1 = root).</summary>
        public int FrameParent(int f) => HwNative.asmtest_descent_frame_parent(_handle, (UIntPtr)(nuint)f);

        /// <summary>The ordered instruction-offset stream recorded in frame <paramref name="f"/>.</summary>
        public ulong[] FrameInsns(int f)
        {
            int n = (int)HwNative.asmtest_descent_frame_insn_count(_handle, (UIntPtr)(nuint)f);
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = HwNative.asmtest_descent_frame_insn_at(_handle, (UIntPtr)(nuint)f, (UIntPtr)(nuint)i);
            return offs;
        }

        /// <summary>The distinct basic-block start offsets recorded in frame <paramref name="f"/>.</summary>
        public ulong[] FrameBlocks(int f)
        {
            int n = (int)HwNative.asmtest_descent_frame_block_count(_handle, (UIntPtr)(nuint)f);
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = HwNative.asmtest_descent_frame_block_at(_handle, (UIntPtr)(nuint)f, (UIntPtr)(nuint)i);
            return offs;
        }

        /// <summary>True if a pool overflowed / a byte failed to decode (the record is incomplete).</summary>
        public bool Truncated() => HwNative.asmtest_descent_truncated(_handle) != 0;

        /// <summary>True if descent stopped at a policy limit (max_depth / budget / recursion cap).</summary>
        public bool DepthCapped() => HwNative.asmtest_descent_depth_capped(_handle) != 0;

        /// <summary>
        /// Free the descent handle (idempotent: NULLs it so a double free is a no-op) and
        /// release the pinned upcall trampolines. The native asmtest_descent_free NULLs
        /// internally too, mirroring the trace-handle discipline.
        /// </summary>
        public void Free()
        {
            if (_handle != IntPtr.Zero)
            {
                HwNative.asmtest_descent_free(_handle);
                _handle = IntPtr.Zero;
            }
            // Safe to drop the trampolines once the native side no longer holds them.
            if (_resolverPin.IsAllocated) _resolverPin.Free();
            if (_denylistPin.IsAllocated) _denylistPin.Free();
            _resolver = null;
            _denylist = null;
        }

        /// <summary>Dispose pattern over <see cref="Free"/>.</summary>
        public void Dispose() => Free();
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

    /// <summary>One stitched hop's position + provenance in the merged trace.</summary>
    public readonly struct StitchHop
    {
        /// <summary>Hop order within the logical operation (0-based).</summary>
        public uint Seq { get; }
        /// <summary>The managed thread the hop was captured on.</summary>
        public int Tid { get; }
        /// <summary>Where this hop's instructions begin in the merged offset stream.</summary>
        public long InsnOffset { get; }
        public StitchHop(uint seq, int tid, long insnOffset)
        { Seq = seq; Tid = tid; InsnOffset = insnOffset; }
    }

    /// <summary>
    /// §D0.4 — follow ONE logical operation across async / thread hops and stitch each
    /// hop's lazy-arm capture into a single ordered trace. An <see cref="AsyncLocal{T}"/>
    /// scope id flows through <c>await</c> continuations (which may resume on a different
    /// thread-pool thread), so slices captured on different threads are recognised as the
    /// same operation; the shipped <c>asmtest_hwtrace_stitch</c> merge core (reached via
    /// <c>asmtest_hwtrace_stitch_handles</c>) concatenates them by hop <see cref="StitchHop.Seq"/>.
    /// This is the first LIVE producer for that core (previously exercised only from
    /// synthetic slices). Each hop reuses the managed-safe lazy-arm path
    /// (<see cref="AsmTrace"/>'s body resolution + <c>call_scoped</c>), so no runtime
    /// machinery is ever stepped.
    /// <code>
    /// using var op = new AsmStitchedTrace();
    /// long r0 = (long)op.Step((Func&lt;long,long,long&gt;)Work, 20, 22);          // hop 0
    /// long r1 = (long)await Task.Run(() => op.Step((Func&lt;long,long,long&gt;)Work, 1, 2)); // hop 1, pool thread
    /// op.Complete();
    /// // op.Path — merged per-hop disassembly; op.Hops — each hop's (seq, tid, offset)
    /// </code>
    /// Only integer <c>(long…)-&gt;long</c> hop signatures are captured in-process today;
    /// any other signature runs uninstrumented for that hop (recorded, never a crash).
    /// </summary>
    public sealed class AsmStitchedTrace : IDisposable
    {
        static readonly AsyncLocal<long> _current = new AsyncLocal<long>();
        static long _nextScopeId;

        sealed class Hop { public IntPtr Handle; public string Text; public uint Seq; public int Tid; public bool Captured; }

        readonly long _scopeId;
        readonly List<Hop> _hops = new List<Hop>();
        readonly object _lock = new object();
        IntPtr _merged = IntPtr.Zero;
        uint _seq;
        bool _completed;
        bool _disposed;

        /// <summary>The scope id flowing through <see cref="AsyncLocal{T}"/> for the active operation (0 = none).</summary>
        public static long CurrentScopeId => _current.Value;
        /// <summary>This operation's scope id.</summary>
        public long ScopeId => _scopeId;
        /// <summary>Honest self-skip reason for any hop that could not be captured in-process ("" when all captured).</summary>
        public string SkipReason { get; private set; } = "";
        /// <summary>Merged per-hop disassembly in seq order (empty until <see cref="Complete"/> / when nothing captured).</summary>
        public string Path { get; private set; } = "";
        /// <summary>True when any stitched hop's trace was truncated.</summary>
        public bool Truncated { get; private set; }
        /// <summary>Per-hop bounds after <see cref="Complete"/>, in seq order.</summary>
        public IReadOnlyList<StitchHop> Hops { get; private set; } = System.Array.Empty<StitchHop>();
        /// <summary>Hops attempted (including any that ran uninstrumented).</summary>
        public int HopCount { get { lock (_lock) return _hops.Count; } }

        public AsmStitchedTrace()
        {
            _scopeId = Interlocked.Increment(ref _nextScopeId);
            _current.Value = _scopeId; // captured into async continuations via ExecutionContext
        }

        /// <summary>
        /// Capture one hop of the operation: lazily arm ONLY <paramref name="target"/>'s
        /// resolved body (<c>call_scoped</c>), tag the slice with this operation's scope id,
        /// the current thread, and the next seq. The call always runs and its result is
        /// returned; an unsupported signature or a failed arm runs uninstrumented (the hop
        /// is recorded, so the seq numbering stays dense) and sets <see cref="SkipReason"/>.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.NoOptimization)]
        public object Step(Delegate target, params object[] args)
        {
            if (target == null) throw new ArgumentNullException(nameof(target));
            if (_disposed) throw new HwTraceException("Step after Dispose");
            if (_completed) throw new HwTraceException("Step after Complete");
            uint seq; lock (_lock) seq = _seq++;
            int tid = Environment.CurrentManagedThreadId;
            _current.Value = _scopeId; // re-assert on whatever context this hop runs on

            if (HwNative.LibAvailable
                && AsmTrace.ResolveJitBody(target, out ulong start, out ulong size) && size != 0
                && AsmTrace.TryBuildIntShim(target, args, out Delegate shim, out long[] iargs)
                && AsmTrace.AutoInitSingleStep() == HwNative.ASMTEST_HW_OK)
            {
                IntPtr handle = HwNative.asmtest_trace_new((UIntPtr)(1 << 16), (UIntPtr)256);
                if (handle != IntPtr.Zero)
                {
                    // REGISTRY-FREE capture ([base,len) direct): a long operation with many
                    // hops must not consume a MAX_REGIONS slot per hop (the fixed 32-slot C
                    // table has no release path — it would exhaust process-wide and silently
                    // disable ALL hwtrace capture). Render THIS hop's body NOW, on this
                    // (possibly pool) thread, while its scope handle is live — the handle is
                    // thread-local and only valid until the next scope on this thread.
                    var scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0, ArmTid = -1 };
                    int rc = HwNative.asmtest_hwtrace_call_scoped_ex(
                        new IntPtr(unchecked((long)start)), (UIntPtr)size, handle,
                        Marshal.GetFunctionPointerForDelegate(shim), iargs, iargs.Length, out long r, ref scope);
                    GC.KeepAlive(shim);
                    if (rc == HwNative.ASMTEST_HW_OK)
                    {
                        string text = RenderScope(scope);
                        lock (_lock) _hops.Add(new Hop { Handle = handle, Text = text, Seq = seq, Tid = tid, Captured = true });
                        return r;
                    }
                    HwNative.asmtest_trace_free(handle);
                    SkipReason = $"hop {seq} did not arm (rc={rc}); ran uninstrumented";
                }
            }
            else if (SkipReason.Length == 0)
                SkipReason = $"hop {seq} signature/resolution unsupported; ran uninstrumented";
            lock (_lock) _hops.Add(new Hop { Handle = IntPtr.Zero, Text = null, Seq = seq, Tid = tid, Captured = false });
            return target.DynamicInvoke(args);
        }

        /// <summary>
        /// Close the operation and stitch every captured hop into one ordered trace by seq.
        /// Populates <see cref="Hops"/>, <see cref="Path"/>, and <see cref="Truncated"/>.
        /// Idempotent; clears the AsyncLocal scope for this context.
        /// </summary>
        public void Complete()
        {
            if (_completed) return;
            _completed = true;
            _current.Value = 0;
            List<Hop> cap;
            lock (_lock) cap = _hops.FindAll(h => h.Captured && h.Handle != IntPtr.Zero);
            if (cap.Count == 0 || !HwNative.LibAvailable) { Path = ""; return; }

            var traces = new IntPtr[cap.Count];
            var scopeIds = new ulong[cap.Count];
            var seqs = new uint[cap.Count];
            var tids = new int[cap.Count];
            var versions = new ulong[cap.Count];
            for (int i = 0; i < cap.Count; i++)
            {
                traces[i] = cap[i].Handle; scopeIds[i] = (ulong)_scopeId;
                seqs[i] = cap[i].Seq; tids[i] = cap[i].Tid; versions[i] = 0;
            }
            _merged = HwNative.asmtest_trace_new((UIntPtr)(1 << 16), (UIntPtr)256);
            if (_merged == IntPtr.Zero) { SkipReason = "merged trace allocation failed"; return; }
            var bounds = new HwNative.SliceBound[cap.Count];
            int rc = HwNative.asmtest_hwtrace_stitch_handles(
                traces, scopeIds, seqs, tids, versions, (UIntPtr)cap.Count, _merged, bounds, out UIntPtr nbUp);
            if (rc != HwNative.ASMTEST_HW_OK) { SkipReason = $"stitch failed (rc={rc})"; return; }
            int nb = (int)nbUp;
            var hops = new List<StitchHop>(nb);
            for (int i = 0; i < nb; i++)
                hops.Add(new StitchHop(bounds[i].Seq, bounds[i].Tid, (long)(ulong)bounds[i].InsnOff));
            Hops = hops;
            Truncated = HwNative.asmtest_emu_trace_truncated(_merged) != 0;

            // Each hop's offsets are RELATIVE TO ITS OWN body, so each hop was rendered at
            // CAPTURE time (in Step, while its handle was live on its own thread) and the
            // text stored; concatenate in seq order. The merged offset stream + bounds are
            // the structural join; Path is the readable per-hop disassembly.
            var sb = new StringBuilder();
            foreach (var b in hops)
            {
                Hop h = cap.Find(x => x.Seq == b.Seq);
                sb.Append($"; hop {b.Seq} (tid {b.Tid}, +{b.InsnOffset}):\n");
                if (h != null && !string.IsNullOrEmpty(h.Text)) sb.Append(h.Text);
            }
            Path = sb.ToString();
        }

        // Render a hop's body from its live scope handle (call this on the CAPTURING thread
        // immediately after the call — the ss frame is thread-local and short-lived).
        static string RenderScope(HwNative.HwScope scope)
        {
            if (!HwNative.asmtest_disas_available()) return "";
            int need = HwNative.asmtest_hwtrace_render_scope(scope, null, UIntPtr.Zero);
            if (need <= 0) return "";
            var buf = new byte[need + 1];
            HwNative.asmtest_hwtrace_render_scope(scope, buf, (UIntPtr)(need + 1));
            return Encoding.ASCII.GetString(buf, 0, need);
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (!_completed) Complete();
            if (!HwNative.LibAvailable) return;
            lock (_lock)
                foreach (var h in _hops)
                    if (h.Handle != IntPtr.Zero) HwNative.asmtest_trace_free(h.Handle);
            if (_merged != IntPtr.Zero) { HwNative.asmtest_trace_free(_merged); _merged = IntPtr.Zero; }
        }
    }

    /// <summary>
    /// §Z4 (managed-wholewindow-compose T11) — the AMBIENT escalation of
    /// <see cref="AsmStitchedTrace"/>: follow ONE logical operation across async / thread
    /// hops with ZERO calls in the user's body. A static <see cref="AsyncLocal{T}"/>
    /// constructed with a value-changed handler fires synchronously on the exact OS thread
    /// performing every execution-context transition, so the producer opens a per-thread
    /// Intel-PT slice when the flow LANDS on a thread (attach) and decodes-at-disable when
    /// it LEAVES (detach), then stitches the slices at close.
    /// <code>
    /// using (var op = new AsmAmbientStitchedTrace())
    ///     await SomeAsyncWork();     // each thread the flow touches becomes one PT slice
    /// // op.Hops — each slice's (seq, tid, offset); op.Path — per-slice disassembly
    /// </code>
    /// PT-ONLY by construction: the per-thread hop is a per-tid <c>intel_pt</c> event (the
    /// T10 hop surface), and the §D3 single-thread stepper follows one thread and cannot
    /// exercise a hop. Off Intel PT (and its <c>perf_event_paranoid&lt;0</c>/CAP_PERFMON)
    /// the scope sets <see cref="SkipReason"/> and runs the body UNINSTRUMENTED — never a
    /// hard failure, never in-process TF single-step. Unlike <see cref="AsmStitchedTrace.Step"/>
    /// there are no per-hop calls; the price is the hardware gate. One operation per async
    /// context (not nested), mirroring the explicit-Step sibling. The full live chain (a
    /// live managed runtime ON bare-metal Intel PT) has no CI coverage — a real hardware
    /// gate; the hook→tag→merge LOGIC is CI-covered everywhere via the T12 stub-capture twin.
    /// </summary>
    public sealed class AsmAmbientStitchedTrace : IDisposable
    {
        /// <summary>The per-thread PT hop capture seam. The real implementation P/Invokes the
        /// T10 hop surface; the T12 twin injects a scripted stub so the hook→park→stitch
        /// logic is host-testable with no PT hardware.</summary>
        internal interface IHopCapture
        {
            bool Available { get; }
            IntPtr Open(int tid);                                             // hop ctx (Zero on failure)
            int CloseDecode(IntPtr ctx, IntPtr img, ulong when, IntPtr trace); // rc; fills `trace`
        }

        // The real capture: a per-tid intel_pt AUX event (T10), decode-at-disable.
        sealed class PtHopCapture : IHopCapture
        {
            public bool Available =>
                HwNative.LibAvailable &&
                HwNative.asmtest_hwtrace_available((int)HwBackend.IntelPt) != 0;
            public IntPtr Open(int tid) =>
                HwNative.asmtest_hwtrace_pt_hop_open(tid, out IntPtr ctx) == HwNative.ASMTEST_HW_OK
                    ? ctx : IntPtr.Zero;
            public int CloseDecode(IntPtr ctx, IntPtr img, ulong when, IntPtr trace) =>
                HwNative.asmtest_hwtrace_pt_hop_close(ctx, img, when, trace);
        }

        // The boxed value that flows through ExecutionContext. Identity (not content) drives
        // the handler: a transition to/from THIS instance fires an attach/detach pair.
        sealed class AmbientScope { public readonly AsmAmbientStitchedTrace Owner; public AmbientScope(AsmAmbientStitchedTrace o) { Owner = o; } }

        sealed class OpenHop { public IntPtr Ctx; public uint Seq; public int Tid; }
        sealed class Parked { public IntPtr Trace; public uint Seq; public int Tid; public ulong Version; public string Text; }

        // ONE static AsyncLocal + handler serves every operation; the handler routes each
        // transition to the right instance via the boxed value's Owner.
        static readonly AsyncLocal<AmbientScope> _current = new AsyncLocal<AmbientScope>(OnAmbientChanged);

        readonly IHopCapture _capture;
        readonly JitMethodMap _map;            // §Z3 code-image for decode-at-version (null for the stub twin)
        readonly AmbientScope _scope;          // this operation's boxed value (null when uninstrumented)
        readonly ConcurrentDictionary<int, OpenHop> _openHops = new ConcurrentDictionary<int, OpenHop>();
        readonly ConcurrentQueue<Parked> _parked = new ConcurrentQueue<Parked>();
        int _seq;                              // Interlocked hop counter (lock-free hot path)
        IntPtr _merged = IntPtr.Zero;
        bool _completed;
        bool _disposed;

        // The close-out gate. `_completed` alone cannot order the ambient handler against
        // Complete/Dispose: the handler fires on pool threads the flow may still be leaving
        // AFTER the `using` closed, so (a) an attach that read `_completed == false` could
        // publish a hop into _openHops after Complete's drain had already passed it, and
        // (b) a detach could be inside CloseHop — reading _map's code-image — while Dispose
        // freed that image underneath it. Both are use-after-free (the C-side codeimage lock
        // guards a LIVE image, not a freed one). _gate makes the `_completed`-check and the
        // _openHops add/remove atomic with each other; _inFlight counts hop closes currently
        // touching _map/_parked so Dispose can wait them out before freeing either.
        readonly object _gate = new object();
        int _inFlight;
        int _opening;   // attaches past the _completed check but not yet parked in _openHops

        /// <summary>Honest self-skip reason when the ambient producer could not run ("" when it ran).</summary>
        public string SkipReason { get; private set; } = "";
        /// <summary>Merged per-slice disassembly in seq order (empty until <see cref="Complete"/>).</summary>
        public string Path { get; private set; } = "";
        /// <summary>True when any stitched slice's trace was truncated.</summary>
        public bool Truncated { get; private set; }
        /// <summary>Per-slice bounds after <see cref="Complete"/>, in seq order.</summary>
        public IReadOnlyList<StitchHop> Hops { get; private set; } = System.Array.Empty<StitchHop>();
        /// <summary>Slices parked so far (each a successfully decoded per-thread hop).</summary>
        public int HopCount => _parked.Count;

        /// <summary>Public entry: real per-tid PT capture (the T10 hop surface).</summary>
        public AsmAmbientStitchedTrace() : this(null) { }

        /// <summary>Test seam (T12): inject a scripted <see cref="IHopCapture"/> so the
        /// hook→tag→merge logic is CI-covered with no PT hardware. Not part of the public
        /// contract — the shipped ambient producer always uses the real per-tid capture.</summary>
        internal AsmAmbientStitchedTrace(IHopCapture captureForTest)
        {
            _capture = captureForTest ?? new PtHopCapture();
            if (!_capture.Available)
            {
                SkipReason = "ambient stitching needs Intel PT per-thread events — the " +
                             "explicit-Step AsmStitchedTrace works everywhere";
                return; // body runs uninstrumented: no scope flows, the handler never sees us
            }
            // The real capture tracks JIT'd bytes so each hop decodes at the version live in
            // its window; the stub scripts offsets directly and needs no image.
            if (captureForTest == null) _map = new JitMethodMap(trackBytes: true);
            _scope = new AmbientScope(this);
            _current.Value = _scope;   // ctor set: ThreadContextChanged==false (bookkeeping only, no attach fires)
            OnThreadAttach();          // so open the FIRST hop on the ctor thread explicitly (the sync slice)
        }

        // The ambient handler: fires INLINE on the transitioning OS thread for every
        // execution-context change (RunInternal on each await resumption, the thread-pool
        // dispatch loop, ResetThreadPoolThread). Allocation-light, lock-free, and swallows
        // everything — a dead producer only loses stitching, never the process.
        static void OnAmbientChanged(AsyncLocalValueChangedArgs<AmbientScope> args)
        {
            try
            {
                if (!args.ThreadContextChanged) return;        // explicit Value set -> bookkeeping only
                if (args.CurrentValue != null)
                    args.CurrentValue.Owner.OnThreadAttach();  // the flow LANDED on this thread
                else if (args.PreviousValue != null)
                    args.PreviousValue.Owner.OnThreadDetach(); // the flow LEFT this thread (pool reset)
            }
            catch { }
        }

        void OnThreadAttach()
        {
            if (_completed) return;                            // fast path; re-checked under _gate
            int tid = HwNative.asmtest_ss_self_tid();
            // One PT event per tid: if a slice is already open on THIS thread for this op,
            // keep it (resumed execution rides the same slice). A given tid runs on one
            // thread at a time, so this check-then-open needs no lock.
            if (_openHops.ContainsKey(tid)) return;
            // RESERVE under the gate before opening anything. Opening first and discarding
            // the loser afterwards would break the producer's own invariant that every
            // opened hop is stitched (the T12 twin asserts exactly that, and caught it):
            // the capture would have recorded an attach that never produces a slice. So the
            // `_completed` re-check happens BEFORE Open, and `_opening` holds Complete's
            // drain back until this reservation has landed in _openHops.
            lock (_gate)
            {
                if (_completed) return;                        // closed out: never open at all
                _opening++;
            }
            try
            {
                IntPtr ctx = _capture.Open(tid);               // slow (perf_event_open) — off the gate
                if (ctx == IntPtr.Zero) return;                // open failed -> this hop uninstrumented
                uint seq = unchecked((uint)(Interlocked.Increment(ref _seq) - 1));
                lock (_gate) { _openHops[tid] = new OpenHop { Ctx = ctx, Seq = seq, Tid = tid }; }
            }
            finally { lock (_gate) { _opening--; Monitor.PulseAll(_gate); } }
        }

        void OnThreadDetach()
        {
            int tid = HwNative.asmtest_ss_self_tid();
            OpenHop h;
            lock (_gate)
            {
                if (!_openHops.TryRemove(tid, out h)) return;  // Complete already took it
                _inFlight++;                                   // hold _map/_parked open
            }
            try { CloseHop(h); }
            finally { lock (_gate) { _inFlight--; Monitor.PulseAll(_gate); } }
        }

        // Decode-at-disable this hop against the version live in the window, then park its
        // trace with (seq, tid, version). Runs inline on the detaching thread.
        void CloseHop(OpenHop h)
        {
            IntPtr img = _map != null ? _map.ImageHandle : IntPtr.Zero;
            ulong when = _map != null ? _map.ImageNow : 0;
            IntPtr trace = HwNative.asmtest_trace_new((UIntPtr)(1 << 16), (UIntPtr)256);
            if (trace == IntPtr.Zero) { _capture.CloseDecode(h.Ctx, IntPtr.Zero, 0, IntPtr.Zero); return; } // teardown only
            int rc = _capture.CloseDecode(h.Ctx, img, when, trace);
            if (rc != HwNative.ASMTEST_HW_OK) { HwNative.asmtest_trace_free(trace); return; } // decode failed -> drop
            if (HwNative.asmtest_emu_trace_truncated(trace) != 0) Truncated = true;
            string text = RenderSlice(img, when, trace);
            _parked.Enqueue(new Parked { Trace = trace, Seq = h.Seq, Tid = h.Tid, Version = when, Text = text });
        }

        static string RenderSlice(IntPtr img, ulong when, IntPtr trace)
        {
            if (img == IntPtr.Zero || !HwNative.asmtest_disas_available()) return "";
            int need = HwNative.asmtest_hwtrace_render_versioned(img, when, trace, null, UIntPtr.Zero);
            if (need <= 0) return "";
            var buf = new byte[need + 1];
            HwNative.asmtest_hwtrace_render_versioned(img, when, trace, buf, (UIntPtr)(need + 1));
            return Encoding.ASCII.GetString(buf, 0, need);
        }

        /// <summary>Close the operation: close any hop still open, then stitch every parked
        /// slice into one ordered trace by seq. Populates <see cref="Hops"/>, <see cref="Path"/>,
        /// <see cref="Truncated"/>. Idempotent; clears the ambient scope for this context.</summary>
        public void Complete()
        {
            // Close every hop still open — the closing thread's, plus any that never detached
            // (a thread the flow left without a reset). Taking them under _gate (with
            // `_completed` set in the SAME critical section) makes the drain atomic against
            // both a detach racing Complete and an attach racing it, so no hop is closed
            // twice and none is published after the drain. The closes themselves run OFF the
            // gate — CloseHop decodes, which must never block the ambient handler.
            var take = new List<OpenHop>();
            lock (_gate)
            {
                if (_completed) return;
                _completed = true;
                // Setting _completed stops NEW attaches; these are the ones already past that
                // check and still opening. Wait for them to land so the drain below sees every
                // hop that was ever opened — otherwise one arrives after the drain and is never
                // stitched. Bounded, so a wedged perf_event_open cannot hang Complete.
                long deadline = Environment.TickCount64 + 5000;
                while (_opening > 0)
                {
                    int left = (int)(deadline - Environment.TickCount64);
                    if (left <= 0 || !Monitor.Wait(_gate, left)) break;
                }
                foreach (var kv in _openHops)
                    if (_openHops.TryRemove(kv.Key, out OpenHop h)) take.Add(h);
                _inFlight += take.Count;
            }
            try { foreach (var h in take) CloseHop(h); }
            finally { lock (_gate) { _inFlight -= take.Count; Monitor.PulseAll(_gate); } }
            if (_scope != null) _current.Value = null; // ThreadContextChanged==false -> no detach fires

            // A detach on a pool thread can be inside CloseHop right now, about to enqueue its
            // slice. Snapshotting _parked without waiting drops that slice from the stitch even
            // though the hop was opened AND closed — a pre-existing defect this plan's new
            // ambient-stress lane exposed (the T12 twin's "every attached hop stitched" went
            // 4 vs 5 under repetition, reproduced against the unmodified upstream producer).
            // Bounded, like the other waits: a stalled hop must not hang Complete.
            lock (_gate)
            {
                long dl = Environment.TickCount64 + 5000;
                while (_inFlight > 0)
                {
                    int left = (int)(dl - Environment.TickCount64);
                    if (left <= 0 || !Monitor.Wait(_gate, left)) break;
                }
            }

            var parked = _parked.ToArray();
            System.Array.Sort(parked, (a, b) => a.Seq.CompareTo(b.Seq));
            if (parked.Length == 0 || !HwNative.LibAvailable) { Path = ""; return; }

            var traces = new IntPtr[parked.Length];
            var scopeIds = new ulong[parked.Length];
            var seqs = new uint[parked.Length];
            var tids = new int[parked.Length];
            var versions = new ulong[parked.Length];
            for (int i = 0; i < parked.Length; i++)
            {
                traces[i] = parked[i].Trace; scopeIds[i] = 0;
                seqs[i] = parked[i].Seq; tids[i] = parked[i].Tid; versions[i] = parked[i].Version;
            }
            _merged = HwNative.asmtest_trace_new((UIntPtr)(1 << 16), (UIntPtr)256);
            if (_merged == IntPtr.Zero) { SkipReason = "merged trace allocation failed"; return; }
            var bounds = new HwNative.SliceBound[parked.Length];
            int rc = HwNative.asmtest_hwtrace_stitch_handles(
                traces, scopeIds, seqs, tids, versions, (UIntPtr)parked.Length, _merged, bounds, out UIntPtr nbUp);
            if (rc != HwNative.ASMTEST_HW_OK) { SkipReason = $"stitch failed (rc={rc})"; return; }
            int nb = (int)nbUp;
            var hops = new List<StitchHop>(nb);
            for (int i = 0; i < nb; i++)
                hops.Add(new StitchHop(bounds[i].Seq, bounds[i].Tid, (long)(ulong)bounds[i].InsnOff));
            Hops = hops;
            if (HwNative.asmtest_emu_trace_truncated(_merged) != 0) Truncated = true;

            var sb = new StringBuilder();
            foreach (var b in hops)
            {
                Parked p = System.Array.Find(parked, x => x.Seq == b.Seq);
                sb.Append($"; hop {b.Seq} (tid {b.Tid}, +{b.InsnOffset}):\n");
                if (p != null && !string.IsNullOrEmpty(p.Text)) sb.Append(p.Text);
            }
            Path = sb.ToString();
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (!_completed) Complete();
            // A detach on a pool thread the flow is still leaving can be inside CloseHop right
            // now, decoding against _map's code-image and enqueuing onto _parked. Freeing
            // either underneath it is a use-after-free, so wait the in-flight closes out
            // first. Bounded: on timeout we LEAK rather than free — a stalled hop must never
            // turn a teardown into a crash.
            lock (_gate)
            {
                long deadline = Environment.TickCount64 + 5000;
                while (_inFlight > 0)
                {
                    int left = (int)(deadline - Environment.TickCount64);
                    if (left <= 0 || !Monitor.Wait(_gate, left)) break;
                }
                if (_inFlight > 0) return; // still busy: leak the image/traces, never free under it
            }
            if (_map != null) _map.Dispose();
            if (!HwNative.LibAvailable) return;
            foreach (var p in _parked)
                if (p.Trace != IntPtr.Zero) HwNative.asmtest_trace_free(p.Trace);
            if (_merged != IntPtr.Zero) { HwNative.asmtest_trace_free(_merged); _merged = IntPtr.Zero; }
        }
    }
}
