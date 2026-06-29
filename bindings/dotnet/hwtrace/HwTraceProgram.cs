// HwTraceProgram.cs — runnable smoke test for the single-step hardware-trace
// wrapper (HwTrace.cs), mirroring bindings/python/tests/test_hwtrace.py.
//
// The SINGLESTEP backend runs on ANY x86-64 Linux, so this asserts a real, live
// trace here and in CI/containers, self-skipping only off x86-64 Linux (lib
// absent / backend unavailable) — printing "# SKIP ..." and exiting 0 so the lane
// stays green. Otherwise it traces both fixtures live, prints TAP-style
// "ok N - ..." lines, and returns nonzero on any assertion failure.
using System;
using Asmtest;

static class HwTraceProgram
{
    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
    static readonly byte[] ROUTINE =
    {
        0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
        0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
    };

    // mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (a real loop, no depth ceiling)
    static readonly byte[] LOOP =
    {
        0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3,
    };

    static int _n;
    static bool _failed;

    // TAP-style result line: "ok N - desc" on pass, "not ok N - desc" on fail.
    static void Check(bool cond, string desc)
    {
        _n++;
        if (cond)
            Console.WriteLine($"ok {_n} - {desc}");
        else
        {
            Console.WriteLine($"not ok {_n} - {desc}");
            _failed = true;
        }
    }

    // Render an offset list as "[0x0, 0x3, 0x6, ...]" for the verbatim dump.
    static string Hex(ulong[] offs) =>
        "[" + string.Join(", ", Array.ConvertAll(offs, o => "0x" + o.ToString("x"))) + "]";

    static bool Eq(ulong[] a, ulong[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++)
            if (a[i] != b[i]) return false;
        return true;
    }

    static int Main()
    {
        // Self-skip unless the single-step backend can run on this host.
        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# SKIP single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }

        try
        {
            HwTrace.Init(HwBackend.SingleStep);
        }
        catch (HwTraceException e)
        {
            Console.WriteLine($"# SKIP hwtrace init failed: {e.Message}");
            return 0;
        }

        try
        {
            // --- fixture 1: ROUTINE, exact instruction stream + block coverage --- //
            var code = NativeCode.FromBytes(ROUTINE);
            var tr = HwTrace.Create(blocks: 64, instructions: 64);
            tr.Register("add2", code);

            long r = 0;
            tr.Region("add2", () => { r = code.Call(20, 22); }); // 42 <= 100 -> jle taken, dec skipped
            Check(r == 42, $"add2(20,22) == 42 (got {r})");

            var insns = tr.InsnOffsets();
            var wantInsns = new ulong[] { 0x0, 0x3, 0x6, 0xC, 0x11 };
            Check(Eq(insns, wantInsns), $"insn offsets {Hex(insns)} == {Hex(wantInsns)}");
            Check(tr.InsnsTotal() == 5, $"insns_total == 5 (got {tr.InsnsTotal()})");
            Check(tr.Covered(0) && tr.Covered(0x11), "entry + tail blocks covered");
            Check(tr.BlocksLen() == 2, $"blocks_len == 2 (got {tr.BlocksLen()})");
            Check(!tr.Truncated(), "not truncated");

            tr.Free();
            code.Free();

            // --- fixture 2: LOOP, no depth ceiling (every back-edge captured) --- //
            var code2 = NativeCode.FromBytes(LOOP);
            var tr2 = HwTrace.Create(blocks: 64, instructions: 256);
            tr2.Register("loop", code2);

            long r2 = 0;
            tr2.Region("loop", () => { r2 = code2.Call(1, 20); });
            Check(r2 == 20, $"loop(1,20) == 20 (got {r2})");
            Check(tr2.InsnsTotal() == 62, $"insns_total == 62 (got {tr2.InsnsTotal()})"); // 1 + 20*3 + 1
            Check(tr2.Covered(0) && tr2.Covered(0x7), "loop preamble + body blocks covered");
            Check(tr2.BlocksLen() == 2, $"blocks_len == 2 (got {tr2.BlocksLen()})");
            Check(!tr2.Truncated(), "not truncated");

            tr2.Free();
            code2.Free();

            // --- auto-select: selection invariants (hold on every host) --- //
            // Mirrors test_auto_resolve_selection_invariants: resolve(BEST) returns
            // only available backends in ascending-enum order with no dups;
            // CEILING_FREE excludes AMD_LBR and is a subset of BEST; auto(BEST) is the
            // head of resolve(BEST) (or -3 when the cascade is empty).
            var best = HwTrace.Resolve(HwPolicy.Best);
            var cf = HwTrace.Resolve(HwPolicy.CeilingFree);

            bool okAvail = true, okOrder = true;
            for (int i = 0; i < best.Length; i++)
            {
                if (!HwTrace.Available((HwBackend)best[i])) okAvail = false;
                if (i > 0 && best[i] <= best[i - 1]) okOrder = false;
            }
            Check(okAvail, "auto: Resolve(Best) returns only available backends");
            Check(okOrder, "auto: Resolve(Best) ordered by descending fidelity, no dups");

            bool cfNoAmd = true, cfSubset = true;
            foreach (int b in cf)
            {
                if (b == (int)HwBackend.AmdLbr) cfNoAmd = false;
                if (Array.IndexOf(best, b) < 0) cfSubset = false;
            }
            Check(cfNoAmd, "auto: CeilingFree never selects AMD LBR");
            Check(cfSubset, "auto: CeilingFree is a subset of Best");

            int ab = HwTrace.Auto(HwPolicy.Best);
            bool headOk = best.Length == 0
                ? ab == HwNative.ASMTEST_HW_EUNAVAIL
                : ab == best[0];
            Check(headOk, "auto: Auto(Best) is the head of Resolve(Best)");
        }
        finally
        {
            HwTrace.Shutdown();
        }

        // --- auto-select: live trace through whatever auto picks --- //
        // Mirrors test_auto_resolve_traces_live: single-step keeps the cascade
        // non-empty here, so auto() resolves a usable backend. Owns its own
        // init/shutdown (one global lifecycle, distinct from the bracket above).
        {
            var best = HwTrace.Resolve(HwPolicy.Best);
            int ab = HwTrace.Auto(HwPolicy.Best);
            Check(best.Length >= 1 && ab >= 0, "auto: resolves a backend (single-step floor)");

            if (ab >= 0)
            {
                HwTrace.Init((HwBackend)ab);
                try
                {
                    var code = NativeCode.FromBytes(ROUTINE);
                    var tr = HwTrace.Create(blocks: 64, instructions: 64);
                    tr.Register("auto", code);

                    long r = 0;
                    tr.Region("auto", () => { r = code.Call(20, 22); });
                    Check(r == 42, $"auto: auto-selected backend traces a live call (got {r})");
                    Check(tr.Covered(0), "auto: auto-selected backend covers block offset 0");
                    if (ab == HwNative.SINGLESTEP)
                    {
                        var insns = tr.InsnOffsets();
                        var wantInsns = new ulong[] { 0x0, 0x3, 0x6, 0xC, 0x11 };
                        Check(Eq(insns, wantInsns), $"auto: single-step pick yields {Hex(wantInsns)} (got {Hex(insns)})");
                    }

                    tr.Free();
                    code.Free();
                }
                finally
                {
                    HwTrace.Shutdown();
                }
            }
        }

        Console.WriteLine($"1..{_n}");
        return _failed ? 1 : 0;
    }
}
