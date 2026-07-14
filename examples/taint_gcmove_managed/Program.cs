// Program.cs — the MANAGED GC-move SURVIVAL workload for the taint tier (Increment 7, Slice 2).
// Run under
//   CORECLR_ENABLE_PROFILING=1 CORECLR_PROFILER=<clsid> CORECLR_PROFILER_PATH=libgcprobe.so \
//   DOTNET_PerfMapEnabled=1 drrun -c <taint client>.so gcmove methodscan=GcMove -- \
//     dotnet taint_gcmove_managed.dll [seed|noseed] [/shm-name]
//
// This is the end-to-end proof that a taint seed painted on a GC-MOVABLE managed object
// SURVIVES a compacting GC that relocates the object — the capability the Increment-5 managed
// lane deliberately deferred (it seeds a stable NATIVE buffer). The choreography:
//   1. allocate a managed byte[] and promote it to gen2;
//   2. get its CURRENT data address by a BRIEF pin (GCHandle.Pinned -> AddrOfPinnedObject ->
//      Free) — no allocation in between, so the address is exact — then paint that address's
//      shadow tainted via the P/Invoked shim (shim_seed_at). The pin is released BEFORE the
//      move so the GC is free to relocate the object;
//   3. allocate heavy fragmented garbage + survivors and force compacting gen2 GCs that
//      RELOCATE the object. The in-process MovedReferences2 profiler feeds every moved range
//      to the DR taint client's DR-API-free live remap (at_gc_remap_live) at the GC fence,
//      which carries the tag from the old range to the new — mapping a never-touched
//      destination leaf with a bare mmap syscall (Slice 2's raw leaf allocator);
//   4. drive an INSTRUMENTED sink method (GcMoveSink, NoInlining -> its own perfmap entry so
//      methodscan=GcMove auto-registers it) that reads the MOVED object through a `fixed`
//      pointer and branches on it: a tainted load->cmp->branch at the object's NEW address
//      trips the client's branch-condition sink -> a hit crosses to the out-of-process
//      validator. Sink fires => the seed survived the move.
// "noseed" is the negative control: the object is never painted, so no hit fires — proving no
// phantom taint is conjured at the moved-into (freshly mmap'd) destination leaf.
using System;
using System.Collections.Generic;
using System.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

static class TaintGcMoveManaged
{
    const string Shim = "taint_managed_shim";

    [DllImport(Shim)] static extern int shim_init(string name);
    [DllImport(Shim)] static extern void shim_seed_at(IntPtr addr, ulong len);
    [DllImport(Shim)] static extern void shim_finish(long result);

    // Keep the seeded object alive across the whole run (a static root so a Gen-2 collection
    // never reclaims it — it must SURVIVE and MOVE, not die).
    static byte[] s_obj;

    // The instrumented sink method: read the (moved) managed object through a pinning `fixed`,
    // then use its first 8 bytes as a LOOP BOUND so a real conditional branch reads the tainted
    // value -> taints eflags -> trips the client's branch-condition sink. NoInlining keeps it a
    // distinct JIT'd method with its own perfmap entry so methodscan=GcMove registers it. A loop
    // bound (not `if (x==...)`, which the JIT folds to a branchless cmov) forces a real branch.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static unsafe long GcMoveSink(byte[] obj)
    {
        fixed (byte* p = obj)
        {
            long x = *(long*)p;              // tainted load at the object's CURRENT (moved) address
            long acc = 0;
            for (long i = 0; i < x; i++)     // loop bound depends on tainted x -> real branch
                acc += i;
            return acc;
        }
    }

    // Current data address of a byte[] via a BRIEF pin (exact: no allocation between pin and Free,
    // and AddrOfPinnedObject for a byte[] is &obj[0], the same address `fixed` yields).
    static IntPtr DataAddr(byte[] obj)
    {
        var h = GCHandle.Alloc(obj, GCHandleType.Pinned);
        IntPtr a = h.AddrOfPinnedObject();
        h.Free();
        return a;
    }

    // Force compacting Gen-2 GCs that relocate surviving objects (the gcmover pattern: heavy
    // fragmented garbage interleaved with retained survivors). Returns after the object has had
    // ample opportunity to move.
    static void ForceCompactingMoves()
    {
        var survivors = new object[20000];
        for (int round = 0; round < 4; round++)
        {
            var garbage = new List<byte[]>();
            for (int i = 0; i < 120000; i++)
            {
                var b = new byte[64];
                if ((i & 3) == 0) survivors[i % survivors.Length] = b; // fragmented survivors
                else garbage.Add(b);
            }
            garbage.Clear();
            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
        }
        GC.KeepAlive(survivors);
    }

    static int Main(string[] args)
    {
        bool seed = args.Length < 1 || args[0] != "noseed";
        string shm = args.Length > 1 ? args[1] : "";

        if (shim_init(shm) != 0)
        {
            Console.WriteLine("HELLO_GCMOVE_MANAGED shim_init FAILED");
            return 2;
        }

        // The GC-movable object to seed. First 8 bytes = a small positive loop bound (5).
        s_obj = new byte[64];
        s_obj[0] = 5;

        // Promote it to gen2 so the later compacting gen2 GCs relocate it.
        GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
        IntPtr addrOld = DataAddr(s_obj);

        // Seed the object's shadow at its CURRENT address, then let it move (pin already freed).
        if (seed)
            shim_seed_at(addrOld, 8);

        // JIT the sink once (pre-instrumentation) so it lands in the perfmap for methodscan.
        _ = GcMoveSink(s_obj);

        // Force compacting gen2 GCs that RELOCATE the object; the profiler feeds the moves.
        ForceCompactingMoves();

        IntPtr addrNew = DataAddr(s_obj);
        bool moved = addrNew != addrOld;
        Console.WriteLine("GCMOVE_MANAGED moved=" + (moved ? 1 : 0) +
                          " old=0x" + addrOld.ToString("x") + " new=0x" + addrNew.ToString("x"));

        // Drive the instrumented sink over the MOVED object: loop with Thread.Sleep so the ~10 ms
        // perfmap poller registers GcMoveSink and re-flushes it into instrumentation; the tainted
        // load->cmp->branch then trips the sink at the object's NEW address. No allocation in this
        // loop, so no further GC moves the object — addrNew stays valid.
        long r = 0;
        for (int i = 0; i < 200; i++)
        {
            r = GcMoveSink(s_obj);
            Thread.Sleep(10);
        }

        shim_finish(r);
        Console.WriteLine("HELLO_GCMOVE_MANAGED seed=" + (seed ? 1 : 0) +
                          " moved=" + (moved ? 1 : 0) + " r=" + r);
        return 0;
    }
}
