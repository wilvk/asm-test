// examples/dotnet/perf-triage-drill — the real profiler workflow, in two phases.
//
//   warm-up:          for (int i=0;i<N;i++) { Cheap; Light; Medium; Hot; }   // OUTSIDE any scope
//   PHASE 1 (survey): using (var w = new AsmTrace(emit:false, byMethod:true, withRundown:true))
//                         { Cheap(a,b); Light(a,b); Medium(a,b); Hot(a,b); }   // one whole window
//                     // w.Methods is the by-method histogram — find the hottest USER method.
//   PHASE 2 (drill):  using (var m = AsmTrace.Method((Func<long,long,long>)Hot, emit:false))
//                         r = (long)m.Invoke(a,b);        // step EXACTLY that one body
//                     // m.Path is that body's executed instructions — zero runtime amplification.
//
// This is the MOTIVATED framing of examples/dotnet/single-method. single-method shows the drill
// in isolation — but a profiler never knows the hot method up front; it FINDS it. Here Phase 1
// captures one whole window and attributes it by managed method (§D0.1/§D0.2), so the survey
// itself names the hottest body; Phase 2 then lazy-arms EFLAGS.TF INSIDE the native call to step
// only that body — the survey (a whole-window single-step over the JIT/GC/BCL too) points the
// drill (one isolated JIT'd body) at exactly the right target.
//
// Why warm up first: the survey measures STEADY-STATE cost, so the workloads are JIT'd BEFORE the
// window. A profiler cares about a method's running cost, not its one-time compile cost — and a
// cold body single-stepped in-window drags the whole RyuJIT compile through the ring too, which
// both drowns the real hot body and can truncate the window before the last-called method even
// runs. Warm bodies keep the window to the executions themselves; `withRundown` (§D0.2) names
// them retroactively (the cold-only §D0.1 listener sees only in-window JITs).
//
// Crash-safety: a whole-window in-process capture single-steps the calling thread, so the Phase 1
// block MUST be straight-line managed arithmetic — no thread spawn, no in-window throw (stepping a
// glibc pthread_create or the EH unwinder in-window is fatal: the kernel force-kills on a blocked
// SIGTRAP). The four workloads below are pure loops. Self-skips (exit 0) where single-step is off.

using System;
using Asmtest;

internal static class Program
{
    // Four workloads with deliberately different costs, so the by-method survey has a clear
    // winner. NoInlining makes each a standalone JIT'd body the rundown can name and the drill
    // can resolve. Cost is set by the loop trip count: Cheap << Light << Medium << Hot.
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Cheap(long a, long b) => a + b + (a ^ b) + 3;   // a few adds, no loop

    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Light(long a, long b)                            // short loop
    {
        long acc = 0;
        for (long i = 0; i < 16; i++) acc += a + i;
        return acc + b;
    }

    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Medium(long a, long b)                           // small loop
    {
        long acc = 0;
        for (long i = 0; i < 64; i++) acc += (a * i) ^ b;
        return acc;
    }

    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Hot(long a, long b)                              // big loop, data-dependent branch
    {
        long acc = 0;
        for (long i = 0; i < 384; i++)
            acc += (i & 1) == 0 ? a + b : a - b;
        return acc;
    }

    // The candidate set the survey chooses from — a tag we can spot in a method name, paired
    // with the delegate the drill re-invokes. Delegates to the NoInlining bodies above.
    static readonly (string Tag, Func<long, long, long> Fn)[] Workloads =
    {
        ("Cheap",  Cheap),
        ("Light",  Light),
        ("Medium", Medium),
        ("Hot",    Hot),
    };

    static int Main()
    {
        Console.WriteLine("== perf triage: survey a whole window by method, then drill the hottest ==\n");

        // The whole-window survey and the drill both ride the single-step tier; gate once.
        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }

        const long A = 6, B = 7;

        // Warm the workloads OUTSIDE any scope so the survey measures steady-state execution
        // cost, not one-time JIT cost (see the "Why warm up first" note up top). At full native
        // speed here — nothing is armed yet.
        long warm = 0;
        for (int i = 0; i < 200; i++)
            warm += Cheap(A, B) + Light(A, B) + Medium(A, B) + Hot(A, B);
        Console.WriteLine($"warmed the 4 workloads (checksum {warm}); now surveying steady state.\n");

        // ---- PHASE 1: whole-window by-method SURVEY -------------------------------------
        // Straight-line managed arithmetic only — see the crash-safety note up top.
        long sink;
        AsmTrace w;
        using (w = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            sink = Cheap(A, B) + Light(A, B) + Medium(A, B) + Hot(A, B);

        if (!w.Armed)
        {
            Console.WriteLine($"# self-skip: {w.SkipReason}");
            return 0;
        }

        // Pick the hottest method whose name is one of ours (ignore the runtime/BCL remainder).
        (string drillTag, Func<long, long, long> drill, AsmMethod hottest, bool found) = PickHottest(w);
        Report.Survey(w, sink, hottest, found);

        // ---- PHASE 2: DRILL into exactly the hottest body ------------------------------
        // AsmTrace.Method lazy-arms inside the native call, so only this one JIT'd body is
        // stepped — none of the ~1M runtime instructions the survey also had to record.
        AsmTrace m;
        long r;
        using (m = AsmTrace.Method((Func<long, long, long>)drill, emit: false))
            r = (long)m.Invoke(A, B);
        Report.Drill(m, drillTag, r);

        return 0;
    }

    // The survey's verdict: among methods whose name is one of ours, the one with the most
    // captured instructions. Falls back to Hot (the expected winner) if attribution named none.
    static (string, Func<long, long, long>, AsmMethod, bool) PickHottest(AsmTrace w)
    {
        long best = -1;
        string tag = "Hot";
        Func<long, long, long> fn = Hot;
        AsmMethod hottest = default;
        bool found = false;
        foreach (AsmMethod meth in w.Methods)
        {
            if (!meth.Name.Contains("Program")) continue;   // user type only, not runtime/BCL
            foreach ((string Tag, Func<long, long, long> Fn) wl in Workloads)
                if (meth.Name.Contains(wl.Tag) && meth.Count > best)
                {
                    best = meth.Count;
                    tag = wl.Tag;
                    fn = wl.Fn;
                    hottest = meth;
                    found = true;
                }
        }
        return (tag, fn, hottest, found);
    }
}
