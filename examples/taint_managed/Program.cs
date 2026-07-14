// Program.cs — the MANAGED seed->sink workload for the taint tier (Increment 5 exit
// criterion 3). Run under
//   DOTNET_PerfMapEnabled=1 drrun -c <taint client>.so methodscan=Hot -- \
//     dotnet taint_managed.dll [seed|noseed] [/shm-name]
//
// It demonstrates a taint seed on a buffer flowing through REAL JIT'd managed code to a
// branch-condition sink that is reported out-of-process via POSIX shared memory:
//   * a native shim (libtaint_managed_shim.so, P/Invoked below) maps the shm channel,
//     registers the sink report, and paints a native seed buffer's shadow via the DR taint
//     client's seed marker;
//   * HotSeedSink() — [MethodImpl(NoInlining)] so it JITs as its own method and lands in the
//     .NET perfmap, where the client's methodscan=Hot poller auto-registers its range — reads
//     the seeded buffer through a raw pointer and branches on it; once instrumented, the
//     tainted load->cmp->branch trips the client's branch-condition sink and one at_taint_hit_t
//     crosses to the validator.
// The driver loops with Thread.Sleep so the 10 ms poller catches HotSeedSink after it JITs
// and re-flushes it into instrumentation before the run ends. "noseed" is the negative
// control: the buffer is never painted, so the branch's eflags is clean and no hit fires.
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

static class TaintManaged
{
    const string Shim = "taint_managed_shim";

    [DllImport(Shim)] static extern int shim_init(string name);
    [DllImport(Shim)] static extern IntPtr shim_seedbuf();
    [DllImport(Shim)] static extern void shim_seed(ulong val);
    [DllImport(Shim)] static extern void shim_finish(long result);

    // The instrumented managed method: load the seeded native buffer through a raw pointer
    // (a plain `mov reg,[p]` in the JIT'd body), then use it as a LOOP BOUND. NoInlining keeps
    // it a distinct JIT'd method with its own perfmap entry so methodscan=Hot registers it.
    // A loop bound (not `if (x==...)`, which the JIT folds to a branchless cmov) forces a real
    // conditional branch whose `cmp i,x` reads the tainted x -> taints eflags -> trips the
    // client's branch-condition sink.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static unsafe long HotSeedSink(IntPtr p)
    {
        long x = *(long*)p;              // seeded (tainted) load
        long acc = 0;
        for (long i = 0; i < x; i++)     // loop bound depends on tainted x -> real branch
            acc += i;
        return acc;
    }

    static int Main(string[] args)
    {
        bool seed = args.Length < 1 || args[0] != "noseed";
        string shm = args.Length > 1 ? args[1] : "";

        if (shim_init(shm) != 0)
        {
            Console.WriteLine("HELLO_TAINT_MANAGED shim_init FAILED");
            return 2;
        }
        IntPtr buf = shim_seedbuf();
        if (seed)
            shim_seed(7);    // paint the buffer's shadow BEFORE the instrumented calls

        long r = 0;
        for (int i = 0; i < 200; i++)
        {
            r = HotSeedSink(buf);     // first calls JIT+register; later calls instrumented
            Thread.Sleep(10);         // ~2 s: time for the poller to register + flush
        }

        shim_finish(r);
        Console.WriteLine("HELLO_TAINT_MANAGED seed=" + (seed ? 1 : 0) + " r=" + r);
        return 0;
    }
}
