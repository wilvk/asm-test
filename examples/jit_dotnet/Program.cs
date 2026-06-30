// Minimal .NET (CoreCLR) app for the real-runtime W2 trace lane: a hot loop calling a
// non-inlined static method, so the JIT compiles `Program::Add` once (with
// DOTNET_TieredCompilation=0) at a stable address that the tracer can resolve from the
// perf-map, run_to, and single-step. [MethodImpl(NoInlining)] keeps it a real call
// target; the int32 add allocates nothing, so the GC never runs during the trace.
using System.Runtime.CompilerServices;

class Program
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Add(int a, int b) => a + b;

    static void Main()
    {
        int s = 0;
        for (;;)
            s = Add(s, 1);
    }
}
