using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

// F4 GC-fence FREEZE probe VICTIM: a PLAIN long-running dotnet process, started with NO CORECLR_*
// environment variables at all — the ptrace live-attach tier attaches to processes it did not
// launch, so it cannot pre-set CORECLR_ENABLE_PROFILING (see f4-attach-profiler-probe-findings.md).
//
// Modelled on examples/attachprof_probe/victim, with the one addition the freeze MEASUREMENT needs:
// a MANAGED HOT LOOP on a known, identifiable worker thread. The worker publishes its OS thread id
// (gettid) so the stepper can PTRACE_ATTACH to exactly that thread, and it both allocates and
// touches long-lived survivors, so it is a thread that (a) genuinely retires instructions, (b) hits
// GC-safe points, and (c) has objects the compaction actually relocates.
//
// The main thread meanwhile forces COMPACTING gen2 GCs that RELOCATE survivors, so an attached
// profiler sees MovedReferences2 ranges with old != new. Nothing here is tuned to make the traced
// thread park or spin: the victim is deliberately an ordinary allocating workload, and the point of
// the probe is to observe which of the two CoreCLR does.
class GcFenceVictim {
    [DllImport("libc", SetLastError = true)]
    static extern int gettid();

    static readonly object[] mainSurvivors = new object[40000];
    static readonly object[] workerSurvivors = new object[20000];
    static volatile bool stop;
    static long workerIters;
    static bool workerAllocates = true;

    static void Main() {
        int seconds = Env("GCFENCE_VICTIM_SECONDS", 40);
        int sleepMs = Env("GCFENCE_VICTIM_SLEEP_MS", 200);
        // GCFENCE_WORKER_ALLOC=0 makes the traced worker a PURE COMPUTE loop (no allocation), which
        // drops the process's GC rate by ~100x AND removes the allocation helper — the GC poll a
        // hot managed loop normally reaches. It exists to check whether the measured behaviour is a
        // property of this victim's (very high) GC rate or of single-stepping managed code as such.
        workerAllocates = Environment.GetEnvironmentVariable("GCFENCE_WORKER_ALLOC") != "0";
        Console.WriteLine("GCFENCE_VICTIM_START pid=" + Environment.ProcessId + " seconds=" + seconds +
                          " worker_allocates=" + workerAllocates);
        Console.Out.Flush();

        var t = new Thread(Worker, 1 << 20) { Name = "gcfence_hot", IsBackground = false };
        t.Start();

        var sw = Stopwatch.StartNew();
        int round = 0;
        while (sw.Elapsed.TotalSeconds < seconds) {
            var garbage = new List<byte[]>(4096);
            for (int i = 0; i < 20000; i++) {
                var b = new byte[64];
                if ((i & 3) == 0) mainSurvivors[i % mainSurvivors.Length] = b; // fragmented survivors
                else garbage.Add(b);
            }
            garbage.Clear();
            // The fence under measurement: a blocking, COMPACTING gen2 GC, which relocates the
            // fragmented survivors of both threads.
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
            Console.WriteLine("GCFENCE_VICTIM_HEARTBEAT round=" + (round++) + " gen2=" + GC.CollectionCount(2) +
                              " worker_iters=" + Interlocked.Read(ref workerIters) +
                              " t=" + sw.Elapsed.TotalSeconds.ToString("F1"));
            Console.Out.Flush();
            Thread.Sleep(sleepMs);
        }
        stop = true;
        t.Join(10000);
        Console.WriteLine("GCFENCE_VICTIM_END gen2=" + GC.CollectionCount(2) +
                          " worker_iters=" + Interlocked.Read(ref workerIters));
        Console.Out.Flush();
    }

    // The traced thread. Allocates, keeps survivors, and does arithmetic on them — an ordinary
    // managed hot loop, i.e. the thing the live-attach tier would be single-stepping for real.
    static void Worker() {
        int tid = gettid();
        Console.WriteLine("GCFENCE_VICTIM_WORKER tid=" + tid + " managed_id=" + Environment.CurrentManagedThreadId);
        Console.Out.Flush();
        long acc = 0;
        var local = new List<byte[]>(512);
        for (int i = 0; i < workerSurvivors.Length; i++) workerSurvivors[i] = new byte[64];
        while (!stop) {
            for (int i = 0; i < 256; i++) {
                if (workerAllocates) {
                    var b = new byte[64];
                    b[0] = (byte)(i + acc);
                    acc += b[0];
                    if ((i & 15) == 0) workerSurvivors[(int)(workerIters + i) % workerSurvivors.Length] = b;
                    else local.Add(b);
                } else {
                    // Pure compute over already-allocated survivors: no allocation, so no allocation
                    // helper and no GC of the worker's own making.
                    var s2 = (byte[])workerSurvivors[(int)(workerIters + i) % workerSurvivors.Length];
                    acc += s2[0] + i;
                    s2[1] = (byte)acc;
                }
            }
            local.Clear();
            // Touch a survivor so the worker really depends on objects the compaction relocates.
            var s = workerSurvivors[(int)(workerIters % workerSurvivors.Length)] as byte[];
            if (s != null) acc += s[0];
            Interlocked.Increment(ref workerIters);
        }
        Console.WriteLine("GCFENCE_VICTIM_WORKER_END iters=" + workerIters + " acc=" + acc);
        Console.Out.Flush();
    }

    static int Env(string name, int dflt) {
        var s = Environment.GetEnvironmentVariable(name);
        return int.TryParse(s, out int v) && v > 0 ? v : dflt;
    }
}
