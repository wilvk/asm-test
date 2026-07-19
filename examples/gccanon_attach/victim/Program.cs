using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// F4 increments 1+2 VICTIM — the managed fixture whose memory def-use must survive a GC compaction
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
// HOW MANY compacting GCs land in one call-out window is the fixture's MAIN KNOB
// (GCCANON_GCS_PER_WINDOW, default 1), and it is choreographed rather than hoped for. The step
// counter is FROZEN across the call-out (the region-gated counter records nothing over the helper),
// so every GC in one window is stamped with the SAME S0 — and asmtest_gcmove_canon applies at most
// ONE relocation per step-group, because a group models ONE GCBulkMovedObjectRanges batch whose old
// ranges are disjoint.
//
//   GCCANON_GCS_PER_WINDOW=1  increment 1's choreography, unchanged: one GC per window, so no two
//                             GCs ever share a step and the batch model holds as shipped.
//   GCCANON_GCS_PER_WINDOW=N  increment 2's: N compacting GCs inside ONE window, each moving the
//                             SAME object again (A->B->C->...). All N are stamped with the SAME S0
//                             and collapse into one batch — the limitation increment 1 landed with,
//                             provoked on purpose so the tracer can reproduce the resulting missing
//                             edge as a FAILING case and then fix it by CHAINING the moves.
//
// Moving the object AGAIN on the window's second GC is not automatic and is the whole trick: a
// compaction only slides a survivor down into holes BELOW it, and the window's first GC just closed
// all of them. So each of the window's GCs gets its own WAVE of fresh garbage to close — see
// DropWave — dropped between GCs, on the driver thread, allocating nothing.
static class GcCanonVictim
{
    [DllImport("libc", SetLastError = true)]
    static extern int gettid();

    // The value stored before the move and loaded back after it. Recognizable in the value trace:
    // an 8-byte MEM_ABS write whose value is this, then an 8-byte MEM_ABS read of the same value at
    // a DIFFERENT address. It is passed in as a parameter (never a JIT constant).
    public const long Sentinel = 0x5EEDCAFE12345678L;

    // Increment 4 (T5) ALIAS FIXTURE (GCCANON_ALIAS_FIXTURE=1). Sentinel2 is STORED into the object
    // about to die (doomed); Sentinel3 is pre-seeded into the object slid onto its slot (live) and
    // LOADED back. Distinct 8-byte values so the tracer finds the two records unambiguously — kept in
    // step with gccanon_tracer.c GCCANON_SENTINEL2/3.
    public const long Sentinel2 = 0x2EED000212340002L;
    public const long Sentinel3 = 0x3EED000312340003L;
    // doomed/live are LARGE arrays (> 85000 bytes) so they live on the Large Object Heap. .NET 8's
    // region GC relocates ordinary survivors into destination regions — it will NOT slide `live` onto
    // `doomed`'s vacated slot (measured) — but the LOH never moves objects and frees to a FREE LIST,
    // so a same-size LOH allocation after doomed dies reuses doomed's EXACT block and lands back at X.
    const int AliasLen = 16384; // 128 KiB



    // The seeded, GC-movable object. A static root, so a gen2 collection never reclaims it — it must
    // SURVIVE and MOVE. long[] deliberately: a primitive element store needs no write barrier, so
    // the region stays a clean store/call/load with no helper call before the one we want.
    static long[] s_obj;
    static long[] s_warm;
    static object[] s_fill;        // fragmentation: survivors interleaved with dropped garbage
    static object[] s_keep;
    // Keep the most recent traced objects alive so the one the tracer captured is still in the heap
    // when the T3 dumper snapshots it (post-detach). Power-of-two ring; ~128 rounds >> the dump time.
    static readonly object[] s_alive = new object[128];
    static int s_aliveIdx;

    // Increment 4 (T5) alias fixture. `doomed` receives the store then dies; `live` is slid onto its
    // vacated slot and holds the load value. See RegionAlias / AliasSetup.
    static long[] s_doomed;
    static long[] s_live;
    static int s_alias;            // 1 => GCCANON_ALIAS_FIXTURE mode
    static long s_aliasX;          // &doomed[0] BEFORE the window == &live[0] AFTER it (the alias X)
    static long s_aliasLiveNew;    // &live[0] AFTER the window (must equal s_aliasX)
    // Every round's `live` is KEPT (never reused): the traced round's live must still be at its own,
    // never-reused slot X when the post-detach dumper snapshots the heap, so the objid join can
    // resolve the load to it and there is no later slide onto X to confuse the inverse walk. Capped,
    // then the driver idles — the tracer captures an early round, long before the cap.
    static readonly List<long[]> s_aliasLives = new List<long[]>();
    const int AliasCap = 300;

    static volatile int s_armed;   // 1 once the driver/worker handshake is live
    static volatile int s_ready;   // driver: fragmentation prepared, worker may enter the region
    static volatile int s_inPark;  // worker: parked inside the region's call-out
    static volatile int s_gcDone;  // driver: ALL of this window's compacting GCs have run
    static volatile bool s_stop;
    static long s_rounds, s_moved, s_sink;

    // How many compacting gen2 GCs the driver puts inside ONE call-out window. 1 = increment 1's
    // choreography (no two GCs share a step); >1 provokes increment 2's collapse with a REAL
    // multiply-moved object. Capped at 3 by the four-bucket wave scheme in DropWave.
    static int s_gcsPerWindow = 1;
    // The object's address after each of the window's GCs — chain[0]=A (before any), chain[g]=its
    // home after GC g. The victim's OWN ground truth for the chain, allocated ONCE (outside any
    // window) so that recording it never allocates between two of the window's GCs.
    static long[] s_chain;

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

    // Increment 4 (T5) THE ALIAS REGION. The store and the load hit the SAME address X but belong to
    // DIFFERENT objects: `doomed` receives the store at X and then dies inside the call-out; a fresh
    // object is allocated onto X (s_live) and the load reads it there. Address identity keys both on X
    // and forges a false def-use edge; OBJECT identity re-keys the store out of the object space (its
    // object is dead, no node) while the load keys the live object's node, and the edge is gone.
    //
    // .NET 8 is REGION-based: a compacting gen2 GC relocates a survivor into a destination region, it
    // does NOT slide it onto a specific vacated slot — so "live slides onto doomed's slot" (segment
    // thinking) does not happen. What IS reliable is gen0 slot reuse: after doomed (the first gen0
    // object, at X) dies and a gen0 GC resets the region's bump pointer, the very next gen0 allocation
    // lands back at X. The driver does exactly that DURING Park() and publishes it as s_live.
    //
    // `doomed`'s LAST use is the store, so with precise (non-tiered) GC info its argument reference is
    // dead across Park(), and the driver also nulls the doomed static — so the gen0 GC frees it.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long RegionAlias(long[] doomed, long v)
    {
        Volatile.Write(ref doomed[0], v);        // STORE at X — into the object about to die
        long p = Park();                         // doomed dies; the driver allocates s_live back at X
        return Volatile.Read(ref s_live[0]) + p; // LOAD at X — a DIFFERENT object (reused slot)
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
        s_obj = obj;
        // 4. ... and drop this window's FIRST wave of garbage: the holes BELOW the object that the
        //    window's first compaction closes, sliding the object DOWN. The later waves are held
        //    back for the window's later GCs (DropWave), because a second compaction with nothing
        //    left to close would not move the object at all.
        DropWave(1);
    }

    // Drop wave `k`'s garbage — the holes the window's k-th GC closes. Buckets are `i & 3`:
    //
    //   bucket 0    NEVER dropped. A quarter of the fill survives every round, so the object always
    //               has live neighbours below it and a compaction has something to slide it over.
    //   buckets 1-3 shared out over the waves, so EVERY GC in the window has fresh holes beneath the
    //               object and moves it AGAIN. With s_gcsPerWindow=1 all three go in wave 1, which
    //               is exactly increment 1's "drop three quarters" — its fixture is unchanged, so
    //               its asserts stay a regression test rather than a rewrite. With 2 they split
    //               2:1 (~2.4 MB then ~1.2 MB of holes), with 3, 1:1:1.
    //
    // Allocates NOTHING (a reference store is a write barrier, not an allocation), which is what
    // lets it run BETWEEN two of the window's GCs without perturbing the window it is choreographing.
    static void DropWave(int k)
    {
        int waves = s_gcsPerWindow;
        for (int i = 0; i < s_fill.Length; i++)
        {
            int b = i & 3;
            if (b == 0) continue;                 // the permanent survivors
            if (1 + (b - 1) * waves / 3 == k) s_fill[i] = null;
        }
    }

    static void Driver()
    {
        Console.WriteLine("GCCANON_VICTIM_DRIVER tid=" + gettid() + " gcs_per_window=" + s_gcsPerWindow);
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
            // THE WINDOW. The worker is parked in the region's call-out, so the tracer's
            // region-gated step counter is frozen and EVERY GC from here until s_gcDone is stamped
            // with the same S0. s_gcsPerWindow blocking compacting gen2 GCs go in, each preceded by
            // its own fresh wave of holes below the object so that each one moves it AGAIN:
            // A -> B -> C. Nothing here allocates (DropWave stores nulls; the chain array is
            // pre-allocated), so the window contains exactly the GCs it is meant to contain.
            s_chain[0] = (long)DataAddr(s_obj);       // A — before any of the window's GCs
            for (int g = 1; g <= s_gcsPerWindow; g++)
            {
                if (g > 1) DropWave(g);               // fresh holes BELOW the object for THIS GC
                GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                s_chain[g] = (long)DataAddr(s_obj);   // ... -> B -> C
            }
            s_gcDone = 1;
            s_ready = 0;
            for (int i = 0; i < 5000 && s_inPark == 1 && !s_stop; i++) Thread.Sleep(1);
            // Report the chain only AFTER the worker has left the call-out: formatting allocates,
            // and an allocation-triggered gen0 GC while the worker was still parked would land an
            // UNPLANNED extra GC in the window being measured.
            var sb = new StringBuilder("GCCANON_VICTIM_WINDOW gcs=" + s_gcsPerWindow + " chain=0x")
                .Append(s_chain[0].ToString("x"));
            int real = 0;
            for (int g = 1; g <= s_gcsPerWindow; g++)
            {
                sb.Append("->0x").Append(s_chain[g].ToString("x"));
                if (s_chain[g] != s_chain[g - 1]) real++;
            }
            Console.WriteLine(sb.Append(" real_moves=" + real + "/" + s_gcsPerWindow).ToString());
            Console.Out.Flush();
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
            s_alive[s_aliveIdx++ & (s_alive.Length - 1)] = obj; // survive to the post-detach snapshot
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

    // ---- THE ALIAS FIXTURE (increment 4 / T5) --------------------------------------------------
    // Set up one round: empty the ephemeral generations so `doomed` is the FIRST gen0 object, at the
    // gen0 region start — a slot X the driver's window GC will hand straight back to `live`.
    static void AliasSetup()
    {
        // doomed and live are BOTH on the LOH, allocated FRESH on TOP of the accumulated (packed,
        // hole-free) survivors — so this round's only LOH hole, after doomed dies, is doomed's, and
        // the one-shot LOH COMPACTION in the window slides `live` straight down onto it (X). An LOH
        // slide IS reported by MovedReferences2 (live: old -> X), the move the objid join needs to
        // tell doomed@X (store) from live@X (load) apart. Crucially `live` is KEPT (never reused): the
        // older lives packed below cannot slide, so each round's live stays at its own X forever, and
        // the traced round's live is still there — at a never-reused X — when the dumper snapshots.
        var doomed = new long[AliasLen];     // LOH, on top
        var live = new long[AliasLen];       // LOH, above doomed
        live[0] = Sentinel3;
        s_aliasLives.Add(live);              // KEEP it — the traced live must reach the snapshot
        s_aliasX = (long)DataAddr(doomed);   // X — doomed's LOH data address
        s_live = live;
        s_doomed = doomed;                   // published LAST, so the worker never sees a stale pair
    }

    static void AliasDriver()
    {
        Console.WriteLine("GCCANON_VICTIM_DRIVER tid=" + gettid() + " alias=1");
        Console.Out.Flush();
        while (!s_stop)
        {
            if (s_armed == 0) { Thread.Sleep(2); continue; }
            if (s_aliasLives.Count >= AliasCap) { s_ready = 0; Thread.Sleep(50); continue; } // cap; then idle, keeping all lives alive
            AliasSetup();
            s_aliasLiveNew = 0;
            s_gcDone = 0;
            s_ready = 1;
            for (int i = 0; i < 20000 && s_inPark == 0 && !s_stop; i++) Thread.Sleep(1);
            if (s_stop) break;
            if (s_inPark == 0) { s_ready = 0; continue; }
            // THE WINDOW. The worker is parked past the store, with `doomed` dead (precise liveness).
            // Null the doomed static so its only other reference is that dead param, then a one-shot
            // LOH-COMPACTING gen2 GC: doomed's LOH block becomes the single hole and `live` slides down
            // onto it -> &live[0] == X. The slide is a real relocation, so MovedReferences2 stamps it
            // into the feed (live: old -> X) for the objid join.
            s_doomed = null;
            GCSettings.LargeObjectHeapCompactionMode = GCLargeObjectHeapCompactionMode.CompactOnce;
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            s_aliasLiveNew = (long)DataAddr(s_live);   // live's new address — must equal X
            s_gcDone = 1;
            s_ready = 0;
            for (int i = 0; i < 5000 && s_inPark == 1 && !s_stop; i++) Thread.Sleep(1);
            int ok = (s_aliasX != 0 && s_aliasLiveNew == s_aliasX) ? 1 : 0;
            Console.WriteLine("GCCANON_VICTIM_ALIAS ok=" + ok + " x=0x" + s_aliasX.ToString("x") +
                              " live_new=0x" + s_aliasLiveNew.ToString("x"));
            Console.Out.Flush();
        }
    }

    static void AliasWorker()
    {
        int tid = gettid();
        if (s_live == null) s_live = new long[8];   // null-safety for the warm-up / pre-window load
        // Warm up RegionAlias at its stable (non-tiered) address so it is in the perf map before the
        // tracer attaches. s_armed is 0, so Park returns immediately and no choreography runs.
        s_sink = RegionAlias(new long[8], 1);
        Console.WriteLine("GCCANON_VICTIM_WORKER tid=" + tid + " warm=" + s_sink);
        Console.Out.Flush();

        while (!s_stop)
        {
            if (s_armed == 0 || s_ready == 0) { Thread.Sleep(1); continue; }
            // Pass s_doomed DIRECTLY into the call: no worker-frame local keeps `doomed` alive across
            // Park(), so its only references during the window are RegionAlias's own param (dead after
            // the store) and the static the driver nulls once we are parked.
            s_sink = RegionAlias(s_doomed, Sentinel2);   // <-- the traced invocation
            long n = Interlocked.Increment(ref s_rounds);
            if (s_aliasLiveNew == s_aliasX) Interlocked.Increment(ref s_moved);
            // Mirror the numeric fixture's per-round line so the lane's rounds/moved counting works;
            // the definitive alias report is GCCANON_VICTIM_ALIAS from the driver.
            Console.WriteLine("GCCANON_VICTIM_ROUND n=" + n + " moved=" +
                              (s_aliasLiveNew == s_aliasX ? 1 : 0) + " old=0x" + s_aliasX.ToString("x") +
                              " new=0x" + s_aliasLiveNew.ToString("x") + " val=0x" + s_sink.ToString("x"));
            Console.Out.Flush();
            Thread.Sleep(20);
        }
        Console.WriteLine("GCCANON_VICTIM_WORKER_END rounds=" + s_rounds + " moved=" + s_moved);
        Console.Out.Flush();
    }

    static void Main()
    {
        int seconds = Env("GCCANON_VICTIM_SECONDS", 90);
        // 1 = increment 1's one-GC window. >1 = increment 2's collapse fixture: that many
        // compacting GCs inside ONE call-out window, all stamped with the same S0.
        s_gcsPerWindow = Env("GCCANON_GCS_PER_WINDOW", 1);
        if (s_gcsPerWindow > 3) s_gcsPerWindow = 3;   // DropWave's bucket scheme: 1 survivor + 3 waves
        s_chain = new long[s_gcsPerWindow + 1];
        s_alias = Env("GCCANON_ALIAS_FIXTURE", 0);    // increment 4 / T5: the store/load-alias fixture
        Console.WriteLine("GCCANON_VICTIM_START pid=" + Environment.ProcessId + " seconds=" + seconds +
                          " gcs_per_window=" + s_gcsPerWindow + " alias=" + s_alias);
        Console.Out.Flush();

        s_fill = new object[60000];
        s_keep = new object[256];
        for (int i = 0; i < s_keep.Length; i++) s_keep[i] = new byte[64];
        // The object itself is (re)allocated by Fragment() every round, deliberately ON TOP of that
        // round's fresh garbage — see the ordering note there. This one is only so the worker's
        // warm-up JIT pass has something to run against.
        s_obj = new long[8];
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);

        ThreadStart workerBody = s_alias != 0 ? AliasWorker : Worker;
        ThreadStart driverBody = s_alias != 0 ? AliasDriver : Driver;
        var w = new Thread(workerBody, 1 << 20) { Name = "gccanon_region", IsBackground = false };
        var d = new Thread(driverBody, 1 << 20) { Name = "gccanon_gcdriver", IsBackground = true };
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
