// examples/dotnet/trace-cost-overhead — reporting (presentation only), split from Program.cs.
// Renders the per-tier cost table (wall/call, stop count, x-vs-untraced) and the mechanism
// legend. No measurement or tracing logic here — Program.cs owns all of that.

using System;
using System.Collections.Generic;

internal static class Report
{
    // One measured tier. A traced row carries its per-call wall time, a stop count with a kind
    // label, and a note; an unavailable row carries only its skip reason (wall/x/stops = n/a).
    public sealed class Row
    {
        public string Tier;
        public bool Available;
        public bool IsBaseline;
        public double PerCallMs;   // valid when Available
        public long Stops;         // -1 => not applicable / unknown
        public string StopKind;
        public string Note;

        public static Row Baseline(double perCallMs) => new Row
        {
            Tier = "untraced", Available = true, IsBaseline = true,
            PerCallMs = perCallMs, Stops = -1, StopKind = "native", Note = "the baseline",
        };

        public static Row Traced(string tier, double perCallMs, long stops, string kind, string note)
            => new Row
            {
                Tier = tier, Available = true, PerCallMs = perCallMs,
                Stops = stops, StopKind = kind, Note = note,
            };

        public static Row Na(string tier, string reason) => new Row
        {
            Tier = tier, Available = false, Stops = -1, StopKind = "", Note = reason,
        };
    }

    public static void Print(List<Row> rows, double baseMs, long checksum)
    {
        Console.WriteLine($"{"tier",-24}{"wall/call",12}{"  stops",-22}{"x vs untraced",14}");
        Console.WriteLine(new string('-', 72));
        foreach (Row r in rows)
        {
            if (!r.Available)
            {
                Console.WriteLine($"{r.Tier,-24}{"n/a",12}{"n/a",-22}{"n/a",14}   ({r.Note})");
                continue;
            }
            string wall = FormatWall(r.PerCallMs);
            string stops = r.Stops < 0 ? "—" : $"{r.Stops:N0} {r.StopKind}";
            string x = r.IsBaseline ? "1.0x" : FormatX(r.PerCallMs / baseMs);
            Console.WriteLine($"{r.Tier,-24}{wall,12}{"  " + stops,-22}{x,14}");
        }
        Console.WriteLine(new string('-', 72));

        Console.WriteLine("\nper-tier note:");
        foreach (Row r in rows)
            Console.WriteLine($"    {r.Tier,-24} {r.Note}");

        Console.WriteLine(
            "\n-> the same routine, priced across tiers. Any EXACT tier that stops per INSTRUCTION\n"
            + "   costs 4-5 orders of magnitude; block-step slashes that by stopping only per TAKEN\n"
            + "   BRANCH while reconstructing the IDENTICAL trace (same InsnsTotal, far fewer stops) —\n"
            + "   the cheapest exact tier. The two per-instruction tiers trade off: in-proc single-step\n"
            + "   skips the fork but pays a managed-runtime SIGTRAP per stop, while OOP single-step\n"
            + "   pays fork+attach but its ptrace stop is cheaper — the table shows which wins here.\n"
            + "   AMD LBR is out-of-band (a few PMIs), near-native. (The baseline includes per-call\n"
            + "   managed marshalling the forked ptrace legs call natively without — the native loop\n"
            + "   dominates, so it is negligible.)");
        Console.WriteLine($"\n(checksum={checksum}) — kept live so no leg is optimised away.");
    }

    static string FormatWall(double ms) =>
        ms < 1.0 ? $"{ms * 1000.0:F2} us" : $"{ms:F2} ms";

    static string FormatX(double x) =>
        x >= 100 ? $"{x:N0}x" : x >= 10 ? $"{x:F0}x" : $"{x:F1}x";
}
