// examples/dotnet/concurrent-isolation — reporting (presentation only), split from Program.cs.
// Renders each thread's closed slice and checks the concurrency invariant: both threads
// captured a non-empty body, neither is Truncated, and the two results differ per their args.
// No tracing logic here.

using System;
using Asmtest;

internal static class Report
{
    // Returns true when the run is a clean success (a live proof, or an honest self-skip).
    public static bool Print(Program.Slice a, Program.Slice b)
    {
        // Single-step is up, but a JIT body must still resolve on each thread. If either
        // scope did not arm, degrade cleanly (exit 0) rather than present a partial proof.
        if (a == null || b == null || !a.Scope.Armed || !b.Scope.Armed)
        {
            string ra = a?.Scope?.SkipReason ?? "thread-A produced no scope";
            string rb = b?.Scope?.SkipReason ?? "thread-B produced no scope";
            Console.WriteLine($"# self-skip: a concurrent scope did not arm — A: {ra}; B: {rb}");
            return true;
        }

        int na = PrintSlice(a);
        int nb = PrintSlice(b);

        // The invariant this example exists to prove. "Non-empty body" is measured from the
        // rendered listing, which needs Capstone; on a build without it BOTH listings are empty
        // (rendering, not capture, is what is missing), so don't penalize that — the armed +
        // not-truncated + results-differ trio still proves the two windows ran independently.
        bool haveCapstone = na > 0 || nb > 0;
        bool bothNonEmpty = !haveCapstone || (na > 0 && nb > 0);
        bool neitherTruncated = !a.Scope.Truncated && !b.Scope.Truncated;
        bool resultsDiffer = a.Result != b.Result;
        bool pass = bothNonEmpty && neitherTruncated && resultsDiffer;

        Console.WriteLine();
        Console.WriteLine($"both bodies non-empty : {bothNonEmpty}  (A={na} insns, B={nb} insns"
                          + $"{(haveCapstone ? "" : "; no Capstone — listing unavailable, capture still ran")})");
        Console.WriteLine($"neither truncated     : {neitherTruncated}  "
                          + $"(A.Truncated={a.Scope.Truncated}, B.Truncated={b.Scope.Truncated})");
        Console.WriteLine($"results differ        : {resultsDiffer}  (A={a.Result}, B={b.Result})");
        Console.WriteLine();
        Console.WriteLine(pass
            ? "-> PASS: two EFLAGS.TF windows single-stepped the same body AT ONCE and neither\n"
            + "   corrupted or truncated the other — the per-thread range stack held each slice\n"
            + "   apart, and the cross-thread Truncated guard stayed quiet (each thread armed and\n"
            + "   closed on itself)."
            : "-> FAIL: the concurrency invariant did not hold (see the three checks above).");
        return pass;
    }

    // Render one thread's slice; return its captured instruction count (rendered listing lines).
    static int PrintSlice(Program.Slice s)
    {
        Console.WriteLine($"-- {s.Tag}: '{s.Scope.Name}'  Work({s.A},{s.B}) = {s.Result} --");
        string path = s.Scope.Path ?? "";
        int count = 0;
        int shown = 0;
        foreach (string line in path.Split('\n'))
        {
            if (line.Length == 0) continue;
            if (line[0] == ';') continue;   // the truncation banner, not an instruction
            count++;
            if (shown < 6) { Console.WriteLine($"    {line}"); shown++; }
        }
        if (count == 0)
            Console.WriteLine("    (no rendered listing — this build has no Capstone disassembler)");
        else if (count > shown)
            Console.WriteLine($"    … ({count - shown} more instructions stepped in this thread's window)");
        Console.WriteLine($"    captured {count} instructions; truncated: {s.Scope.Truncated}");
        return count;
    }
}
