using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

// F4 increment 1 VICTIM — the managed fixture whose memory def-use must survive a GC compaction
// captured on a LIVE ATTACH (docs/internal/plans/live-attach-dataflow-followup-plan.md F4).
//
// A PLAIN long-running dotnet process: no CORECLR_* environment variables at all, because the
// ptrace live-attach tier attaches to processes it did not launch and the profiler arrives over the
// diagnostics port (f4-attach-profiler-probe-findings.md). It IS started with the repo's usual JIT
// method-resolution knobs (DOTNET_PerfMapEnabled=1, DOTNET_TieredCompilation=0) — the same pair
// examples/dotnet/* and the taint lanes use — because the tier resolves a managed region through the
// perf map, and a stable non-tiered address means the region is not re-JIT'd mid-capture.
//
// THE SHAPE IS THE WHOLE POINT, and it is dictated by two measured facts:
//
//  1. The traced thread must NOT be the one triggering the GC. f4-gc-fence-freeze-probe-findings.md
//     measured that single-stepping an ALLOCATING thread makes its GC fence never complete: the
//     runtime cannot park it (a SIGRTMIN activation livelock) and every allocating thread in the
//     process stalls ~19.8 s until the tracer detaches. So the traced worker allocates NOTHING, and
//     a SEPARATE driver thread forces the compacting gen2 GC.
//
//  2. The GC must land inside the region's CALL-OUT. src/dataflow_ptrace.c is scoped to
//     [base, base+len): when the PC leaves the region it either steps OVER a call-out (int3 at the
//     call's fall-through + PTRACE_CONT, i.e. the target runs at NATIVE SPEED and forwards signals)
//     or ends the capture as a return. So a region whose middle is a `call` gives the GC a window in
//     which the traced thread is running natively — it parks in Thread.Sleep, in PREEMPTIVE GC mode,
//     which the runtime suspends with no hijack at all — and the capture resumes afterwards. A
//     region that merely span-loops would instead be single-stepped through the fence and hit the
//     measured stall.
//
// So Region() is: store to the heap object (OLD address) -> call out (the GC relocates it here) ->
// load it back (NEW address). That is exactly the def-use edge F4 exists to preserve.
//
// EXACTLY ONE compacting GC per call-out window is required, and is choreographed rather than hoped
// for: the step counter is FROZEN across the call-out (the region-gated counter records nothing over
// the helper), so every GC in one window is stamped with the SAME S0 — and asmtest_gcmove_canon
// applies at most ONE relocation per step-group, because a group models ONE GCBulkMovedObjectRanges
// batch whose old ranges are disjoint. Two GCs sharing a step would violate that model. Hence the
// driver prepares its fragmentation OUTSIDE the window and does a single blocking GC inside it.
static class GcCanonVictim
{
    [DllImport("libc", SetLastError = true)]
    static extern int gettid();

    // The value stored before the move and loaded back after it. Recognizable in the value trace:
    // an 8-byte MEM_ABS write whose value is this, then an 8-byte MEM_ABS read of the same value at
    // a DIFFERENT address. It is passed in as a parameter (never a JIT constant).
    public const long Sentinel = 0x5EEDCAFE12345678L;

    // The seeded, GC-movable object. A static root, so a gen2 collection never reclaims it — it must
    // SURVIVE and MOVE. long[] deliberately: a primitive element store needs no write barrier, so
    // the region stays a clean store/call/load with no helper call before the one we want.
    static long[] s_obj;
    static long[] s_warm;
    static object[] s_fill;        // fragmentation: survivors interleaved with dropped garbage
    static object[] s_keep;

    static volatile int s_armed;   // 1 once the driver/worker handshake is live
    static volatile int s_ready;   // driver: fragmentation prepared, worker may enter the region
    static volatile int s_inPark;  // worker: parked inside the region's call-out
    static volatile int s_gcDone;  // driver: the one compacting GC for this window has run
    static volatile bool s_stop;
    static long s_rounds, s_moved, s_sink;

    // ---- THE TRACED REGION ---------------------------------------------------------------------
    // NoInlining => its own JIT'd body and its own perf-map entry, which is how the tracer resolves
    // [base, base+len). Volatile.Write/Read pin the store and the load down to real memory accesses:
    // an ordinary store-then-reload could in principle be forwarded by the JIT, and the whole
    // experiment depends on the load genuinely re-reading the object at its post-move address. (The
    // opaque Park() call already invalidates heap value-numbering, so this is belt-and-braces — but
    // a silently forwarded load would make the test vacuous, which is exactly what must not happen.)
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long Region(long[] obj, long v)
    {
        Volatile.Write(ref obj[0], v);          // STORE — at the object's OLD address
        long p = Park();                        // call-out — the compacting GC relocates obj HERE
        return Volatile.Read(ref obj[0]) + p;   // LOAD — at the object's NEW address
    }

    // The call-out. Runs at native speed under the tracer's step-over, and blocks in Thread.Sleep —
    // i.e. in PREEMPTIVE GC mode, which the runtime suspends without hijacking anything.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long Park()
    {
        if (s_armed == 0) return 0;             // warm-up JIT pass: do not park
        s_inPark = 1;
        for (int i = 0; i < 30000 && s_gcDone == 0 && !s_stop; i++) Thread.Sleep(1);
        s_inPark = 0;
        return 0;
    }

    // Current data address of a long[] via a BRIEF pin — exact (no allocation between pin and Free),
    // and the pin is released immediately so the GC stays free to relocate the object. A PINNED
    // object does not move, which would make the whole run vacuous.
    static IntPtr DataAddr(Array obj)
    {
        var h = GCHandle.Alloc(obj, GCHandleType.Pinned);
        IntPtr a = h.AddrOfPinnedObject();
        h.Free();
        return a;
    }

    // Rebuild fragmentation so the NEXT compacting gen2 GC has something to slide the object over.
    // Runs OUTSIDE the call-out window, on the driver thread, so that the window itself contains
    // exactly one GC and no allocation of ours.
    //
    // ORDER IS EVERYTHING, and getting it wrong is silent: a compaction slides a survivor DOWN into
    // the holes BELOW it, so the object must be allocated AFTER the garbage it is meant to slide
    // over. An object allocated first sits at the bottom of gen2 with nothing beneath it and never
    // moves at all — measured, on the first run of this lane: 25/25 rounds with old == new while the
    // very same GCs relocated 15,000 other ranges. Hence a FRESH object every round, allocated on
    // top of fresh garbage.
    static void Fragment()
    {
        // 1. garbage first ...
        for (int i = 0; i < s_fill.Length; i++) s_fill[i] = new byte[64];
        // 2. ... then the object, so it lands ABOVE the holes about to be punched ...
        var obj = new long[8];
        // 3. ... promote the lot to gen2, so a gen2 compaction is what relocates them ...
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        // 4. ... and drop three quarters of the garbage: the holes BELOW the object that the next
        //    compaction closes, sliding the object DOWN.
        for (int i = 0; i < s_fill.Length; i++) if ((i & 3) != 0) s_fill[i] = null;
        s_obj = obj;
    }

    static void Driver()
    {
        Console.WriteLine("GCCANON_VICTIM_DRIVER tid=" + gettid());
        Console.Out.Flush();
        while (!s_stop)
        {
            if (s_armed == 0) { Thread.Sleep(2); continue; }
            Fragment();                 // outside the window
            s_gcDone = 0;
            s_ready = 1;
            // Wait for the worker to enter the region's call-out.
            for (int i = 0; i < 20000 && s_inPark == 0 && !s_stop; i++) Thread.Sleep(1);
            if (s_stop) break;
            if (s_inPark == 0) { s_ready = 0; continue; }
            // THE WINDOW: exactly one blocking, compacting gen2 GC that relocates survivors.
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            s_gcDone = 1;
            s_ready = 0;
            for (int i = 0; i < 5000 && s_inPark == 1 && !s_stop; i++) Thread.Sleep(1);
        }
    }

    static void Worker()
    {
        int tid = gettid();
        // Warm up: JIT Region + Park at their stable (non-tiered) addresses so they are in the perf
        // map BEFORE anyone attaches. s_armed is still 0, so Park returns immediately.
        s_warm = new long[8];
        s_sink = Region(s_warm, 1);
        Console.WriteLine("GCCANON_VICTIM_WORKER tid=" + tid + " warm=" + s_sink);
        Console.Out.Flush();

        while (!s_stop)
        {
            if (s_armed == 0 || s_ready == 0) { Thread.Sleep(1); continue; }
            // Read the static ONCE: the driver installs a fresh object each round (see Fragment),
            // and only ever while s_ready == 0, but the traced invocation must provably be about
            // the same object the addresses either side of it describe.
            long[] obj = s_obj;
            IntPtr before = DataAddr(obj);
            s_sink = Region(obj, Sentinel);         // <-- the traced invocation
            IntPtr after = DataAddr(obj);
            long n = Interlocked.Increment(ref s_rounds);
            if (before != after) Interlocked.Increment(ref s_moved);
            // The victim's OWN ground truth that the object relocated across the region — printed
            // independently of anything the tracer or profiler believes.
            Console.WriteLine("GCCANON_VICTIM_ROUND n=" + n + " moved=" + (before != after ? 1 : 0) +
                              " old=0x" + before.ToString("x") + " new=0x" + after.ToString("x") +
                              " val=0x" + s_sink.ToString("x"));
            Console.Out.Flush();
            Thread.Sleep(20);
        }
        Console.WriteLine("GCCANON_VICTIM_WORKER_END rounds=" + s_rounds + " moved=" + s_moved);
        Console.Out.Flush();
    }

    static void Main()
    {
        int seconds = Env("GCCANON_VICTIM_SECONDS", 90);
        Console.WriteLine("GCCANON_VICTIM_START pid=" + Environment.ProcessId + " seconds=" + seconds);
        Console.Out.Flush();

        s_fill = new object[60000];
        s_keep = new object[256];
        for (int i = 0; i < s_keep.Length; i++) s_keep[i] = new byte[64];
        // The object itself is (re)allocated by Fragment() every round, deliberately ON TOP of that
        // round's fresh garbage — see the ordering note there. This one is only so the worker's
        // warm-up JIT pass has something to run against.
        s_obj = new long[8];
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);

        var w = new Thread(Worker, 1 << 20) { Name = "gccanon_region", IsBackground = false };
        var d = new Thread(Driver, 1 << 20) { Name = "gccanon_gcdriver", IsBackground = true };
        w.Start();
        d.Start();

        // Let the worker warm up (JIT + perf map) before arming, so the first ARMED invocation is
        // already at the stable address the tracer resolved.
        Thread.Sleep(1500);
        Console.WriteLine("GCCANON_VICTIM_READY obj_len=" + s_obj.Length);
        Console.Out.Flush();
        s_armed = 1;

        var sw = Stopwatch.StartNew();
        while (sw.Elapsed.TotalSeconds < seconds) Thread.Sleep(200);
        s_stop = true;
        w.Join(10000);
        Console.WriteLine("GCCANON_VICTIM_END rounds=" + Interlocked.Read(ref s_rounds) +
                          " moved=" + Interlocked.Read(ref s_moved) +
                          " gen2=" + GC.CollectionCount(2));
        Console.Out.Flush();
    }

    static int Env(string name, int dflt)
    {
        var s = Environment.GetEnvironmentVariable(name);
        return int.TryParse(s, out int v) && v > 0 ? v : dflt;
    }
}
