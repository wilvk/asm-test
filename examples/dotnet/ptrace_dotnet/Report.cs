// examples/dotnet/ptrace_dotnet — reporting (presentation only). Renders the out-of-process
// trace of the live JIT method: instruction count + the disassembled body read from the
// runtime's live code image. No tracing logic here.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Asmtest;

internal static class Report
{
    public static void Print(HwTrace tr, long result, IntPtr methodBase, byte[] code)
    {
        Console.WriteLine($"single-stepped {tr.InsnsTotal()} instructions of the REAL JIT'd Program::Add in the\n"
                          + $"other process (it returned {result}); entry covered: {tr.Covered(0)}"
                          + (tr.Truncated() ? "; truncated" : "") + ".\n");

        var count = new Dictionary<ulong, long>();
        foreach (ulong o in tr.InsnOffsets()) { count.TryGetValue(o, out long c); count[o] = c + 1; }
        var offs = new List<ulong>(count.Keys);
        offs.Sort();

        if (code != null && code.Length > 0)
        {
            Console.WriteLine("executed instructions (disassembled from the runtime's live code image):");
            foreach (ulong o in offs)
                Console.WriteLine($"    0x{o:x2}  x{count[o],-2}  {Disas(code, (ulong)methodBase.ToInt64(), o)}");
        }
        else
        {
            Console.WriteLine("executed instruction offsets: "
                              + string.Join(", ", offs.ConvertAll(o => "0x" + o.ToString("x"))));
        }

        Console.WriteLine("\n-> a real method in a live, GC'd, multi-threaded CoreCLR was single-stepped from\n"
                          + "   OUTSIDE — the exact scenario in-process single-step is forbidden to do.");
    }

    // Decode the instruction at code[off], where the bytes run at methodBase (so PC-relative
    // operands resolve). Pins the buffer read from the target's /proc/<pid>/mem.
    static string Disas(byte[] code, ulong methodBase, ulong off)
    {
        if (off >= (ulong)code.Length) return "(past end)";
        var h = GCHandle.Alloc(code, GCHandleType.Pinned);
        try
        {
            var buf = new byte[64];
            HwNative.asmtest_disas(0 /* X86_64 */, h.AddrOfPinnedObject(), (UIntPtr)code.Length,
                                   methodBase, off, buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf<byte>(buf, 0);
            if (z < 0) z = buf.Length;
            return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(buf, 0, z);
        }
        finally { h.Free(); }
    }
}
