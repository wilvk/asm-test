// examples/dotnet/trace-diff — the EXACT trace DELTA between two runs of one native routine:
// which basic blocks / instructions a change (here, a different input) turned ON or OFF.
//
//     var A = HwTrace.Create(...); A.Register("runA", code); A.Region("runA", () => code.Call(20, 22));
//     var B = HwTrace.Create(...); B.Register("runB", code); B.Region("runB", () => code.Call(60, 60));
//     // set-diff A.BlockOffsets() vs B.BlockOffsets()  ->  ON-in-B, OFF-in-B, common
//
// Distinct from its two neighbours:
//   * coverage  UNIONs many inputs into one covered set ("did the suite reach this block?").
//   * codeimage compares two DIFFERENT code bodies at one address (a recompile).
//   * trace-diff DIFFS run A against run B of the SAME body — the regression/coverage delta:
//     exactly the blocks and per-instruction execution counts that changed between them.
//
// One HwTrace recorder per run (registering the SAME NativeCode under two names), single-step
// region-scoped so each trace is deterministic and noise-free. Runs live via the single-step
// WEAK tier; self-skips (exit 0) cleanly where it can't run.

using System;
using Asmtest;

internal static class Program
{
    // add2(a,b)=a+b, clamped so a+b>100 costs one extra instruction:
    //   00: mov rax,rdi ; 03: add rax,rsi ; 06: cmp rax,100 ; 0c: jle +3 ; 0e: dec rax ; 11: ret
    // The jle at 0x0c falls through into the `dec` block only when a+b>100 — so that block is
    // reached by run B (120) and skipped by run A (42). Report self-checks the return values.
    static readonly byte[] ADD2 =
    {
        0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64,
        0x00, 0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3,
    };

    static int Main()
    {
        Console.WriteLine("== trace DELTA between two runs of one native routine ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);

        var code = NativeCode.FromBytes(ADD2);
        code.Call(20, 22); // warm the call thunk before either window
        Report.Print(code);
        code.Free();
        return 0;
    }
}
