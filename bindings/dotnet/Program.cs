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

        // --- Tier 1: cross-arch emulator guests (raw bytes, any host) ------- //
        foreach (var (arch, code, regname) in new (GuestArch, byte[], string)[]
        {
            (GuestArch.Arm64, new byte[] { 0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6 }, "x0"),
            (GuestArch.RiscV, new byte[] { 0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00 }, "a0"),
            (GuestArch.Arm, new byte[] { 0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1 }, "r0"),
        })
        {
            using var g = new Guest(arch);
            using var res = g.Call(code, 40, 2);
            Check($"emu_{arch.ToString().ToLower()}.add", !res.Faulted && res.Reg(regname) == 42);
        }

        // --- Tier 1: extended x86-64 emulator calls (raw bytes) ------------- //
        using (var e = new Emu())
        {
            using (var res = e.CallBytes(new byte[] { 0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3 }, 10, 20, 12))
                Check("emu.wide_int", !res.Faulted && res.Reg("rax") == 42);
            using (var res = e.CallFp(new byte[] { 0xF2, 0x0F, 0x58, 0xC1, 0xC3 }, null, new double[] { 1.5, 2.25 }))
                Check("emu.fp_add", !res.Faulted && res.XmmF64(0, 0) == 3.75);
            using (var res = e.CallVec(new byte[] { 0x0F, 0x58, 0xC1, 0xC3 }, null,
                       new float[][] { new float[] { 1, 2, 3, 4 }, new float[] { 10, 20, 30, 40 } }))
                Check("emu.vec_add4f", !res.Faulted && res.XmmF32(0, 0) == 11 && res.XmmF32(0, 3) == 44);
            using (var res = e.CallWin64(new byte[] { 0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3 }, 40, 2))
                Check("emu.win64_add", !res.Faulted && res.Reg("rax") == 42);
        }

        // --- Tier 1: execution trace / coverage (cross-arch arm64) ---------- //
        using (var g = new Guest(GuestArch.Arm64))
        using (var tr = new Trace())
        {
            byte[] sel = { 0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
                           0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6 };
            using var res = g.CallTraced(sel, new long[] { 0 }, tr);
            Check("emu_arm64.trace_sel", !res.Faulted && res.Reg("x0") == 42
                && tr.Covered(0) && tr.Covered(12) && !tr.Covered(4));
        }

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

        // Track F: mid-execution guards (byte-literal routines).
        using (var e = new Emu())
        {
            byte[] twoWrites = { 0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3 };
            e.Map(0x400000UL, (nuint)0x1000);
            var w = e.WatchWrites(0x400000UL, (nuint)8, 1);
            e.CallBytes(twoWrites, 0x400000).Dispose();
            e.WatchClear();
            Check("guard.watch_escape", w.Violated && w.Addr == 0x400800UL && w.RipOff == 3);
            w.Dispose();
            byte[] clobber = { 0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3 };
            var g = e.GuardReg("rbx", 0);
            e.CallBytes(clobber).Dispose();
            e.GuardRegClear();
            Check("guard.reg_invariant", g != null && g.Violated && g.Got == 0x99);
            g?.Dispose();
        }

        // Track E: coverage-guided fuzzing + mutation testing over classify3.
        using (var e = new Emu())
        {
            byte[] classify3 = { 0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
                0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3 };
            var fixedS = e.FuzzCover(classify3, 5, 5, 1);
            var guided = e.FuzzCover(classify3, -50, 50, 2000);
            Check("fuzz.coverage_beats_fixed", guided.Blocks > fixedS.Blocks);
            var weak = e.MutationTest(classify3, new long[] { 5 });
            var strong = e.MutationTest(classify3, new long[] { -7, 0, 9 });
            Check("mutation.strong_kills_more", weak.Survived > 0 && strong.Survived < weak.Survived);
        }

        // Track D: AVX2 256-bit capture (self-skips off-AVX2).
        if (Avx.CpuHasAvx2())
        {
            var outv = Avx.CaptureVec256(Routine("vec_add4d"),
                new[] { new double[] { 1, 2, 3, 4 }, new double[] { 10, 20, 30, 40 } });
            Check("vec256.add4d", outv[0] == 11 && outv[1] == 22 && outv[2] == 33 && outv[3] == 44);
        }
        else { Console.WriteLine("ok - vec256.add4d # SKIP no AVX2"); }

        Console.WriteLine($"# {total - fails} passed, {fails} failed, {total} total");
        return fails == 0 ? 0 : 1;
    }
}
