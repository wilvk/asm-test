// Program.cs — a long-running MANAGED (.NET) victim for the DR ATTACH tier's Increment-6
// managed-attach go/no-go PROBE (dynamorio-attach-tier-plan.md). The managed analog of
// examples/attach_probe_victim.c: started as a PLAIN `dotnet`/apphost process (NOT under drrun),
// it prints its pid then loops a HOT method (which .NET's tiered JIT recompiles tier-0 -> tier-1
// mid-run) with periodic heartbeats, bounded by wall-clock so it always terminates. The probe
// lane injects DR + the minimal counting client via `drrun -attach <pid>` mid-run, then detaches,
// and checks the managed process SURVIVES takeover + detach — no swallowed .NET SIGSEGV/SIGTRAP,
// no crash/hang — the plan's Increment-6 KILL-CRITERION question. This is a throwaway diagnostic
// (managed attach is research-gated; the managed default stays launch-under-DR / ptrace).
//
// The inner work loop is kept modest so heartbeats stay frequent enough to observe survival even
// while the per-instruction counting client makes the process much slower under attach; the total
// call volume across the run is still large enough to tier the hot method up.
using System;
using System.Diagnostics;
using System.Threading;

class Program
{
    // A small hot method: called enough times across the run to tier up (tier-0 -> tier-1) while
    // running, so DR's code cache must cope with .NET rewriting live managed code under attach.
    static long Work(int x) => ((long)x * x + x) ^ (x >> 1);

    static void Main()
    {
        int pid = Environment.ProcessId;
        Console.Error.WriteLine("MANAGED_VICTIM_START pid=" + pid);
        Console.Error.Flush();

        var sw = Stopwatch.StartNew();
        long acc = 0;
        int beat = 0;
        // ~30 s or 600 beats, whichever first — bounded so it always exits, but long enough that the
        // probe can attach mid-run, detach, and still observe native heartbeats AFTER the detach
        // (per-instruction instrumentation makes the process very slow while attached, and the
        // wall-clock keeps ticking, so a short bound could otherwise exit during the attach window).
        while (sw.Elapsed.TotalSeconds < 30.0 && beat < 600)
        {
            for (int i = 0; i < 200_000; i++) // real managed work DR instruments
                acc += Work(i & 4095);
            Console.Error.WriteLine("MANAGED_VICTIM_HEARTBEAT beat=" + beat + " acc=" + acc);
            Console.Error.Flush();
            Thread.Sleep(50);
            beat++;
        }

        Console.Error.WriteLine("MANAGED_VICTIM_END beats=" + beat + " acc=" + acc);
        Console.Error.Flush();
    }
}
