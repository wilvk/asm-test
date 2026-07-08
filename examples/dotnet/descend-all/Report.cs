// examples/dotnet/descend-all — reporting (presentation only). Renders one DescendAll run: the
// discovered frame tree (depth / parent / self insns), any calls the denylist refused (edges), and
// the honest Truncated()/DepthCapped() flags. No tracing here — Program does that.
//
// NOTE on identifying frames: with DescendKnown (`descent`) each frame's base is the exact region
// you AllowRegion()'d, so FrameBase names the callee. DescendAll declares NO extents — it discovers
// callees mid-step, so every frame's base is the ENCLOSING executable mapping (all identical) and
// FrameLen is the whole page. The honest callee identity is therefore the frame's FIRST recorded
// instruction offset (FrameInsns(f)[0]) — where the child actually entered — which is what we label by.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(string runLabel, NativeCode code, Descent d, long result)
    {
        int frames = d.FramesLen();

        // Callee entry = first executed offset in the frame (see file header); its own insn count.
        var entry = new long[frames];
        var self = new long[frames];
        var incl = new long[frames];
        for (int f = 0; f < frames; f++)
        {
            var ins = d.FrameInsns(f);
            self[f] = ins.Length;
            entry[f] = ins.Length > 0 ? (long)ins[0] : -1;
        }
        // inclusive[f] = self + all descendants: fold each frame's self up its parent chain.
        for (int f = 0; f < frames; f++)
            for (int g = f; g >= 0; g = d.FrameParent(g))
            {
                incl[g] += self[f];
                if (d.FrameParent(g) < 0) break;   // reached the root (parent -1)
            }

        Console.WriteLine($"[{runLabel}]");
        Console.WriteLine($"  A(100) = {result} (expect 107 = 100 + 4[C] + 2[B] + 1[A]); "
                          + $"{frames} frame(s) discovered"
                          + $"  Truncated={Low(d.Truncated())}  DepthCapped={Low(d.DepthCapped())}.");

        // children[] for a depth-first tree print.
        var children = new Dictionary<int, List<int>>();
        int root = 0;
        for (int f = 0; f < frames; f++)
        {
            int p = d.FrameParent(f);
            if (p < 0) { root = f; continue; }
            (children.TryGetValue(p, out var l) ? l : (children[p] = new List<int>())).Add(f);
        }

        Console.WriteLine($"  {"frame (by entry)",-22}{"depth",7}{"parent",8}{"self",7}{"incl",7}");
        Console.WriteLine("  " + new string('-', 51));
        PrintFrame(d, children, root, 0, entry, self, incl);

        // Honest completeness signal: a depth/budget/watchdog cap folds the deeper callee's
        // instructions into its PARENT frame (no new frame) and flips DepthCapped — so the tree
        // is a PREFIX of the real one. (The denylist, by contrast, steps a call OVER as an edge.)
        if (d.DepthCapped())
            Console.WriteLine("  -> DepthCapped: descent hit a guardrail; the deeper callee's insns FOLDED into its\n"
                            + "     parent (watch B's self grow), so no frame was opened for it. The tree is a prefix.");

        var edges = d.Edges();
        if (edges.Length > 0)
        {
            Console.WriteLine("  calls the denylist refused (stepped OVER as edges, not descended):");
            foreach (DescentEdge e in edges)
                Console.WriteLine($"    site 0x{e.Site:x} -> 0x{e.Target:x} (caller depth {e.Depth})");
        }
        Console.WriteLine();
    }

    static void PrintFrame(Descent d, Dictionary<int, List<int>> children, int f, int depth,
                           long[] entry, long[] self, long[] incl)
    {
        string label = new string(' ', depth * 2) + Name(entry[f]);
        if (label.Length > 22) label = label.Substring(0, 22);
        Console.WriteLine($"  {label,-22}{d.FrameDepth(f),7}{d.FrameParent(f),8}{self[f],7}{incl[f],7}");
        if (children.TryGetValue(f, out var kids))
            foreach (int c in kids) PrintFrame(d, children, c, depth + 1, entry, self, incl);
    }

    // Label a frame by the offset it was ENTERED at: A/B/C for the three known bodies.
    static string Name(long off) => off switch
    {
        0x00 => "A @0x00",
        0x0d => "B @0x0d",
        0x17 => "C @0x17",
        < 0 => "(empty)",
        _ => $"0x{off:x}",
    };

    static string Low(bool b) => b ? "true" : "false";
}
