// examples/dotnet/coverage-guided-fuzz — reporting (presentation only). Renders the per-input
// keep/discard decision (with the marginal blocks each input unlocked, disassembled) and the
// final corpus summary. No tracing policy lives here — the fuzz loop and accounting are in
// Program.cs; this file only formats what it is handed.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Header()
    {
        Console.WriteLine("feeding a corpus into validate(x), a fresh region-scoped trace per input.");
        Console.WriteLine("an input is KEPT iff it covers a basic block no kept input covered before");
        Console.WriteLine("(its MARGINAL coverage); a range-duplicate adds nothing and is discarded.\n");
    }

    public static void Row(NativeCode code, int i, long input, long result, int nblocks,
                           List<ulong> unlocked, bool keep, bool reachedBug,
                           int corpusSize, int totalBlocks)
    {
        string decision = keep ? "KEEP   " : "discard";
        string marginal = unlocked.Count == 0
            ? "(none — redundant)"
            : $"+{unlocked.Count} block(s): {Blocks(code, unlocked)}";

        Console.Write($"  [{i}] validate({input,5}) = {result,-6} {decision}  {marginal}");
        Console.WriteLine(reachedBug ? "   <== DEEP bug block reached (the 'crash')" : "");
        if (keep)
            Console.WriteLine($"        -> corpus grows to {corpusSize}; cumulative coverage {totalBlocks} blocks");
    }

    public static void Summary(List<long> kept, int totalBlocks, bool bugFound, long bugInput)
    {
        Console.WriteLine();
        Console.WriteLine($"final corpus ({kept.Count} inputs kept of the stream): {{{string.Join(", ", kept)}}}");
        Console.WriteLine($"cumulative coverage: {totalBlocks} distinct basic blocks.\n");
        if (bugFound)
            Console.WriteLine($"-> input {bugInput} (0x{bugInput:x}) is the INTERESTING one: it first reached the deep\n"
                              + "   guarded block no range-typical input touches — precisely the coverage signal a\n"
                              + "   coverage-guided fuzzer promotes and minimizes toward. Exact, not sampled:\n"
                              + "   single-step, region-scoped, deterministic — the same block set every run.");
        else
            Console.WriteLine("-> no input reached the deep bug block in this corpus.");
    }

    // Render a marginal-coverage block list as "0xNN (entry instruction), ..." — each offset with
    // its disassembled entry instruction, so the unlocked path is tangible.
    static string Blocks(NativeCode code, List<ulong> offs)
    {
        var sb = new System.Text.StringBuilder();
        for (int k = 0; k < offs.Count; k++)
        {
            if (k > 0) sb.Append(", ");
            sb.Append($"0x{offs[k]:x2} ({Disas(code, offs[k])})");
        }
        return sb.ToString();
    }

    // Decode the single instruction at code[off] for display, via the internal Capstone wrapper
    // (this example compiles the binding source, so HwNative is in-assembly).
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
