// examples/dotnet/single-method — trace EXACTLY one managed method body, safely.
//
//     using var m = AsmTrace.Method((Func<long,long,long>)Work, emit: false);
//     long r = (long)m.Invoke(21, 21);   // arm/step/disarm happen INSIDE the native call
//     // m.Path — that body's executed instructions; m.Methods — the one method named
//
// This is the managed counterpart to `region` (which scopes one NATIVE routine). Every OTHER
// managed example here is a WHOLE-WINDOW capture — it records the method AND the ~1M
// instructions of runtime (JIT, GC, BCL) that ran alongside it, then attributes them apart.
// AsmTrace.Method is the opposite: it lazy-arms EFLAGS.TF *inside* the native call
// (call_scoped), so only the one JIT'd body is single-stepped — zero runtime amplification.
//
// It is also the SAFE single-step posture: a whole-window `new AsmTrace()` on a runtime thread
// is fatal if the runtime spawns a thread in-window (glibc pthread_create blocks SIGTRAP → the
// kernel force-kills the process). Method() only ever steps the body, never the caller's setup,
// so that failure mode cannot arise. Pass outOfProcess:true to step it out of band instead
// (crash-proof for ANY body). Runs live via the single-step tier; self-skips (exit 0) cleanly.
//
// Constraint: the in-process fast-path covers (long…)->long / (double…)->double signatures with
// arity <= 6 (the reverse-P/Invoke shim set); any other signature auto-routes out-of-process.

using System;
using Asmtest;

internal static class Program
{
    // A small (long,long)->long body with a loop + a branch, so the stepped stream is
    // interesting: sum a-of-b's-triangular plus a data-dependent branch. NoInlining so it
    // JITs as its own standalone body the resolver can find.
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Work(long a, long b)
    {
        long acc = 0;
        for (long i = 0; i < b; i++)
            acc += (i & 1) == 0 ? a : -1;   // a data-dependent branch inside the loop
        return acc;
    }

    static int Main()
    {
        Console.WriteLine("== single managed method body: AsmTrace.Method(del).Invoke ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }

        // In-process lazy-arm: only Work's body is stepped, none of the runtime around it.
        AsmTrace m;
        long r;
        using (m = AsmTrace.Method((Func<long, long, long>)Work, emit: false))
        {
            r = (long)m.Invoke(21L, 8L);
        }
        Report.PrintInProcess(m, r);

        // The crash-proof variant: same one-method scope, stepped OUT OF PROCESS by the
        // reverse-attach helper — no EFLAGS.TF on this thread at all. Safe for ANY body.
        AsmTrace o;
        long ro;
        using (o = AsmTrace.Method((Func<long, long, long>)Work, emit: false, outOfProcess: true))
        {
            ro = (long)o.Invoke(21L, 8L);
        }
        Report.PrintOutOfProcess(o, ro);

        return 0;
    }
}
