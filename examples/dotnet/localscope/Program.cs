// examples/dotnet/localscope — a whole-window scope wrapped around a BODY of inline C#,
// with NO helper method to call.
//
//     using (var ww = new AsmTrace(byMethod: true, withRundown: true))
//     {
//         int[] xs = { 5, 3, 8, ... };
//         int sq = xs.Where(v => v % 2 == 1).Select(v => v * v).Sum();  // LINQ
//         (int lo, int hi) = MinMax(xs);                                // tuple + local fn
//         string shape = spread switch { 0 => "flat", < 4 => "tight", ... };
//         ...                                                           // and much more
//     }   // Dispose renders whatever the calling thread executed in the block
//
// Unlike the sibling examples (rundown/, methods/), the traced work is NOT a single
// NoInlining Work() call — it is a whole block of ordinary C# exercising a spread of
// language features (LINQ filter/project/aggregate, lambdas + a captured closure, tuples +
// deconstruction, pattern-matching switch expressions, static + recursive local functions,
// generic Dictionary/List + foreach, StringBuilder + interpolation, null-coalescing). The
// §D0.1 byMethod map + §D0.2 withRundown rundown name the managed methods that surface in
// the captured window — the report shows the block's OWN compiled code (Program::Main, the
// lambda display class, delegate construction) named, proving the inline block itself (not a
// separate method) is the traced unit.
//
// A whole-window scope captures EVERYTHING the thread runs between the ctor and Dispose
// (single-step WEAK tier) — ~1M runtime instructions per arm, of which only a slice resolves
// to a managed name; Truncated is expected and handled. The point is that a rich inline block
// is traced and its own code attributed, NOT a tidy per-feature BCL count (see methods/ /
// descent/ for tight single-body attribution). Self-skips cleanly (exit 0) where single-step
// / diagnostics are unavailable.
//
// NOTE: this block deliberately stays clear of two features that destabilize the portable
// managed single-step tier when combined with the rest of the block — a thrown/caught
// exception (CLR two-pass dispatch under EFLAGS.TF) and a LINQ aggregate over a *capturing*
// closure — both of which reproduced a SIGTRAP (exit 133) here. That fragility is the
// documented "managed single-step posture" open question, not a property of this example.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Asmtest;

internal static class Program
{
    static int Main()
    {
        Console.WriteLine("== localscope: a whole block of inline C# as the traced window (no Work() method) ==\n");

        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default,\n"
                          + "auto-inited by the AsmTrace ctor; byMethod (§D0.1) + withRundown (§D0.2)\n"
                          + "name the managed methods in the window — including the block's own code.\n");

        // Declared BEFORE the scope so the results outlive Dispose AND so the JIT cannot
        // elide the block as dead (Report reads them, and Console.WriteLine consumes them).
        int sumOfSquares = 0, aboveThreshold = 0, distinctBuckets = 0;
        long fib = 0;
        string summary = "";

        AsmTrace ww;
        using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
        {
            int[] xs = { 5, 3, 8, 1, 9, 2, 7, 4, 6 };

            // LINQ: filter -> project -> aggregate (lambdas, lazy iterators)
            sumOfSquares = xs.Where(v => v % 2 == 1).Select(v => v * v).Sum();

            // tuple + deconstruction via a STATIC local function
            (int lo, int hi) = MinMax(xs);
            int threshold = lo + 2;
            foreach (int v in xs) if (v > threshold) aboveThreshold++;

            // generic collections + foreach + enumerator
            var buckets = new Dictionary<int, int>();
            var evens = new List<int>();
            foreach (int v in xs)
            {
                buckets[v % 3] = buckets.TryGetValue(v % 3, out int c) ? c + 1 : 1;
                if (v % 2 == 0) evens.Add(v);
            }
            distinctBuckets = buckets.Count;

            // recursion via a static local function
            fib = Fib(12);

            // pattern matching: switch EXPRESSION with relational patterns
            int spread = hi - lo;
            string shape = spread switch { 0 => "flat", < 4 => "tight", < 8 => "spread", _ => "wide" };

            // StringBuilder + interpolation + string.Join over a generic list
            var sb = new StringBuilder();
            sb.Append("evens=[").Append(string.Join(",", evens)).Append(']');
            summary = $"sumSq={sumOfSquares} range=[{lo},{hi}] {shape} "
                    + $"above={aboveThreshold} buckets={distinctBuckets} fib12={fib} {sb}";

            static (int lo, int hi) MinMax(int[] a)
            {
                int mn = int.MaxValue, mx = int.MinValue;
                foreach (int v in a) { if (v < mn) mn = v; if (v > mx) mx = v; }
                return (mn, mx);
            }
            static long Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);
        }

        return Report.Print(ww, summary, sumOfSquares, aboveThreshold, fib) ? 0 : 1;
    }
}
