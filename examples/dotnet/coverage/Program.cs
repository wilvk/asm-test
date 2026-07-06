// examples/dotnet/coverage — BASIC-BLOCK coverage of a branchy native routine, accumulated
// across inputs (the HwTrace block-coverage API that no other example touches).
//
//     var tr = HwTrace.Create(blocks: N); tr.Register("r", code); tr.Region("r", () => code.Call(x));
//     foreach (ulong b in tr.BlockOffsets()) ...   // the blocks input x executed
//
// Different inputs take different branches -> cover different basic blocks. Running a "test
// suite" of positive inputs leaves the negative-path block uncovered — the classic missing-test
// case, made exact (single-step, region-scoped: deterministic, zero runtime noise, CI-runnable).

using System;
using Asmtest;

internal static class Program
{
    // classify(x): 1 if x<0, 2 if 0<=x<100, 3 if x>=100. Hand-assembled x86-64 so different
    // inputs take different branches (see Report for the block layout); Report self-checks the
    // return values decode as intended.
    //   0: cmp rdi,0 ; jl .neg(0x1e) ; cmp rdi,100 ; jl .small(0x15)
    //   c: mov rax,3 ; jmp .end   (.big)   15: mov rax,2 ; jmp .end  (.small)
    //  1e: mov rax,1 (.neg)       25: ret  (.end)
    static readonly byte[] CLASSIFY =
    {
        0x48, 0x83, 0xFF, 0x00,               // cmp rdi, 0
        0x7C, 0x18,                           // jl  .neg  (-> 0x1e)
        0x48, 0x83, 0xFF, 0x64,               // cmp rdi, 100
        0x7C, 0x09,                           // jl  .small(-> 0x15)
        0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00, // mov rax, 3   (.big)
        0xEB, 0x10,                           // jmp .end  (-> 0x25)
        0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00, // mov rax, 2   (.small)
        0xEB, 0x07,                           // jmp .end  (-> 0x25)
        0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00, // mov rax, 1   (.neg)
        0xC3,                                 // ret          (.end)
    };

    static int Main()
    {
        Console.WriteLine("== basic-block coverage of a branchy native routine ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);

        var code = NativeCode.FromBytes(CLASSIFY);
        Report.Print(code);
        code.Free();
        return 0;
    }
}
