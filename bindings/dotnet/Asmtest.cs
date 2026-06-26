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
        // Optional: the disassembler (Capstone), present only in libasmtest_emu_full.
        [DllImport(EMU)] public static extern UIntPtr emu_disas(
            int arch, byte[] code, UIntPtr codeLen, ulong baseAddr, ulong off,
            byte[] buf, UIntPtr bufLen);
        [DllImport(EMU)] [return: MarshalAs(UnmanagedType.I1)]
        public static extern bool emu_disas_available();
        [DllImport(EMU)] public static extern int asmtest_emu_result_faulted(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_result_fault_addr(IntPtr r);
        [DllImport(EMU)] public static extern int asmtest_emu_result_fault_kind(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_x86_reg(IntPtr r, string name);
        [DllImport(EMU)] public static extern double asmtest_emu_x86_xmm_f64(IntPtr r, int index, int lane);
        [DllImport(EMU)] public static extern float asmtest_emu_x86_xmm_f32(IntPtr r, int index, int lane);

        // Extended x86 emulator calls (array form: explicit code + length, so raw
        // machine-code bytes run directly). The bool return is unused.
        [DllImport(EMU)] public static extern int emu_call(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern int emu_call_fp(IntPtr e, byte[] code, nuint len, long[] ia, int ni, double[] fa, int nf, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern int emu_call_vec(IntPtr e, byte[] code, nuint len, long[] ia, int ni, float[] va, int nv, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern int emu_call_win64(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern int emu_call_traced(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o, IntPtr trace);

        // Opaque execution-trace handle.
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_trace_new(nuint ic, nuint bc);
        [DllImport(EMU)] public static extern void asmtest_emu_trace_free(IntPtr t);
        [DllImport(EMU)] public static extern int asmtest_emu_trace_covered(IntPtr t, ulong off);

        // Cross-arch guests (raw bytes, any host) + per-arch result accessors.
        [DllImport(EMU)] public static extern IntPtr emu_arm64_open();
        [DllImport(EMU)] public static extern void emu_arm64_close(IntPtr e);
        [DllImport(EMU)] public static extern int emu_arm64_call(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern int emu_arm64_call_traced(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o, IntPtr trace);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_arm64_result_new();
        [DllImport(EMU)] public static extern void asmtest_emu_arm64_result_free(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_arm64_reg(IntPtr r, string name);
        [DllImport(EMU)] public static extern IntPtr emu_riscv_open();
        [DllImport(EMU)] public static extern void emu_riscv_close(IntPtr e);
        [DllImport(EMU)] public static extern int emu_riscv_call(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_riscv_result_new();
        [DllImport(EMU)] public static extern void asmtest_emu_riscv_result_free(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_riscv_reg(IntPtr r, string name);
        [DllImport(EMU)] public static extern IntPtr emu_arm_open();
        [DllImport(EMU)] public static extern void emu_arm_close(IntPtr e);
        [DllImport(EMU)] public static extern int emu_arm_call(IntPtr e, byte[] code, nuint len, long[] args, int nargs, ulong mi, IntPtr o);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_arm_result_new();
        [DllImport(EMU)] public static extern void asmtest_emu_arm_result_free(IntPtr r);
        [DllImport(EMU)] public static extern ulong asmtest_emu_arm_reg(IntPtr r, string name);

        // Mid-execution guards (Track F).
        [DllImport(EMU)][return: MarshalAs(UnmanagedType.I1)] public static extern bool emu_map(IntPtr e, ulong addr, nuint size);
        [DllImport(EMU)] public static extern void emu_watch_writes(IntPtr e, ulong addr, nuint size, int mode, IntPtr w);
        [DllImport(EMU)] public static extern void emu_watch_clear(IntPtr e);
        [DllImport(EMU)][return: MarshalAs(UnmanagedType.I1)] public static extern bool emu_guard_reg(IntPtr e, string name, ulong want, IntPtr g);
        [DllImport(EMU)] public static extern void emu_guard_reg_clear(IntPtr e);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_watch_new();
        [DllImport(EMU)] public static extern void asmtest_emu_watch_free(IntPtr w);
        [DllImport(EMU)] public static extern int asmtest_emu_watch_violated(IntPtr w);
        [DllImport(EMU)] public static extern ulong asmtest_emu_watch_addr(IntPtr w);
        [DllImport(EMU)] public static extern ulong asmtest_emu_watch_rip_off(IntPtr w);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_reg_guard_new();
        [DllImport(EMU)] public static extern void asmtest_emu_reg_guard_free(IntPtr g);
        [DllImport(EMU)] public static extern int asmtest_emu_reg_guard_violated(IntPtr g);
        [DllImport(EMU)] public static extern ulong asmtest_emu_reg_guard_got(IntPtr g);
        // Coverage-guided fuzzing + mutation testing (Track E).
        [DllImport(EMU)][return: MarshalAs(UnmanagedType.I1)] public static extern bool emu_fuzz_cover1(IntPtr e, byte[] code, nuint len, long lo, long hi, ulong iters, ulong seed, IntPtr uni, IntPtr st);
        [DllImport(EMU)] public static extern nuint emu_mutation_test1(IntPtr e, byte[] code, nuint len, long[] inputs, nuint n, ulong maxm, ulong seed, IntPtr st);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_fuzz_stat_new();
        [DllImport(EMU)] public static extern void asmtest_emu_fuzz_stat_free(IntPtr s);
        [DllImport(EMU)] public static extern ulong asmtest_emu_fuzz_blocks_reached(IntPtr s);
        [DllImport(EMU)] public static extern ulong asmtest_emu_fuzz_corpus_len(IntPtr s);
        [DllImport(EMU)] public static extern IntPtr asmtest_emu_mutation_stat_new();
        [DllImport(EMU)] public static extern void asmtest_emu_mutation_stat_free(IntPtr s);
        [DllImport(EMU)] public static extern ulong asmtest_emu_mutation_killed(IntPtr s);
        [DllImport(EMU)] public static extern ulong asmtest_emu_mutation_survived(IntPtr s);
        // AVX2 256-bit capture (Track D).
        [DllImport(EMU)] public static extern void asm_call_capture_vec256([Out] byte[] vec, IntPtr fn, long[] iargs, byte[] vargs);
        [DllImport(EMU)] public static extern int asmtest_cpu_has_avx2();

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

        // Whether the loaded lib carries the disassembler (Capstone) — only
        // libasmtest_emu_full does; the lean libasmtest_emu / _emu_asm do not.
        public static readonly bool DisasAvailable = ProbeDisas();
        static bool ProbeDisas()
        {
            try
            {
                var p = Environment.GetEnvironmentVariable("ASMTEST_LIB");
                var h = string.IsNullOrEmpty(p) ? NativeLibrary.Load(EMU) : NativeLibrary.Load(p);
                // Symbol-presence only — resolver-free, like ProbeAsm. Calling the
                // emu_disas_available() DllImport here would need the DllImport
                // resolver, which the static ctor registers AFTER these field
                // initializers run; the runtime Capstone check lives in
                // Emu.DisasAvailable (read later, with the resolver up).
                return NativeLibrary.TryGetExport(h, "emu_disas", out _)
                    && NativeLibrary.TryGetExport(h, "emu_disas_available", out _);
            }
            catch { return false; }
        }
    }

    /// <summary>Target architecture for <see cref="Emu.Assemble"/> (mirrors asm_arch_t).</summary>
    public enum AsmArch { X86_64 = 0, Arm64 = 1, RiscV64 = 2, Arm32 = 3 }

    /// <summary>Input assembly syntax (x86 only); mirrors asm_syntax_t.</summary>
    public enum AsmSyntax { Intel = 0, Att = 1, Nasm = 2, Masm = 3, Gas = 4 }

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

        /// <summary>Run raw x86-64 machine-code <paramref name="code"/> with up to six integer args.</summary>
        public EmuResult CallBytes(byte[] code, params long[] args)
        {
            var res = new EmuResult();
            Native.emu_call(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle);
            return res;
        }

        /// <summary>Run raw bytes marshalling doubles into the FP arg registers (scalar return = XmmF64(0, 0)).</summary>
        public EmuResult CallFp(byte[] code, long[] iargs, double[] fargs)
        {
            var res = new EmuResult();
            iargs ??= Array.Empty<long>();
            fargs ??= Array.Empty<double>();
            Native.emu_call_fp(_h, code, (nuint)code.Length, iargs, iargs.Length, fargs, fargs.Length, 0, res.Handle);
            return res;
        }

        /// <summary>Run raw bytes marshalling 128-bit vectors (each four float32 lanes) into xmm0..7.</summary>
        public EmuResult CallVec(byte[] code, long[] iargs, float[][] vargs)
        {
            var res = new EmuResult();
            iargs ??= Array.Empty<long>();
            var flat = new float[vargs.Length * 4];
            for (int i = 0; i < vargs.Length; i++)
                for (int l = 0; l < 4; l++) flat[i * 4 + l] = vargs[i][l];
            Native.emu_call_vec(_h, code, (nuint)code.Length, iargs, iargs.Length, flat, vargs.Length, 0, res.Handle);
            return res;
        }

        /// <summary>Run raw bytes under the Microsoft x64 (Win64) convention (args in rcx, rdx, r8, r9).</summary>
        public EmuResult CallWin64(byte[] code, params long[] args)
        {
            var res = new EmuResult();
            Native.emu_call_win64(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle);
            return res;
        }

        /// <summary>Like <see cref="CallBytes"/>, but record an execution trace / coverage into <paramref name="trace"/>.</summary>
        public EmuResult CallTraced(byte[] code, long[] args, Trace trace)
        {
            var res = new EmuResult();
            args ??= Array.Empty<long>();
            Native.emu_call_traced(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle, trace.Handle);
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

        /// <summary>Whether the loaded native lib carries the disassembler (Capstone) —
        /// only libasmtest_emu_full does; the lean libasmtest_emu / _emu_asm do not.
        /// Combines the symbol-presence probe with the runtime Capstone check (safe
        /// to call here: the DllImport resolver is registered by now).</summary>
        public static bool DisasAvailable => Native.DisasAvailable && Native.emu_disas_available();

        /// <summary>
        /// Disassemble the one instruction at byte <paramref name="off"/> of
        /// <paramref name="code"/> for <paramref name="arch"/>. <paramref name="baseAddr"/>
        /// is the address the bytes run at (EMU_CODE_BASE) so PC-relative operands resolve.
        /// Returns "mnemonic operands", or "" with no disassembler / on a decode miss.
        /// </summary>
        public static string Disas(byte[] code, ulong off = 0, AsmArch arch = AsmArch.X86_64,
                                   ulong baseAddr = 0x00100000)
        {
            if (!DisasAvailable) return "";
            var buf = new byte[160];
            nuint n = (nuint)Native.emu_disas((int)arch, code, (UIntPtr)code.Length, baseAddr, off,
                                              buf, (UIntPtr)buf.Length);
            if (n == 0) return "";
            int z = Array.IndexOf(buf, (byte)0);
            return System.Text.Encoding.UTF8.GetString(buf, 0, z < 0 ? buf.Length : z);
        }

        // --- Mid-execution guards (Track F) --- //
        /// <summary>Map a guest RW region [addr, addr+size) the routine can use.</summary>
        public bool Map(ulong addr, nuint size) => Native.emu_map(_h, addr, size);

        /// <summary>Arm a memory-write watchpoint over [addr, addr+size): mode 1 =
        /// only (flag a write that escapes it), 0 = never (one that touches it).</summary>
        public Watch WatchWrites(ulong addr, nuint size, int mode)
        {
            var w = new Watch();
            Native.emu_watch_writes(_h, addr, size, mode, w.Handle);
            return w;
        }
        public void WatchClear() => Native.emu_watch_clear(_h);

        /// <summary>Arm a register invariant; null for an unknown register name.</summary>
        public RegGuard GuardReg(string name, ulong want)
        {
            var g = new RegGuard();
            if (!Native.emu_guard_reg(_h, name, want, g.Handle)) { g.Dispose(); return null; }
            return g;
        }
        public void GuardRegClear() => Native.emu_guard_reg_clear(_h);

        // --- Coverage-guided fuzzing + mutation testing (Track E) --- //
        /// <summary>Coverage-guided input search over one-int-arg code; (blocks, corpus).</summary>
        public (ulong Blocks, ulong Corpus) FuzzCover(byte[] code, long lo, long hi, ulong iters)
        {
            var uni = Native.asmtest_emu_trace_new(0, 256);
            var st = Native.asmtest_emu_fuzz_stat_new();
            Native.emu_fuzz_cover1(_h, code, (nuint)code.Length, lo, hi, iters, 0xC0FFEE, uni, st);
            var outv = (Native.asmtest_emu_fuzz_blocks_reached(st), Native.asmtest_emu_fuzz_corpus_len(st));
            Native.asmtest_emu_fuzz_stat_free(st);
            Native.asmtest_emu_trace_free(uni);
            return outv;
        }
        /// <summary>Bit-flip mutation testing of code against inputs; (killed, survived).</summary>
        public (ulong Killed, ulong Survived) MutationTest(byte[] code, long[] inputs)
        {
            var st = Native.asmtest_emu_mutation_stat_new();
            Native.emu_mutation_test1(_h, code, (nuint)code.Length, inputs, (nuint)inputs.Length, 0, 0xABCD, st);
            var outv = (Native.asmtest_emu_mutation_killed(st), Native.asmtest_emu_mutation_survived(st));
            Native.asmtest_emu_mutation_stat_free(st);
            return outv;
        }

        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.emu_close(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>A memory-write watchpoint result (Track F). Dispose when done.</summary>
    public sealed class Watch : IDisposable
    {
        IntPtr _h;
        public Watch() => _h = Native.asmtest_emu_watch_new();
        public IntPtr Handle => _h;
        public bool Violated => Native.asmtest_emu_watch_violated(_h) != 0;
        public ulong Addr => Native.asmtest_emu_watch_addr(_h);
        public ulong RipOff => Native.asmtest_emu_watch_rip_off(_h);
        public void Dispose() { if (_h != IntPtr.Zero) { Native.asmtest_emu_watch_free(_h); _h = IntPtr.Zero; } }
    }

    /// <summary>A register-invariant guard result (Track F). Dispose when done.</summary>
    public sealed class RegGuard : IDisposable
    {
        IntPtr _h;
        public RegGuard() => _h = Native.asmtest_emu_reg_guard_new();
        public IntPtr Handle => _h;
        public bool Violated => Native.asmtest_emu_reg_guard_violated(_h) != 0;
        public ulong Got => Native.asmtest_emu_reg_guard_got(_h);
        public void Dispose() { if (_h != IntPtr.Zero) { Native.asmtest_emu_reg_guard_free(_h); _h = IntPtr.Zero; } }
    }

    /// <summary>AVX2 256-bit capture (Track D) + the CPUID probe that gates it.</summary>
    public static class Avx
    {
        /// <summary>True if the host CPU + OS support AVX2.</summary>
        public static bool CpuHasAvx2() => Native.asmtest_cpu_has_avx2() != 0;

        /// <summary>Run <paramref name="fn"/> with 256-bit vector args (each four
        /// f64 lanes) and return the four f64 lanes of ymm0 (the vector return).</summary>
        public static double[] CaptureVec256(IntPtr fn, double[][] vargs)
        {
            var outv = new byte[16 * 32];
            var va = new byte[8 * 32];
            for (int i = 0; i < Math.Min(vargs.Length, 8); i++)
                for (int l = 0; l < 4; l++)
                    BitConverter.GetBytes(vargs[i][l]).CopyTo(va, i * 32 + l * 8);
            Native.asm_call_capture_vec256(outv, fn, new long[6], va);
            var ret = new double[4];
            for (int l = 0; l < 4; l++) ret[l] = BitConverter.ToDouble(outv, l * 8);
            return ret;
        }
    }

    /// <summary>An emulator run's outcome — faults surfaced as data, not a crash.</summary>
    /// <summary>Invalid-access kind reported by <see cref="EmuResult.FaultKind"/> (mirrors emu_fault_kind_t).</summary>
    public enum FaultKind { None = 0, Read = 1, Write = 2, Fetch = 3 }

    public sealed class EmuResult : IDisposable
    {
        IntPtr _h;
        public EmuResult() => _h = Native.asmtest_emu_result_new();
        internal IntPtr Handle => _h;

        /// <summary>Whether the routine took an invalid-memory fault.</summary>
        public bool Faulted => Native.asmtest_emu_result_faulted(_h) != 0;

        /// <summary>Faulting guest address; only meaningful when <see cref="Faulted"/>.</summary>
        public ulong FaultAddr => Native.asmtest_emu_result_fault_addr(_h);

        /// <summary>Why the access was invalid; only meaningful when <see cref="Faulted"/>.</summary>
        public FaultKind FaultKind => (FaultKind)Native.asmtest_emu_result_fault_kind(_h);

        /// <summary>Read an x86-64 guest register by name — GP plus "rip" / "rflags".</summary>
        public ulong Reg(string name) => Native.asmtest_emu_x86_reg(_h, name);

        /// <summary>Lane (0..1) of guest XMM register <paramref name="index"/> as a double (scalar return = XmmF64(0, 0)).</summary>
        public double XmmF64(int index, int lane) => Native.asmtest_emu_x86_xmm_f64(_h, index, lane);

        /// <summary>Lane (0..3) of guest XMM register <paramref name="index"/> as a float.</summary>
        public float XmmF32(int index, int lane) => Native.asmtest_emu_x86_xmm_f32(_h, index, lane);

        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.asmtest_emu_result_free(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>An opaque execution-trace / basic-block coverage recorder. Dispose to free.</summary>
    public sealed class Trace : IDisposable
    {
        IntPtr _h;
        public Trace(nuint insnsCap = 4096, nuint blocksCap = 4096)
            => _h = Native.asmtest_emu_trace_new(insnsCap, blocksCap);
        internal IntPtr Handle => _h;
        /// <summary>True if the basic block at byte-offset <paramref name="off"/> was entered.</summary>
        public bool Covered(ulong off) => Native.asmtest_emu_trace_covered(_h, off) != 0;
        public void Dispose()
        {
            if (_h != IntPtr.Zero) { Native.asmtest_emu_trace_free(_h); _h = IntPtr.Zero; }
        }
    }

    /// <summary>A cross-arch guest ISA.</summary>
    public enum GuestArch { Arm64, RiscV, Arm }

    /// <summary>A cross-arch run's outcome; registers are read by name. Dispose to free.</summary>
    public sealed class GuestResult : IDisposable
    {
        IntPtr _h;
        readonly GuestArch _arch;
        internal GuestResult(GuestArch arch)
        {
            _arch = arch;
            _h = arch switch
            {
                GuestArch.Arm64 => Native.asmtest_emu_arm64_result_new(),
                GuestArch.RiscV => Native.asmtest_emu_riscv_result_new(),
                GuestArch.Arm => Native.asmtest_emu_arm_result_new(),
                _ => IntPtr.Zero,
            };
        }
        internal IntPtr Handle => _h;
        public bool Faulted => Native.asmtest_emu_result_faulted(_h) != 0;
        /// <summary>Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0").</summary>
        public ulong Reg(string name) => _arch switch
        {
            GuestArch.Arm64 => Native.asmtest_emu_arm64_reg(_h, name),
            GuestArch.RiscV => Native.asmtest_emu_riscv_reg(_h, name),
            GuestArch.Arm => Native.asmtest_emu_arm_reg(_h, name),
            _ => 0,
        };
        public void Dispose()
        {
            if (_h == IntPtr.Zero) return;
            switch (_arch)
            {
                case GuestArch.Arm64: Native.asmtest_emu_arm64_result_free(_h); break;
                case GuestArch.RiscV: Native.asmtest_emu_riscv_result_free(_h); break;
                case GuestArch.Arm: Native.asmtest_emu_arm_result_free(_h); break;
            }
            _h = IntPtr.Zero;
        }
    }

    /// <summary>
    /// A cross-arch Unicorn guest (arm64/riscv/arm) running raw machine-code bytes
    /// — emulated regardless of host arch. Dispose to close.
    /// </summary>
    public sealed class Guest : IDisposable
    {
        IntPtr _h;
        readonly GuestArch _arch;
        public Guest(GuestArch arch)
        {
            _arch = arch;
            _h = arch switch
            {
                GuestArch.Arm64 => Native.emu_arm64_open(),
                GuestArch.RiscV => Native.emu_riscv_open(),
                GuestArch.Arm => Native.emu_arm_open(),
                _ => IntPtr.Zero,
            };
        }
        /// <summary>Run raw bytes with integer args in the guest ABI registers.</summary>
        public GuestResult Call(byte[] code, params long[] args)
        {
            var res = new GuestResult(_arch);
            switch (_arch)
            {
                case GuestArch.Arm64: Native.emu_arm64_call(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle); break;
                case GuestArch.RiscV: Native.emu_riscv_call(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle); break;
                case GuestArch.Arm: Native.emu_arm_call(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle); break;
            }
            return res;
        }
        /// <summary>Like <see cref="Call"/>, but record an execution trace / coverage (arm64).</summary>
        public GuestResult CallTraced(byte[] code, long[] args, Trace trace)
        {
            if (_arch != GuestArch.Arm64) throw new AsmtestException("traced guest run only wired for arm64");
            var res = new GuestResult(_arch);
            args ??= Array.Empty<long>();
            Native.emu_arm64_call_traced(_h, code, (nuint)code.Length, args, args.Length, 0, res.Handle, trace.Handle);
            return res;
        }
        public void Dispose()
        {
            if (_h == IntPtr.Zero) return;
            switch (_arch)
            {
                case GuestArch.Arm64: Native.emu_arm64_close(_h); break;
                case GuestArch.RiscV: Native.emu_riscv_close(_h); break;
                case GuestArch.Arm: Native.emu_arm_close(_h); break;
            }
            _h = IntPtr.Zero;
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
        public static void Fault(EmuResult res)
        {
            if (!res.Faulted) throw new AsmtestException("expected a fault, but the run completed cleanly");
        }
        public static void EmuReg(EmuResult res, string name, ulong want)
        {
            if (res.Reg(name) != want)
                throw new AsmtestException($"emu {name}: got {res.Reg(name)}, want {want}");
        }
        public static void GuestReg(GuestResult res, string name, ulong want)
        {
            if (res.Reg(name) != want)
                throw new AsmtestException($"guest {name}: got {res.Reg(name)}, want {want}");
        }
        public static void Covered(Trace trace, ulong off)
        {
            if (!trace.Covered(off)) throw new AsmtestException($"block {off}: expected covered");
        }
    }
}
