// HwTraceProgram.cs — runnable smoke test for the single-step hardware-trace
// wrapper (HwTrace.cs), mirroring bindings/python/tests/test_hwtrace.py.
//
// The SINGLESTEP backend runs on ANY x86-64 Linux, so this asserts a real, live
// trace here and in CI/containers, self-skipping only off x86-64 Linux (lib
// absent / backend unavailable) — printing "# SKIP ..." and exiting 0 so the lane
// stays green. Otherwise it traces both fixtures live, prints TAP-style
// "ok N - ..." lines, and returns nonzero on any assertion failure.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
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

        // Slow-host crash-avoidance stress (managed-singlestep-lazy-arm-plan,
        // "Sharpening 1"): ASMTEST_METHOD_STRESS=<N> runs ONLY the unpinned
        // tiering-worker stress and exits — see MethodStress below.
        string stress = Environment.GetEnvironmentVariable("ASMTEST_METHOD_STRESS");
        if (!string.IsNullOrEmpty(stress))
        {
            MethodStress(int.TryParse(stress, out int sn) && sn > 0 ? sn : 60);
            Console.WriteLine($"1..{_n}");
            return _failed ? 1 : 0;
        }

        // --- §Z0 auto-init: a whole-window scope needs no explicit HwTrace.Init --- //
        // BEFORE any Init below: the empty-ctor whole-window ctor must lazily bring up the
        // single-step tier itself, so `using (new AsmTrace())` works with zero setup.
        AsmTrace autoInit;
        using (autoInit = new AsmTrace(emit: false)) { }
        Check(autoInit.Armed,
              $"AsmTrace(): ctor auto-inits the single-step tier without an explicit HwTrace.Init ({autoInit.SkipReason})");

        try
        {
            HwTrace.Init(HwBackend.SingleStep);
        }
        catch (HwTraceException e)
        {
            Console.WriteLine($"# SKIP hwtrace init failed: {e.Message}");
            return 0;
        }

        // --- AsmMethod name parsing (pure, host-independent) --- //
        AsmMethodChecks();

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

            // --- §Z5 renderPath: opt-in whole-window render into Path --- //
            // The data-only default leaves Path empty (Addresses/Disassembly instead);
            // renderPath: true renders the window via the native render_window. The
            // routine's `ret` executed in-window, so it must appear in the text.
            AsmTrace wp;
            using (wp = new AsmTrace(emit: false, renderPath: true))
            {
                code4.Call(20, 22);
            }
            if (wp.Armed)
            {
                if (HwNative.asmtest_disas_available())
                    Check(wp.Path.Length > 0 && wp.Path.Contains("ret"),
                          $"AsmTrace(renderPath): whole-window Path renders the live bytes ({wp.Path.Length} chars)");
                else
                    Console.WriteLine("# SKIP renderPath assert: build without Capstone");
            }
            code4.Free();

            // --- §Z1 byMethod: annotated disassembly (Disassembly) --- //
            // A byMethod whole-window scope over a COLD, pure-compute managed method: the
            // labelled instructions are disassembled from live memory and paired with their
            // method name. Assert the invariant Disassembly.Count == LabelledInstructions
            // (Capstone present) and that the cold method is named in the stream.
            long tm = 0;
            AsmTrace wm;
            using (wm = new AsmTrace(emit: false, byMethod: true)) // cold-only labelling
            {
                tm = TinyManaged(1000);
            }
            if (wm.Armed)
            {
                Check(tm == 500500, $"AsmTrace(byMethod): TinyManaged(1000) == 500500 (got {tm})");
                if (wm.DisassemblyAvailable)
                {
                    Check(wm.Disassembly.Count == wm.LabelledInstructions,
                          $"AsmTrace(byMethod): Disassembly.Count == LabelledInstructions ({wm.Disassembly.Count} vs {wm.LabelledInstructions})");
                    bool allText = wm.Disassembly.Count > 0, named = false;
                    foreach (AsmInstruction ins in wm.Disassembly)
                    {
                        if (string.IsNullOrEmpty(ins.Text) || string.IsNullOrEmpty(ins.Method)) allText = false;
                        if (ins.Method.Contains("TinyManaged")) named = true;
                    }
                    Check(allText, "AsmTrace(byMethod): every disassembled insn has text + a method");
                    Check(named, "AsmTrace(byMethod): the cold method 'TinyManaged' is named in the stream");
                }
                else
                    Console.WriteLine("# SKIP annotated disassembly asserts: build without Capstone");
            }

            // --- §D0.2 withRundown: WARM + R2R naming via the diagnostics-socket rundown --- //
            // Console's write path is R2R BCL (precompiled, never JIT'd), so the cold-only
            // listener can never name it; only the jitdump rundown can. Any [PreJIT] entry
            // in Methods proves both halves: the rundown named an R2R method AND captured
            // instructions were attributed to it. Self-skips (no failure) where the
            // diagnostics socket is off (DOTNET_EnableDiagnostics=0) — RundownEnabled is
            // then false and only cold methods are named.
            AsmTrace wr;
            using (wr = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            {
                Console.WriteLine("# (inside the rundown window — this write path is R2R BCL)");
            }
            if (wr.Armed && wr.RundownEnabled)
            {
                bool r2rNamed = false;
                foreach (AsmMethod m in wr.Methods)
                    if (m.Name.Contains("[PreJIT]")) { r2rNamed = true; break; }
                Check(r2rNamed,
                      $"AsmTrace(withRundown): an R2R [PreJIT] method is named in the window ({wr.Methods.Count} methods)");
            }
            else if (wr.Armed)
                Console.WriteLine("# SKIP rundown asserts: diagnostics socket unavailable (cold-only result)");

            // --- §D0.3: the NAMED-METHOD form — AsmTrace.Method(delegate) --- //
            // A fresh COLD method: PrepareMethod inside Method() forces its standalone
            // JIT, the listener resolves (addr, size), and the scope is a region over
            // exactly that body — reliable offsets, not a best-effort window.
            long nr = 0;
            AsmTrace nm;
            using (nm = AsmTrace.Method((Func<long, long, long>)NamedCold, emit: false))
            {
                nr = (long)nm.Invoke(20L, 22L);
            }
            Check(nr == 42, $"AsmTrace.Method: NamedCold(20,22) == 42 (got {nr})");
            Check(nm.Armed, $"AsmTrace.Method: named-method scope armed ({nm.SkipReason})");
            if (nm.Armed)
            {
                Check(!nm.Truncated, "AsmTrace.Method: body capture not truncated");
                // B (lazy-arm) parity: the in-process arm→call→disarm must CAPTURE the
                // body, not silently miss it. With Capstone present the rendered Path is
                // non-empty and ends in ret — proof the reverse-P/Invoke pointer landed in
                // the resolved [base,len). A silent empty trace (the call still returns 42)
                // is the failure mode this asserts against, so it is a hard Check, not a skip.
                if (HwNative.asmtest_disas_available())
                {
                    Check(nm.Path.Length > 0,
                          $"AsmTrace.Method: lazy-arm captured a non-empty body ({nm.Path.Length} chars)");
                    Check(nm.Path.Contains("ret"),
                          "AsmTrace.Method: lazy-arm body renders (ends in ret) — landed in the resolved body");
                }
                else
                    Console.WriteLine("# SKIP Method render assert: build without Capstone");
            }

            // --- §D3: the same form OUT OF PROCESS (stealth stepper) --- //
            // Invoke routes through asmtest_hwtrace_stealth_trace: a bundled helper
            // reverse-attaches (PR_SET_PTRACER + PTRACE_SEIZE) and steps the body out
            // of band — this thread is never armed with EFLAGS.TF. Self-skips where
            // Yama refuses the attach or the helper is unavailable.
            long or2 = 0;
            AsmTrace om;
            using (om = AsmTrace.Method((Func<long, long, long>)NamedColdOop, emit: false,
                                        outOfProcess: true))
            {
                or2 = (long)om.Invoke(6L, 7L);
            }
            Check(or2 == 42, $"AsmTrace.Method(oop): NamedColdOop(6,7) == 42 (got {or2})");
            if (om.Armed)
            {
                if (om.Path.Length > 0)
                    Check(om.Path.Contains("ret"),
                          "AsmTrace.Method(oop): stealth-stepped body renders (ret present)");
                else
                    Console.WriteLine("# SKIP Method(oop) render assert: build without Capstone");
            }
            else
                Console.WriteLine($"# SKIP stealth stepper: {om.SkipReason}");

            // --- Option B FP shim: a (double...)->double method traces IN-PROCESS --- //
            // The integer (long...)->long shim set can't express this signature; the FP
            // family (call_scoped_fp, xmm0..7) keeps it in-process instead of forcing the
            // out-of-process fallback. Same managed-safe lazy-arm guarantee.
            double fr = 0;
            AsmTrace fm;
            using (fm = AsmTrace.Method((Func<double, double, double>)NamedFpCold, emit: false))
            {
                fr = (double)fm.Invoke(1.5, 2.5);
            }
            Check(fr == 4.0, $"AsmTrace.Method(fp): NamedFpCold(1.5,2.5) == 4.0 (got {fr})");
            Check(fm.Armed, $"AsmTrace.Method(fp): FP lazy-arm scope armed in-process ({fm.SkipReason})");
            if (fm.Armed && HwNative.asmtest_disas_available())
                Check(fm.Path.Length > 0 && fm.Path.Contains("ret"),
                      "AsmTrace.Method(fp): FP body captured in-process (non-empty, ends in ret)");

            // --- §D0.4 async-hop stitching: one logical op across a real thread hop --- //
            StitchChecks().GetAwaiter().GetResult();

            // --- §1 handle-keyed scopes: concurrent SAME-SITE regions don't alias --- //
            // Both threads construct their scope through SameSiteScope below, so they
            // share ONE auto-name (member:line). begin_scope gives each its own
            // per-thread frame + handle; each Dispose must render ITS OWN routine.
            // Event-sequenced so the arm windows overlap deterministically:
            // A arms -> B arms -> A runs+closes -> B runs+closes.
            var codeA = NativeCode.FromBytes(ROUTINE);   // renders cmp/jle
            var codeB = NativeCode.FromBytes(LOOP);      // renders dec/jne
            string pathA = null, pathB = null;
            bool armedA = false, armedB = false;
            using (var aArmed = new ManualResetEventSlim(false))
            using (var bArmed = new ManualResetEventSlim(false))
            using (var aClosed = new ManualResetEventSlim(false))
            {
                var ta = new Thread(() =>
                {
                    var s = SameSiteScope(codeA);
                    armedA = s.Armed;
                    aArmed.Set();
                    bArmed.Wait(5000);       // both same-site scopes armed concurrently
                    codeA.Call(20, 22);
                    s.Dispose();
                    pathA = s.Path;
                    aClosed.Set();
                });
                var tb = new Thread(() =>
                {
                    aArmed.Wait(5000);
                    var s = SameSiteScope(codeB);
                    armedB = s.Armed;
                    bArmed.Set();
                    aClosed.Wait(5000);      // A ran and closed while B stayed open
                    codeB.Call(7, 3);
                    s.Dispose();
                    pathB = s.Path;
                });
                ta.Start(); tb.Start();
                ta.Join(); tb.Join();
            }
            if (armedA && armedB)
            {
                Check(pathA != null && pathA.Contains("cmp"),
                      "handle-keyed: thread A's same-site scope renders ITS routine (cmp)");
                Check(pathB != null && (pathB.Contains("dec") || pathB.Contains("jne")),
                      "handle-keyed: thread B's same-site scope renders ITS loop (dec/jne)");
                Check(pathB != null && !pathB.Contains("cmp"),
                      "handle-keyed: B's slice carries none of A's instructions");
            }
            else
                Console.WriteLine($"# SKIP handle-keyed concurrency: arm failed (A={armedA}, B={armedB})");
            codeA.Free();
            codeB.Free();

            // --- TF sibling stress: scope armed while GC/alloc threads run free --- //
            // The accepted §Z1 posture single-steps THIS thread only; sibling runtime
            // threads must run native and the process must stay healthy. Bounded churn.
            var stop = new bool[1];
            var churn = new Thread[3];
            for (int i = 0; i < churn.Length; i++)
            {
                churn[i] = new Thread(() =>
                {
                    long acc = 0;
                    while (!Volatile.Read(ref stop[0]))
                    {
                        var a = new byte[512];
                        acc += a.Length;
                        if ((acc & 0x3ffff) == 0) GC.Collect(0);
                    }
                });
                churn[i].IsBackground = true;
                churn[i].Start();
            }
            long sr = 0;
            AsmTrace st;
            using (st = new AsmTrace(emit: false, byMethod: true))
            {
                sr = TinyManaged(500);
            }
            Volatile.Write(ref stop[0], true);
            foreach (var t2 in churn) t2.Join();
            Check(sr == 125250, $"TF stress: TinyManaged(500) == 125250 under sibling churn (got {sr})");
            Check(st.Armed, $"TF stress: scope armed + closed with GC/alloc siblings live ({st.SkipReason})");

            // §Z4 cross-thread Dispose is intentionally NOT exercised in-process here:
            // a single-step whole-window scope arms EFLAGS.TF on the CALLING thread, and
            // only that thread can clear it — disposing on another thread flags Truncated
            // but leaves the arming thread stepping (a guaranteed SIGTRAP crash). This is
            // the very hazard that forbids single-step for managed cross-thread work; the
            // C suite covers the truncated-flag path with a phantom handle instead
            // (examples/test_hwtrace.c), and the safe managed cross-thread route is §D3.

            // --- tier-up observation (best-effort; background tiering is async) --- //
            var tmap = new JitMethodMap();
            long tacc = 0;
            for (int i = 0; i < 400; i++) tacc += HotTier(i);
            Thread.Sleep(300); // give the rejitted body's MethodLoadVerbose a moment
            int vers = tmap.CountFor("HotTier");
            tmap.Dispose();
            if (vers >= 2)
                Check(true, $"tier-up: {vers} versions of HotTier observed at distinct addresses");
            else
                Console.WriteLine($"# SKIP tier-up not observed in time (versions={vers}); nondeterministic");

            // --- §Z5.2: the composed degradation ladder --- //
            string note = HwTrace.DegradationNote();
            Check(note.Contains("SingleStep") || note.Contains("using"),
                  $"degradation note composes the ladder ({note})");

            // --- Disas classifiers over live bytes (structural, not string-parsed) --- //
            var codeCls = NativeCode.FromBytes(ROUTINE);
            var codeBlob = NativeCode.FromBytes(DESCENT_BLOB);
            if (Disas.Available)
            {
                ulong cb = (ulong)(long)codeCls.Base;
                Check(Disas.IsRet(cb + 0x11), "Disas.IsRet: ret at +0x11");
                Check(Disas.IsBranch(cb + 0xc) && !Disas.IsCall(cb + 0xc),
                      "Disas: jle at +0xc is a branch, not a call");
                Check(!Disas.IsBranch(cb) && !Disas.IsRet(cb), "Disas: mov at +0 is neither");
                ulong bb = (ulong)(long)codeBlob.Base;
                Check(Disas.IsCall(bb + 3), "Disas.IsCall: call at +3 in the descent blob");
                Check(Disas.TryCallTarget(bb + 3, out ulong tgt) && tgt == bb + 0xc,
                      $"Disas.TryCallTarget resolves S at base+0xc (got +0x{tgt - bb:x})");
            }
            else
                Console.WriteLine("# SKIP Disas classifier asserts: build without Capstone");
            codeCls.Free();
            codeBlob.Free();

            // --- symbolize_bucket: address→module attribution (whole-window noise) --- //
            // HwTrace.SymbolizeBuckets classifies a list of ABSOLUTE addresses by the
            // mapped-file / perf-map region containing each — the post-close primitive that
            // names the ~1M-instruction runtime lump a whole-window scope captures. Feed it a
            // known interior address (an exec_alloc'd mapping) twice plus one unmapped address
            // (1): every address must land in SOME bucket (total count preserved), and the
            // unmapped one falls under "[unknown]".
            var codeSym = NativeCode.FromBytes(ROUTINE);
            ulong known = (ulong)codeSym.Base.ToInt64();
            var buckets = HwTrace.SymbolizeBuckets(new ulong[] { known, known, 1UL });
            ulong bTotal = 0; bool hasUnknown = false;
            foreach (HwBucket b in buckets) { bTotal += b.Count; if (b.Label == "[unknown]") hasUnknown = true; }
            Check(buckets.Length >= 1 && bTotal == 3,
                  $"symbolize_bucket: all 3 addresses bucketed ({buckets.Length} buckets, total {bTotal})");
            Check(hasUnknown, "symbolize_bucket: the unmapped address (1) buckets under [unknown]");
            Check(HwTrace.SymbolizeBuckets(Array.Empty<ulong>()).Length == 0,
                  "symbolize_bucket: empty input yields no buckets");
            codeSym.Free();

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
                    // Mirror the C reference (examples/test_hwtrace.c test_auto_select): the pick is
                    // single-step on a PT-/AMD-LBR-less host (block 0 covered), but AMD LBR on a Zen
                    // 3+/4/5 host with perf HONESTLY TRUNCATES this tiny single-shot fixture (too
                    // short to be sampled in-region — see amd_backend.c). So the honest invariant is
                    // "covered OR truncated", not "covered". The byte-exact stream is asserted only
                    // for the single-step pick below.
                    Check(tr.Covered(0) || tr.Truncated(),
                          "auto: auto-selected backend covers block offset 0 (or honestly truncates)");
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

        // --- auto-escalating CALL-OWNING cross-tier trace (HwTrace.TraceCallAuto) --- //
        // Mirrors test_trace_call_auto_owns_the_call_and_completes: it self-manages the
        // tier lifecycle, so it runs STANDALONE here (no HwTrace.Init bracket around it).
        TraceCallAutoChecks();

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

        // --- §E1: AsmTrace.WindowHybrid (survey -> hot-slice exact window) --- //
        WindowHybridChecks();

        // --- §E3: sibling-thread live JIT publish (extensions plan E3) --- //
        // Mechanism first (ptrace-free — validates the listener -> queue -> sibling ->
        // channel handoff on ANY host, privileged or not), then the OOP-window
        // integration case (self-skips where the reverse attach is denied).
        SiblingPublisherChecks();
        WindowLiveJitChecks();

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

    // Pure-parse checks for AsmMethod.Assembly / .ShortName (no backend needed). The
    // jitdump/perf-map grammar is "<ret> [<assembly>] Type::Method(sig)[tier]"; the assembly
    // must anchor on the "] " immediately before "::" — NOT the first "[" — so a bracketed
    // return type ("!0[]" array / generic), a generic TYPE ("[System.__Canon]") right before
    // "::", and an "[Assembly]" inside the ARG list are all handled.
    static void AsmMethodChecks()
    {
        var plain = new AsmMethod("void [System.Console] System.Console::WriteLine(string)[PreJIT]", 1);
        Check(plain.Assembly == "System.Console", $"AsmMethod.Assembly plain == System.Console (got '{plain.Assembly}')");
        Check(plain.ShortName == "System.Console::WriteLine(string)", $"AsmMethod.ShortName plain (got '{plain.ShortName}')");

        // Array return "!0[]": its "[]" must NOT be mistaken for the assembly tag.
        var arr = new AsmMethod("instance !0[] [System.Private.CoreLib] System.Buffers.SharedArrayPool`1[System.Char]::Rent(int32)[PreJIT]", 1);
        Check(arr.Assembly == "System.Private.CoreLib", $"AsmMethod.Assembly array-return == CoreLib (got '{arr.Assembly}')");
        Check(arr.ShortName == "System.Buffers.SharedArrayPool`1[System.Char]::Rent(int32)", $"AsmMethod.ShortName array-return (got '{arr.ShortName}')");

        // Generic type "[System.__Canon]" right before "::": assembly is still CoreLib.
        var gen = new AsmMethod("void [System.Private.CoreLib] System.Runtime.InteropServices.Marshalling.SafeHandleMarshaller`1+ManagedToUnmanagedIn[System.__Canon]::FromManaged(!0)[PreJIT]", 1);
        Check(gen.Assembly == "System.Private.CoreLib", $"AsmMethod.Assembly generic-type == CoreLib (got '{gen.Assembly}')");

        // "[System.Runtime]" inside the arg list (after "::") must not be picked.
        var arg = new AsmMethod("void [System.Console] System.ConsolePal::Write(class [System.Runtime]Microsoft.Win32.SafeHandles.SafeFileHandle,bool)[PreJIT]", 1);
        Check(arg.Assembly == "System.Console", $"AsmMethod.Assembly arg-bracket == System.Console (got '{arg.Assembly}')");

        // Listener-spelled dotted name carries no assembly tag.
        var dotted = new AsmMethod("System.Buffer.Memmove", 1);
        Check(dotted.Assembly == "", $"AsmMethod.Assembly dotted == '' (got '{dotted.Assembly}')");
        Check(dotted.ShortName == "System.Buffer.Memmove", $"AsmMethod.ShortName dotted (got '{dotted.ShortName}')");

        // .Tier — the trailing [tier] tag, NOT a generic-arg bracket or a missing tag.
        Check(plain.Tier == "PreJIT", $"AsmMethod.Tier plain == PreJIT (got '{plain.Tier}')");
        Check(arr.Tier == "PreJIT", $"AsmMethod.Tier array-return == PreJIT (got '{arr.Tier}')");
        var warm = new AsmMethod("void [prog] Program::Main()[MinOptJitted]", 1);
        Check(warm.Tier == "MinOptJitted", $"AsmMethod.Tier warm == MinOptJitted (got '{warm.Tier}')");
        Check(dotted.Tier == "", $"AsmMethod.Tier dotted == '' (got '{dotted.Tier}')");
        // A name ending in a generic-arg bracket (no tier) must NOT report it as a tier.
        var genEnd = new AsmMethod("System.Collections.Generic.List`1[System.String]", 1);
        Check(genEnd.Tier == "", $"AsmMethod.Tier generic-end == '' (got '{genEnd.Tier}')");

        // --- §E2: AsmMethod.Weight — the explicit statistical/exact semantic (== Count) --- //
        Check(plain.Weight == plain.Count, $"AsmMethod.Weight == Count (got {plain.Weight} vs {plain.Count})");
        Check(new AsmMethod("x", 7).Weight == 7, $"AsmMethod.Weight carries the value (got {new AsmMethod("x", 7).Weight})");

        // --- §E1: HotPrefix — the smallest DESCENDING-weight prefix reaching hotFraction --- //
        // Weights [50,30,15,5] (total 100), descending as AsmTrace.Methods is.
        var byW = new List<AsmMethod>
        {
            new AsmMethod("A", 50), new AsmMethod("B", 30),
            new AsmMethod("C", 15), new AsmMethod("D", 5),
        };
        Check(AsmTrace.HotPrefix(byW, 0.9).Count == 3, $"HotPrefix(.9): 50+30+15=95>=90 -> 3 (got {AsmTrace.HotPrefix(byW, 0.9).Count})");
        Check(AsmTrace.HotPrefix(byW, 0.5).Count == 1, $"HotPrefix(.5): 50>=50 -> 1 (got {AsmTrace.HotPrefix(byW, 0.5).Count})");
        Check(AsmTrace.HotPrefix(byW, 0.8).Count == 2, $"HotPrefix(.8): 50+30=80>=80 -> 2 (got {AsmTrace.HotPrefix(byW, 0.8).Count})");
        Check(AsmTrace.HotPrefix(byW, 1.0).Count == 4, $"HotPrefix(1.0): needs all -> 4 (got {AsmTrace.HotPrefix(byW, 1.0).Count})");
        Check(AsmTrace.HotPrefix(byW, 0.0).Count == 1, $"HotPrefix(0): hottest always kept -> 1 (got {AsmTrace.HotPrefix(byW, 0.0).Count})");
        Check(AsmTrace.HotPrefix(byW, 0.9)[0].Name == "A", "HotPrefix keeps the hottest method first");
        Check(AsmTrace.HotPrefix(new List<AsmMethod>(), 0.9).Count == 0, "HotPrefix([]) is empty (degrade signal)");
        // Fraction is clamped, not thrown on: >1 behaves as 1.0.
        Check(AsmTrace.HotPrefix(byW, 5.0).Count == 4, $"HotPrefix(>1 clamps to 1.0) -> 4 (got {AsmTrace.HotPrefix(byW, 5.0).Count})");
    }

    // §E1 fixtures for WindowHybridChecks — two distinct NoInlining managed methods so the
    // survey can rank them and the pass-2 map can resolve each by name. HybridHot is called in
    // a loop (dominates the sampled survey); HybridCold is called ONCE, first (so a plain
    // Window captures it before the hot loop fills the stream — the contrast the hybrid elides).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HybridHot(long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += (i * 2654435761L) ^ (i + 7);
        return s;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HybridCold(long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += (i ^ 0x5bd1e995L) + (i << 1);
        return s;
    }

    // §E1 — AsmTrace.WindowHybrid: pass-1 AMD-LBR survey picks the hot method set, pass-2 exact
    // out-of-process window captures ONLY that slice. Asserts (a) an already-closed scope with a
    // populated Survey, (b) a clean DEGRADE (never throws) when AMD LBR and/or ptrace are
    // absent — the whole-family invariant — and (c), when the survey armed AND the window armed,
    // that the hot slice is captured while a cold helper is elided relative to a plain Window.
    static void WindowHybridChecks()
    {
        long total = 0;
        // Kept SHORT on purpose: this shared self-test process runs with tiered compilation ON
        // (the DOTNET_TC_* footgun guard), so a LONG hot loop would trip OSR / tier-up mid-window
        // and re-JIT under single-step (abort). A short block never tiers, so this gate stays
        // crash-free everywhere; it validates the COMPOSITION + DEGRADE. The full AMD hot-slice
        // RESTRICTION (which needs a long survey loop + tiering off) is demonstrated by
        // examples/dotnet/windowhybrid. The comparison below still fires if the survey happens to
        // rank methods on a fast host; otherwise it is skipped cleanly.
        Action work = () =>
        {
            total += HybridCold(800);
            for (int k = 0; k < 8; k++) total += HybridHot(800);
        };

        AsmTrace ww;
        try { ww = AsmTrace.WindowHybrid(work, hotFraction: 0.9); }
        catch (Exception e) { Check(false, $"WindowHybrid must never throw (got {e.GetType().Name}: {e.Message})"); return; }

        Check(ww != null, "WindowHybrid returns a scope");
        if (ww == null) return;
        // The pass-1 survey is always exposed, armed or not (it drives, or explains, the choice).
        Check(ww.Survey != null, "WindowHybrid.Survey is populated (the pass-1 WindowHot result)");

        if (!ww.Armed)
        {
            // Clean self-skip: ptrace refused (the plain lane) — a reason, no exception.
            Check(ww.SkipReason.Length > 0, $"WindowHybrid: unarmed scope records a self-skip reason ({ww.SkipReason})");
            Console.WriteLine($"# NOTE WindowHybrid degraded/self-skipped: survey.Armed={ww.Survey?.Armed}, reason='{ww.SkipReason}'");
            return;
        }

        // Armed: the exact pass ran. Whether hot-restricted or degraded-to-full, it is a valid
        // exact capture — the block's result and captured stream are honest.
        Check(ww.Addresses.Length > 0, $"WindowHybrid: exact pass captured instructions (got {ww.Addresses.Length})");
        Check(!ww.IsStatistical, "WindowHybrid: the returned (pass-2) scope is EXACT, not statistical");

        if (ww.Survey != null && ww.Survey.Armed && ww.Survey.Methods.Count > 0)
        {
            // Full hybrid: AMD survey drove a hot-slice restriction. Compare against a plain
            // Window over the SAME work — the cold helper must be elided (the real guarantee).
            long hotHybrid = ww.WeightIn("HybridHot");
            long coldHybrid = ww.WeightIn("HybridCold");
            // The hot method is captured ONLY when its pass-2 region resolved to the address that
            // actually executed. This shared self-test process runs with tiering ON, so a hot loop
            // can OSR / tier-up mid-window and re-JIT to a DIFFERENT address than the survey/rundown
            // resolved; the published hot region then misses it and it is (correctly) stepped-over.
            // So confirm the hot capture when it resolved, else NOTE it — never a hard failure here
            // (the stable tiering-off demonstration is examples/dotnet/windowhybrid). The ELISION
            // invariants below hold unconditionally and are the real hot-slice guarantee.
            if (hotHybrid > 0)
                Check(true, $"WindowHybrid: the hot method IS captured exactly (HybridHot weight {hotHybrid})");
            else
                Console.WriteLine("# NOTE WindowHybrid: hot method not captured in this tiering-ON self-test "
                                  + "(OSR/tier-up moved its code past the resolved hot region); hot-slice "
                                  + "capture is shown stably by examples/dotnet/windowhybrid");

            var plainW = AsmTrace.Window(work);
            if (plainW.Armed)
            {
                long coldPlain = plainW.InstructionsIn("HybridCold");
                long hotPlain = plainW.InstructionsIn("HybridHot");
                Console.WriteLine($"# NOTE hybrid vs plain Window — HybridHot: {hotHybrid} vs {hotPlain}; "
                                  + $"HybridCold: {coldHybrid} vs {coldPlain}");
                // The restriction: hybrid never captures MORE cold than plain, and — when plain
                // saw the cold helper at all — hybrid captures strictly fewer (ideally zero).
                Check(coldHybrid <= coldPlain,
                      $"WindowHybrid: cold helper not captured MORE than plain Window ({coldHybrid} <= {coldPlain})");
                if (coldPlain > 0)
                    Check(coldHybrid < coldPlain,
                          $"WindowHybrid: hot-slice publish ELIDES the cold helper vs plain Window ({coldHybrid} < {coldPlain})");
                else
                    Console.WriteLine("# NOTE plain Window captured no HybridCold (buffer/timing) — restriction shown by hot capture only");
            }
            else
                Console.WriteLine($"# NOTE plain Window self-skipped ({plainW.SkipReason}) — hot capture asserted, comparison skipped");
        }
        else
            Console.WriteLine($"# NOTE WindowHybrid degraded to full exact Window (no AMD survey: {ww.Survey?.SkipReason}) — {ww.Methods.Count} methods named");
    }

    // §E3 probe — JIT'd for the FIRST time inside SiblingPublisherChecks (nothing else may
    // call or PrepareMethod it), so its MethodLoadVerbose event flows through the live
    // pipeline under test: listener -> lock-free queue -> sibling thread -> channel.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long SiblingPublishProbe(int n)
    {
        long s = 0;
        for (int i = 0; i < n; i++) s += i;
        return s;
    }

    // §E3 mechanism (ptrace-free): JitMethodMap.SetPublishChannel must publish a freshly
    // JIT'd method's (base,len) into an address channel FROM THE SIBLING THREAD — validated
    // against a process-local ring, no window/stepper needed, so it asserts (never skips) on
    // plain unprivileged hosts where the OOP integration case below may self-skip. The stop
    // semantics (join-before-free) are what make RunWindowOutOfProcess's teardown UAF-free.
    static void SiblingPublisherChecks()
    {
        var map = new JitMethodMap();
        IntPtr chan = HwNative.asmtest_addr_channel_new();
        Check(chan != IntPtr.Zero, "E3: process-local addr channel allocates");
        if (chan == IntPtr.Zero) { map.Dispose(); return; }

        map.SetPublishChannel(chan); // starts the sibling publisher thread
        // First-time JIT of the probe: MethodLoadVerbose -> enqueue -> sibling publish.
        RuntimeHelpers.PrepareMethod(typeof(HwTraceProgram)
            .GetMethod(nameof(SiblingPublishProbe), BindingFlags.NonPublic | BindingFlags.Static)
            .MethodHandle);
        // EventPipe dispatch is asynchronous — poll generously (CI can be loaded); the
        // publish normally lands within milliseconds of the Prepare above.
        long deadline = Environment.TickCount64 + 10000;
        while (map.LivePublished == 0 && Environment.TickCount64 < deadline)
            Thread.Sleep(10);
        long published = map.LivePublished;
        Check(published >= 1, $"E3: sibling thread published the fresh JIT to the channel (got {published})");
        // The ring's producer cursor (`head`, the first u32 of asmtest_addr_channel_t — a
        // layout pinned as cross-process ABI by asmtest_addr_channel.h) must show the
        // records really landed NATIVELY, not just that the managed counter moved.
        int head = Marshal.ReadInt32(chan, 0);
        Check(head >= 1 && head >= (int)Math.Min(published, int.MaxValue),
              $"E3: channel head advanced natively (head={head}, published={published})");

        map.SetPublishChannel(IntPtr.Zero); // stop + JOIN: the ring is now safe to free
        long after = map.LivePublished;
        Thread.Sleep(50); // any straggler would show up here — the join must prevent it
        Check(map.LivePublished == after, "E3: no publishes after SetPublishChannel(0) joined the sibling");
        map.SetPublishChannel(IntPtr.Zero); // idempotent stop is a no-op
        map.Dispose();                      // Dispose after stop is also a no-op stop
        HwNative.asmtest_addr_channel_free(chan);
    }

    // §E3 fixture — JIT'd for the FIRST time INSIDE the out-of-process window (nothing else
    // may call it), so its code lands OUTSIDE the pre-window coarse ranges: without the live
    // sibling publish the stepper elides it (the pre-E3 honest-partial gap); with E3 it must
    // be published mid-window and resolve by name. The loop is long enough in STEPPED time
    // (~6 insns x 30000 iterations, each a ptrace round-trip) that the EventPipe -> sibling
    // -> channel publish lands while the method is still executing — the plan's
    // publish-before-execute ordering risk, budgeted for rather than assumed away.
    // MidWindowJitLoop(30000) == 7500 * (1+2+3+4) == 75000.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long MidWindowJitLoop(int n)
    {
        long acc = 0;
        for (int i = 0; i < n; i++) acc += (i & 3) + 1;
        return acc;
    }

    // §E3 integration — the deep mid-window JIT attribution gap, closed: a method JIT'd
    // fresh mid-window resolves in the OOP whole-window trace. Self-skips (reason asserted)
    // where the reverse attach is denied — the mechanism itself is still covered above.
    static void WindowLiveJitChecks()
    {
        long r = 0;
        AsmTrace ww = AsmTrace.Window(() => { r = MidWindowJitLoop(30000); });
        Check(r == 75000, $"E3 Window: mid-window-JIT'd loop returns 75000 (got {r})");
        if (!ww.Armed)
        {
            Check(ww.SkipReason.Length > 0,
                  $"E3 Window: unarmed scope records a self-skip reason ({ww.SkipReason})");
            Console.WriteLine($"# SKIP E3 OOP-window integration: {ww.SkipReason}");
            return;
        }
        // The sibling published at least the fresh method while the window was open.
        Check(ww.LiveJitPublished >= 1,
              $"E3 Window: sibling live-published >= 1 mid-window JIT record (got {ww.LiveJitPublished})");
        // ... and the stepper recorded it + close-time attribution named it. The 64Ki
        // instruction budget can honestly truncate around a late-published loop (the AMD-LBR
        // "covered OR truncated" lesson, generalized) — but a full miss on an untruncated
        // capture is a real E3 regression and must fail.
        long inLoop = ww.InstructionsIn("MidWindowJitLoop");
        Check(inLoop > 0 || ww.Truncated,
              $"E3 Window: mid-window JIT resolves in the trace, or capture honestly truncated "
              + $"(got {inLoop} insns, truncated={ww.Truncated}, methods={ww.Methods.Count})");
        if (inLoop > 0)
            Console.WriteLine($"# E3: MidWindowJitLoop captured with {inLoop} attributed instructions "
                              + $"({ww.LiveJitPublished} live-published records)");
        else
            Console.WriteLine("# NOTE E3: stream truncated before the live-published loop was recorded");
    }

    // A pure-compute COLD method for the byMethod annotated-disassembly test — arithmetic
    // only (no I/O: single-stepping a managed Console call is the documented footgun).
    // TinyManaged(1000) == 1000*1001/2 == 500500.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long TinyManaged(long n)
    {
        long s = 0;
        for (long i = 1; i <= n; i++) s += i;
        return s;
    }

    // §1 fixture: ONE construction site shared by every caller, so concurrent callers
    // collide on the same [CallerMemberName]:[CallerLineNumber] auto-name — exactly
    // the aliasing case the handle-keyed begin_scope exists to disambiguate.
    static AsmTrace SameSiteScope(NativeCode c) => new AsmTrace(c, emit: false);

    // §D0.3 fixtures: FRESH cold methods (first call happens inside the scope test) so
    // PrepareMethod's forced JIT is what the listener observes. Kept trivially small.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long NamedCold(long a, long b) => a + b;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static long NamedColdOop(long a, long b) => a * b;

    // (double…)->double: the FP shim family — the integer family can't express it.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static double NamedFpCold(double a, double b) => a + b;

    // §D0.4 stitched-hop fixture: a fresh managed method resolved + lazy-arm captured
    // per hop; kept NoInlining so the standalone JIT'd body is what runs and is traced.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long StitchWork(long a, long b) => a + b;

    // §D0.4 — one logical operation across a REAL thread hop (Task.Run resumes on a
    // pool thread). The AsyncLocal scope id must flow across the hop, and the two per-hop
    // lazy-arm captures must stitch in seq order into one merged trace.
    static async System.Threading.Tasks.Task StitchChecks()
    {
        using var op = new AsmStitchedTrace();
        Check(AsmStitchedTrace.CurrentScopeId == op.ScopeId,
              "stitched: AsyncLocal carries the operation scope id");

        long r0 = (long)op.Step((Func<long, long, long>)StitchWork, 20, 22); // hop 0, this thread
        long scopeInHop = -1;
        long r1 = (long)await System.Threading.Tasks.Task.Run(() =>
        {
            scopeInHop = AsmStitchedTrace.CurrentScopeId; // flowed across the thread hop
            return op.Step((Func<long, long, long>)StitchWork, 60, 60);       // hop 1, pool thread
        });
        op.Complete();

        Check(r0 == 42 && r1 == 120, $"stitched: both hops ran (r0={r0}, r1={r1})");
        Check(scopeInHop == op.ScopeId,
              $"stitched: scope id flowed across the await/thread hop ({scopeInHop} vs {op.ScopeId})");
        Check(op.Hops.Count == 2, $"stitched: two hops stitched ({op.Hops.Count}; skip='{op.SkipReason}')");
        if (op.Hops.Count == 2)
        {
            Check(op.Hops[0].Seq == 0 && op.Hops[1].Seq == 1, "stitched: hops merged in seq order");
            Check(op.Hops[0].InsnOffset == 0 && op.Hops[1].InsnOffset > 0,
                  $"stitched: hop 1 concatenates AFTER hop 0 (offset {op.Hops[1].InsnOffset})");
            if (op.Hops[0].Tid != op.Hops[1].Tid)
                Check(true, "stitched: hops captured on DIFFERENT threads (real async hop)");
            else
                Console.WriteLine("# note stitched: both hops on the same thread (pool reused caller); stitch still correct");
            if (HwNative.asmtest_disas_available())
                Check(op.Path.Contains("hop 0") && op.Path.Contains("hop 1") && op.Path.Contains("ret"),
                      "stitched: merged Path renders both hops (per-hop disassembly)");
        }

        // Registry-exhaustion guard: >MAX_REGIONS (32) captured hops process-wide must NOT
        // silently disable capture — AsmStitchedTrace uses the registry-free call path, so
        // 40 operations all capture (the old register_region-per-hop path EFULL'd after 32).
        int captured = 0;
        for (int i = 0; i < 40; i++)
        {
            using var o = new AsmStitchedTrace();
            o.Step((Func<long, long, long>)StitchWork, i, 1);
            o.Complete();
            if (o.Hops.Count == 1) captured++;
        }
        Check(captured == 40,
              $"stitched: 40 operations (>32 MAX_REGIONS) all captured ({captured}/40) — registry-free, no exhaustion");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static long NamedStress(long a, long b) => a + b;

    // Slow-host crash-avoidance stress (managed-singlestep-lazy-arm-plan "Sharpening 1").
    // The original defect: a TF-armed thread that runs code which blocks SIGTRAP is
    // force-killed (exit 133) — CoreCLR's tiering worker idle-exits (~4 s default) and is
    // respawned via pthread_create ON WHICHEVER THREAD next enqueues tier-up work. This
    // stress recreates that environment with the worker UNPINNED (the suites' usual
    // DOTNET_TC_BackgroundWorkerTimeoutMs pin must NOT be set): sleep past the worker's
    // idle-exit, churn tier-up enqueues on THIS thread (fresh DynamicMethods driven past
    // the call-count threshold), and interleave lazy-arm Invoke()s. Under the old
    // stepped-DynamicInvoke Invoke this died deterministically on loaded CI runners;
    // under lazy-arm the armed window holds only the body, so the process must survive
    // with every capture intact. Surviving to the summary line IS the pass signal.
    static void MethodStress(int n)
    {
        string pin = Environment.GetEnvironmentVariable("DOTNET_TC_BackgroundWorkerTimeoutMs");
        Check(string.IsNullOrEmpty(pin),
              "stress precondition: tiering worker is UNPINNED (no DOTNET_TC_BackgroundWorkerTimeoutMs)");

        using var t = AsmTrace.Method((Func<long, long, long>)NamedStress, emit: false);
        Check(t.SkipReason.Length == 0,
              $"stress: named-method scope resolved ({t.SkipReason})");

        long bad = 0, invoked = 0;
        for (int i = 0; i < n; i++)
        {
            // Twice per run, park past the worker's idle-exit so the NEXT tier-up
            // enqueue must respawn it (the pthread_create the old code stepped over).
            if (i == 0 || i == n / 2)
                Thread.Sleep(5500);

            // Fresh tier-0 JIT + enough calls to trip the tier-up counter: enqueues
            // background recompilation from THIS thread, adjacent to the armed windows.
            var dm = new System.Reflection.Emit.DynamicMethod(
                $"stress{i}", typeof(long), new[] { typeof(long), typeof(long) });
            var il = dm.GetILGenerator();
            il.Emit(System.Reflection.Emit.OpCodes.Ldarg_0);
            il.Emit(System.Reflection.Emit.OpCodes.Ldarg_1);
            il.Emit(System.Reflection.Emit.OpCodes.Add);
            il.Emit(System.Reflection.Emit.OpCodes.Ret);
            var churn = (Func<long, long, long>)dm.CreateDelegate(typeof(Func<long, long, long>));
            long acc = 0;
            for (int c = 0; c < 100; c++) acc += churn(c, 1);

            long r = (long)t.Invoke((long)i, acc % 7);
            invoked++;
            if (r != i + acc % 7) bad++;
        }
        Check(bad == 0, $"stress: all {invoked} lazy-arm Invokes returned correct values ({bad} bad)");
        Check(t.Armed, $"stress: lazy-arm armed and captured in-process ({t.SkipReason})");
        Console.WriteLine("# stress: survived the unpinned tiering-worker respawn windows — "
                        + "the armed TF window never contained runtime machinery");
    }

    // Tier-up fixture: hot enough that default tiered compilation re-JITs it.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HotTier(long x)
    {
        long s = 0;
        for (int i = 0; i < 64; i++) s += x ^ i;
        return s;
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

    // asmtest_trace_call_auto: the CALL-OWNING auto-escalating cross-tier trace. It owns
    // the invocation and self-manages the tier lifecycle, so this runs with NO Init/arm
    // around it. Mirrors bindings/python's test_trace_call_auto_owns_the_call_and_completes
    // AND the C reference test_call_auto: accept OK or EUNAVAIL (self-skip off a host with
    // no call-owning native tier); assert the completed trace's shape only when OK.
    static void TraceCallAutoChecks()
    {
        // fixture 1: ROUTINE — the fast tier captures the whole path (2 blocks, complete).
        var code = NativeCode.FromBytes(ROUTINE);
        var res = HwTrace.TraceCallAuto(code, new long[] { 20, 22 }); // 42 <= 100 -> jle taken
        Check(res.Rc == HwNative.ASMTEST_HW_OK || res.Rc == HwNative.ASMTEST_HW_EUNAVAIL,
              $"call_auto: rc is OK or EUNAVAIL (got {res.Rc})");
        if (res.Rc == HwNative.ASMTEST_HW_OK)
        {
            Check(res.Result == 42, $"call_auto: add2(20,22) == 42 (got {res.Result})");
            Check(!res.Truncated, "call_auto: some tier captured the whole path (not truncated)");
            Check(res.Trace.Covered(0), "call_auto: entry block 0 covered");
            Check(res.Used.HasValue && res.Used.Value.Tier == TraceTier.HwTrace,
                  $"call_auto: used tier is HwTrace (got {res.Used})");
            res.Trace.Free();
        }
        code.Free();

        // fixture 2: LOOP — 25 back-edges exceed the 16-deep LBR window, so on a
        // ceiling-bounded fast backend the trace escalates to a ceiling-free tier; the
        // single-step floor completes it directly elsewhere. Either way: complete.
        var loop = NativeCode.FromBytes(LOOP);
        var lres = HwTrace.TraceCallAuto(loop, new long[] { 1, 25 }); // 25 back-edges > 16
        Check(lres.Rc == HwNative.ASMTEST_HW_OK || lres.Rc == HwNative.ASMTEST_HW_EUNAVAIL,
              $"call_auto: loop rc is OK or EUNAVAIL (got {lres.Rc})");
        if (lres.Rc == HwNative.ASMTEST_HW_OK)
        {
            Check(lres.Result == 25, $"call_auto: loop(1,25) == 25 (got {lres.Result})");
            Check(!lres.Truncated, "call_auto: escalated to a ceiling-free tier (not truncated)");
            Check(lres.Trace.Covered(0x7), "call_auto: loop-body block 0x7 covered");
            lres.Trace.Free();
        }
        loop.Free();
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

        // (1a) block-step (PTRACE_SINGLEBLOCK): same trace, ~4-10x fewer tracer stops.
        if (Ptrace.BlockstepAvailable())
        {
            var code = NativeCode.FromBytes(ROUTINE);
            var tr = HwTrace.Create(blocks: 64, instructions: 64);
            long result = Ptrace.TraceCallBlockstep(code.Base, (nuint)code.Length,
                                                    new long[] { 20, 22 }, tr.Handle);
            Check(result == 42, $"ptrace: block-step trace_call(20,22) == 42 (got {result})");
            var insns = tr.InsnOffsets();
            var wantInsns = new ulong[] { 0x0, 0x3, 0x6, 0xC, 0x11 };
            Check(Eq(insns, wantInsns),
                  $"ptrace: block-step reconstructs the exact single-step offsets {Hex(insns)}");
            Check(!tr.Truncated(), "ptrace: block-step trace not truncated");
            tr.Free();
            code.Free();
        }
        else
            Console.WriteLine("# SKIP ptrace block-step: PTRACE_SINGLEBLOCK unavailable here");

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
