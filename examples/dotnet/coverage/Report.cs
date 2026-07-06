// examples/dotnet/coverage — reporting (presentation only). Drives the HwTrace block-coverage
// API over several inputs and prints covered/uncovered basic blocks. No tracing policy beyond
// choosing inputs lives here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    // The basic-block offsets ONE input executes. A fresh trace per input, so BlockOffsets() is
    // exactly that input's blocks — union them in managed (correct regardless of whether repeated
    // Region() calls on one trace accumulate).
    static SortedSet<ulong> BlocksFor(NativeCode code, long input, out long result)
    {
        var tr = HwTrace.Create(blocks: 32, instructions: 64);
        tr.Register("classify", code);
        long r = 0;
        tr.Region("classify", () => r = code.Call(input, 0));
        result = r;
        var s = new SortedSet<ulong>();
        foreach (ulong b in tr.BlockOffsets()) s.Add(b);
        tr.Free();
        return s;
    }

    public static void Print(NativeCode code)
    {
        // The full reachable block set: a representative input for each branch.
        long[] repr = { -5, 50, 200 };
        var all = new SortedSet<ulong>();
        var result = new Dictionary<long, long>();
        foreach (long x in repr)
        {
            all.UnionWith(BlocksFor(code, x, out long r));
            result[x] = r;
        }

        // Self-check the hand-assembled routine — proves the bytes decode + run as intended.
        Console.WriteLine($"classify(-5)={result[-5]}, classify(50)={result[50]}, classify(200)={result[200]}  "
                          + "(expect 1, 2, 3)\n");

        // A "test suite" exercising only the POSITIVE inputs — it misses the negative path.
        long[] tests = { 50, 200 };
        var covered = new SortedSet<ulong>();
        foreach (long x in tests) covered.UnionWith(BlocksFor(code, x, out _));

        double pct = all.Count > 0 ? 100.0 * covered.Count / all.Count : 0;
        Console.WriteLine($"test inputs {{{string.Join(",", tests)}}} cover {covered.Count} of {all.Count} "
                          + $"reachable basic blocks ({pct:F0}%):\n");
        Console.WriteLine($"  {"block",-8} {"covered",-8} entry instruction");
        Console.WriteLine("  " + new string('-', 48));
        foreach (ulong off in all)
            Console.WriteLine($"  0x{off:x2}{"",-4} {(covered.Contains(off) ? "  yes" : "  NO "),-8} {Disas(code, off)}");

        Console.WriteLine();
        foreach (ulong off in all)
            if (!covered.Contains(off))
                Console.WriteLine($"-> block 0x{off:x} ({Disas(code, off)}) is NEVER covered by the positive-only tests —\n"
                                  + "   the missing test case is a NEGATIVE input (x < 0). This is exact, not sampled.");
    }

    // Decode the single instruction at code[off] for display (the block's entry instruction),
    // via the internal Capstone wrapper (this example compiles the binding source).
    static string Disas(NativeCode code, ulong off)
    {
        var buf = new byte[64];
        HwNative.asmtest_disas(0 /* X86_64 */, code.Base, (UIntPtr)code.Length,
                               (ulong)code.Base.ToInt64(), off, buf, (UIntPtr)buf.Length);
        int z = Array.IndexOf<byte>(buf, 0);
        if (z < 0) z = buf.Length;
        return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(buf, 0, z);
    }
}
