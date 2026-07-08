// examples/dotnet/concurrent-isolation — two managed threads single-step the SAME method
// body AT THE SAME INSTANT, and each keeps its OWN independent slice.
//
//     // on EACH of two threads, released together by a Barrier:
//     using (var m = AsmTrace.Method((Func<long,long,long>)Work, emit: false))
//         long r = (long)m.Invoke(a, b);   // <- two EFLAGS.TF windows, overlapping
//     // m.Path / m.Truncated are THIS thread's slice — never the other thread's
//
// Every OTHER managed example here is single-threaded; this is the affirmative proof that the
// single-step tier is per-thread. It rests on two facts the binding/native side maintain:
//   1. The single-step range stack is __thread — each thread arms its OWN EFLAGS.TF and
//      records into its OWN per-thread capture frame (keyed by frame handle + arming tid), so
//      two windows armed concurrently never share or corrupt state.
//   2. The region registry, by contrast, is process-global and NAME-keyed. AsmTrace.Method
//      auto-names a scope by its call site (member:line), so the two threads run their scope
//      through two thin sibling wrappers (ScopeOnThreadA / …B) — distinct names, hence a
//      registry slot + trace apiece. (One shared name would alias r->trace and merge the two
//      captures; the C harness's test_concurrent_singlestep names its two threads apart for
//      exactly this reason.)
// Because each thread creates, arms AND disposes its scope on its own thread, the reactive
// cross-thread Truncated guard never trips — Truncated stays false on BOTH. (That guard is
// what SETS Truncated when a scope armed on one thread is closed on another; this is the
// well-behaved case it exists to protect.) Dedicated System.Threading.Thread (not a pool
// thread) is deliberate: a pool thread can migrate OS threads mid-window, and EFLAGS.TF is
// per-OS-thread. Runs live via the single-step WEAK tier; self-skips (exit 0) where it can't.

using System;
using System.Threading;
using Asmtest;

internal static class Program
{
    // A small (long,long)->long body: a data-dependent branch inside a loop, so the stepped
    // stream is thousands of instructions wide — long enough that both threads are provably
    // inside their TF window at the same time. NoInlining so it JITs as a standalone body the
    // resolver can find (the same shape as examples/dotnet/single-method's Work).
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Work(long a, long b)
    {
        long acc = 0;
        for (long i = 0; i < b; i++)
            acc += (i & 1) == 0 ? a : -1;   // a data-dependent branch inside the loop
        return acc;
    }

    // One captured slice: which thread, its args, its result, and its (closed) scope object —
    // whose Path/Truncated/Armed managed properties survive Dispose for the report to read.
    internal sealed class Slice
    {
        public string Tag;
        public long A, B, Result;
        public AsmTrace Scope;
    }

    // The two sibling wrappers. Identical bodies ON PURPOSE: distinct methods give distinct
    // CallerMemberName auto-names, so each thread's scope lands in its own registry slot. Each
    // rendezvouses at the barrier INSIDE the scope (after the body has resolved) so the two
    // EFLAGS.TF windows open together, then single-steps Work concurrently with its sibling.
    static Slice ScopeOnThreadA(long a, long b, Barrier gate)
    {
        AsmTrace m;
        long r;
        using (m = AsmTrace.Method((Func<long, long, long>)Work, emit: false))
        {
            gate.SignalAndWait();          // release both threads' windows at the same instant
            r = (long)m.Invoke(a, b);      // single-stepped concurrently with thread B's Invoke
        }
        return new Slice { Tag = "thread-A", A = a, B = b, Result = r, Scope = m };
    }

    static Slice ScopeOnThreadB(long a, long b, Barrier gate)
    {
        AsmTrace m;
        long r;
        using (m = AsmTrace.Method((Func<long, long, long>)Work, emit: false))
        {
            gate.SignalAndWait();
            r = (long)m.Invoke(a, b);
        }
        return new Slice { Tag = "thread-B", A = a, B = b, Result = r, Scope = m };
    }

    static int Main()
    {
        Console.WriteLine("== concurrent single-step isolation: two AsmTrace.Method scopes at once ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        // Bring the portable tier up ONCE, single-threaded, before the two ctors race to
        // auto-init it (region does the same); the per-thread arming below is what overlaps.
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier — per-thread EFLAGS.TF, per-thread capture frame.\n");

        // Two threads, DIFFERENT args, released together so their windows overlap. add/sub over
        // an even loop count: Work(a,b) = (b/2)*(a-1), so the two results differ by construction.
        var gate = new Barrier(2);
        Slice sa = null, sb = null;
        var ta = new Thread(() => sa = ScopeOnThreadA(7, 800, gate)) { Name = "trace-A" };
        var tb = new Thread(() => sb = ScopeOnThreadB(11, 1200, gate)) { Name = "trace-B" };
        ta.Start();
        tb.Start();
        ta.Join();
        tb.Join();

        return Report.Print(sa, sb) ? 0 : 1;
    }
}
