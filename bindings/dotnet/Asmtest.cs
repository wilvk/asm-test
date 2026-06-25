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
        [DllImport(EMU)] public static extern void asmtest_capture_vec_f32(
            IntPtr o, IntPtr fn, float[] lanes, int nvec);
        [DllImport(EMU)] public static extern ulong asmtest_regs_ret(IntPtr r);
        [DllImport(EMU)] public static extern double asmtest_regs_fret(IntPtr r);
        [DllImport(EMU)] public static extern float asmtest_regs_vec_f32(IntPtr r, int index, int lane);
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
        // missing entry point. The widened shim takes six scalar args + syntax
        // + an instruction cap; the assemble-only shim is multi-arch.
        [DllImport(EMU)] public static extern int asmtest_emu_call_asm6(
            IntPtr e, string src, int syntax, long a0, long a1, long a2, long a3,
            long a4, long a5, int nargs, ulong maxInsns, IntPtr o);
        [DllImport(EMU)] public static extern int asmtest_asm_bytes(
            int arch, int syntax, string src, ulong addr, byte[] buf, int cap);
        [DllImport(EMU)] static extern IntPtr asmtest_asm_last_error();
        [DllImport(EMU)] public static extern int asmtest_emu_result_faulted(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_x86_reg(IntPtr r, string name);

        /// <summary>The Keystone diagnostic from the most recent assemble (thread-local; "" on success).</summary>
        public static string AsmError()
        {
            var p = asmtest_asm_last_error();
            return p == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(p);
        }

        // Whether the "asmtest_emu" library that will be loaded (ASMTEST_LIB, or
        // the default by name) exports the in-line assembler.
        public static readonly bool AsmAvailable = ProbeAsm();
        static bool ProbeAsm()
        {
            try
            {
                var p = Environment.GetEnvironmentVariable("ASMTEST_LIB");
                var h = string.IsNullOrEmpty(p) ? NativeLibrary.Load(EMU) : NativeLibrary.Load(p);
                return NativeLibrary.TryGetExport(h, "asmtest_emu_call_asm6", out _);
            }
            catch { return false; }
        }
    }

    /// <summary>Target architecture for <see cref="Emu.Assemble"/> (mirrors asm_arch_t).</summary>
    public enum AsmArch { X86_64 = 0, Arm64 = 1, RiscV64 = 2, Arm32 = 3 }

    /// <summary>Input assembly syntax (x86 only); mirrors asm_syntax_t.</summary>
    public enum AsmSyntax { Intel = 0, Att = 1 }

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

        /// <summary>
        /// Call <paramref name="fn"/> with up to eight 128-bit vector args (each four
        /// float32 lanes), capturing the vector register file. Read the vector return
        /// with <see cref="VecF32"/>(0).
        /// </summary>
        public void CaptureVecF32(IntPtr fn, float[][] vectors)
        {
            var flat = new float[vectors.Length * 4];
            for (int i = 0; i < vectors.Length; i++)
                for (int l = 0; l < 4; l++)
                    flat[i * 4 + l] = vectors[i][l];
            Native.asmtest_capture_vec_f32(_h, fn, flat, vectors.Length);
        }

        /// <summary>The integer return value (rax / x0).</summary>
        public ulong Ret => Native.asmtest_regs_ret(_h);

        /// <summary>The scalar FP return value (xmm0 / d0).</summary>
        public double FRet => Native.asmtest_regs_fret(_h);

        /// <summary>The four float32 lanes of vector register <paramref name="index"/> (0 = the vector return).</summary>
        public float[] VecF32(int index = 0)
        {
            var o = new float[4];
            for (int l = 0; l < 4; l++) o[l] = Native.asmtest_regs_vec_f32(_h, index, l);
            return o;
        }

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
        /// Assemble x86-64 <paramref name="src"/> in <paramref name="syntax"/> and run it
        /// with up to six integer <paramref name="args"/>, stopping after
        /// <paramref name="maxInsns"/> instructions (0 = run to <c>ret</c>). Returns the
        /// run's <see cref="EmuResult"/>; throws <see cref="AsmtestException"/> carrying
        /// the Keystone diagnostic if the string fails to assemble. Only when
        /// <see cref="AsmAvailable"/> — needs the Keystone-backed native lib.
        /// </summary>
        public EmuResult CallAsm(string src, long[] args = null,
                                 AsmSyntax syntax = AsmSyntax.Intel, ulong maxInsns = 0)
        {
            if (!Native.AsmAvailable) throw new AsmtestException("in-line assembler not in this build");
            args ??= Array.Empty<long>();
            long A(int i) => i < args.Length ? args[i] : 0;
            int nargs = Math.Min(args.Length, 6);
            var res = new EmuResult();
            int ok = Native.asmtest_emu_call_asm6(_h, src, (int)syntax,
                A(0), A(1), A(2), A(3), A(4), A(5), nargs, maxInsns, res.Handle);
            if (ok == 0) { res.Dispose(); throw new AsmtestException("in-line assembly failed: " + Native.AsmError()); }
            return res;
        }

        /// <summary>
        /// Assemble <paramref name="src"/> for <paramref name="arch"/> /
        /// <paramref name="syntax"/> at load address <paramref name="addr"/> and return
        /// the machine-code bytes. Multi-arch (unlike <see cref="CallAsm"/>, which runs
        /// on the x86-64 guest). Throws <see cref="AsmtestException"/> on a Keystone error.
        /// </summary>
        public static byte[] Assemble(string src, AsmArch arch = AsmArch.X86_64,
                                      AsmSyntax syntax = AsmSyntax.Intel, ulong addr = 0x00100000)
        {
            if (!Native.AsmAvailable) throw new AsmtestException("in-line assembler not in this build");
            var buf = new byte[256];
            int n = Native.asmtest_asm_bytes((int)arch, (int)syntax, src, addr, buf, buf.Length);
            if (n == 0) throw new AsmtestException("assemble failed: " + Native.AsmError());
            if (n > buf.Length) { buf = new byte[n]; Native.asmtest_asm_bytes((int)arch, (int)syntax, src, addr, buf, n); }
            Array.Resize(ref buf, n);
            return buf;
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
        public static void VecF32(Regs r, int index, float[] want)
        {
            var got = r.VecF32(index);
            for (int i = 0; i < want.Length; i++)
                if (got[i] != want[i])
                    throw new AsmtestException($"vec[{index}] lane {i}: got {got[i]}, want {want[i]}");
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
