// examples/dotnet/methods — label a whole-window capture by MANAGED METHOD (§D0.1).
//
//     using AsmTest;
//     using var map = new JitMethodMap();   // enable BEFORE the method JITs
//     using (new AsmTrace())                // empty ctor: zero config
//     {
//         ColdPath(data);                   // an arbitrary, COLD managed method
//     }                                     // its executed asm is captured …
//     // … then map.Resolve() labels the captured addresses by method name.
//
// This is the buildable half of the zero-config plan's §Z3 that needs NO Intel PT:
// an in-process EventListener consumes CoreCLR MethodLoadVerbose events to map JIT
// addresses -> method names, so the empty scope's captured window is broken down by
// managed method — turning "N runtime instructions" into a per-method view that
// identifies the arbitrary method BY NAME. Runs via the single-step WEAK tier: honest
// but intrusive (it also single-steps the JIT compiling the cold method). The clean,
// non-intrusive path is the STRONG whole-window PT tier (forward-look on this AMD host).

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Asmtest;

internal static class Program
{
    // A genuinely COLD managed method — JIT'd on its first call, inside the scope.
    // NoInlining so it has its own JIT'd body with a resolvable address range.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static long ColdPath(long a, long n)
    {
        long s = 0;
        for (long i = 0; i < n; i++) s += a * (i + 1);
        return s;
    }

    static int Main()
    {
        Console.WriteLine("== labelling a cold managed method in a whole-window scope (§D0.1) ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        // §D0.1: enable the method map BEFORE ColdPath JITs (an in-proc listener sees
        // only methods JIT'd after it is enabled).
        using var map = new JitMethodMap();

        long r = 0;
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false))
        {
            r = ColdPath(7, 100);   // COLD: JIT compiles it here, then the body runs
        }
        // Stop ingesting now so the map holds the methods seen while the scope was open,
        // not the classification code's own post-scope JIT; snapshot the count before it.
        map.Stop();
        int observed = map.Count;
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return 0;
        }
        map.Freeze();

        // Classify every captured address by managed method (else the native runtime).
        var by = new Dictionary<string, long>();
        long labelled = 0;
        foreach (ulong ip in ww.Addresses)
        {
            string m = map.Resolve(ip);
            if (m != null) labelled++;
            string key = m ?? "[native runtime]";
            by.TryGetValue(key, out long c);
            by[key] = c + 1;
        }

        Console.WriteLine($"ColdPath(7,100) = {r}; captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "")
                          + $"; {observed} methods observed by the JIT map; {labelled} instructions labelled by method.\n");

        Console.WriteLine("top managed methods that executed in the window (by instruction count):");
        var top = new List<KeyValuePair<string, long>>(by);
        top.Sort((x, y) => y.Value.CompareTo(x.Value));
        int shown = 0;
        foreach (var kv in top)
        {
            if (kv.Key == "[native runtime]") continue;
            Console.WriteLine($"    {kv.Value,8}  {kv.Key}");
            if (++shown >= 10) break;
        }

        long cold = 0;
        foreach (var kv in by) if (kv.Key.Contains("ColdPath")) cold += kv.Value;
        Console.WriteLine();
        if (cold > 0)
            Console.WriteLine($"-> the arbitrary COLD method 'ColdPath' is identified BY NAME: {cold} instructions.");
        else
            Console.WriteLine("-> ColdPath's body ran past the captured window (its own JIT compilation filled\n"
                              + "   it); the methods above are what surfaced. That is the honest single-step\n"
                              + "   limit — the STRONG PT tier (non-intrusive, filtered at decode) is the clean path.");

        by.TryGetValue("[native runtime]", out long native);
        Console.WriteLine($"\n(the native runtime — RyuJIT, GC, PAL — is the unlabelled remainder: {native} instructions.)");
        return 0;
    }
}
