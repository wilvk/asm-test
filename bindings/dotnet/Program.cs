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

            r.CaptureVecF32(Routine("vec_add4f"),
                new float[][] { new float[] { 1, 2, 3, 4 }, new float[] { 10, 20, 30, 40 } });
            var v = r.VecF32(0);
            Check("vec_add4f.basic", v[0] == 11 && v[1] == 22 && v[2] == 33 && v[3] == 44);
        }

        // --- Tier 1: corpus replay (emulator, x86-64 guest) ----------------- //
        using (var e = new Emu())
        using (var res = e.Call2(Routine("add_signed"), 40, 2))
            Check("emu.add_signed", !res.Faulted && res.Reg("rax") == 42);

        // read_fault dereferences an unmapped address: the fault is data — where
        // (FaultAddr) and why (FaultKind) — not a crash.
        using (var e = new Emu())
        using (var res = e.Call2(Routine("read_fault"), 0x00DEAD00, 0))
            Check("emu.read_fault", res.Faulted && res.FaultAddr == 0x00DEAD00UL
                && res.FaultKind == FaultKind.Read);

        // int_to_double lands (double)42 in xmm0 (the XMM file, beyond the GP regs);
        // a clean run also keeps rflags live (x86 holds bit 1 set).
        using (var e = new Emu())
        using (var res = e.Call2(Routine("int_to_double"), 42, 0))
            Check("emu.int_to_double", !res.Faulted && res.XmmF64(0, 0) == 42.0
                && (res.Reg("rflags") & 0x2UL) != 0);

        // --- Tier 1: in-line assembly (Keystone) replays add_signed --------- //
        // Only when the loaded lib carries the assembler (libasmtest_emu_asm).
        if (Emu.AsmAvailable)
        {
            using (var e = new Emu())
            using (var res = e.CallAsm("mov rax, rdi; add rax, rsi; ret", new long[] { 40, 2 }))
                Check("asm.add_signed", !res.Faulted && res.Reg("rax") == 42);

            // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
            using (var e = new Emu())
            using (var res = e.CallAsm("mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
                                       new long[] { 10, 20, 12 }, AsmSyntax.Att))
                Check("asm.att_3arg", !res.Faulted && res.Reg("rax") == 42);

            // Failure path: a bad string throws with the Keystone diagnostic, not a silent miss.
            bool threw = false;
            try { using var e = new Emu(); e.CallAsm("mov rax, nonsense_token"); }
            catch (AsmtestException ex) { threw = ex.Message.Length > "in-line assembly failed: ".Length; }
            Check("asm.bad_source_throws", threw);

            // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
            byte[] a64 = Emu.Assemble("ret", AsmArch.Arm64);
            Check("asm.arm64_bytes", a64.Length == 4 && a64[0] == 0xC0 && a64[3] == 0xD6);
        }

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
            r.CaptureVecF32(Routine("vec_add4f"),
                new float[][] { new float[] { 1, 2, 3, 4 }, new float[] { 10, 20, 30, 40 } });
            Assert.VecF32(r, 0, new float[] { 11, 22, 33, 44 });
            using (var e = new Emu())
            using (var res = e.Call2(Routine("read_fault"), 0x00DEAD00, 0))
                Assert.Fault(res);
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
