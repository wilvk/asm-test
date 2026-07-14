// Program.cs — managed victim for the MANAGED-ATTACH SAFEPOINT plan Increment-1 suspend-primitive
// probe. A hot method (tiers up mid-run) driven in a bounded heartbeat loop; the co-loaded
// suspendprof profiler SuspendRuntime/ResumeRuntime-cycles the runtime while this runs. The lane
// asserts the heartbeats keep advancing (managed progress resumes after each suspend) and the
// process exits clean. Bounded by wall-clock so it always terminates.
using System;
using System.Diagnostics;
using System.Threading;

class Program
{
    static long Work(int x) => ((long)x * x + x) ^ (x >> 1);

    static void Main()
    {
        Console.Error.WriteLine("SUSPENDPROF_VICTIM start pid=" + Environment.ProcessId);
        Console.Error.Flush();

        // Wall-clock bound (seconds); the suspend-then-seize lane (Increment 2) sets it longer so the
        // victim outlives the suspend-hold + attach window (the Stopwatch keeps ticking while the
        // runtime is suspended). Default suits the Increment-1 cycle lane.
        double secs = 14.0;
        var envSecs = Environment.GetEnvironmentVariable("SUSPENDPROF_VICTIM_SECS");
        if (envSecs != null && double.TryParse(envSecs, out var v) && v > 0 && v < 300) secs = v;

        var sw = Stopwatch.StartNew();
        long acc = 0;
        int beat = 0;
        while (sw.Elapsed.TotalSeconds < secs && beat < 4000)
        {
            for (int i = 0; i < 200_000; i++)
                acc += Work(i & 4095);
            Console.Error.WriteLine("SUSPENDPROF_VICTIM heartbeat beat=" + beat + " acc=" + acc);
            Console.Error.Flush();
            Thread.Sleep(50);
            beat++;
        }

        Console.Error.WriteLine("SUSPENDPROF_VICTIM done beats=" + beat + " acc=" + acc);
        Console.Error.Flush();
    }
}
