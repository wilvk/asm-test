// DrTrace.cs — asm-test .NET binding: the in-process DynamoRIO native-trace tier.
//
// The peer of Asmtest.cs for the optional native-trace tier (see
// include/asmtest_drtrace.h and docs/native-tracing.md). Where the emulator tier
// (Asmtest.Emu / Asmtest.Trace) traces isolated guest bytes, NativeTrace traces
// host-native code as it runs *inside this .NET process*: initialize DynamoRIO
// once at startup, materialize host-native machine code, mark a region, call into
// it through a function pointer, and read back basic-block coverage / the ordered
// instruction stream.
//
// All P/Invoke (DllImport) stays inside the Native class, mirroring Asmtest.cs.
// The logical library "asmtest_drapp" is resolved by a DllImportResolver:
// ASMTEST_DRAPP_LIB -> <repo>/build/libasmtest_drapp.so -> NativeLibrary.Load.
// libdynamorio is dlopen()ed lazily by the C side after the client is configured,
// so nothing here links DynamoRIO. Advanced, Linux-x86-64-only, opt-in:
// NativeTrace.Available() reports whether the tier can run so callers self-skip.
using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Asmtest
{
    /// <summary>Process-init default recording mode (mirrors asmtest_drtrace_mode_t).</summary>
    public enum DrTraceMode { Blocks = 0, Insns = 1, Events = 2 }

    // The native entry points for libasmtest_drapp. Internal: callers use the
    // typed DrTrace / NativeTrace / NativeCode classes below, never DllImport
    // directly. Loading is wrapped so a missing lib (no DynamoRIO) self-skips
    // cleanly via DrTrace.Available() rather than throwing out.
    internal static class DrNative
    {
        const string DRAPP = "asmtest_drapp";

        public const int ASMTEST_DR_OK = 0;

        // asmtest_drtrace_options_t: four pointer/int fields. Strings are marshalled
        // by hand (Marshal.StringToHGlobalAnsi) into the IntPtr fields so a null
        // client/home maps to NULL — the C side then falls back to its env vars
        // (ASMTEST_DRCLIENT / ASMTEST_DR_LIB).
        [StructLayout(LayoutKind.Sequential)]
        public struct Options
        {
            public IntPtr DynamorioHome;  // const char*
            public IntPtr ClientPath;     // const char*
            public IntPtr ClientOptions;  // const char*
            public int Mode;              // asmtest_drtrace_mode_t
        }

        // asmtest_exec_code_t: {void* base; size_t len}.
        [StructLayout(LayoutKind.Sequential)]
        public struct ExecCode
        {
            public IntPtr Base;  // executable mapping holding the bytes (offset 0 = entry)
            public UIntPtr Len;  // number of code bytes
        }

        // The resolved libasmtest_drapp handle (IntPtr.Zero if it could not load).
        // Loaded eagerly here — NOT in the static ctor — because C# runs static
        // FIELD initializers (LibHandle, LibAvailable below) BEFORE the static
        // constructor. The probe must therefore load the lib itself rather than lean
        // on the DllImport resolver, which the static ctor registers later (the same
        // ordering gotcha Asmtest.cs's ProbeAsm documents). The resolver then just
        // hands back this already-loaded handle.
        public static readonly IntPtr LibHandle = LoadLib();

        // The absolute path of the libasmtest_drapp actually loaded (null if none
        // loaded). Lets a clean-room test assert the bundled tier — not a leaked
        // build/ tree — satisfied the load; exposed via DrTrace.LibraryPath().
        public static string ResolvedPath { get; private set; }

        static IntPtr LoadLib()
        {
            foreach (var cand in Candidates())
                if (NativeLibrary.TryLoad(cand, out var h)) { ResolvedPath = cand; return h; }
            return IntPtr.Zero;
        }

        // Map the logical name "asmtest_drapp" to the handle resolved above. Registers
        // on first access to the type (after the field initializers have run).
        static DrNative()
        {
            NativeLibrary.SetDllImportResolver(typeof(DrNative).Assembly, (name, asm, paths) =>
                name == DRAPP && LibHandle != IntPtr.Zero ? LibHandle : IntPtr.Zero);
        }

        // Candidate paths, in priority order: ASMTEST_DRAPP_LIB, then the NuGet
        // bundled slot (runtimes/<rid>/native/, staged by `make dotnet-package`),
        // then the in-tree build dir (<repo>/build/libasmtest_drapp.so), then the
        // bare soname. The bundled slot is tried BEFORE the dev build/ tree, so an
        // installed package never prefers a leaked checkout; system search is last.
        static System.Collections.Generic.IEnumerable<string> Candidates()
        {
            var env = Environment.GetEnvironmentVariable("ASMTEST_DRAPP_LIB");
            if (!string.IsNullOrEmpty(env)) yield return env;
            var bundled = BundledPath(LibFileName());
            if (bundled != null) yield return bundled;
            var repo = RepoRoot();
            if (repo != null) yield return Path.Combine(repo, "build", LibFileName());
            yield return LibFileName();
        }

        // The platform-specific libasmtest_drapp file name (mirrors the Python
        // wrapper: .dylib on macOS, .so elsewhere).
        static string LibFileName() =>
            RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? "libasmtest_drapp.dylib" : "libasmtest_drapp.so";

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

        // The bundled DR client (libasmtest_drclient) shipped in the same NuGet
        // runtimes/<rid>/native/ slot as libasmtest_drapp, or null if not present.
        // Used by DrTrace.Initialize to default the client to the bundled one.
        public static string BundledClientPath() =>
            BundledPath(RuntimeInformation.IsOSPlatform(OSPlatform.OSX)
                ? "libasmtest_drclient.dylib" : "libasmtest_drclient.so");

        // Walk up from the assembly's directory looking for the repo root (the dir
        // holding "build/"). The binding's bin/ output sits several levels under the
        // repo, so this finds <repo>/build/libasmtest_drapp.so without an env var.
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
        [DllImport(DRAPP)] public static extern int asmtest_dr_available();
        [DllImport(DRAPP)] public static extern int asmtest_dr_init(ref Options opts);
        [DllImport(DRAPP)] public static extern int asmtest_dr_start();
        [DllImport(DRAPP)] public static extern int asmtest_dr_stop();
        [DllImport(DRAPP)] public static extern void asmtest_dr_shutdown();
        [DllImport(DRAPP)] public static extern int asmtest_dr_marker_error();
        [DllImport(DRAPP)] public static extern int asmtest_dr_under_dynamorio();

        // ---- region registration + markers (const char* name) ----
        [DllImport(DRAPP, CharSet = CharSet.Ansi)]
        public static extern int asmtest_dr_register_region(
            [MarshalAs(UnmanagedType.LPStr)] string name, IntPtr @base, UIntPtr len, IntPtr trace);
        [DllImport(DRAPP, CharSet = CharSet.Ansi)]
        public static extern int asmtest_dr_register_symbol(
            [MarshalAs(UnmanagedType.LPStr)] string symbol, UIntPtr maxLen, IntPtr trace);
        [DllImport(DRAPP, CharSet = CharSet.Ansi)]
        public static extern int asmtest_dr_unregister_region([MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(DRAPP, CharSet = CharSet.Ansi)]
        public static extern void asmtest_trace_begin([MarshalAs(UnmanagedType.LPStr)] string name);
        [DllImport(DRAPP, CharSet = CharSet.Ansi)]
        public static extern void asmtest_trace_end([MarshalAs(UnmanagedType.LPStr)] string name);

        // ---- host-native executable code ----
        [DllImport(DRAPP)] public static extern int asmtest_exec_alloc(byte[] bytes, UIntPtr len, out ExecCode outCode);
        [DllImport(DRAPP)] public static extern void asmtest_exec_free(ref ExecCode code);

        // ---- symbol-mode fixture (a*2+b) traced by name ----
        [DllImport(DRAPP)] public static extern long asmtest_symbol_demo(long a, long b);

        // ---- trace handle + accessors (insns_cap FIRST, blocks_cap SECOND) ----
        [DllImport(DRAPP)] public static extern IntPtr asmtest_trace_new(UIntPtr insnsCap, UIntPtr blocksCap);
        [DllImport(DRAPP)] public static extern void asmtest_trace_free(IntPtr trace);
        [DllImport(DRAPP)] public static extern int asmtest_trace_covered(IntPtr trace, ulong off);
        [DllImport(DRAPP)] public static extern ulong asmtest_emu_trace_blocks_len(IntPtr trace);
        [DllImport(DRAPP)] public static extern ulong asmtest_emu_trace_insns_total(IntPtr trace);
        [DllImport(DRAPP)] public static extern ulong asmtest_emu_trace_block_at(IntPtr trace, UIntPtr i);
        [DllImport(DRAPP)] public static extern ulong asmtest_emu_trace_insns_len(IntPtr trace);
        [DllImport(DRAPP)] public static extern ulong asmtest_emu_trace_insn_at(IntPtr trace, UIntPtr i);

        // Whether libasmtest_drapp loaded at all. A missing lib (no DynamoRIO build)
        // self-skips via DrTrace.Available() rather than crashing. Derived from the
        // handle resolved by LoadLib() above (resolver-free, so it is robust to the
        // field-initializer-before-static-ctor ordering).
        public static readonly bool LibAvailable = LibHandle != IntPtr.Zero;
    }

    /// <summary>Thrown by the native-trace wrappers on a nonzero native status code.</summary>
    public sealed class DrTraceException : Exception
    {
        public DrTraceException(string message) : base(message) { }
    }

    /// <summary>The signature generated code is invoked through: two longs in, a long out (SysV).</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate long Func2(long a, long b);

    /// <summary>
    /// Process-wide DynamoRIO lifecycle for the native-trace tier. Bring DR up once
    /// with <see cref="Initialize"/>, allocate per-trace recorders with
    /// <see cref="NativeTrace.Create"/>, and tear down with <see cref="Shutdown"/>.
    /// </summary>
    public static class DrTrace
    {
        /// <summary>
        /// True if the tier can run: libasmtest_drapp loaded AND libdynamorio is
        /// resolvable at runtime (asmtest_dr_available() != 0). Never throws — a
        /// missing lib or unavailable DR returns false so callers self-skip.
        /// </summary>
        public static bool Available()
        {
            if (!DrNative.LibAvailable) return false;
            try { return DrNative.asmtest_dr_available() != 0; }
            catch { return false; }
        }

        /// <summary>
        /// The absolute path of the libasmtest_drapp this process loaded (null if the
        /// lib is not available). Lets a clean-room test assert the bundled tier — not
        /// a leaked build/ tree — satisfied the load.
        /// </summary>
        public static string LibraryPath() => DrNative.ResolvedPath;

        /// <summary>
        /// Bring DynamoRIO up in-process and take over (dr_app_setup + dr_app_start).
        /// <paramref name="client"/> is the path to libasmtest_drclient.so (null ->
        /// NULL -> the C side falls back to $ASMTEST_DRCLIENT);
        /// <paramref name="dynamorioHome"/> lets the C side find libdynamorio.
        /// Throws <see cref="DrTraceException"/> on a nonzero status.
        /// </summary>
        public static void Initialize(string client = null, string dynamorioHome = null,
                                      string clientOptions = null, int mode = 0)
        {
            // When no client is passed, prefer the bundled DR client shipped alongside
            // libasmtest_drapp (honoring $ASMTEST_DRCLIENT first) — mirroring the
            // Python wrapper's _default_client(). Falls through to null (the C side's
            // $ASMTEST_DRCLIENT / repo-build fallback) when nothing is bundled.
            client ??= DefaultClient();
            // Marshal the (optional) strings to native ANSI; a null/empty string maps
            // to IntPtr.Zero so the C side hits its env-var fallback. Free them after
            // init copies what it needs.
            IntPtr home = ToNative(dynamorioHome);
            IntPtr cpath = ToNative(client);
            IntPtr copts = ToNative(clientOptions);
            try
            {
                var opts = new DrNative.Options
                {
                    DynamorioHome = home,
                    ClientPath = cpath,
                    ClientOptions = copts,
                    Mode = mode,
                };
                int rc = DrNative.asmtest_dr_init(ref opts);
                if (rc != DrNative.ASMTEST_DR_OK)
                    throw new DrTraceException($"asmtest_dr_init failed: {rc}");
                rc = DrNative.asmtest_dr_start();
                if (rc != DrNative.ASMTEST_DR_OK)
                    throw new DrTraceException($"asmtest_dr_start failed: {rc}");
            }
            finally
            {
                FreeNative(home);
                FreeNative(cpath);
                FreeNative(copts);
            }
        }

        /// <summary>Tear DynamoRIO down (dr_app_stop_and_cleanup), back to UNINIT.</summary>
        public static void Shutdown() => DrNative.asmtest_dr_shutdown();

        /// <summary>Count of unbalanced marker operations since init (0 means all balanced).</summary>
        public static int MarkerError() => DrNative.asmtest_dr_marker_error();

        /// <summary>True if the calling thread is currently executing under DynamoRIO's control.</summary>
        public static bool UnderDynamoRio() => DrNative.asmtest_dr_under_dynamorio() != 0;

        /// <summary>The exported fixture (a*2+b) the symbol-mode test traces by name.</summary>
        public static long SymbolDemo(long a, long b) => DrNative.asmtest_symbol_demo(a, b);

        static IntPtr ToNative(string s) =>
            string.IsNullOrEmpty(s) ? IntPtr.Zero : Marshal.StringToHGlobalAnsi(s);
        static void FreeNative(IntPtr p)
        {
            if (p != IntPtr.Zero) Marshal.FreeHGlobal(p);
        }

        // The DR client to hand asmtest_dr_init when the caller passes none: honor
        // $ASMTEST_DRCLIENT first, else the bundled libasmtest_drclient shipped next
        // to libasmtest_drapp in the NuGet runtimes/<rid>/native/ slot (else null →
        // the C side's own $ASMTEST_DRCLIENT / repo-build fallback). Mirrors the
        // Python wrapper's _default_client().
        static string DefaultClient()
        {
            var env = Environment.GetEnvironmentVariable("ASMTEST_DRCLIENT");
            if (!string.IsNullOrEmpty(env)) return env;
            return DrNative.BundledClientPath();
        }
    }

    /// <summary>
    /// Host-native machine code in real executable (W^X) memory. Allocate with
    /// <see cref="FromBytes"/>, invoke through a function pointer with
    /// <see cref="Call"/>, and release with <see cref="Free"/>.
    /// </summary>
    public sealed class NativeCode : IDisposable
    {
        DrNative.ExecCode _code;
        bool _freed;

        NativeCode(DrNative.ExecCode code) => _code = code;

        /// <summary>Map executable memory and copy the host-native machine-code bytes into it.</summary>
        public static NativeCode FromBytes(byte[] bytes)
        {
            int rc = DrNative.asmtest_exec_alloc(bytes, (UIntPtr)bytes.Length, out var ec);
            if (rc != DrNative.ASMTEST_DR_OK)
                throw new DrTraceException($"asmtest_exec_alloc failed: {rc}");
            return new NativeCode(ec);
        }

        /// <summary>The executable mapping's base address (offset 0 = entry).</summary>
        public IntPtr Base => _code.Base;

        /// <summary>The number of code bytes.</summary>
        public long Length => (long)_code.Len;

        /// <summary>
        /// Invoke the code through a function pointer with two integer args, reading
        /// the result as a long (the SysV integer ABI).
        /// </summary>
        public long Call(long a, long b)
        {
            var fn = Marshal.GetDelegateForFunctionPointer<Func2>(_code.Base);
            return fn(a, b);
        }

        /// <summary>Unmap the executable memory. Unregister any region keyed to it FIRST.</summary>
        public void Free()
        {
            if (!_freed)
            {
                DrNative.asmtest_exec_free(ref _code);
                _freed = true;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Dispose pattern over <see cref="Free"/> (idempotent).</summary>
        public void Dispose() => Free();

        // Finalizer backstop (B3): a dropped (never-Free'd) NativeCode still
        // unmaps its executable memory. Guarded + try/catch so it never throws on
        // the finalizer thread (mirrors the AmdSampler finalizer in HwTrace.cs);
        // asmtest_exec_free is thread-agnostic (a munmap).
        ~NativeCode()
        {
            if (_freed || !DrNative.LibAvailable) return;
            _freed = true;
            try { DrNative.asmtest_exec_free(ref _code); } catch { }
        }
    }

    /// <summary>
    /// An app-owned coverage recorder for a registered native region. Create one with
    /// <see cref="Create"/>, register a <see cref="NativeCode"/> under a name, run it
    /// inside <see cref="Region"/>, then read back coverage / the instruction stream.
    /// </summary>
    public sealed class NativeTrace : IDisposable
    {
        IntPtr _handle;

        NativeTrace(IntPtr handle) => _handle = handle;

        /// <summary>
        /// Allocate a trace recorder. Block recording is on when blocks &gt; 0,
        /// instruction recording when instructions &gt; 0. NOTE: the native
        /// asmtest_trace_new takes insns_cap FIRST, blocks_cap SECOND.
        /// </summary>
        public static NativeTrace Create(int blocks = 64, int instructions = 0)
        {
            IntPtr h = DrNative.asmtest_trace_new((UIntPtr)(nuint)instructions, (UIntPtr)(nuint)blocks);
            if (h == IntPtr.Zero) throw new DrTraceException("asmtest_trace_new failed");
            return new NativeTrace(h);
        }

        /// <summary>Register a native code range under <paramref name="name"/>, recording into this trace.</summary>
        public NativeTrace Register(string name, NativeCode code)
        {
            int rc = DrNative.asmtest_dr_register_region(
                name, code.Base, (UIntPtr)code.Length, _handle);
            if (rc != DrNative.ASMTEST_DR_OK)
                throw new DrTraceException($"register_region(\"{name}\") failed: {rc}");
            return this;
        }

        /// <summary>
        /// Symbol mode: trace a named exported function with no begin/end markers —
        /// always-on recording for [entry, entry + <paramref name="maxLen"/>).
        /// </summary>
        public NativeTrace RegisterSymbol(string symbol, nuint maxLen)
        {
            int rc = DrNative.asmtest_dr_register_symbol(symbol, (UIntPtr)maxLen, _handle);
            if (rc != DrNative.ASMTEST_DR_OK)
                throw new DrTraceException($"register_symbol(\"{symbol}\") failed: {rc}");
            return this;
        }

        /// <summary>Drop the named region (and the client's cached translation).</summary>
        public void Unregister(string name) => DrNative.asmtest_dr_unregister_region(name);

        /// <summary>
        /// Open recording for <paramref name="name"/>, run <paramref name="body"/>, and
        /// close recording — even if the body throws (markers must stay balanced).
        /// </summary>
        public void Region(string name, Action body)
        {
            DrNative.asmtest_trace_begin(name);
            try { body(); }
            finally { DrNative.asmtest_trace_end(name); }
        }

        /// <summary>True if the basic block at byte-offset <paramref name="off"/> was entered.</summary>
        public bool Covered(ulong off) => DrNative.asmtest_trace_covered(_handle, off) != 0;

        /// <summary>The number of distinct basic blocks recorded.</summary>
        public ulong BlocksLen => DrNative.asmtest_emu_trace_blocks_len(_handle);

        /// <summary>The total count of instructions in the recorded stream.</summary>
        public ulong InsnsTotal => DrNative.asmtest_emu_trace_insns_total(_handle);

        /// <summary>
        /// The distinct basic-block start offsets recorded, in storage (first-seen)
        /// order. Read one-by-one through the opaque-handle accessor.
        /// </summary>
        public ulong[] BlockOffsets()
        {
            int n = (int)BlocksLen;
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = DrNative.asmtest_emu_trace_block_at(_handle, (UIntPtr)(nuint)i);
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
            int n = (int)DrNative.asmtest_emu_trace_insns_len(_handle);
            var offs = new ulong[n];
            for (int i = 0; i < n; i++)
                offs[i] = DrNative.asmtest_emu_trace_insn_at(_handle, (UIntPtr)(nuint)i);
            return offs;
        }

        /// <summary>Free the trace recorder.</summary>
        public void Free()
        {
            if (_handle != IntPtr.Zero)
            {
                DrNative.asmtest_trace_free(_handle);
                _handle = IntPtr.Zero;
            }
            GC.SuppressFinalize(this);
        }

        /// <summary>Dispose pattern over <see cref="Free"/> (idempotent).</summary>
        public void Dispose() => Free();

        // Finalizer backstop (B3): a dropped recorder still frees its native
        // handle (asmtest_trace_free is a plain free(), thread-agnostic).
        ~NativeTrace()
        {
            if (_handle == IntPtr.Zero || !DrNative.LibAvailable) return;
            IntPtr h = _handle; _handle = IntPtr.Zero;
            try { DrNative.asmtest_trace_free(h); } catch { }
        }
    }
}
