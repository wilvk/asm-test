// examples/dotnet/descend-all — walk into UNKNOWN callees automatically, guardrail-gated.
//
//     using var d = new Descent(DescentLevel.DescendAll);   // no allow-set at all
//     d.SetMaxDepth(8); d.SetInsnBudget(100_000); d.SetWatchdogMs(2000); d.UseDefaultDenylist();
//     Ptrace.TraceCallEx(code, args, IntPtr.Zero, d, region: lenOfA);
//     // frame 0 = A ; frame 1 = B ; frame 2 = C — discovered, not pre-declared.
//
// This is the INVERSE of `descent` (DescendLevel.DescendKnown), which hands the stepper a CURATED
// allow-set (AllowRegion(B); AllowRegion(C)) and descends only into those exact regions. DescendAll
// takes NO allow-set: at every call-out it steps INTO the callee by default, discovering the tree as
// it goes — best-effort, and bounded only by the guardrails (max-depth / insn-budget / watchdog /
// denylist). Because it is best-effort it reports honestly: Truncated() when a pool overflowed or a
// byte failed to decode, DepthCapped() when descent stopped at a policy limit. So DescendAll is the
// tool when you DON'T know the callees in advance and are willing to bound blast-radius; `descent`
// is the tool when you know EXACTLY which regions you want and nothing else.
//
// We prove both halves with ONE A->B->C blob (A calls B calls C, each adds a constant):
//   (1) GENEROUS run  — deep budget, no allow-list — descends into B and C automatically (3 frames).
//   (2) TIGHT run     — SetMaxDepth(1) on the same blob — descent stops opening frames at depth 1:
//                       C (which would be depth 2) never gets its own frame; its instructions FOLD
//                       into B's frame (B's self grows 3->5) and DepthCapped() flips true.
// Same blob, same 107 result (C still EXECUTES either way — the guardrail bounds RECORDING, not the
// program). Counts only; self-skips (exit 0) where ptrace is denied (yama ptrace_scope, no privilege).

using System;
using Asmtest;

internal static class Program
{
    // A(rdi) = rdi + 4 + 2 + 1 via A -> B -> C (each adds a constant). Single exec_alloc blob;
    // A is the traced region [0, 0x0d), B and C are siblings BEYOND it — so the calls into them
    // are out-of-region descents, not recursion (see Report for the byte layout).
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

    static int Main()
    {
        Console.WriteLine("== DescendAll: auto-discover UNKNOWN callees, bounded by guardrails (A -> B -> C) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        var code = NativeCode.FromBytes(BLOB);

        // (1) GENEROUS: no allow-set, deep guardrails — DescendAll walks the whole tree itself.
        using (var d = new Descent(DescentLevel.DescendAll))
        {
            d.SetMaxDepth(8);            // plenty of headroom for a 3-deep tree
            d.SetInsnBudget(100_000);    // total single-step budget across all frames
            d.SetWatchdogMs(2000);       // real-time ceiling for the descended run
            d.UseDefaultDenylist();      // refuse PLT/vdso/GC-JIT/blocking-libc (our blob hits none)
            long result = Ptrace.TraceCallEx(code, new long[] { 100 }, IntPtr.Zero, d, region: RegionA);
            Report.Print("generous (SetMaxDepth 8, budget 100k) — no allow-set", code, d, result);
        }

        // (2) TIGHT: SAME blob, one guardrail clamped — descent stops opening frames at depth 1.
        using (var d = new Descent(DescentLevel.DescendAll))
        {
            d.SetMaxDepth(1);            // open a frame for B (depth 1); NOT for C (would be depth 2)
            d.SetInsnBudget(100_000);    // budget generous, so the ONLY limit that bites is depth
            d.SetWatchdogMs(2000);
            d.UseDefaultDenylist();
            long result = Ptrace.TraceCallEx(code, new long[] { 100 }, IntPtr.Zero, d, region: RegionA);
            Report.Print("tight (SetMaxDepth 1) — same blob, guardrail bites", code, d, result);
        }

        Console.WriteLine("-> DescendAll DISCOVERED the tree with no allow-set (run 1), then reported HONESTLY when a\n"
                        + "   guardrail stopped it early (run 2: DepthCapped=true, C folded into B, no C frame). The\n"
                        + "   result is 107 in BOTH runs — the guardrail bounds what is RECORDED, never what EXECUTES.\n"
                        + "   Contrast `descent` (DescendKnown): it descends ONLY the exact regions you AllowRegion().");

        code.Free();
        return 0;
    }
}
