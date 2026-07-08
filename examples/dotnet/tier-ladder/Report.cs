// examples/dotnet/tier-ladder — reporting (presentation only), split from Program.cs.
// Renders the cascade API's answers as a ladder: the per-backend availability table, the
// backend-only and cross-tier cascades, the single top pick, and the rungs each policy drops.

using System;
using Asmtest;

internal static class Report
{
    // Friendly fixed-width names for the four backends (the enum ToString is CamelCase).
    static string Name(HwBackend b)
    {
        switch (b)
        {
            case HwBackend.IntelPt:    return "Intel PT";
            case HwBackend.AmdLbr:     return "AMD LBR";
            case HwBackend.SingleStep: return "single-step";
            case HwBackend.CoreSight:  return "CoreSight";
            default:                   return b.ToString();
        }
    }

    // The availability table — name / available / reason, most-faithful first.
    public static void Backends(HwBackend[] backends)
    {
        Console.WriteLine("per-backend availability (most-faithful first):");
        foreach (var b in backends)
        {
            bool ok = HwTrace.Available(b);
            string state = ok ? "available" : $"unavailable — {HwTrace.SkipReason(b)}";
            Console.WriteLine($"  {Name(b),-12}: {state}");
        }
        Console.WriteLine();
    }

    // The backend-only hardware cascade (int enums) + its single top pick.
    public static void HwCascade()
    {
        int[] cascade = HwTrace.Resolve(HwPolicy.Best);
        Console.WriteLine("hardware-tier cascade — Resolve(Best) (backend enums, most-faithful first):");
        if (cascade.Length == 0)
            Console.WriteLine("  (empty — no hardware-trace backend available on this host)");
        else
            for (int i = 0; i < cascade.Length; i++)
                Console.WriteLine($"  [{i}] {Name((HwBackend)cascade[i])}");
        int top = HwTrace.Auto(HwPolicy.Best);
        Console.WriteLine(top < 0
            ? "  Auto(Best) -> none (no hardware-trace backend)"
            : $"  Auto(Best) -> {Name((HwBackend)top)}");
        Console.WriteLine();
    }

    // A cross-tier cascade, one TierChoice per line (ToString is already readable).
    public static void Cascade(string title, TierChoice[] choices)
    {
        Console.WriteLine($"{title} (tier / backend / fidelity):");
        if (choices.Length == 0)
            Console.WriteLine("  (empty)");
        else
            foreach (var c in choices)
                Console.WriteLine($"  {c}");
    }

    // The single top cross-tier pick under a policy.
    public static void Top(string label, TierChoice? pick)
    {
        Console.WriteLine(pick.HasValue ? $"  {label} -> {pick.Value}" : $"  {label} -> none");
        Console.WriteLine();
    }

    // Diff a restricted cascade against Best and name the rungs it dropped. A rung absent
    // from THIS host's Best cascade to begin with cannot be dropped — say so honestly.
    public static void PolicyDrop(string policy, TierChoice[] best, TierChoice[] restricted, string expected)
    {
        Console.WriteLine($"policy {policy} ({restricted.Length} rungs vs Best's {best.Length}) — expected to drop {expected}:");
        var dropped = new System.Collections.Generic.List<TierChoice>();
        foreach (var c in best)
            if (Array.IndexOf(restricted, c) < 0) dropped.Add(c);
        if (dropped.Count == 0)
            Console.WriteLine("  (no rung dropped — that rung is not present on this host's Best cascade to begin with)");
        else
            foreach (var c in dropped)
                Console.WriteLine($"  drops {c}");
        Console.WriteLine();
    }
}
