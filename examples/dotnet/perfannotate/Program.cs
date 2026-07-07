// examples/dotnet/perfannotate — `perf annotate`, instruction-EXACT (not sampled).
//
//     tr.Region("r", () => code.Call(n, 0));
//     foreach (ulong off in tr.InsnOffsets()) ...   // the full ordered execution stream
//
// perf annotate shows source/asm lines with a sampled execution-count heat column. Region-scoped
// single-step gives the SAME view but EXACT: every instruction of a native routine with its true
// execution count and a proportional bar. A branchy loop (add only even i) makes the counts vary
// WITHIN the body — the conditional-guarded add runs half as often as the loop overhead. Counts
// only; deterministic, zero runtime noise, CI-runnable.

using System;
using Asmtest;

internal static class Program
{
    // count_even(n): acc = sum of even i in [0, n). Hand-assembled x86-64 with an in-loop
    // branch so the guarded add runs fewer times than the loop overhead (see Report for layout).
    //   0x00 xor rax,rax ; 0x03 xor rcx,rcx ; 0x06 cmp rcx,rdi ; 0x09 jge end
    //   0x0b test rcx,1  ; 0x12 jne odd     ; 0x14 add rax,rcx ; 0x17 inc rcx
    //   0x1a jmp top     ; 0x1c ret
    static readonly byte[] COUNT_EVEN =
    {
        0x48, 0x31, 0xC0,                         // 0x00 xor rax, rax
        0x48, 0x31, 0xC9,                         // 0x03 xor rcx, rcx
        0x48, 0x39, 0xF9,                         // 0x06 cmp rcx, rdi   (TOP)
        0x7D, 0x11,                               // 0x09 jge end (-> 0x1c)
        0x48, 0xF7, 0xC1, 0x01, 0x00, 0x00, 0x00, // 0x0b test rcx, 1
        0x75, 0x03,                               // 0x12 jne odd (-> 0x17)
        0x48, 0x01, 0xC8,                         // 0x14 add rax, rcx   (even only)
        0x48, 0xFF, 0xC1,                         // 0x17 inc rcx        (ODD)
        0xEB, 0xEA,                               // 0x1a jmp top (-> 0x06)
        0xC3,                                     // 0x1c ret            (END)
    };

    static int Main()
    {
        Console.WriteLine("== perf annotate, instruction-exact: per-instruction execution counts ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);

        var code = NativeCode.FromBytes(COUNT_EVEN);
        const long n = 10;
        var tr = HwTrace.Create(blocks: 32, instructions: 512);
        tr.Register("count_even", code);
        long result = 0;
        tr.Region("count_even", () => result = code.Call(n, 0));
        Report.Print(code, tr, n, result);
        tr.Free();
        code.Free();
        return 0;
    }
}
