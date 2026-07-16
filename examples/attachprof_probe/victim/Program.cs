using System;
using System.Collections.Generic;
// F4 attach-mode probe VICTIM: a PLAIN long-running dotnet process — started with NO CORECLR_*
// environment variables at all, which is the whole point (the ptrace live-attach tier attaches to
// processes it did not launch, so it cannot pre-set CORECLR_ENABLE_PROFILING).
//
// Modelled on examples/gcprofiler_probe/gcmover: each round allocates a fragmented mix of garbage
// and survivors, then forces a COMPACTING gen2 GC, which RELOCATES the survivors — so a profiler
// that is listening sees MovedReferences2 ranges. Unlike gcmover it runs many slow rounds and
// self-reports its pid, so a harness can attach a profiler MID-RUN and still have plenty of
// relocating GCs left to observe.
class AttachProfVictim {
    static object[] survivors = new object[40000];
    static void Main() {
        int rounds = Env("ATTACHPROF_VICTIM_ROUNDS", 30);
        int sleepMs = Env("ATTACHPROF_VICTIM_SLEEP_MS", 300);
        Console.WriteLine("ATTACHPROF_VICTIM_START pid=" + Environment.ProcessId);
        Console.Out.Flush();
        for (int round = 0; round < rounds; round++) {
            var garbage = new List<byte[]>();
            for (int i = 0; i < 60000; i++) {
                var b = new byte[64];
                if ((i & 3) == 0) survivors[i % survivors.Length] = b; // fragmented survivors
                else garbage.Add(b);
            }
            garbage.Clear();
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
            Console.WriteLine("ATTACHPROF_VICTIM_HEARTBEAT round=" + round + " gen2=" + GC.CollectionCount(2));
            Console.Out.Flush();
            System.Threading.Thread.Sleep(sleepMs);
        }
        Console.WriteLine("ATTACHPROF_VICTIM_END gen2=" + GC.CollectionCount(2));
        Console.Out.Flush();
    }
    static int Env(string name, int dflt) {
        var s = Environment.GetEnvironmentVariable(name);
        return int.TryParse(s, out int v) && v > 0 ? v : dflt;
    }
}
