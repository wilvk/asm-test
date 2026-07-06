// examples/dotnet/wholewindow — reporting (presentation only), split from Program.cs.
// Reads the closed whole-window scope + the two native leaves and prints the by-origin
// attribution of the captured window. No tracing logic here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(AsmTrace ww, NativeCode add2, NativeCode sub2, long r1, long r2)
    {
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            return;
        }

        // Attribute every captured address to its origin by range — the leaves are
        // known [base,len) mappings; everything else is the runtime.
        long ca = ww.CountInRange((ulong)add2.Base.ToInt64(), (ulong)add2.Length);
        long cb = ww.CountInRange((ulong)sub2.Base.ToInt64(), (ulong)sub2.Length);
        long other = ww.Addresses.Length - ca - cb;
        Console.WriteLine($"armed '{ww.Name}', add2={r1}, sub2={r2}, captured {ww.Addresses.Length} instructions"
                          + (ww.Truncated ? " (truncated)" : "") + ".");
        Console.WriteLine("attribution of the captured window, by origin:");
        Console.WriteLine($"    leaf A  (add2)         : {ca}      <- mov,add,cmp,jle,ret");
        Console.WriteLine($"    leaf B  (sub2)         : {cb}      <- mov,sub,ret");
        Console.WriteLine($"    runtime (.NET / other) : {other}");
        if (ca == 0 && cb == 0)
            // Both leaves run first inside the scope and reliably surface at this cap, so
            // neither being captured is an anomaly, not the expected amplification.
            Console.WriteLine("(unexpected: neither leaf was captured — the scope likely ran on a different\n"
                              + " thread than the leaves, or the platform captured nothing this run.)");
        else
            Console.WriteLine("-> multiple leaves are told apart from each other and from the runtime.\n"
                              + "   (the ~1M 'runtime' instructions are the .NET runtime's own code — single-step\n"
                              + "    of a managed caller is noisy; the STRONG PT tier filters at decode.)");
    }
}
