// examples/dotnet/localscope_oop_managed — the §D3 crash-proof OUT-OF-PROCESS whole-window
// scope over a block of MANAGED C# (AsmTrace.Window).
//
//     var ww = AsmTrace.Window(() =>
//     {
//         int sq = xs.Where(v => v % 2 == 1).Select(v => v * v).Sum();   // LINQ
//         try { checked { … overflow … } } catch (OverflowException) { }  // a THROW in-window
//         …                                                              // and much more
//     });
//
// This is the out-of-process analog of examples/dotnet/localscope. Same kind of inline
// block (LINQ, generics, tuples, pattern matching, local functions, StringBuilder) — but
// traced by a reverse-attached helper child that single-steps THIS thread out of band, so
// it is NEVER armed with EFLAGS.TF. That is what makes it crash-proof: a ptrace-stop is not
// gated by the tracee's signal mask, so the block survives code the in-process localscope
// is FORBIDDEN to step. To prove it, this block deliberately includes the very thing
// localscope had to remove — an in-scope thrown/caught OverflowException — and it runs
// cleanly here (no exit 133). The block's managed methods (its own JIT'd code + the R2R BCL
// it reaches) are captured via coarse code ranges and named through the same §D0.1/§D0.2
// attribution the in-process form uses. Self-skips (runs the block uninstrumented, exit 0)
// where ptrace is denied (Yama ptrace_scope, no privilege).

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Asmtest;

internal static class Program
{
    static int Main()
    {
        Console.WriteLine("== §D3 crash-proof out-of-process MANAGED whole-window (AsmTrace.Window) ==\n");
        Console.WriteLine("a helper child single-steps THIS thread out of band — never EFLAGS.TF-armed,\n"
                          + "so the block survives the in-scope exception the in-process localscope omits.\n");

        // Results captured by the block (a closure); read after the window closes.
        int sumOfSquares = 0, aboveThreshold = 0, distinctBuckets = 0;
        long fib = 0;
        string summary = "";

        var ww = AsmTrace.Window(() =>
        {
            int[] xs = { 5, 3, 8, 1, 9, 2, 7, 4, 6 };

            // LINQ: filter -> project -> aggregate
            sumOfSquares = xs.Where(v => v % 2 == 1).Select(v => v * v).Sum();

            // tuple + deconstruction via a static local function
            (int lo, int hi) = MinMax(xs);
            int threshold = lo + 2;
            foreach (int v in xs) if (v > threshold) aboveThreshold++;

            // generic collections + foreach
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

            // pattern matching: switch expression
            int spread = hi - lo;
            string shape = spread switch { 0 => "flat", < 4 => "tight", < 8 => "spread", _ => "wide" };

            // StringBuilder + interpolation + string.Join
            var sb = new StringBuilder();
            sb.Append("evens=[").Append(string.Join(",", evens)).Append(']');
            summary = $"sumSq={sumOfSquares} range=[{lo},{hi}] {shape} "
                    + $"above={aboveThreshold} buckets={distinctBuckets} fib12={fib} {sb}";

            // The case examples/dotnet/localscope had to REMOVE: a thrown/caught exception
            // in-scope. In-process it SIGTRAPs (exit 133); out-of-process it steps cleanly.
            try
            {
                checked { int n = int.MaxValue; n += xs.Length; _ = n; }
            }
            catch (OverflowException) { summary += " (overflow caught OOP)"; }

            static (int lo, int hi) MinMax(int[] a)
            {
                int mn = int.MaxValue, mx = int.MinValue;
                foreach (int v in a) { if (v < mn) mn = v; if (v > mx) mx = v; }
                return (mn, mx);
            }
            static long Fib(int n) => n < 2 ? n : Fib(n - 1) + Fib(n - 2);
        });

        return Report.Print(ww, summary, sumOfSquares, aboveThreshold, fib) ? 0 : 1;
    }
}
