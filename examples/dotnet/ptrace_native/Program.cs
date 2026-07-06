// examples/dotnet/ptrace_native — trace native code running OUT OF PROCESS.
//
//     Ptrace.TraceCall(code.Base, len, args, tr.Handle);  // forks a PTRACE_TRACEME child that
//     // runs the code while the PARENT single-steps it, filling the trace.
//
// The structural thing every in-process AsmTrace example CANNOT do: the traced code runs in a
// SEPARATE process, so this process's own thread is never armed with EFLAGS.TF — none of the
// in-process SIGTRAP perturbation. Self-contained (TraceCall forks internally: no external
// process, no perf-map, CI-runnable). The foundation the attach story (ptrace_dotnet) builds on;
// self-skips (exit 0) where ptrace is denied (yama ptrace_scope, no privilege).

using System;
using Asmtest;

internal static class Program
{
    // sum(rdi over rsi iterations) = rdi*rsi :
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
        Console.WriteLine("== out-of-process single-step of a native routine (Ptrace.TraceCall) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        var code = NativeCode.FromBytes(LOOP);
        var tr = HwTrace.Create(blocks: 64, instructions: 256);
        // The CHILD runs sum(3,5)=15 while THIS process single-steps it from outside.
        long result = Ptrace.TraceCall(code.Base, (nuint)code.Length, new long[] { 3, 5 }, tr.Handle);
        Report.Print(code, tr, result);
        tr.Free();
        code.Free();
        return 0;
    }
}
