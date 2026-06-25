// Program.cs — asm-test .NET binding (Track D): the conformance runner.
//
// A thin consumer of the reusable library module (Asmtest.cs): it replays the
// cross-language conformance corpus through the typed Regs / Emu / Assert API
// and never touches P/Invoke itself. Exits nonzero on any mismatch. The native
// libs are found via the loader (LD_LIBRARY_PATH points at the asm-test build
// dir; `make dotnet-test` sets it).
using System;
using Asmtest;

static class Conformance
{
    static int fails = 0, total = 0;
    static void Check(string name, bool ok)
    {
        total++;
        Console.WriteLine((ok ? "ok - " : "not ok - ") + name);
        if (!ok) fails++;
    }

    static IntPtr Routine(string n) => Corpus.Routine(n);

    static int Main()
    {
        // --- Tier 1: corpus replay (capture trampoline) --------------------- //
        using (var r = new Regs())
        {
            r.Capture6(Routine("add_signed"), 40, 2);
            Check("add_signed.basic", r.Ret == 42 && r.AbiPreserved);

            r.Capture6(Routine("sum_via_rbx"), 20, 22);
            Check("sum_via_rbx.abi_preserved", r.Ret == 42 && r.AbiPreserved);

            r.Capture6(Routine("clobbers_rbx"), 1, 2);
            Check("clobbers_rbx.abi_violation_detected", !r.AbiPreserved);

            r.Capture6(Routine("set_carry"));
            Check("set_carry.cf_set", r.FlagSet("CF"));

            r.Capture6(Routine("clear_carry"));
            Check("clear_carry.cf_clear", !r.FlagSet("CF"));

            r.CaptureFp2(Routine("fp_add"), 1.5, 2.25);
            Check("fp_add.basic", r.FRet == 3.75);
        }

        // --- Tier 1: corpus replay (emulator, x86-64 guest) ----------------- //
        using (var e = new Emu())
        using (var res = e.Call2(Routine("add_signed"), 40, 2))
            Check("emu.add_signed", !res.Faulted && res.Reg("rax") == 42);

        // --- Tier 1: in-line assembly (Keystone) replays add_signed --------- //
        // Only when the loaded lib carries the assembler (libasmtest_emu_asm).
        if (Emu.AsmAvailable)
            using (var e = new Emu())
            using (var res = e.CallAsm("mov rax, rdi; add rax, rsi; ret", 40, 2, out bool asmOk))
                Check("asm.add_signed", asmOk && !res.Faulted && res.Reg("rax") == 42);

        // --- Tier 2: idiomatic assertions pass on good input ---------------- //
        bool t2pass = true;
        try
        {
            using var r = new Regs();
            r.Capture6(Routine("add_signed"), 40, 2);
            Assert.Ret(r, 42);
            Assert.AbiPreserved(r);
            r.CaptureFp2(Routine("fp_add"), 1.5, 2.25);
            Assert.Fp(r, 3.75);
        }
        catch (AsmtestException) { t2pass = false; }
        Check("tier2.assertions_pass", t2pass);

        // --- Tier 2: the assertions actually fail when they should ---------- //
        bool t2teeth = false;
        try
        {
            using var r = new Regs();
            r.Capture6(Routine("add_signed"), 40, 2);
            Assert.Ret(r, 99); // wrong on purpose
        }
        catch (AsmtestException) { t2teeth = true; }
        Check("tier2.assertions_have_teeth", t2teeth);

        Console.WriteLine($"# {total - fails} passed, {fails} failed, {total} total");
        return fails == 0 ? 0 : 1;
    }
}
