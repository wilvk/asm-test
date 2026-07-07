// examples/dotnet/codeimage — reporting (presentation only). Shows both timeline versions of the
// one address (disassembled) and that each body really ran. No tracing/patching logic here.

using System;
using Asmtest;

internal static class Report
{
    public static void Print(IntPtr addr, long r0, long r1, ulong whenV0, ulong whenV1,
                             int refreshed, byte[] atV0, byte[] atV1, byte[] v0, byte[] v1)
    {
        Console.WriteLine($"one address 0x{addr.ToInt64():x}, patched in place v0 -> v1:\n");
        Console.WriteLine($"  executed v0 (before patch): returned {r0} (expect 1)");
        Console.WriteLine($"  executed v1 (after  patch): returned {r1} (expect 2)");
        Console.WriteLine($"  code-image versions: v0 at seq {whenV0}, v1 at seq {whenV1} "
                          + $"({refreshed} new version(s) from Refresh)\n");

        if (refreshed < 1 || whenV1 == whenV0)
        {
            Console.WriteLine("# note: the recorder did not observe a page change (no soft-dirty / PAGEMAP_SCAN);\n"
                              + "  the execution above still proves the in-place patch, but the version timeline is flat.");
            return;
        }

        Console.WriteLine("BytesAt the SAME address, read AFTER the patch, at two logical times:");
        Console.WriteLine($"  seq {whenV0}:  {Bytes(atV0)}   {Disas(atV0, addr)}");
        Console.WriteLine($"  seq {whenV1}:  {Bytes(atV1)}   {Disas(atV1, addr)}");

        bool v0ok = PrefixEq(atV0, v0), v1ok = PrefixEq(atV1, v1);
        Console.WriteLine($"\n-> the timeline kept BOTH bodies at one address: seq {whenV0} still decodes the ORIGINAL\n"
                          + $"   v0 (return 1)"
                          + (v0ok ? " ✓" : " (mismatch)") + $", seq {whenV1} the patched v1 (return 2)"
                          + (v1ok ? " ✓" : " (mismatch)") + ". This is the read a\n"
                          + "   branch-trace decoder needs when a JIT reuses an address for a different method.");
        Console.WriteLine("   (Honest caveat: real tier0->tier1 relocates to a NEW address, so a fixed-region code\n"
                          + "    image does not capture managed tiering — this shows the mechanism on a controlled blob.)");
    }

    static string Bytes(byte[] b)
    {
        if (b == null) return "(null)";
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < b.Length && i < 6; i++) sb.Append(b[i].ToString("x2")).Append(' ');
        return sb.ToString().TrimEnd();
    }

    static string Disas(byte[] bytes, IntPtr addr)
    {
        if (bytes == null || bytes.Length == 0) return "(no bytes)";
        var code = NativeCode.FromBytes(bytes);
        try
        {
            var buf = new byte[64];
            HwNative.asmtest_disas(0 /* X86_64 */, code.Base, (UIntPtr)bytes.Length,
                                   (ulong)addr.ToInt64(), 0, buf, (UIntPtr)buf.Length);
            int z = Array.IndexOf<byte>(buf, 0);
            if (z < 0) z = buf.Length;
            return z == 0 ? "(undecodable)" : System.Text.Encoding.ASCII.GetString(buf, 0, z);
        }
        finally { code.Free(); }
    }

    static bool PrefixEq(byte[] a, byte[] b)
    {
        if (a == null || a.Length < b.Length) return false;
        for (int i = 0; i < b.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }
}
