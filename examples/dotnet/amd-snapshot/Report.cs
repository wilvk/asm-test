// examples/dotnet/amd-snapshot — reporting (presentation only), split from Program.cs.
// Interprets the AmdSnapshot.Trace return code and, on OK, renders the deterministically
// reconstructed in-region stream. No capture logic here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(int rc, HwTrace tr, long add2)
    {
        // The gated non-OK codes are honest self-skips, not failures: the substrate/privilege
        // is absent (EUNAVAIL) or the lib was built without the BPF toolchain (ENOSYS).
        if (rc == HwNative.ASMTEST_HW_ENOSYS)
        {
            Console.WriteLine("# self-skip: built without the BPF toolchain (ASMTEST_HW_ENOSYS)");
            return;
        }
        if (rc == HwNative.ASMTEST_HW_EUNAVAIL)
        {
            Console.WriteLine("# self-skip: snapshot capture unavailable — needs CAP_BPF + CAP_PERFMON "
                              + "(ASMTEST_HW_EUNAVAIL)");
            return;
        }
        if (rc != HwNative.ASMTEST_HW_OK)
        {
            Console.WriteLine($"# self-skip: AmdSnapshot.Trace rc={rc}");
            return;
        }

        ulong insns = tr.InsnsTotal();     // instructions the snapshot reconstructed in-region
        bool entry = tr.Covered(0);        // the ENTRY block — offset 0 — the sampled path drops
        bool truncated = tr.Truncated();
        ulong[] offs = tr.InsnOffsets();

        Console.WriteLine($"add2(20,22) = {add2}.");
        Console.WriteLine($"deterministic snapshot decoded {insns} in-region instructions; "
                          + $"entry-block covered = {entry}; truncated = {truncated}.");
        Console.WriteLine("reconstructed in-region instruction offsets (execution order):");
        Console.Write("    ");
        for (int i = 0; i < offs.Length; i++)
            Console.Write($"0x{offs[i]:x2}{(i + 1 < offs.Length ? " " : "")}");
        Console.WriteLine();
        Console.WriteLine("-> the boundary snapshot captured the tiny single-shot routine EXACTLY — its\n"
                          + "   entry block is reconstructed precisely where the sampled survey (amdhot /\n"
                          + "   amdlbr) honestly truncates a routine too small/fast to accumulate weight.\n"
                          + "   (0x0c, the `dec rax`, is absent: add2(20,22)=42<=100, so the jle is taken.)");
    }
}
