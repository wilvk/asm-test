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
using System.Collections.Generic;
using System.Diagnostics.Tracing;
using System.Globalization;
using System.IO;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
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

        // Must match include/asmtest_hwtrace.h exactly (ASMTEST_HW_*). NOTE: ESTATE is -2,
        // NOT -4 — an earlier value was wrong but never exercised, since callers only hit
        // begin_window's ESTATE path once the empty-ctor scope started auto-initing the tier.
        public const int ASMTEST_HW_OK = 0;
        public const int ASMTEST_HW_EINVAL = -1;   // bad argument
        public const int ASMTEST_HW_ESTATE = -2;   // tier not up / wrong state
        public const int ASMTEST_HW_EUNAVAIL = -3; // no hardware-trace backend available
        public const int ASMTEST_HW_ENOSYS = -5;   // not built / not this platform
        public const int ASMTEST_HW_EFULL = -6;    // this thread's range stack is full
        public const int ASMTEST_HW_EDECODE = -8;  // capture/decode failure
        public const int SINGLESTEP = 3;

        // asmtest_hwtrace_scope_t: a region-free (§Z0/§Z1) scope handle — an index into
        // the calling thread's range stack tagged with a generation counter. Marshals
        // as two consecutive C uint32.
        [StructLayout(LayoutKind.Sequential)]
        public struct HwScope
        {
            public uint Idx;
            public uint Gen;
        }

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

        // §Z0/§Z1 — the region-free (empty-ctor) whole-window scope surface.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_begin_window(IntPtr trace, ref HwScope @out);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_end_window(HwScope handle, IntPtr trace);
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_render_window(HwScope handle, byte[] buf, UIntPtr buflen);

        // §Z3 — version-aware render: disassemble trace's ABSOLUTE addresses against a
        // code-image as of `when` (asmtest_codeimage_now), instead of live self memory.
        [DllImport(HWTRACE)] public static extern int asmtest_hwtrace_render_versioned(
            IntPtr img, ulong when, IntPtr trace, byte[] buf, UIntPtr buflen);

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
                if (Available(b)) { parts.Add($"using {b}"); break; }
                parts.Add($"{b} unavailable ({SkipReason(b)})");
            }
            if (!parts[parts.Count - 1].StartsWith("using", StringComparison.Ordinal))
                parts.Add(Ptrace.Available()
                    ? "out-of-process ptrace stepper available as the fallback"
                    : $"no ptrace fallback ({Ptrace.SkipReason()})");
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
            lock (TierLock)
            {
                var opts = new HwNative.Options { Backend = (int)backend };
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
    public sealed class AsmTrace : IDisposable
    {
        readonly string _name;
        readonly IntPtr _handle;
        readonly bool _emit;
        readonly bool _wholeWindow; // §Z0/§Z1: the region-free empty-ctor form
        readonly bool _renderPath;  // §Z5 opt-in: render the whole window into Path
        readonly bool _rundownRequested; // §D0.2: pair DisablePerfMap with the REQUEST
        readonly JitMethodMap _map; // §D0.1: managed-method labelling (byMethod only)
        readonly Delegate _target;  // §D0.3: the named method (Method(...) scopes only)
        readonly IntPtr _methodBase; // §D0.3: resolved JIT'd body base
        readonly UIntPtr _methodLen; // §D0.3: resolved JIT'd body length
        readonly bool _oop;         // §D3: route Invoke through the stealth stepper
        bool _began;                // an in-process begin succeeded (Dispose must end)
        readonly int _armTid;       // §0.2/B5: managed thread that armed (complements the native OS-tid check)
        HwNative.HwScope _scope;    // region-free scope handle (whole-window only)
        bool _disposed;

        /// <summary>The rendered assembly listing (populated on close).</summary>
        public string Path { get; private set; }
        /// <summary>True if the scope armed (a backend was available and begin succeeded).</summary>
        public bool Armed { get; private set; }
        /// <summary>True if the close hopped OS threads / the capture overflowed (§0.2/§1/§Z4).</summary>
        public bool Truncated { get; private set; }
        /// <summary>§Z5: when the scope did NOT arm, the honest human-readable reason
        /// (no faithful backend, tier not up, not Linux). Empty when armed.</summary>
        public string SkipReason { get; private set; } = "";
        /// <summary>§Z1: the raw ABSOLUTE addresses captured by a whole-window scope, in
        /// execution order (empty for the region-scoped form). Range-classify these
        /// against known native regions to tell multiple leaves apart.</summary>
        public ulong[] Addresses { get; private set; } = System.Array.Empty<ulong>();
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
        /// <paramref name="nameSubstring"/>; 0 unless this is a <c>byMethod</c> scope.</summary>
        public long InstructionsIn(string nameSubstring)
        {
            long n = 0;
            foreach (var m in Methods)
                if (m.Name.Contains(nameSubstring)) n += m.Count;
            return n;
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
        public AsmTrace(bool emit = true, bool byMethod = false, bool withRundown = false,
                        bool renderPath = false,
                        [CallerMemberName] string member = null,
                        [CallerLineNumber] int line = 0)
        {
            _name = ScopeName(member, line);
            _emit = emit;
            _wholeWindow = true;
            _renderPath = renderPath;
            _armTid = Environment.CurrentManagedThreadId;
            // Honor the "never throws" contract: with no native lib loaded, the first P/Invoke
            // (asmtest_trace_new) would throw DllNotFoundException. Self-skip cleanly (Armed
            // stays false, SkipReason set) so `using (new AsmTrace())` degrades, not crashes —
            // the same LibAvailable guard HwTrace.Available/Resolve/Auto already use.
            if (!HwNative.LibAvailable)
            {
                SkipReason = "libasmtest_hwtrace not loaded — set ASMTEST_HWTRACE_LIB or build build/libasmtest_hwtrace.so";
                return;
            }
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
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0 };
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
                // §Z0: the tier was never inited — auto-init the portable single-step backend
                // and retry once, so `using (new AsmTrace())` needs no explicit HwTrace.Init.
                // This fires ONLY when nothing is inited (begin_window returns ESTATE); an
                // explicit Init of any backend is preserved (begin_window then returns OK or
                // EUNAVAIL, never ESTATE). begin_window on ESTATE is a side-effect-free no-op,
                // so the retry is safe.
                int initRc = AutoInitSingleStep();
                rc = initRc == HwNative.ASMTEST_HW_OK
                    ? HwNative.asmtest_hwtrace_begin_window(_handle, ref _scope)
                    : initRc;
            }
            Armed = rc == HwNative.ASMTEST_HW_OK;
            if (!Armed) SkipReason = WholeWindowSkipReason(rc);
        }

        // §Z0: lazily bring up the portable single-step tier for the empty-ctor whole-window
        // scope, so callers need no explicit HwTrace.Init. Serialized via the shared TierLock;
        // skips the native init if the tier is already up (explicit Init or a prior scope) so
        // it can never re-init over a live capture. Returns the native init status.
        static int AutoInitSingleStep()
        {
            lock (HwTrace.TierLock)
            {
                if (HwTrace.TierInited) return HwNative.ASMTEST_HW_OK;
                var opts = new HwNative.Options { Backend = (int)HwBackend.SingleStep };
                int rc = HwNative.asmtest_hwtrace_init(ref opts);
                if (rc == HwNative.ASMTEST_HW_OK) HwTrace.TierInited = true;
                return rc;
            }
        }

        // §Z5: map a begin_window / auto-init failure to an actionable, honest message. The
        // ctor auto-inits the single-step tier, so a failure means the host cannot run it.
        // The EUNAVAIL case appends the §Z5.2 composed LADDER (which tiers were probed and
        // why each self-skipped), so the message names the missing capability, not just "no".
        static string WholeWindowSkipReason(int rc) => rc switch
        {
            HwNative.ASMTEST_HW_EUNAVAIL =>
                "single-step whole-window tier unavailable on this host (needs x86-64 Linux; "
                + "whole-window is single-step only, so a non-single-step backend inited via "
                + "HwTrace.Init will not arm it) — " + HwTrace.DegradationNote(),
            HwNative.ASMTEST_HW_ESTATE => "hwtrace tier not up (auto-init failed)",
            HwNative.ASMTEST_HW_ENOSYS => "whole-window scope is Linux/x86-64 only",
            _ => "whole-window scope did not arm",
        };

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
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0 };
            int brc = HwNative.asmtest_hwtrace_begin_scope(_name, ref _scope);
            Armed = _began = brc == HwNative.ASMTEST_HW_OK;
            // §Z5: the region form has no auto-init retry (only the whole-window ctor
            // does), so say why the arm failed rather than leaving SkipReason empty.
            if (!Armed)
                SkipReason = brc == HwNative.ASMTEST_HW_ESTATE
                    ? "hwtrace tier not up — call HwTrace.Init (the region scope does not auto-init)"
                    : $"region scope did not arm (rc={brc})";
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
            ulong start = 0, size = 0;
            bool found = false;
            if (HwNative.LibAvailable)
            {
                // Map BEFORE PrepareMethod: an in-proc listener only sees methods JIT'd
                // after it enables the keyword (§D0.1 caveat i).
                using (var map = new JitMethodMap())
                {
                    try { RuntimeHelpers.PrepareMethod(target.Method.MethodHandle); }
                    catch { /* generic/abstract handles: fall through to the rundown */ }
                    string typed = (target.Method.DeclaringType?.Name ?? "") + "." + target.Method.Name;
                    found = map.TryResolveEntry(typed, out start, out size)
                         || map.TryResolveEntry("." + target.Method.Name, out start, out size);
                    if (!found && DiagnosticsIpc.EnablePerfMap())
                    {
                        // WARM body (JIT'd before this call): PrepareMethod was a no-op and
                        // no event fired — the jitdump rundown names it ("Type::Method(sig)").
                        map.LoadJitDump(DiagnosticsIpc.JitDumpPath());
                        DiagnosticsIpc.DisablePerfMap();
                        found = map.TryResolveEntry("::" + target.Method.Name + "(", out start, out size)
                             || map.TryResolveEntry(typed, out start, out size);
                    }
                }
            }
            return new AsmTrace(target, start, size, found, emit, outOfProcess, member, line);
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
            HwNative.asmtest_hwtrace_register_region(_name, _methodBase, _methodLen, _handle);
            if (_oop) return; // §D3: armed lazily by Invoke (the helper steps, not TF)
            _scope = new HwNative.HwScope { Idx = 0xffffffffu, Gen = 0 };
            int rc = HwNative.asmtest_hwtrace_begin_scope(_name, ref _scope);
            if (rc == HwNative.ASMTEST_HW_ESTATE)
            {
                // §Z0 parity with the empty ctor: auto-init the portable tier and retry.
                rc = AutoInitSingleStep() == HwNative.ASMTEST_HW_OK
                    ? HwNative.asmtest_hwtrace_begin_scope(_name, ref _scope)
                    : rc;
            }
            Armed = _began = rc == HwNative.ASMTEST_HW_OK;
            if (!Armed) SkipReason = $"named-method scope did not arm (rc={rc})";
        }

        /// <summary>
        /// §D0.3: invoke the named method inside this scope through the library's own
        /// call site. In-process, this is a plain (indirect) delegate invocation — the
        /// armed region records exactly the body's executed offsets, and the
        /// reflection overhead around it is out-of-range noise the region filter drops.
        /// With <c>outOfProcess: true</c> (§D3), the call runs under the stealth
        /// stepper: a helper process reverse-attaches, steps the body out of band, and
        /// this thread is never armed with EFLAGS.TF; on a refused attach the method
        /// still runs (uninstrumented) and <see cref="SkipReason"/> says why.
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining | MethodImplOptions.NoOptimization)]
        public object Invoke(params object[] args)
        {
            if (_target == null)
                throw new HwTraceException("Invoke is only valid on an AsmTrace.Method(...) scope");
            if (_disposed)
                throw new HwTraceException("Invoke after Dispose");
            if (!_oop || !HwNative.LibAvailable || _handle == IntPtr.Zero)
                return _target.DynamicInvoke(args);
            object ret = null;
            Exception thrown = null;
            bool ran = false;
            HwNative.StealthRunFn cb = _ =>
            {
                ran = true;
                try { ret = _target.DynamicInvoke(args); }
                catch (Exception e) { thrown = e; }
            };
            int rc = HwNative.asmtest_hwtrace_stealth_trace(
                _methodBase, _methodLen, _handle, out long _, cb, IntPtr.Zero);
            GC.KeepAlive(cb);
            if (rc == HwNative.ASMTEST_HW_OK) Armed = true;
            else SkipReason = $"stealth stepper did not run (rc={rc}) — Yama ptrace_scope, "
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
            if (_disposed) return;
            _disposed = true;
            // With no native lib the ctor self-skipped and allocated no native resources; skip
            // the (P/Invoking) teardown so Dispose also honors "never throws".
            if (!HwNative.LibAvailable) { Path = ""; return; }
            // Always render (size-then-allocate) to populate Path — emit gates only the
            // sink write, not producing Path. The whole-window (§Z1) path is handle-keyed
            // and renders the recorded ABSOLUTE addresses from live self memory; the
            // region path is name-keyed and renders base-relative offsets.
            int need;
            need = 0;
            if (_wholeWindow)
            {
                HwNative.asmtest_hwtrace_end_window(_scope, _handle);
                // §D0.1: freeze the JIT map to exactly the methods seen while the scope
                // was open (before the readback/classification below JITs anything more).
                // §Z3: also pin the code-image version AT CLOSE — the labelled
                // disassembly below decodes against it, so a method that re-tiers/moves
                // AFTER the window still renders the bytes that actually ran.
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
                // §D0.1: attribute the captured addresses to managed methods — DATA only;
                // the caller decides how to present Methods / LabelledInstructions.
                if (_map != null)
                {
                    // §D0.2: fold in the jitdump rundown (WARM + R2R methods too) if it was
                    // enabled; a missing/partial file just leaves the cold-only entries.
                    if (RundownEnabled)
                        _map.LoadJitDump(DiagnosticsIpc.JitDumpPath());
                    _map.Freeze();
                    bool disasOk = HwNative.asmtest_disas_available();
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
                        // Disassemble the labelled instruction — §Z3 versioned-first (the
                        // code-image bytes as of the window close), falling back to LIVE
                        // memory where the address has no tracked version (still mapped;
                        // closing the scope does not unload managed methods).
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
            }
            else
            {
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
        /// <summary>Captured instructions attributed to the method.</summary>
        public long Count { get; }

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

        struct Entry { public ulong Start, End; public string Name; }

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
}
