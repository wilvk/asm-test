// examples/dotnet/instructionmix — reporting (presentation only). Buckets the labelled stream's
// mnemonics into classes and prints a histogram + control-flow density. No tracing here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be classified.");
            return;
        }
        var stream = ww.Disassembly;
        if (stream.Count == 0) { Console.WriteLine("no labelled instructions captured."); return; }

        // Class order controls the print order; every mnemonic maps to exactly one.
        string[] order = { "data-move", "arith/logic", "compare/test", "branch", "call/ret",
                           "stack", "SIMD/FP", "other" };
        var count = new Dictionary<string, long>();
        foreach (string c in order) count[c] = 0;
        long branches = 0, calls = 0;

        bool structural = Disas.Available;
        foreach (AsmInstruction ins in stream)
        {
            string mnem = Mnemonic(ins.Text);
            // Confirm control flow structurally where Capstone is present (robust to encoding
            // variants); otherwise fall back to the mnemonic class table.
            bool isCall = structural ? Disas.IsCall(ins.Address) : mnem == "call";
            bool isRet = structural ? Disas.IsRet(ins.Address) : IsRet(mnem);
            bool isBranch = structural ? Disas.IsBranch(ins.Address) : IsJump(mnem);
            string cls;
            if (isCall || isRet) { cls = "call/ret"; if (isCall) calls++; }
            else if (isBranch) { cls = "branch"; branches++; }
            else cls = Classify(mnem);
            count[cls]++;
        }

        Console.WriteLine($"{stream.Count} labelled instructions classified"
                          + $" ({(structural ? "structurally via Disas" : "by mnemonic prefix")}).\n");
        Console.WriteLine($"  {"class",-14}{"count",10}  {"share",-8} bar");
        Console.WriteLine("  " + new string('-', 60));
        long max = 1;
        foreach (string c in order) if (count[c] > max) max = count[c];
        foreach (string c in order)
        {
            if (count[c] == 0) continue;
            double pct = 100.0 * count[c] / stream.Count;
            string bar = new string('#', (int)Math.Round(24.0 * count[c] / max));
            Console.WriteLine($"  {c,-14}{count[c],10}  {pct,6:F1}%  {bar}");
        }

        double density = 100.0 * (branches + calls) / stream.Count;
        Console.WriteLine($"\ncontrol-flow density: {branches} branches + {calls} calls "
                          + $"= {density:F1} per 100 instructions.");
        Console.WriteLine("-> the mix shows what the code DOES — mostly data movement + arithmetic, punctuated by\n"
                          + "   control flow. A high control-flow density is branchy code (harder to predict/pipeline).");
    }

    static string Mnemonic(string text)
    {
        int sp = text.IndexOf(' ');
        return sp < 0 ? text : text.Substring(0, sp);
    }

    static bool IsJump(string m) => m.Length > 0 && m[0] == 'j';
    static bool IsRet(string m) => m == "ret" || m == "retn" || m == "retf";

    // Map an x86-64 mnemonic to a coarse class. Prefix-based, deliberately simple.
    static string Classify(string m)
    {
        switch (m)
        {
            case "mov": case "movzx": case "movsx": case "movsxd": case "lea":
            case "xchg": case "cmove": case "cmovne": case "cmovl": case "cmovg":
            case "cmovle": case "cmovge": case "cmovb": case "cmova":
                return "data-move";
            case "add": case "sub": case "imul": case "mul": case "idiv": case "div":
            case "inc": case "dec": case "neg": case "and": case "or": case "xor":
            case "not": case "shl": case "shr": case "sar": case "sal": case "rol":
            case "ror": case "adc": case "sbb": case "bt": case "bts": case "btr":
                return "arith/logic";
            case "cmp": case "test":
                return "compare/test";
            case "push": case "pop": case "leave": case "enter":
                return "stack";
        }
        // SIMD / FP: the SSE/AVX family and x87 (v-prefixed, or p*/*ps/*pd/*ss/*sd forms).
        if (m.Length > 0 && (m[0] == 'v'
            || m.StartsWith("movd", StringComparison.Ordinal)
            || m.StartsWith("movq", StringComparison.Ordinal)
            || m.StartsWith("movap", StringComparison.Ordinal)
            || m.StartsWith("movup", StringComparison.Ordinal)
            || m.StartsWith("xmm", StringComparison.Ordinal)
            || m.StartsWith("punpck", StringComparison.Ordinal)
            || m.StartsWith("pxor", StringComparison.Ordinal)
            || m.StartsWith("pcmp", StringComparison.Ordinal)))
            return "SIMD/FP";
        return "other";
    }
}
