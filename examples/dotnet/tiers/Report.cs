// examples/dotnet/tiers — reporting (presentation only). Groups the closed scope's methods by
// AsmMethod.Tier and prints instructions/methods/share per tier. No tracing logic here.

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

        // Sum each executed method's instruction count by its JIT/compilation tier.
        var insns = new Dictionary<string, long>();
        var methods = new Dictionary<string, int>();
        foreach (AsmMethod m in ww.Methods)
        {
            string tier = m.Tier.Length > 0 ? m.Tier : "(untagged)";
            insns.TryGetValue(tier, out long c); insns[tier] = c + m.Count;
            methods.TryGetValue(tier, out int n); methods[tier] = n + 1;
        }
        var ranked = new List<string>(insns.Keys);
        ranked.Sort((a, b) => insns[b].CompareTo(insns[a]));

        Console.WriteLine($"rundown enabled: {ww.RundownEnabled}; {ww.LabelledInstructions} labelled instructions "
                          + $"across {ww.Methods.Count} methods, {ranked.Count} tiers.\n");
        Console.WriteLine($"  {Label("tier"),-38}{"methods",8}{"insns",12}{"share",8}");
        Console.WriteLine("  " + new string('-', 64));
        foreach (string tier in ranked)
        {
            double pct = ww.LabelledInstructions > 0 ? 100.0 * insns[tier] / ww.LabelledInstructions : 0;
            Console.WriteLine($"  {Label(tier),-38}{methods[tier],8}{insns[tier],12}{pct,7:F1}%");
        }
        Console.WriteLine();
        if (!ww.RundownEnabled)
            Console.WriteLine("-> rundown off: R2R ([PreJIT]) methods are absent; only cold JIT tiers show.");
        else
            Console.WriteLine("-> [PreJIT]=ReadyToRun (precompiled BCL, never JIT'd); [MinOptJitted]/[Tier0]=cold JIT;\n"
                              + "   [OptimizedTier1]=hot; (untagged)=listener-observed cold methods (no tier tag).");
    }

    static string Label(string tier) => tier switch
    {
        "tier" => "tier",
        "PreJIT" => "PreJIT — R2R (precompiled BCL)",
        "MinOptJitted" => "MinOptJitted — cold JIT",
        "Tier0" => "Tier0 — cold JIT",
        "OptimizedTier0" => "OptimizedTier0 — JIT",
        "OptimizedTier1" => "OptimizedTier1 — hot JIT",
        "(untagged)" => "(untagged) — cold, listener-observed",
        _ => tier,
    };
}
