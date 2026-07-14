// Program.cs — the managed workload for the taint tier's method-range auto-registration
// slice (Increment 6). Under
//   DOTNET_PerfMapEnabled=1 drrun -c <taint client>.so methodscan=Hot -- dotnet taint_methods.dll
// the .NET runtime writes /tmp/perf-<pid>.map as it JITs, and the client's perfmap poller
// auto-registers every JIT'd method whose symbol contains "Hot" — i.e. HotAlpha + HotBeta —
// as instrumented ranges, WITHOUT any C region marker. So range-count > 1 arises purely from
// .NET method-load, and the client instruments real JIT'd managed code.
//
// The driver loop steps slowly (Thread.Sleep) for a bounded wall-time so the 10 ms poller
// catches HotAlpha/HotBeta after they JIT and re-flushes them into instrumentation, and they
// then execute instrumented a BOUNDED number of times (not millions — keeps the run quick).
// Main is deliberately NOT matched by "Hot", so its loop stays un-instrumented and fast.
using System;
using System.Runtime.CompilerServices;
using System.Threading;

static class TaintMethods
{
    // Two distinct hot methods, each JITed to its OWN address -> two perfmap entries.
    // MethodImplOptions.NoInlining is ESSENTIAL: without it the JIT inlines these tiny
    // expression-bodied methods straight into Main, so they never JIT as separate methods,
    // never get a perfmap entry, and `methodscan=Hot` would register nothing (regions=0).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HotAlpha(int x) => ((long)x * x + 7) ^ (x >> 1);
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long HotBeta(int x) => ((long)x + 3) * (x | 1) - (long)(x >> 2);

    static void Main()
    {
        long acc = 0;
        for (int i = 0; i < 200; i++)
        {
            acc += HotAlpha(i & 4095);
            acc += HotBeta(i & 4095);
            Thread.Sleep(10); // ~2 s total: time for the poller to register + flush
        }
        Console.WriteLine("HELLO_TAINT_METHODS acc=" + acc);
    }
}
