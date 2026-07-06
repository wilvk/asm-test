// HwTraceProgram.cs — runnable smoke test for the single-step hardware-trace
// wrapper (HwTrace.cs), mirroring bindings/python/tests/test_hwtrace.py.
//
// The SINGLESTEP backend runs on ANY x86-64 Linux, so this asserts a real, live
// trace here and in CI/containers, self-skipping only off x86-64 Linux (lib
// absent / backend unavailable) — printing "# SKIP ..." and exiting 0 so the lane
// stays green. Otherwise it traces both fixtures live, prints TAP-style
// "ok N - ..." lines, and returns nonzero on any assertion failure.
using System;
using System.IO;
using System.Runtime.InteropServices;
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

    // Call-descent fixture (x86-64): a caller region R that calls an in-blob leaf S.
    //   R@0:   mov rax,rdi; call S(+4); add rax,rsi; ret   (region = 0xc bytes)
    //   S@0xc: inc rax; ret
    // The traced region is R only (0xc); S lives BEYOND it in the same allocation, so
    // trace_call_ex must be passed region=0xc — passing the whole 0x10-byte allocation
    // would fold S into the region and mis-record the call as recursion.
    static readonly byte[] DESCENT_BLOB =
    {
        0x48, 0x89, 0xF8, 0xE8, 0x04, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xF0, 0xC3, 0x48, 0xFF, 0xC0, 0xC3,
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
            Check(tr2.Covered(0) && tr2.Covered(0x7) && tr2.Covered(0xf),
                  "loop preamble + body + exit blocks covered");
            // 3 blocks {0,0x7,0xf}: the ret after the not-taken jnz is its own block.
            Check(tr2.BlocksLen() == 3, $"blocks_len == 3 (got {tr2.BlocksLen()})");
            Check(!tr2.Truncated(), "not truncated");

            tr2.Free();
            code2.Free();

            // --- reference shim: AsmTrace (using scope + auto-name + render) --- //
            // Statement form so Dispose-populated Path/Truncated are readable after.
            var code3 = NativeCode.FromBytes(ROUTINE);
            long r3 = 0;
            AsmTrace scope;
            using (scope = new AsmTrace(code3, emit: false)) // auto-name at THIS line
            {
                r3 = code3.Call(20, 22);
            }
            Check(r3 == 42, $"AsmTrace: add2(20,22) == 42 (got {r3})");
            Check(scope.Armed, "AsmTrace: armed on an available backend");
            Check(scope.Path != null && scope.Path.Length > 0, "AsmTrace: render-on-close produced text");
            int r3Lines = scope.Path.Split('\n').Length - 1;
            Check(r3Lines == 5, $"AsmTrace: 5 rendered instruction lines (got {r3Lines})");
            Check(scope.Path.Contains("ret"), "AsmTrace: rendered listing includes the ret");
            Check(!scope.Truncated, "AsmTrace: not truncated");
            Check(scope.Name.Contains(":"), $"AsmTrace: auto-name is member:line (got {scope.Name})");
            code3.Free();

            // --- §Z0/§Z1: the aspirational EMPTY-ctor form: using (new AsmTrace()) --- //
            // Region-free WHOLE-WINDOW capture — no NativeCode. The single-step WEAK tier
            // records the ABSOLUTE addresses of WHATEVER ran on the thread (the runtime
            // included), exposed as Addresses for attribution — not auto-rendered.
            var code4 = NativeCode.FromBytes(ROUTINE);
            long r4pre = code4.Call(20, 22); // warm the call path before the window
            long r4 = 0;
            AsmTrace ww;
            using (ww = new AsmTrace(emit: false)) // EMPTY ctor — no region, auto-named here
            {
                r4 = code4.Call(20, 22);
            }
            if (ww.Armed)
            {
                Check(r4 == 42 && r4pre == 42, $"AsmTrace(): whole-window call returns 42 (got {r4})");
                Check(ww.Addresses.Length > 0,
                      $"AsmTrace(): whole-window captured instructions (got {ww.Addresses.Length})");
                Check(ww.Name.Contains(":"), $"AsmTrace(): auto-name is member:line (got {ww.Name})");
            }
            else
            {
                // Honest self-skip (§Z5): no faithful whole-window backend on this host.
                Check(ww.SkipReason.Length > 0, $"AsmTrace(): self-skip records a reason ({ww.SkipReason})");
            }
            code4.Free();

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

            // --- cross-tier orchestrator: structural invariants (every host) --- //
            // Mirrors test_cross_tier_resolve_invariants: every HW choice satisfies the
            // hardware-tier probe; NATIVE choices precede the single VIRTUAL emulator
            // floor (the last entry under Best); NativeOnly is Best minus that floor;
            // CeilingFree drops AMD LBR; AutoTier(Best) is the head of ResolveTiers(Best).
            var tBest = HwTrace.ResolveTiers(TracePolicy.Best);
            var tNat = HwTrace.ResolveTiers(TracePolicy.NativeOnly);
            var tCf = HwTrace.ResolveTiers(TracePolicy.CeilingFree);

            bool ctAvail = true, ctFidelity = true;
            foreach (var c in tBest)
            {
                if (c.Tier == TraceTier.HwTrace && !HwTrace.Available(c.Backend)) ctAvail = false;
                var wantFid = c.Tier == TraceTier.Emulator ? TraceFidelity.Virtual : TraceFidelity.Native;
                if (c.Fidelity != wantFid) ctFidelity = false;
            }
            Check(ctAvail, "cross-tier: every HW-tier choice satisfies the hardware probe");
            Check(ctFidelity, "cross-tier: emulator choice is Virtual, all others Native");

            int nEmu = 0;
            foreach (var c in tBest) if (c.Tier == TraceTier.Emulator) nEmu++;
            Check(tBest.Length > 0 && tBest[tBest.Length - 1].Tier == TraceTier.Emulator,
                  "cross-tier: emulator floor is the last entry under Best");
            Check(nEmu == 1, "cross-tier: exactly one emulator floor under Best");

            bool natNoEmu = true;
            foreach (var c in tNat) if (c.Tier == TraceTier.Emulator) natNoEmu = false;
            Check(natNoEmu, "cross-tier: NativeOnly forbids the native->emulator crossing");
            Check(tNat.Length == tBest.Length - 1, "cross-tier: NativeOnly is Best minus the floor");

            bool cfNoAmdTier = true;
            foreach (var c in tCf)
                if (c.Tier == TraceTier.HwTrace && c.Backend == HwBackend.AmdLbr) cfNoAmdTier = false;
            Check(cfNoAmdTier, "cross-tier: CeilingFree drops AMD LBR");

            var one = HwTrace.AutoTier(TracePolicy.Best);
            Check(one.HasValue
                  && one.Value.Tier == tBest[0].Tier
                  && one.Value.Backend == tBest[0].Backend,
                  "cross-tier: AutoTier(Best) is the head of ResolveTiers(Best)");

            // --- cross-tier: NativeOnly resolves on x86-64 Linux (single-step floor) --- //
            // Mirrors test_cross_tier_native_only_resolves_on_linux_x86_64: single-step
            // is a native floor here, so even NativeOnly never collapses to nothing.
            var pick = HwTrace.AutoTier(TracePolicy.NativeOnly);
            bool hasSsFloor = false;
            foreach (var c in tNat)
                if (c.Tier == TraceTier.HwTrace && c.Backend == HwBackend.SingleStep) hasSsFloor = true;
            Check(tNat.Length > 0 && pick.HasValue && pick.Value.Fidelity == TraceFidelity.Native,
                  "cross-tier: NativeOnly resolves a Native choice on x86-64 Linux");
            Check(hasSsFloor, "cross-tier: single-step is the NativeOnly floor on x86-64 Linux");
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

        // --- out-of-process / foreign-process toolkit (Asmtest.Ptrace) --- //
        // Mirrors the four tests after the "foreign-process toolkit" banner in
        // bindings/python/tests/test_hwtrace.py. Guarded by a ptrace-available skip
        // (the lib is loaded here, but the host may not be Linux x86-64). trace_attached
        // needs no live test — it is referenced (wrapped) by Ptrace.TraceAttached.
        if (Ptrace.Available())
        {
            PtraceChecks();
            DescentChecks();
        }
        else
            Console.WriteLine($"# SKIP ptrace toolkit unavailable: {Ptrace.SkipReason()}");

        // --- conformance replay: the ptrace_descent corpus tier (replay-or-skip) --- //
        // Mirrors bindings/python/tests/test_conformance._run_ptrace_descent: replay the
        // corpus's L1 (edges) and L2 (frames) cases live when ptrace is available AND the
        // corpus arch matches the host, else SKIP (never fail).
        DescentConformanceReplay();

        // --- time-aware code-image recorder (Asmtest.CodeImage) --- //
        // The userspace PERF_RECORD_TEXT_POKE: snapshot a region, then read back the bytes
        // that were live at a logical timestamp. Self-skips off a host without
        // PAGEMAP_SCAN / soft-dirty. The bpf detector is probed separately (it self-skips
        // without libbpf / CAP_BPF).
        if (CodeImage.Available())
            CodeImageChecks();
        else
            Console.WriteLine($"# SKIP codeimage recorder unavailable: {CodeImage.SkipReason()}");

        Console.WriteLine($"1..{_n}");
        return _failed ? 1 : 0;
    }

    // mov rax,rdi; add rax,rsi; ret  (the bytes round-tripped through the code image)
    static readonly byte[] CI_ROUTINE = { 0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3 };

    static void CodeImageChecks()
    {
        // Track a real exec region (this process, pid 0) and round-trip its bytes through
        // the timeline: track snapshots version 0, Now() advances to >= 1, Refresh() is a
        // cheap no-op (>= 0 new versions), and BytesAt(base, 0) returns the latest live
        // bytes — which must equal what we put there.
        var code = NativeCode.FromBytes(CI_ROUTINE);
        try
        {
            using (var img = new CodeImage(0))
            {
                img.Track(code.Base, (nuint)code.Length);
                Check(img.Now() >= 1, $"codeimage: Now() >= 1 after Track (got {img.Now()})");

                int refreshed = img.Refresh();
                Check(refreshed >= 0, $"codeimage: Refresh() >= 0 (got {refreshed})");

                byte[] live = img.BytesAt(code.Base, 0);
                Check(live != null && live.Length >= CI_ROUTINE.Length
                      && PrefixEq(live, CI_ROUTINE),
                      $"codeimage: BytesAt(base, 0) round-trips CI_ROUTINE (got {(live == null ? "null" : live.Length + " bytes")})");
            }
        }
        finally
        {
            code.Free();
        }

        // bpf emission detector: SIDEBAND-only probe. Skip without libbpf / CAP_BPF;
        // otherwise attaching to our own pid must succeed (ASMTEST_CI_OK == 0).
        if (!CodeImage.BpfAvailable())
        {
            Console.WriteLine($"# SKIP codeimage bpf detector unavailable: {CodeImage.BpfSkipReason()}");
        }
        else
        {
            using (var img = new CodeImage(0))
            {
                int rc = img.WatchBpf();
                Check(rc == 0, $"codeimage: WatchBpf() == 0 (got {rc})");
            }
        }
    }

    // True if a[0..b.Length) equals b (the timeline may report more bytes to the region end).
    static bool PrefixEq(byte[] a, byte[] b)
    {
        if (a.Length < b.Length) return false;
        for (int i = 0; i < b.Length; i++)
            if (a[i] != b[i]) return false;
        return true;
    }

    static void PtraceChecks()
    {
        int pid = System.Diagnostics.Process.GetCurrentProcess().Id;

        // (1) trace_call: fork a tracee, single-step it out of process, same offsets.
        {
            var code = NativeCode.FromBytes(ROUTINE);
            var tr = HwTrace.Create(blocks: 64, instructions: 64);
            long result = Ptrace.TraceCall(code.Base, (nuint)code.Length,
                                           new long[] { 20, 22 }, tr.Handle);
            Check(result == 42, $"ptrace: trace_call(20,22) == 42 (got {result})");
            var insns = tr.InsnOffsets();
            var wantInsns = new ulong[] { 0x0, 0x3, 0x6, 0xC, 0x11 };
            Check(Eq(insns, wantInsns), $"ptrace: trace_call offsets {Hex(insns)} == {Hex(wantInsns)}");
            Check(!tr.Truncated(), "ptrace: trace_call not truncated");
            tr.Free();
            code.Free();
        }

        // (1b) run_to drives an attached target to a resolved method (software
        // breakpoint). A live foreign attach is covered by the C suite; exercise the FFI
        // round-trip safely — a NULL target address is rejected (EINVAL, non-zero)
        // before any ptrace call.
        Check(Ptrace.RunTo(pid, IntPtr.Zero) != 0,
              "ptrace: run_to(NULL addr) rejected (EINVAL) via the FFI round-trip");

        // (2) region_by_addr: discover an executable region's extent by an interior
        // address (this process); addr 1 maps nothing.
        {
            var code = NativeCode.FromBytes(ROUTINE);
            var region = Ptrace.ProcRegionByAddr(pid, code.Base + 4);
            Check(region.HasValue, "ptrace: region_by_addr finds the mapping");
            if (region.HasValue)
            {
                Check(region.Value.Base == code.Base, "ptrace: region_by_addr base == code base");
                Check(region.Value.Len >= (nuint)ROUTINE.Length,
                      $"ptrace: region_by_addr len >= {ROUTINE.Length} (got {region.Value.Len})");
            }
            else
            {
                Check(false, "ptrace: region_by_addr base == code base");
                Check(false, $"ptrace: region_by_addr len >= {ROUTINE.Length}");
            }
            Check(Ptrace.ProcRegionByAddr(pid, (IntPtr)1) == null,
                  "ptrace: region_by_addr(addr 1) == null");
            code.Free();
        }

        // (3) perfmap_symbol: write /tmp/perf-<pid>.map, resolve a method by name,
        // and a miss returns null.
        {
            string path = $"/tmp/perf-{pid}.map";
            File.WriteAllText(path, "400000 1a void demo(long, long)\n500000 8 other\n");
            try
            {
                var hit = Ptrace.ProcPerfmapSymbol(pid, "void demo(long, long)");
                Check(hit.HasValue && hit.Value.Base == (IntPtr)0x400000 && hit.Value.Len == (nuint)0x1A,
                      "ptrace: perfmap_symbol resolves (base 0x400000, len 0x1a)");
                Check(Ptrace.ProcPerfmapSymbol(pid, "missing") == null,
                      "ptrace: perfmap_symbol(missing) == null");
            }
            finally
            {
                File.Delete(path);
            }
        }

        // (4) jitdump_find: write a binary jitdump (little-endian) per the Python
        // layout, resolve a method to (addr,size,index,ts) + bytes; a miss is null.
        {
            string path = Path.Combine(Path.GetTempPath(), $"asmtest-jit-{pid}.dump");
            byte[] name = System.Text.Encoding.ASCII.GetBytes("void demo(long, long)");
            using (var fs = new FileStream(path, FileMode.Create, FileAccess.Write))
            using (var w = new BinaryWriter(fs))
            {
                // header: magic, version, total_size=40, elf_mach, pad1, pid, timestamp, flags
                w.Write((uint)0x4A695444); // 'JiTD' (little-endian magic)
                w.Write((uint)1);
                w.Write((uint)40);
                w.Write((uint)62);
                w.Write((uint)0);
                w.Write((uint)0);
                w.Write((ulong)0);
                w.Write((ulong)0);
                // JIT_CODE_LOAD record: id, total, ts
                uint total = (uint)(16 + 40 + (name.Length + 1) + ROUTINE.Length);
                w.Write((uint)0);
                w.Write(total);
                w.Write((ulong)5);
                // body: pid, tid, vma, code_addr, code_size, code_index
                w.Write((uint)0);
                w.Write((uint)0);
                w.Write((ulong)0x2000);
                w.Write((ulong)0x2000);
                w.Write((ulong)ROUTINE.Length);
                w.Write((ulong)9);
                w.Write(name);
                w.Write((byte)0);
                w.Write(ROUTINE);
            }
            try
            {
                var m = Ptrace.JitdumpFind(path, "void demo(long, long)", wantBytes: 64);
                Check(m.HasValue, "ptrace: jitdump_find resolves the method");
                if (m.HasValue)
                {
                    var v = m.Value;
                    Check(v.CodeAddr == 0x2000 && v.CodeSize == (ulong)ROUTINE.Length
                          && v.CodeIndex == 9 && v.Timestamp == 5,
                          $"ptrace: jitdump_find (addr 0x{v.CodeAddr:x}, size {v.CodeSize}, idx {v.CodeIndex}, ts {v.Timestamp})");
                    Check(BytesEq(v.Code, ROUTINE), "ptrace: jitdump_find code bytes == ROUTINE");
                }
                else
                {
                    Check(false, "ptrace: jitdump_find (addr/size/index/ts)");
                    Check(false, "ptrace: jitdump_find code bytes == ROUTINE");
                }
                Check(Ptrace.JitdumpFind(path, "missing") == null,
                      "ptrace: jitdump_find(missing) == null");
            }
            finally
            {
                File.Delete(path);
            }
        }
    }

    static bool BytesEq(byte[] a, byte[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++)
            if (a[i] != b[i]) return false;
        return true;
    }

    // --- call descent (Asmtest.Descent) --------------------------------------- //
    // Mirrors bindings/python/tests/test_hwtrace.py::test_descent_edges_and_frames plus a
    // resolver-upcall smoke that proves the managed delegate fires from the single-step
    // loop. The traced region is R only (0xc); S is the in-blob leaf beyond it.
    static void DescentChecks()
    {
        var code = NativeCode.FromBytes(DESCENT_BLOB);
        ulong codeBase = (ulong)code.Base.ToInt64();
        try
        {
            // L1 RECORD_EDGES: R's own body + one (call -> S) edge; S stepped over.
            using (var d = new Descent(DescentLevel.RecordEdges))
            {
                long r = Ptrace.TraceCallEx(code, new long[] { 20, 22 }, IntPtr.Zero, d, region: (nuint)0xc);
                Check(r == 43, $"descent L1: trace_call_ex(20,22) == 43 (got {r})");
                Check(d.FramesLen() == 1, $"descent L1: frames_len == 1 (got {d.FramesLen()})");
                var f0 = d.FrameInsns(0);
                var wantF0 = new ulong[] { 0x0, 0x3, 0x8, 0xb };
                Check(Eq(f0, wantF0), $"descent L1: frame0 insns {Hex(f0)} == {Hex(wantF0)}");
                var edges = d.Edges();
                Check(edges.Length == 1
                      && edges[0].Site == 0x3
                      && edges[0].Target == codeBase + 0xc
                      && edges[0].Depth == 0,
                      $"descent L1: one edge (site 0x3 -> S at base+0xc, depth 0) (got {edges.Length})");
                Check(!d.Truncated(), "descent L1: not truncated");
            }

            // L2 DESCEND_KNOWN + allow_region(S): S descends as frame 1, no edges.
            using (var d = new Descent(DescentLevel.DescendKnown))
            {
                Check(d.AllowRegion(code.Base + 0xc, (nuint)4) == 0, "descent L2: allow_region(S) ok");
                long r = Ptrace.TraceCallEx(code, new long[] { 20, 22 }, IntPtr.Zero, d, region: (nuint)0xc);
                Check(r == 43, $"descent L2: trace_call_ex(20,22) == 43 (got {r})");
                Check(d.FramesLen() == 2, $"descent L2: frames_len == 2 (got {d.FramesLen()})");
                Check(d.FrameBase(1) == codeBase + 0xc, "descent L2: frame1 base == S (base+0xc)");
                Check(d.FrameDepth(1) == 1, $"descent L2: frame1 depth == 1 (got {d.FrameDepth(1)})");
                Check(d.FrameParent(1) == 0, $"descent L2: frame1 parent == 0 (got {d.FrameParent(1)})");
                var f1 = d.FrameInsns(1);
                var wantF1 = new ulong[] { 0x0, 0x3 };
                Check(Eq(f1, wantF1), $"descent L2: frame1 insns {Hex(f1)} == {Hex(wantF1)}");
                Check(d.Edges().Length == 0, "descent L2: no edges (descended, not stepped over)");
            }

            // Resolver upcall: L2 with NO allow-region — a managed resolver delegate drives
            // the descent into S, proving the trampoline is invoked (and survives GC) from
            // the out-of-process single-step loop.
            using (var d = new Descent(DescentLevel.DescendKnown))
            {
                int calls = 0;
                ulong seen = 0;
                ulong target = codeBase + 0xc;
                d.SetResolver(callee =>
                {
                    calls++;
                    seen = callee;
                    // Resolve S to its 4-byte region; refuse everything else.
                    return callee == target ? (true, target, 4UL) : (false, 0UL, 0UL);
                });
                long r = Ptrace.TraceCallEx(code, new long[] { 20, 22 }, IntPtr.Zero, d, region: (nuint)0xc);
                Check(r == 43, $"descent resolver: trace_call_ex(20,22) == 43 (got {r})");
                Check(calls >= 1, $"descent resolver: upcall fired (calls={calls})");
                Check(seen == target, $"descent resolver: saw S callee 0x{seen:x} == 0x{target:x}");
                Check(d.FramesLen() == 2, $"descent resolver: resolver drove descent into S (frames_len={d.FramesLen()})");
                Check(d.FrameBase(1) == target, "descent resolver: frame1 base == S (base+0xc)");
            }
        }
        finally
        {
            code.Free();
        }
    }

    // Replay the cross-language corpus's ptrace_descent tier (L1 edges + L2 frames).
    // Host-native, so it SKIPs (never fails) unless the corpus arch (x86_64) matches the
    // host AND the out-of-process single-step stepper is available — mirroring Python's
    // test_conformance._run_ptrace_descent and the C reference's ptrace_descent tier.
    static void DescentConformanceReplay()
    {
        if (RuntimeInformation.ProcessArchitecture != Architecture.X64)
        {
            Console.WriteLine("# SKIP ptrace_descent conformance: corpus arch x86_64 != host");
            return;
        }
        if (!Ptrace.Available())
        {
            Console.WriteLine($"# SKIP ptrace_descent conformance: {Ptrace.SkipReason()}");
            return;
        }

        var code = NativeCode.FromBytes(DESCENT_BLOB);
        ulong codeBase = (ulong)code.Base.ToInt64();
        try
        {
            // Case 1 (level 1): result 43, frame0 [0,3,8,11], one edge site 3 -> base+12.
            using (var d = new Descent(DescentLevel.RecordEdges))
            {
                long r = Ptrace.TraceCallEx(code, new long[] { 20, 22 }, IntPtr.Zero, d, region: (nuint)12);
                Check(r == 43, $"conformance ptrace_descent L1: result == 43 (got {r})");
                Check(Eq(d.FrameInsns(0), new ulong[] { 0, 3, 8, 11 }),
                      "conformance ptrace_descent L1: frame0 == [0,3,8,11]");
                var edges = d.Edges();
                Check(edges.Length == 1 && edges[0].Site == 3 && edges[0].Target == codeBase + 12,
                      "conformance ptrace_descent L1: edge site 3 -> base+12");
            }

            // Case 2 (level 2): allow S (base+12,4); frame at base+12 depth 1 insns [0,3].
            using (var d = new Descent(DescentLevel.DescendKnown))
            {
                d.AllowRegion(code.Base + 12, (nuint)4);
                long r = Ptrace.TraceCallEx(code, new long[] { 20, 22 }, IntPtr.Zero, d, region: (nuint)12);
                Check(r == 43, $"conformance ptrace_descent L2: result == 43 (got {r})");
                Check(Eq(d.FrameInsns(0), new ulong[] { 0, 3, 8, 11 }),
                      "conformance ptrace_descent L2: frame0 == [0,3,8,11]");
                int idx = -1;
                for (int i = 1; i < d.FramesLen(); i++)
                    if (d.FrameBase(i) == codeBase + 12) { idx = i; break; }
                Check(idx >= 1, "conformance ptrace_descent L2: descended frame at base+12 present");
                if (idx >= 1)
                {
                    Check(d.FrameDepth(idx) == 1, $"conformance ptrace_descent L2: frame depth == 1 (got {d.FrameDepth(idx)})");
                    Check(Eq(d.FrameInsns(idx), new ulong[] { 0, 3 }),
                          "conformance ptrace_descent L2: frame insns == [0,3]");
                }
            }
        }
        finally
        {
            code.Free();
        }
    }
}
