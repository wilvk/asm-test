// Asmtest.cs — asm-test .NET binding (Track D): the reusable library module.
//
// This is the wrapper a .NET project consumes; it keeps all P/Invoke
// (DllImport) inside, so calling code never declares a native entry point. It
// drives the opaque-handle FFI layer (src/ffi.c), so no C struct layout is
// mirrored: Regs.Capture6 / CaptureFp2 + accessors for the capture tier,
// Emu / EmuResult for the emulator (faults as data), and an Assert helper
// class for Tier-2 idiomatic checks. The native libs are resolved by the
// loader (LD_LIBRARY_PATH / DYLD_LIBRARY_PATH point at the asm-test build dir).
using System;
using System.Runtime.InteropServices;

namespace Asmtest
{
    // The native entry points. Internal: callers use the typed classes below,
    // never DllImport directly. "asmtest_emu" carries the capture trampoline +
    // emulator + accessors; "asmtest_corpus" exports the canonical fixtures.
    internal static class Native
    {
        const string EMU = "asmtest_emu";
        const string COR = "asmtest_corpus";

        // Resolve the two logical libs from the environment, matching the other
        // bindings: ASMTEST_LIB selects which "asmtest_emu" to load — the plain
        // libasmtest_emu or the emu+assembler libasmtest_emu_asm (so CallAsm runs
        // against the latter) — and ASMTEST_CORPUS_LIB selects the fixtures lib.
        // Falls back to default name-based resolution when unset.
        static Native()
        {
            NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, asm, paths) =>
            {
                string env = name == EMU ? "ASMTEST_LIB"
                           : name == COR ? "ASMTEST_CORPUS_LIB" : null;
                var p = env is null ? null : Environment.GetEnvironmentVariable(env);
                return string.IsNullOrEmpty(p) ? IntPtr.Zero : NativeLibrary.Load(p);
            });
        }

        [DllImport(COR)] public static extern IntPtr asmtest_corpus_routine(string name);
        [DllImport(EMU)] public static extern IntPtr asmtest_regs_new();
        [DllImport(EMU)] public static extern void asmtest_regs_free(IntPtr r);
        [DllImport(EMU)] public static extern void asmtest_capture6(
            IntPtr o, IntPtr fn, long a0, long a1, long a2, long a3, long a4, long a5);
        [DllImport(EMU)] public static extern void asmtest_capture_fp2(
            IntPtr o, IntPtr fn, double f0, double f1);
        [DllImport(EMU)] public static extern ulong asmtest_regs_ret(IntPtr r);
        [DllImport(EMU)] public static extern double asmtest_regs_fret(IntPtr r);
        [DllImport(EMU)] public static extern int asmtest_regs_flag_set(IntPtr r, string name);
        [DllImport(EMU)] public static extern int asmtest_check_abi(IntPtr r, IntPtr msg, nuint n);
        [DllImport(EMU)] public static extern IntPtr emu_open();
        [DllImport(EMU)] public static extern void emu_close(IntPtr e);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_result_new();
        [DllImport(EMU)] public static extern void asmtest_emu_result_free(IntPtr r);
        [DllImport(EMU)] public static extern int asmtest_emu_call2(
            IntPtr e, IntPtr fn, long a0, long a1, IntPtr o);
        // Optional: present only in the emu+asm native lib (Keystone). Guard
        // every call with AsmAvailable so a plain libasmtest_emu never hits a
        // missing entry point.
        [DllImport(EMU)] public static extern int asmtest_emu_call_asm(
            IntPtr e, string src, long a0, long a1, IntPtr o);
        [DllImport(EMU)] public static extern int asmtest_emu_result_faulted(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_x86_reg(IntPtr r, string name);

        // Whether the "asmtest_emu" library that will be loaded (ASMTEST_LIB, or
        // the default by name) exports the in-line assembler.
        public static readonly bool AsmAvailable = ProbeAsm();
        static bool ProbeAsm()
        {
            try
            {
                var p = Environment.GetEnvironmentVariable("ASMTEST_LIB");
                var h = string.IsNullOrEmpty(p) ? NativeLibrary.Load(EMU) : NativeLibrary.Load(p);
                return NativeLibrary.TryGetExport(h, "asmtest_emu_call_asm", out _);
            }
            catch { return false; }
        }
    }

    /// <summary>Thrown by the <see cref="Assert"/> helpers on a failed check.</summary>
    public sealed class AsmtestException : Exception
    {
        public AsmtestException(string message) : base(message) { }
    }

    /// <summary>Resolves a canonical corpus routine (e.g. "add_signed") to its address.</summary>
    public static class Corpus
    {
        public static IntPtr Routine(string name) => Native.asmtest_corpus_routine(name);
    }

    /// <summary>
    /// A captured register/flags snapshot. Allocate one, capture into it through
    /// the real ABI, then read fields via the accessors. Dispose to free.
    /// </summary>
    public sealed class Regs : IDisposable
    {
        IntPtr _h;
        public Regs() => _h = Native.asmtest_regs_new();

        /// <summary>Call <paramref name="fn"/> with up to six integer args (missing default to 0).</summary>
        public void Capture6(IntPtr fn, long a0 = 0, long a1 = 0, long a2 = 0,
                             long a3 = 0, long a4 = 0, long a5 = 0)
            => Native.asmtest_capture6(_h, fn, a0, a1, a2, a3, a4, a5);

        /// <summary>Call <paramref name="fn"/> with two double args, capturing the FP return.</summary>
        public void CaptureFp2(IntPtr fn, double f0, double f1)
            => Native.asmtest_capture_fp2(_h, fn, f0, f1);

        /// <summary>The integer return value (rax / x0).</summary>
        public ulong Ret => Native.asmtest_regs_ret(_h);

        /// <summary>The scalar FP return value (xmm0 / d0).</summary>
        public double FRet => Native.asmtest_regs_fret(_h);

        /// <summary>Whether a named condition flag (e.g. "CF", "ZF") is set.</summary>
        public bool FlagSet(string name) => Native.asmtest_regs_flag_set(_h, name) == 1;

        /// <summary>Whether the callee-saved registers were restored (non-jumping verdict shim).</summary>
        public bool AbiPreserved => Native.asmtest_check_abi(_h, IntPtr.Zero, 0) == 0;

        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.asmtest_regs_free(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>An open emulator (x86-64 Unicorn guest). Dispose to close.</summary>
    public sealed class Emu : IDisposable
    {
        IntPtr _h;
        public Emu() => _h = Native.emu_open();

        /// <summary>Run <paramref name="fn"/> in the emulator with two integer args; faults are data.</summary>
        public EmuResult Call2(IntPtr fn, long a0, long a1)
        {
            var res = new EmuResult();
            Native.asmtest_emu_call2(_h, fn, a0, a1, res.Handle);
            return res;
        }

        /// <summary>Whether the loaded native lib carries the in-line assembler (Keystone).</summary>
        public static bool AsmAvailable => Native.AsmAvailable;

        /// <summary>
        /// Assemble x86-64 <paramref name="src"/> (Intel syntax) and run it with two
        /// integer args. <paramref name="ok"/> is false if it failed to assemble. Only
        /// when <see cref="AsmAvailable"/> — needs the Keystone-backed native lib.
        /// </summary>
        public EmuResult CallAsm(string src, long a0, long a1, out bool ok)
        {
            if (!Native.AsmAvailable) throw new AsmtestException("in-line assembler not in this build");
            var res = new EmuResult();
            ok = Native.asmtest_emu_call_asm(_h, src, a0, a1, res.Handle) != 0;
            return res;
        }

        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.emu_close(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>An emulator run's outcome — faults surfaced as data, not a crash.</summary>
    public sealed class EmuResult : IDisposable
    {
        IntPtr _h;
        public EmuResult() => _h = Native.asmtest_emu_result_new();
        internal IntPtr Handle => _h;

        /// <summary>Whether the routine took an invalid-memory fault.</summary>
        public bool Faulted => Native.asmtest_emu_result_faulted(_h) != 0;

        /// <summary>Read an x86-64 guest register by name (e.g. "rax").</summary>
        public ulong Reg(string name) => Native.asmtest_emu_x86_reg(_h, name);

        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.asmtest_emu_result_free(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>
    /// Tier-2 idiomatic assertions: throw <see cref="AsmtestException"/> with a
    /// clear message on failure, so a test runner (xUnit/NUnit) reports it.
    /// </summary>
    public static class Assert
    {
        public static void Ret(Regs r, ulong want)
        {
            if (r.Ret != want) throw new AsmtestException($"ret: got {r.Ret}, want {want}");
        }
        public static void AbiPreserved(Regs r)
        {
            if (!r.AbiPreserved) throw new AsmtestException("ABI not preserved");
        }
        public static void Flag(Regs r, string name, bool set = true)
        {
            if (r.FlagSet(name) != set)
                throw new AsmtestException($"flag {name}: got {(r.FlagSet(name) ? "set" : "clear")}, want {(set ? "set" : "clear")}");
        }
        public static void Fp(Regs r, double want)
        {
            if (r.FRet != want) throw new AsmtestException($"fp: got {r.FRet}, want {want}");
        }
        public static void NoFault(EmuResult res)
        {
            if (res.Faulted) throw new AsmtestException("unexpected fault");
        }
        public static void EmuReg(EmuResult res, string name, ulong want)
        {
            if (res.Reg(name) != want)
                throw new AsmtestException($"emu {name}: got {res.Reg(name)}, want {want}");
        }
    }
}
