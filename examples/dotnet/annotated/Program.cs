// examples/dotnet/annotated — an ANNOTATED EXECUTION TRACE: each executed instruction in
// one column, the method it belongs to in the next. A byMethod+withRundown whole-window
// scope records every stepped instruction (absolute RIP) in execution order; the binding
// disassembles the ones it can NAME (live, on close) and pairs each with its method:
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true)) Work();
//     foreach (AsmInstruction i in ww.Disassembly)   // labelled insns, execution order
//         print(i.Text, i.ShortMethod);              // "mov rax, rdi"  ->  Program.Work
//
// ww.Disassembly holds the FULL labelled stream (~1200 insns for this workload). To keep the
// listing readable this demo prints only the first few instructions of each contiguous
// method run — so you see the whole control flow, Work -> string.Join -> the Console.WriteLine
// descent (Console::WriteLine -> StreamWriter::WriteLine -> ConsolePal::Write -> Sys::Write),
// rather than a flat prefix that never leaves Work's prologue. The unlabelled native-runtime
// instructions (RyuJIT/GC/PAL) between named runs are elided, with a count. Single-step WEAK
// tier; self-skips cleanly where single-step / Capstone is unavailable.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A small workload spanning several methods + assemblies: string.Join + array
    // (System.Private.CoreLib) then the Console write path (System.Console + CoreLib).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    const int PerRun = 3;   // instructions shown per contiguous method run
    const int MaxRows = 90; // total instruction rows printed (the binding holds them all)

    static int Main()
    {
        Console.WriteLine("== annotated execution trace: each instruction next to its method ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default\n"
                          + "(the STRONG Intel-PT / CEILING AMD-LBR tiers are forward-look)");

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
            Work();
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }
        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}\n");

        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be disassembled.");
            return 0;
        }

        long runtime = ww.Addresses.Length - ww.LabelledInstructions;
        Console.WriteLine($"captured {ww.Addresses.Length} instructions; {ww.LabelledInstructions} labelled "
                          + $"by method across {ww.Methods.Count} methods");
        Console.WriteLine($"({runtime} native-runtime instructions elided — RyuJIT/GC/PAL, unnamed).\n");

        // The scope arms mid-constructor, so the first labelled instructions are the tracer's
        // own plumbing (AsmTrace::.ctor / set_Armed / the begin_window P/Invoke stub). Start at
        // the workload (Program.Work) so the listing showcases the user code + the BCL it calls;
        // that suffix is contiguous (Dispose runs AFTER the window closes), so RuntimeBefore
        // stays exact. The binding still holds the full labelled stream in ww.Disassembly.
        var all = ww.Disassembly;
        int start = 0;
        while (start < all.Count && !all[start].Method.Contains("Program.Work")) start++;
        if (start >= all.Count) start = 0; // workload not found — show from the top

        Console.WriteLine($"  {"address",-14}  {"instruction",-38}  method");
        Console.WriteLine($"  {new string('-', 14)}  {new string('-', 38)}  {new string('-', 34)}");
        if (start > 0)
            Console.WriteLine($"  (skipped {start} labelled tracer-setup instructions before the workload)");

        // Walk the labelled stream in execution order; print up to PerRun instructions of each
        // contiguous same-method run (with a "+N more" note), and the native-runtime gap that
        // preceded each shown instruction. This keeps the whole flow on screen instead of a
        // flat prefix — so the Console.WriteLine descent (deep past string.Join) actually shows.
        string cur = null, curShort = "";
        int runLen = 0, elided = 0, rows = 0, consumed = 0;
        for (int k = start; k < all.Count && rows < MaxRows; k++, consumed++)
        {
            AsmInstruction i = all[k];
            if (i.Method != cur) // entered a new method run
            {
                if (elided > 0) Console.WriteLine($"  {"",-14}  ... +{elided} more in {curShort} ...");
                cur = i.Method; curShort = Clip(i.ShortMethod, 60); runLen = 0; elided = 0;
            }
            if (runLen < PerRun)
            {
                if (i.RuntimeBefore > 0)
                    Console.WriteLine($"  {"",-14}  ... {i.RuntimeBefore} native-runtime insns ...");
                Console.WriteLine($"  0x{i.Address,-12:x}  {Clip(i.Text, 38),-38}  {Clip(i.ShortMethod, 60)}");
                rows++;
            }
            else elided++;
            runLen++;
        }
        if (elided > 0) Console.WriteLine($"  {"",-14}  ... +{elided} more in {curShort} ...");
        int left = (all.Count - start) - consumed;
        if (left > 0)
            Console.WriteLine($"\n  (stopped after {rows} rows; {left} more labelled instructions in ww.Disassembly)");
        Console.WriteLine("\n-> each executed instruction is shown next to the method it ran in;\n"
                          + "   the unnamed native-runtime instructions between the named runs are elided.");
        return 0;
    }

    static string Clip(string s, int n) => s.Length <= n ? s : s.Substring(0, n - 1) + "…";
}
