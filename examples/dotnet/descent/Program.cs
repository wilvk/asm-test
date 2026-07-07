// examples/dotnet/descent — the EXACT nested call tree, out of process, via call descent.
//
//     using var d = new Descent(DescentLevel.DescendKnown);
//     d.AllowRegion(B); d.AllowRegion(C);
//     Ptrace.TraceCallEx(code, args, trace, d, region: lenOfA);
//     // frame 0 = A ; frame 1 = B (descended) ; frame 2 = C — with self/inclusive insn counts.
//
// The callgraph example RECONSTRUCTS a tree in-process (approximate: it string/structure-walks the
// labelled stream and cannot see native runtime stubs). This is the exact one: the ptrace stepper
// single-steps a forked child and, guided by the Descent allow-set, steps INTO each callee as a
// nested FRAME — so every frame's own (self) and subtree (inclusive) instruction counts are exact,
// not inferred. A 3-level A->B->C blob (A calls B calls C). Counts only; self-skips where ptrace
// is denied (yama ptrace_scope, no privilege).

using System;
using Asmtest;

internal static class Program
{
    // A(rdi) = rdi + 4 + 2 + 1 via A -> B -> C (each adds a constant). Single exec_alloc blob;
    // A is the traced region [0, 0x0d), B and C are siblings BEYOND it (see Report for layout).
    //   A@0x00: mov rax,rdi ; call B(+5) ; add rax,1 ; ret        (region = 0x0d)
    //   B@0x0d: call C(+5)  ; add rax,2 ; ret
    //   C@0x17: add rax,4   ; ret
    static readonly byte[] BLOB =
    {
        0x48, 0x89, 0xF8,             // 0x00 mov rax, rdi   (A)
        0xE8, 0x05, 0x00, 0x00, 0x00, // 0x03 call B (-> 0x0d)
        0x48, 0x83, 0xC0, 0x01,       // 0x08 add rax, 1
        0xC3,                         // 0x0c ret            (A end, region len 0x0d)
        0xE8, 0x05, 0x00, 0x00, 0x00, // 0x0d call C (-> 0x17) (B)
        0x48, 0x83, 0xC0, 0x02,       // 0x12 add rax, 2
        0xC3,                         // 0x16 ret
        0x48, 0x83, 0xC0, 0x04,       // 0x17 add rax, 4     (C)
        0xC3,                         // 0x1b ret
    };
    const nuint RegionA = 0x0d; // A only; B and C are called siblings outside the traced region.
    const int OffB = 0x0d, LenB = 0x0a, OffC = 0x17, LenC = 0x05;

    static int Main()
    {
        Console.WriteLine("== exact nested call tree via call descent (A -> B -> C) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        var code = NativeCode.FromBytes(BLOB);
        var tr = HwTrace.Create(blocks: 64, instructions: 256);   // frame-0 (A) flat view
        long result;
        using (var d = new Descent(DescentLevel.DescendKnown))
        {
            d.SetMaxDepth(8);
            // Allow descent into B and C (their absolute extents); the region arg keeps A's own
            // body the frame-0 region so the call to B is an out-of-region descent, not recursion.
            d.AllowRegion(code.Base + OffB, (nuint)LenB);
            d.AllowRegion(code.Base + OffC, (nuint)LenC);
            result = Ptrace.TraceCallEx(code, new long[] { 100 }, tr.Handle, d, region: RegionA);
            Report.Print(code, tr, d, result);
        }
        tr.Free();
        code.Free();
        return 0;
    }
}
