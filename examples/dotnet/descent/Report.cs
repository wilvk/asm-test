// examples/dotnet/descent — reporting (presentation only). Renders the descent frames as a tree
// with self (own insns) vs inclusive (subtree insns) counts, plus any recorded edges. No tracing.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(NativeCode code, HwTrace tr, Descent d, long result)
    {
        ulong codeBase = (ulong)code.Base.ToInt64();
        int frames = d.FramesLen();

        Console.WriteLine($"A(100) = {result} (expect 107, = 100 + 4[C] + 2[B] + 1[A]); "
                          + $"{frames} frame(s) recorded"
                          + (d.Truncated() ? " (truncated)" : "") + (d.DepthCapped() ? " (depth-capped)" : "") + ".\n");

        // self[f] = instructions executed in frame f's OWN body; inclusive[f] = self + all
        // descendants. Accumulate each frame's self into every ancestor (parent chain).
        var self = new long[frames];
        var incl = new long[frames];
        for (int f = 0; f < frames; f++) self[f] = d.FrameInsns(f).Length;
        for (int f = 0; f < frames; f++)
            for (int g = f; g >= 0; g = d.FrameParent(g))
            {
                incl[g] += self[f];
                if (d.FrameParent(g) < 0) break;   // reached the root (parent -1)
            }

        // children[] for a depth-first tree print.
        var children = new Dictionary<int, List<int>>();
        int root = 0;
        for (int f = 0; f < frames; f++)
        {
            int p = d.FrameParent(f);
            if (p < 0) { root = f; continue; }
            (children.TryGetValue(p, out var l) ? l : (children[p] = new List<int>())).Add(f);
        }

        Console.WriteLine("call tree (indented by depth; self = own insns, incl = subtree insns):");
        Console.WriteLine($"  {"frame",-24}{"self",8}{"incl",8}");
        Console.WriteLine("  " + new string('-', 40));
        PrintFrame(d, children, root, 0, codeBase, self, incl);

        var edges = d.Edges();
        if (edges.Length > 0)
        {
            Console.WriteLine("\nstepped-over edges (calls NOT descended):");
            foreach (DescentEdge e in edges)
                Console.WriteLine($"  site 0x{e.Site:x} -> {Name(e.Target - codeBase)} (depth {e.Depth})");
        }

        Console.WriteLine("\n-> every count is EXACT — the stepper single-stepped INTO B and C as nested frames,\n"
                          + "   so A's inclusive (9) covers B (5) which covers C (2). This is the noise-free tree the\n"
                          + "   in-process callgraph example can only APPROXIMATE (it cannot see across runtime stubs).");
    }

    static void PrintFrame(Descent d, Dictionary<int, List<int>> children, int f, int depth,
                           ulong codeBase, long[] self, long[] incl)
    {
        string label = new string(' ', depth * 2) + Name(d.FrameBase(f) - codeBase);
        if (label.Length > 24) label = label.Substring(0, 24);
        Console.WriteLine($"  {label,-24}{self[f],8}{incl[f],8}");
        if (children.TryGetValue(f, out var kids))
            foreach (int c in kids) PrintFrame(d, children, c, depth + 1, codeBase, self, incl);
    }

    // Label a frame by its offset within the blob: A/B/C for the three known bodies.
    static string Name(ulong off) => off switch
    {
        0x00 => "A @0x00",
        0x0d => "B @0x0d",
        0x17 => "C @0x17",
        _ => $"0x{off:x}",
    };
}
