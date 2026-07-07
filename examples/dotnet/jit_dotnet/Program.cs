// Minimal .NET (CoreCLR) app for the real-runtime W2 trace lane: a hot loop calling a
// non-inlined static method, so the JIT compiles `Program::Add` once (with
// DOTNET_TieredCompilation=0) at a stable address that the tracer can resolve from the
// perf-map, run_to, and single-step. [MethodImpl(NoInlining)] keeps it a real call
// target; the int32 add allocates nothing, so the GC never runs during the trace.
//
// Run with the "bcl" argument (the dotnet-bcl lane) to instead spin on a real FRAMEWORK
// method, System.Console::WriteLine — see the loop below.
using System;
using System.IO;
using System.Runtime.CompilerServices;

class Program
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Add(int a, int b) => a + b;

    // A NON-leaf pair for the call-descent lane (examples/dotnet/descent_dotnet): Chain calls
    // Leaf twice, both NoInlining, so Chain's JIT'd body has real call-outs a descent stepper can
    // step INTO as nested frames. Only reached with the "chain" argument — the default Add loop
    // and the "bcl" loop are unchanged (the C jit_trace lanes and ptrace_dotnet still see Add).
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Leaf(int a, int b) => a * b + 1;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Chain(int a, int b) => Leaf(a, b) + Leaf(b, a);

    static void Main(string[] args)
    {
        // "chain": spin on Program::Chain (which calls Program::Leaf twice), so a tracer can
        // resolve both from the perf-map and single-step Chain INTO Leaf as a descended frame.
        if (args.Length > 0 && args[0] == "chain")
        {
            int c = 0;
            for (;;)
                c = Chain(c & 0xffff, (c + 1) & 0xffff);
        }

        // "bcl": trace a real .NET *framework* method instead of user code. Console.WriteLine
        // ships as ReadyToRun *precompiled* native, so the JIT never emits it by default; the
        // harness sets DOTNET_ReadyToRun=0 to force the whole BCL to JIT on demand, after which
        // Console::WriteLine resolves in the perf-map and single-steps like any other method.
        // Out is sinked to Stream.Null so the hot loop neither floods stdout nor pays a write(2)
        // each pass — the JITted WriteLine body we trace (it just calls Out.WriteLine) is
        // identical whatever Out points to.
        if (args.Length > 0 && args[0] == "bcl")
        {
            Console.SetOut(new StreamWriter(Stream.Null));
            for (;;)
                Console.WriteLine("x");
        }

        int s = 0;
        for (;;)
            s = Add(s, 1);
    }
}
