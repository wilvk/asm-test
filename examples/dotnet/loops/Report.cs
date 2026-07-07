// examples/dotnet/loops — reporting (presentation only). Detects backedges in the ordered
// instruction stream and reports each loop's trip count. No tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(NativeCode code, HwTrace tr, long m, long n, long result)
    {
        Console.WriteLine($"nested({m},{n}) = {result} (expect {m * n}); "
                          + $"{tr.InsnsTotal()} instructions executed"
                          + (tr.Truncated() ? " (truncated)" : "") + ".\n");

        // A backedge is a taken branch to a LOWER offset. Walk consecutive offsets; a decrease
        // means the branch at `from` jumped back to loop header `to`. Count each (from -> to).
        ulong[] stream = tr.InsnOffsets();
        var trips = new Dictionary<(ulong From, ulong To), long>();
        bool structural = Disas.Available;
        for (int i = 1; i < stream.Length; i++)
        {
            ulong from = stream[i - 1], to = stream[i];
            if (to >= from) continue;                        // forward / straight-line
            // Confirm `from` is a branch where Capstone is present (guards against any
            // non-branch backward artifact); otherwise trust the offset decrease.
            if (structural && !Disas.IsBranch((ulong)code.Base.ToInt64() + from)) continue;
            trips.TryGetValue((from, to), out long c);
            trips[(from, to)] = c + 1;
        }

        // Rank loops by header offset (outer first, then inner — outer header is the lower one).
        var loops = new List<KeyValuePair<(ulong From, ulong To), long>>(trips);
        loops.Sort((a, b) => a.Key.To.CompareTo(b.Key.To));

        Console.WriteLine($"detected {loops.Count} loop(s) (one per distinct backedge):\n");
        Console.WriteLine($"  {"header",-8}{"backedge",-12}{"trips",8}  back-branch instruction");
        Console.WriteLine("  " + new string('-', 64));
        foreach (var lp in loops)
            Console.WriteLine($"  0x{lp.Key.To:x2}{"",-4}0x{lp.Key.From:x2} ->{"",-4}{lp.Value,8}  {DisasAt(code, lp.Key.From)}");

        Console.WriteLine($"\n-> the inner loop (header 0x0e) runs {m}*{n} = {m * n} iterations total; the outer\n"
                          + $"   loop (header 0x06) runs {m}. The trip counts come from the DYNAMIC trace — a static\n"
                          + "   CFG shows the loops exist, not how many times each actually ran. Exact, not sampled.");
    }

    static string DisasAt(NativeCode code, ulong off)
    {
        var buf = new byte[64];
        HwNative.asmtest_disas(0 /* X86_64 */, code.Base, (UIntPtr)code.Length,
                               (ulong)code.Base.ToInt64(), off, buf, (UIntPtr)buf.Length);
        int z = Array.IndexOf<byte>(buf, 0);
        if (z < 0) z = buf.Length;
        return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(buf, 0, z);
    }
}
