// examples/dotnet/blockstep — the SAME out-of-process trace, at ~1 stop per branch.
//
//     Ptrace.TraceCall(...)           // one tracer round-trip per INSTRUCTION
//     Ptrace.TraceCallBlockstep(...)  // one per TAKEN BRANCH (PTRACE_SINGLEBLOCK)
//
// Block-step drives the CPU's BTF (branch-trap flag): the debug exception fires once per
// taken branch instead of once per instruction, so a hot loop costs a tracer stop per
// ITERATION rather than per instruction in the body — 4-10x fewer stops on compute
// kernels. It reconstructs the byte-identical asmtest_trace_t (each block's straight-line
// run is disassembled between branch targets), and it is the ONLY exact real-CPU capture
// on Zen 2 (no branch-record hardware) and rootless everywhere (no CAP_PERFMON). This demo
// traces a real loop BOTH ways and shows the streams are identical while the stop count
// drops. Self-skips (exit 0) where ptrace / PTRACE_SINGLEBLOCK is unavailable.

using System;
using Asmtest;

internal static class Program
{
    // sum(rdi over rsi iterations) = rdi*rsi:
    //   mov rax,0 ; L: add rax,rdi ; dec rsi ; jnz L ; ret   (a real loop with a back-edge)
    static readonly byte[] LOOP =
    {
        0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,  // 0x0:  mov rax, 0
        0x48, 0x01, 0xF8,                          // 0x7:  L: add rax, rdi
        0x48, 0xFF, 0xCE,                          // 0xa:  dec rsi
        0x75, 0xF8,                                // 0xd:  jnz L
        0xC3,                                      // 0xf:  ret
    };

    static int Main()
    {
        Console.WriteLine("== out-of-process BTF block-step (Ptrace.TraceCallBlockstep) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }
        if (!Ptrace.BlockstepAvailable())
        {
            Console.WriteLine("# self-skip: PTRACE_SINGLEBLOCK block-step unavailable on this host");
            return 0;
        }

        var code = NativeCode.FromBytes(LOOP);
        const int trips = 20; // 20 loop iterations => 19 taken back-edges

        // Single-step: one tracer stop per executed instruction.
        var ss = HwTrace.Create(blocks: 64, instructions: 256);
        long r1 = Ptrace.TraceCall(code.Base, (nuint)code.Length, new long[] { 1, trips }, ss.Handle);

        // Block-step: one tracer stop per taken branch — the SAME reconstructed trace.
        var bs = HwTrace.Create(blocks: 64, instructions: 256);
        long r2 = Ptrace.TraceCallBlockstep(code.Base, (nuint)code.Length, new long[] { 1, trips }, bs.Handle);

        Report.Print(code, ss, bs, r1, r2, trips);
        ss.Free();
        bs.Free();
        code.Free();
        return 0;
    }
}
