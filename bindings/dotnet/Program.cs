// Program.cs — asm-test .NET binding (Track D), via P/Invoke.
//
// Replays the conformance corpus through the opaque-handle FFI layer (no struct
// layout needed): asmtest_corpus_routine for addresses, asmtest_capture6 / _fp2
// + accessors for capture, and asmtest_emu_call2 + accessors for the emulator.
// Exits nonzero on any mismatch. The native libs are found via the loader
// (LD_LIBRARY_PATH points at the asm-test build dir; `make dotnet-test` sets it).
//
//   ASMTEST_LIB / ASMTEST_CORPUS_LIB document the libs; the loader resolves
//   "asmtest_emu" / "asmtest_corpus" by soname.
using System;
using System.Runtime.InteropServices;

static class Native
{
    const string EMU = "asmtest_emu";
    const string COR = "asmtest_corpus";

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
    [DllImport(EMU)] public static extern int asmtest_emu_result_faulted(IntPtr r);
    [DllImport(EMU)] public static extern ulong asmtest_emu_x86_reg(IntPtr r, string name);
}

sealed class AsmtestException : Exception
{
    public AsmtestException(string message) : base(message) { }
}

static class Conformance
{
    static int fails = 0, total = 0;

    // Tier-2 idiomatic assertions: throw with a clear message on failure.
    static void AssertRet(IntPtr r, ulong e)
    {
        var got = Native.asmtest_regs_ret(r);
        if (got != e) throw new AsmtestException($"ret: got {got}, want {e}");
    }
    static void AssertAbiPreserved(IntPtr r)
    {
        if (Native.asmtest_check_abi(r, IntPtr.Zero, 0) != 0)
            throw new AsmtestException("ABI not preserved");
    }
    static void AssertFp(IntPtr r, double e)
    {
        var got = Native.asmtest_regs_fret(r);
        if (got != e) throw new AsmtestException($"fp: got {got}, want {e}");
    }

    static void Check(string name, bool ok)
    {
        total++;
        if (ok) Console.WriteLine($"ok - {name}");
        else { fails++; Console.WriteLine($"not ok - {name}"); }
    }

    static IntPtr Routine(string n) => Native.asmtest_corpus_routine(n);

    static int Main()
    {
        IntPtr r = Native.asmtest_regs_new();

        Native.asmtest_capture6(r, Routine("add_signed"), 40, 2, 0, 0, 0, 0);
        Check("add_signed.basic",
            Native.asmtest_regs_ret(r) == 42 && Native.asmtest_check_abi(r, IntPtr.Zero, 0) == 0);

        Native.asmtest_capture6(r, Routine("sum_via_rbx"), 20, 22, 0, 0, 0, 0);
        Check("sum_via_rbx.abi_preserved",
            Native.asmtest_regs_ret(r) == 42 && Native.asmtest_check_abi(r, IntPtr.Zero, 0) == 0);

        Native.asmtest_capture6(r, Routine("clobbers_rbx"), 1, 2, 0, 0, 0, 0);
        Check("clobbers_rbx.abi_violation_detected",
            Native.asmtest_check_abi(r, IntPtr.Zero, 0) != 0);

        Native.asmtest_capture6(r, Routine("set_carry"), 0, 0, 0, 0, 0, 0);
        Check("set_carry.cf_set", Native.asmtest_regs_flag_set(r, "CF") == 1);

        Native.asmtest_capture6(r, Routine("clear_carry"), 0, 0, 0, 0, 0, 0);
        Check("clear_carry.cf_clear", Native.asmtest_regs_flag_set(r, "CF") == 0);

        Native.asmtest_capture_fp2(r, Routine("fp_add"), 1.5, 2.25);
        Check("fp_add.basic", Native.asmtest_regs_fret(r) == 3.75);
        Native.asmtest_regs_free(r);

        // Emulator: faults as data via the opaque handle.
        IntPtr e = Native.emu_open();
        IntPtr res = Native.asmtest_emu_result_new();
        Native.asmtest_emu_call2(e, Routine("add_signed"), 40, 2, res);
        Check("emu.add_signed",
            Native.asmtest_emu_result_faulted(res) == 0
            && Native.asmtest_emu_x86_reg(res, "rax") == 42);
        Native.asmtest_emu_result_free(res);
        Native.emu_close(e);

        // Tier-2 idiomatic assertions: pass paths succeed, failure paths bite.
        IntPtr r2 = Native.asmtest_regs_new();
        bool t2pass = true;
        try
        {
            Native.asmtest_capture6(r2, Routine("add_signed"), 40, 2, 0, 0, 0, 0);
            AssertRet(r2, 42);
            AssertAbiPreserved(r2);
            Native.asmtest_capture_fp2(r2, Routine("fp_add"), 1.5, 2.25);
            AssertFp(r2, 3.75);
        }
        catch (AsmtestException) { t2pass = false; }
        Check("tier2.assertions_pass", t2pass);

        bool t2teeth = false;
        try
        {
            Native.asmtest_capture6(r2, Routine("add_signed"), 40, 2, 0, 0, 0, 0);
            AssertRet(r2, 99); // wrong on purpose
        }
        catch (AsmtestException) { t2teeth = true; }
        Check("tier2.assertions_have_teeth", t2teeth);
        Native.asmtest_regs_free(r2);

        Console.WriteLine($"# {total - fails} passed, {fails} failed, {total} total");
        return fails == 0 ? 0 : 1;
    }
}
