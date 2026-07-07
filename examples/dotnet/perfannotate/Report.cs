// examples/dotnet/perfannotate — reporting (presentation only). Counts InsnOffsets() per offset
// and prints the region's instructions in ADDRESS order with heat bars. No tracing here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(NativeCode code, HwTrace tr, long n, long result)
    {
        // Expected: sum of even i in [0, n). For n=10 -> {0,2,4,6,8} = 20.
        long expect = 0;
        for (long i = 0; i < n; i++) if ((i & 1) == 0) expect += i;
        Console.WriteLine($"count_even({n}) = {result} (expect {expect}); "
                          + $"{tr.InsnsTotal()} instructions executed over the region"
                          + (tr.Truncated() ? " (truncated)" : "") + ".\n");

        // Per-offset execution count from the ordered stream.
        var count = new Dictionary<ulong, long>();
        foreach (ulong o in tr.InsnOffsets()) { count.TryGetValue(o, out long c); count[o] = c + 1; }
        var offs = new List<ulong>(count.Keys);
        offs.Sort();
        long max = 1;
        foreach (long c in count.Values) if (c > max) max = c;

        // Annotate in address order (perf annotate reads top-to-bottom, not by heat).
        Console.WriteLine($"  {"count",6}  {"heat",-16}  addr   instruction");
        Console.WriteLine("  " + new string('-', 60));
        foreach (ulong o in offs)
        {
            string bar = new string('#', (int)Math.Round(16.0 * count[o] / max));
            Console.WriteLine($"  {count[o],6}  {bar,-16}  0x{o:x2}   {Disas(code, o)}");
        }

        Console.WriteLine("\n-> the loop overhead (cmp/jge/test/jne/inc/jmp) runs every iteration; the guarded\n"
                          + "   `add rax, rcx` runs only on EVEN i — half as hot. This is EXACT (single-step),\n"
                          + "   not sampled: every count is the true execution count, deterministic across runs.");
    }

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
