// examples/dotnet/codeimage — one address, two code bodies over LOGICAL time.
//
//     img.Track(base, len);            // version 0 snapshot (body v0)
//     <overwrite base with body v1>
//     img.Refresh();                   // detects the changed page -> version 1
//     img.BytesAt(base, whenV0);       // STILL returns v0 — the timeline kept it
//     img.BytesAt(base, whenV1);       // returns v1
//
// The time-aware code image is a userspace PERF_RECORD_TEXT_POKE: it answers "what bytes were
// live at this address as of logical time N", which a branch-trace decoder needs when a JIT
// frees an address and reuses it for a different method. This self-patches ONE executable blob
// (v0: return 1 -> v1: return 2) and shows both bodies survive in the timeline AND both really
// run. Honest caveat: a real CoreCLR tier0->tier1 relocates to a NEW address, so a fixed-region
// code image does not capture managed tiering — this shows the mechanism on a controlled blob.
// Self-skips (exit 0) where the recorder or RWX patching is unavailable.

using System;
using System.Runtime.InteropServices;
using Asmtest;

internal static class Program
{
    // mov eax, 1 ; ret   (eax zero-extends into rax, so Call returns 1)
    static readonly byte[] V0 = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    // mov eax, 2 ; ret
    static readonly byte[] V1 = { 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3 };

    const int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
    [DllImport("libc", SetLastError = true)]
    static extern int mprotect(IntPtr addr, UIntPtr len, int prot);

    static int Main()
    {
        Console.WriteLine("== code image: one address holding two code bodies over logical time ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))   // gates the native lib being present
        {
            Console.WriteLine($"# self-skip: hwtrace lib unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        if (!CodeImage.Available())
        {
            Console.WriteLine($"# self-skip: code-image recorder unavailable: {CodeImage.SkipReason()}");
            return 0;
        }

        var code = NativeCode.FromBytes(V0);            // executable (R+X) blob at a fixed address
        long r0 = code.Call(0, 0);                      // runs body v0 -> 1

        using (var img = new CodeImage(0))              // track THIS process
        {
            img.Track(code.Base, (nuint)code.Length);   // version 0: snapshot v0 + arm change detect
            ulong whenV0 = img.Now();

            // Re-arm the page writable to self-patch it, then restore R+X. A hardened W^X kernel
            // may refuse PROT_WRITE|PROT_EXEC together — self-skip honestly if the toggle fails.
            var pageBase = PageAlign(code.Base, out UIntPtr span, code.Length);
            if (mprotect(pageBase, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            {
                Console.WriteLine("# self-skip: could not make the code page writable (W^X kernel policy).");
                code.Free();
                return 0;
            }
            Marshal.Copy(V1, 0, code.Base, V1.Length);   // overwrite v0 -> v1 in place
            mprotect(pageBase, span, PROT_READ | PROT_EXEC);

            int refreshed = img.Refresh();               // detect the changed page -> version 1
            ulong whenV1 = img.Now();
            long r1 = code.Call(0, 0);                   // SAME address now runs body v1 -> 2

            // Read both versions back FROM THE TIMELINE (after the patch): v0 must have survived.
            byte[] atV0 = img.BytesAt(code.Base, whenV0);
            byte[] atV1 = img.BytesAt(code.Base, whenV1);
            Report.Print(code.Base, r0, r1, whenV0, whenV1, refreshed, atV0, atV1, V0, V1);
        }
        code.Free();
        return 0;
    }

    // The page-aligned base + byte span covering [base, base+len) for mprotect.
    static IntPtr PageAlign(IntPtr addr, out UIntPtr span, long len)
    {
        long pageSize = Environment.SystemPageSize;
        long a = addr.ToInt64();
        long start = a & ~(pageSize - 1);
        long end = (a + len + pageSize - 1) & ~(pageSize - 1);
        span = (UIntPtr)(nuint)(end - start);
        return new IntPtr(start);
    }
}
