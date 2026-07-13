// Program.cs — the managed workload for the taint tier's launch-under-DR dotnet
// coexistence smoke (Increment 5). It runs a HOT method in a long loop so .NET's tiered
// JIT recompiles it (tier-0 -> tier-1) mid-run, forcing DR to invalidate + rebuild its
// code cache for live-rewritten managed code — the "does DR coexist with .NET tiered
// JIT" hypothesis. Success = it prints HELLO_TAINT_DOTNET and exits 0 under
// `drrun -c <taint client>.so -- dotnet taint_hello.dll` (no SIGTRAP/SIGSEGV/crash/hang).
using System;

class Program
{
    // A small hot method: called millions of times so it tiers up while running.
    static long Work(int x) => ((long)x * x + x) ^ (x >> 1);

    static void Main()
    {
        long acc = 0;
        for (int i = 0; i < 4_000_000; i++)
            acc += Work(i % 4096);
        Console.WriteLine("HELLO_TAINT_DOTNET acc=" + acc);
    }
}
