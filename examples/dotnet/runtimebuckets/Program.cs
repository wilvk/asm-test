// examples/dotnet/runtimebuckets — name the native-runtime lump by MODULE.
//
//     using (var ww = new AsmTrace()) Work();
//     HwTrace.SymbolizeBuckets(ww.Addresses);   // -> per-module counts (libcoreclr, [anon JIT]…)
//
// The whole-window scope captures ~1M runtime instructions; the amplification example counts
// them, this one NAMES them. SymbolizeBuckets P/Invokes asmtest_hwtrace_symbolize_bucket, which
// resolves each ABSOLUTE address to its perf-map JIT symbol or mapped-file region (libcoreclr.so,
// libclrjit.so, the JIT's own [anon] code) — the surviving post-close primitive (§3.1c), so it
// runs AFTER the scope closed, on the retained Addresses. Counts only; the ctor auto-inits the
// single-step tier.

using System;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Work()
    {
        string joined = string.Join(",", new[] { "alpha", "beta", "gamma" });
        Console.WriteLine(joined);
    }

    static int Main()
    {
        Console.WriteLine("== runtime buckets: the ~1M-instruction runtime lump, named by module ==\n");
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false))   // whole-window: retains the raw Addresses
            Work();
        Report.Print(ww);
        return 0;
    }
}
