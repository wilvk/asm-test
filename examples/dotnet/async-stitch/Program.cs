// examples/dotnet/async-stitch — follow ONE logical operation across an await / thread-pool hop
// and stitch each hop's capture into a single seq-ordered trace.
//
//     using var op = new AsmStitchedTrace();
//     long r0 = (long)op.Step((Func<long,long,long>)Work, 20, 8);                     // hop 0, this thread
//     long r1 = (long)await Task.Run(() => op.Step((Func<long,long,long>)Work, 6, 9)); // hop 1, POOL thread
//     long r2 = (long)op.Step((Func<long,long,long>)Work, 11, 7);                     // hop 2, after the await
//     op.Complete();  // op.Hops — each (Seq, Tid, InsnOffset); op.Path — merged per-hop listing
//
// Every OTHER managed example scopes ONE body on ONE thread. This is the only one that spans a
// thread boundary: an AsyncLocal scope id rides the `await` continuation, so a hop that resumes on
// a different thread-pool thread is still recognised as the SAME operation, and the shipped
// asmtest_hwtrace_stitch merge core concatenates every hop by Seq into one ordered trace. This is
// the first LIVE producer of that core (previously exercised only from synthetic slices).
//
// Each hop reuses AsmTrace's managed-safe lazy-arm (call_scoped) — only the resolved (long…)->long
// body is single-stepped, none of the runtime around it — so the pool hop is captured without ever
// arming EFLAGS.TF over runtime machinery. Only integer (long…)->long hops are captured in-process;
// any other signature (or an unavailable single-step tier) runs that hop uninstrumented and sets
// op.SkipReason — the hop is recorded, never a crash. Self-skips (exit 0) when nothing was captured.

using System;
using Asmtest;

internal static class Program
{
    // A small (long,long)->long body: a bounded loop with a data-dependent branch, so each hop's
    // stepped stream is short but non-trivial. NoInlining so it JITs as its own resolvable body.
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long Work(long a, long b)
    {
        long acc = 0;
        for (long i = 0; i < b; i++)
            acc += (i & 1) == 0 ? a : -1;   // a data-dependent branch inside the loop
        return acc;
    }

    static async System.Threading.Tasks.Task<int> Main()
    {
        Console.WriteLine("== one operation across an await/thread-pool hop: AsmStitchedTrace ==\n");

        int callerTid = Environment.CurrentManagedThreadId;
        AsmStitchedTrace op;
        long r0, r1, r2;
        using (op = new AsmStitchedTrace())
        {
            // hop 0 — synchronous, on THIS thread.
            r0 = (long)op.Step((Func<long, long, long>)Work, 20L, 8L);

            // hop 1 — the operation HOPS onto a thread-pool thread. The AsyncLocal scope id rides
            // the await continuation, so this hop stitches into the SAME operation despite running
            // on a different Tid. Step resolves Work itself; the lambda is just the pool delivery.
            r1 = (long)await System.Threading.Tasks.Task.Run(
                () => op.Step((Func<long, long, long>)Work, 6L, 9L));

            // hop 2 — synchronous again. A console app has no SynchronizationContext, so the
            // continuation after the await may resume on a pool thread rather than callerTid; the
            // stitch is by Seq, not by thread, so wherever it lands it is still this operation.
            r2 = (long)op.Step((Func<long, long, long>)Work, 11L, 7L);

            op.Complete();
        }

        Report.Print(op, callerTid, r0, r1, r2);
        return 0;
    }
}
