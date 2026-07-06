// examples/dotnet/callgraph — reporting (presentation only). Walks the closed scope's labelled
// stream with a shadow stack to reconstruct call edges + a first-entry call tree. No tracing here.

using System;
using System.Collections.Generic;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }
        if (!ww.DisassemblyAvailable)
        {
            Console.WriteLine("# self-skip: this build has no Capstone, so instructions cannot be disassembled.");
            return;
        }
        var stream = ww.Disassembly;
        if (stream.Count == 0) { Console.WriteLine("no labelled instructions captured."); return; }

        // Shadow stack of method names; classify each method transition by the PRIOR labelled
        // instruction's mnemonic. `call` -> descend (edge caller->callee), `ret` -> pop, anything
        // else (jmp/tail-call/stub-mediated) -> pop to it if already on the stack, else descend.
        var edges = new Dictionary<(string, string), long>();
        var stack = new List<string> { stream[0].ShortMethod };
        var parent = new Dictionary<string, string>();   // each method's first-entry caller
        var order = new List<string> { stream[0].ShortMethod };
        var seen = new HashSet<string> { stream[0].ShortMethod };
        int maxDepth = 0;

        for (int k = 1; k < stream.Count; k++)
        {
            string pm = stream[k - 1].ShortMethod, cm = stream[k].ShortMethod;
            if (cm == pm) continue;
            string op = Mnemonic(stream[k - 1].Text);
            if (op == "call")
            {
                Edge(edges, pm, cm);
                stack.Add(cm);
            }
            else if (op == "ret" || op == "retn" || op == "retf")
            {
                PopTo(stack, cm);
            }
            else if (stack.Contains(cm)) // jmp/other back into a frame already open -> return-ish
            {
                PopTo(stack, cm);
            }
            else // tail-call / stub-mediated call into a new method
            {
                Edge(edges, pm, cm);
                stack.Add(cm);
            }
            if (stack.Count - 1 > maxDepth) maxDepth = stack.Count - 1;
            if (seen.Add(cm)) { parent[cm] = pm; order.Add(cm); }
        }

        // First-entry call tree: each method's parent is who it was entered from the first time.
        var children = new Dictionary<string, List<string>>();
        string root = stream[0].ShortMethod;
        foreach (string m in order)
            if (m != root && parent.TryGetValue(m, out string p))
                (children.TryGetValue(p, out var l) ? l : (children[p] = new List<string>())).Add(m);

        Console.WriteLine($"{seen.Count} methods, {edges.Count} distinct call edges, max call depth {maxDepth}.\n");
        Console.WriteLine("call tree (first entry into each method, indented by depth):");
        PrintTree(children, root, 0);

        Console.WriteLine("\ntop call edges (by frequency):");
        var ranked = new List<KeyValuePair<(string, string), long>>(edges);
        ranked.Sort((a, b) => b.Value.CompareTo(a.Value));
        for (int i = 0; i < ranked.Count && i < 20; i++)
            Console.WriteLine($"  {ranked[i].Value,4}x  {ranked[i].Key.Item1}  ->  {ranked[i].Key.Item2}");

        Console.WriteLine("\n-> the tree/edges are reconstructed from the labelled stream (call/ret mnemonics);\n"
                          + "   approximate where a call reaches a method through a native runtime stub.");
    }

    static void Edge(Dictionary<(string, string), long> e, string a, string b)
    {
        e.TryGetValue((a, b), out long c); e[(a, b)] = c + 1;
    }

    static void PopTo(List<string> stack, string target)
    {
        int idx = stack.LastIndexOf(target);
        if (idx >= 0) stack.RemoveRange(idx + 1, stack.Count - idx - 1);
        // Unknown return target -> leave the stack as-is (approximate).
    }

    static void PrintTree(Dictionary<string, List<string>> children, string node, int depth)
    {
        Console.WriteLine("  " + new string(' ', depth * 2) + node);
        if (children.TryGetValue(node, out var kids))
            foreach (string c in kids) PrintTree(children, c, depth + 1);
    }

    static string Mnemonic(string text)
    {
        int sp = text.IndexOf(' ');
        return sp < 0 ? text : text.Substring(0, sp);
    }
}
