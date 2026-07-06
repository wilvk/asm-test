// examples/dotnet/ptrace_native — reporting (presentation only). Renders the out-of-process
// trace: the executed instructions (disassembled from the code image) with step counts. No
// tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(NativeCode code, HwTrace tr, long result)
    {
        Console.WriteLine($"child computed sum(3,5) = {result} (expect 15); the tracer single-stepped "
                          + $"{tr.InsnsTotal()} instructions across the process boundary"
                          + (tr.Truncated() ? " (truncated)" : "") + ".\n");

        // Per-instruction step count: the loop body (add/dec/jnz) repeats, the prologue/ret run once.
        var count = new Dictionary<ulong, long>();
        foreach (ulong o in tr.InsnOffsets()) { count.TryGetValue(o, out long c); count[o] = c + 1; }
        var offs = new List<ulong>(count.Keys);
        offs.Sort();

        Console.WriteLine("executed instructions (disassembled from the child's code image), with step count:");
        foreach (ulong o in offs)
            Console.WriteLine($"    0x{o:x2}  x{count[o],-3}  {Disas(code, o)}");
        Console.WriteLine($"\nbasic blocks entered: {tr.BlocksLen()}");
        Console.WriteLine("-> the traced code ran in a SEPARATE process (a forked PTRACE_TRACEME child); this\n"
                          + "   process's own thread was never armed with EFLAGS.TF — no in-process SIGTRAP.");
    }

    // Decode the single instruction at code[off] for display, via the internal Capstone wrapper
    // (this example compiles the binding source).
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
