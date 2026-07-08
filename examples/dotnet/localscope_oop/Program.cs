// examples/dotnet/localscope_oop — the §D3 crash-proof, OUT-OF-PROCESS whole-window scope.
//
//     using var chan = new AddrChannel();          // the regions the window will call into
//     chan.Publish(leafA); chan.Publish(leafB);
//     Ptrace.TraceWindowCall(driver.Base, driver.Length, args, chan, tr.Handle);
//     // tr now holds the ABSOLUTE addresses of the driver AND both leaves, in order.
//
// The out-of-process analog of the in-process whole-window AsmTrace scope
// (examples/dotnet/localscope), and the answer to the one thing that scope CANNOT do:
// survive arbitrary code. The in-process single-step tier arms EFLAGS.TF on THIS thread,
// so a window containing a thrown exception or a pthread_create dies with SIGTRAP (the
// kernel force-resets a masked synchronous #DB to SIG_DFL — exit 133). Here the stepper
// is a SEPARATE process: the #DB is delivered to the tracer via waitpid, and a ptrace-stop
// is NOT gated by the tracee's signal mask — so the traced code may block SIGTRAP freely
// and never dies. This is the §D3 whole-window channel: the stepper cannot see the
// runtime's own JIT events, so the caller PUBLISHES the (base,len) of every region the
// window calls into on an AddrChannel; the stepper records those and steps over the rest.
//
// Fixture (native, CI-runnable, no managed runtime in the child): a DRIVER frame that calls
// two leaf "methods" at their own mappings; the window captures the driver AND both
// published leaves, in execution order, across the process boundary. The SAME primitive a
// managed binding uses to trace a whole block of managed code out-of-process. Self-skips
// (exit 0) where ptrace is denied (yama ptrace_scope, no privilege).

using System;
using Asmtest;

internal static class Program
{
    // Leaf A: rax = rdi + rsi   (mov rax,rdi; add rax,rsi; ret)
    static readonly byte[] LEAF_A = { 0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3 };
    // Leaf B: rax = rdi - rsi   (mov rax,rdi; sub rax,rsi; ret)
    static readonly byte[] LEAF_B = { 0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3 };

    static int Main()
    {
        Console.WriteLine("== §D3 crash-proof out-of-process whole-window scope (Ptrace.TraceWindowCall) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        // The two "JIT'd methods" the window calls into, at their own executable mappings.
        var leafA = NativeCode.FromBytes(LEAF_A);
        var leafB = NativeCode.FromBytes(LEAF_B);

        // The window FRAME: a driver that calls leaf A then leaf B (absolute indirect calls),
        // built once the leaves' addresses are known — the leaves inherit the frame's rdi/rsi.
        //   movabs rax,&A ; call rax ; movabs rax,&B ; call rax ; ret
        long a = leafA.Base.ToInt64(), b = leafB.Base.ToInt64();
        var drv = new byte[25]
        {
            0x48, 0xB8, 0,0,0,0,0,0,0,0,   // 0x00 movabs rax, &A
            0xFF, 0xD0,                    // 0x0a call rax
            0x48, 0xB8, 0,0,0,0,0,0,0,0,   // 0x0c movabs rax, &B
            0xFF, 0xD0,                    // 0x16 call rax
            0xC3,                          // 0x18 ret
        };
        BitConverter.GetBytes(a).CopyTo(drv, 2);
        BitConverter.GetBytes(b).CopyTo(drv, 14);
        var driver = NativeCode.FromBytes(drv);

        // Publish the regions the window will call into — the stepper (a separate process)
        // learns the JIT addresses only through this channel.
        using var chan = new AddrChannel();
        chan.Publish(leafA);
        chan.Publish(leafB);

        var tr = HwTrace.Create(blocks: 64, instructions: 256);
        // The whole window — driver + both leaves — is captured OUT OF PROCESS. args (7,3)
        // flow into the leaves: A=7+3=10, B=7-3=4; the frame returns B's value.
        long result = Ptrace.TraceWindowCall(driver.Base, (nuint)driver.Length,
                                             new long[] { 7, 3 }, chan, tr.Handle);

        int ok = Report.Print(tr, driver, leafA, leafB, result) ? 0 : 1;
        tr.Free();
        driver.Free();
        leafA.Free();
        leafB.Free();
        return ok;
    }
}
