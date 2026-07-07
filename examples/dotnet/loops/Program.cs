// examples/dotnet/loops — per-loop trip counts from backedges.
//
//     tr.Region("r", () => code.Call(m, n));
//     // a BACKEDGE is a taken branch to a LOWER offset; count each = that loop's trip count.
//
// A static CFG shows a loop exists; only the DYNAMIC trace shows how many times it ran. This
// walks the ordered InsnOffsets() stream: any consecutive (from -> to) pair with to < from is a
// taken backedge — the loop's back-branch firing once per iteration. A NESTED loop (outer m,
// inner n) has two backedges; the inner one fires m*n times, the outer m. Counts only;
// region-scoped single-step, deterministic and CI-runnable.

using System;
using Asmtest;

internal static class Program
{
    // nested(m, n): acc = m * n via an outer loop (rdi=m) over an inner loop (rsi=n). All short
    // jumps; two backedges — inner jmp 0x1a->0x0e, outer jmp 0x1f->0x06 (see Report for layout).
    static readonly byte[] NESTED =
    {
        0x48, 0x31, 0xC0,       // 0x00 xor rax, rax
        0x48, 0x89, 0xF9,       // 0x03 mov rcx, rdi   (outer counter)
        0x48, 0x85, 0xC9,       // 0x06 test rcx, rcx  (OUTER_TOP)
        0x74, 0x16,             // 0x09 jz end (-> 0x21)
        0x48, 0x89, 0xF2,       // 0x0b mov rdx, rsi   (inner counter)
        0x48, 0x85, 0xD2,       // 0x0e test rdx, rdx  (INNER_TOP)
        0x74, 0x09,             // 0x11 jz inner_done (-> 0x1c)
        0x48, 0x83, 0xC0, 0x01, // 0x13 add rax, 1
        0x48, 0xFF, 0xCA,       // 0x17 dec rdx
        0xEB, 0xF2,             // 0x1a jmp inner_top (-> 0x0e)
        0x48, 0xFF, 0xC9,       // 0x1c dec rcx        (INNER_DONE)
        0xEB, 0xE5,             // 0x1f jmp outer_top (-> 0x06)
        0xC3,                   // 0x21 ret            (END)
    };

    static int Main()
    {
        Console.WriteLine("== loops: per-loop trip counts from backedges (a nested loop) ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);

        var code = NativeCode.FromBytes(NESTED);
        const long m = 3, n = 4;
        var tr = HwTrace.Create(blocks: 32, instructions: 512);
        tr.Register("nested", code);
        long result = 0;
        tr.Region("nested", () => result = code.Call(m, n));
        Report.Print(code, tr, m, n, result);
        tr.Free();
        code.Free();
        return 0;
    }
}
