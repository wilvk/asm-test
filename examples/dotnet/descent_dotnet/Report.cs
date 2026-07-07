// examples/dotnet/descent_dotnet — reporting (presentation only). Renders the descent frames /
// edges captured from the live runtime as a self-vs-inclusive tree. No tracing logic here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(HwTrace tr, Descent d, long result, IntPtr chainBase,
                             (IntPtr Base, nuint Len)? leaf)
    {
        int frames = d.FramesLen();
        ulong cbase = (ulong)chainBase.ToInt64();
        ulong lbase = leaf.HasValue ? (ulong)leaf.Value.Base.ToInt64() : 0;

        Console.WriteLine($"single-stepped the REAL JIT'd Program::Chain out of process (it returned {result}); "
                          + $"{frames} frame(s)"
                          + (d.Truncated() ? " (truncated)" : "") + (d.DepthCapped() ? " (depth-capped)" : "") + ".\n");

        // self / inclusive instruction counts (accumulate each frame's self into its ancestors).
        var self = new long[frames];
        var incl = new long[frames];
        for (int f = 0; f < frames; f++) self[f] = d.FrameInsns(f).Length;
        for (int f = 0; f < frames; f++)
            for (int g = f; g >= 0; g = d.FrameParent(g))
            {
                incl[g] += self[f];
                if (d.FrameParent(g) < 0) break;
            }

        var children = new Dictionary<int, List<int>>();
        int root = 0;
        for (int f = 0; f < frames; f++)
        {
            int p = d.FrameParent(f);
            if (p < 0) { root = f; continue; }
            (children.TryGetValue(p, out var l) ? l : (children[p] = new List<int>())).Add(f);
        }

        Console.WriteLine("call tree (self = own insns, incl = subtree insns):");
        Console.WriteLine($"  {"frame",-22}{"self",8}{"incl",8}");
        Console.WriteLine("  " + new string('-', 38));
        PrintFrame(d, children, root, 0, cbase, lbase, self, incl);

        var edges = d.Edges();
        if (edges.Length > 0)
        {
            Console.WriteLine("\nrecorded edges (call-site -> callee):");
            foreach (DescentEdge e in edges)
                Console.WriteLine($"  site 0x{e.Site:x} -> {NameAbs(e.Target, cbase, lbase)} (depth {e.Depth})");
        }

        bool descended = false;
        for (int f = 1; f < frames; f++) if (leaf.HasValue && d.FrameBase(f) == lbase) descended = true;
        Console.WriteLine();
        if (descended)
            Console.WriteLine("-> the stepper stepped INTO Program::Leaf as a nested frame while Chain ran in ANOTHER,\n"
                              + "   live, GC'd CoreCLR — an EXACT out-of-process managed call tree with self/inclusive\n"
                              + "   counts. This is what in-process single-step is forbidden to do (the managed footgun).");
        else if (edges.Length > 0)
            Console.WriteLine("-> descent ran out of process against the live JIT'd Chain and recorded its call edges;\n"
                              + "   the callee reached through a runtime stub was stepped over (Leaf's final body was not\n"
                              + "   in the allow-set at that address). The edges are the honest out-of-process record.");
        else
            Console.WriteLine("-> descent ran out of process against the live JIT'd Chain; this pass recorded only its\n"
                              + "   own frame (the call-out did not resolve to the allow-listed Leaf body this run).");
    }

    static void PrintFrame(Descent d, Dictionary<int, List<int>> children, int f, int depth,
                           ulong cbase, ulong lbase, long[] self, long[] incl)
    {
        string label = new string(' ', depth * 2) + NameAbs(d.FrameBase(f), cbase, lbase);
        if (label.Length > 22) label = label.Substring(0, 22);
        Console.WriteLine($"  {label,-22}{self[f],8}{incl[f],8}");
        if (children.TryGetValue(f, out var kids))
            foreach (int c in kids) PrintFrame(d, children, c, depth + 1, cbase, lbase, self, incl);
    }

    static string NameAbs(ulong addr, ulong cbase, ulong lbase)
    {
        if (addr == cbase) return "Program::Chain";
        if (lbase != 0 && addr == lbase) return "Program::Leaf";
        return $"0x{addr:x}";
    }
}
