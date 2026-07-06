// examples/dotnet/wholewindow — the aspirational EMPTY-ctor whole-window scope.
//
//     using (new AsmTrace())      // no NativeCode, no [base,len) — zero config
//     {
//         HotPath(...);           // whatever runs here is captured …
//     }                           // … then attribute the captured addresses
//
// This is the §Z0/§Z1 zero-config scope. On THIS machine (an AMD Zen 5 with no
// Intel PT) it runs via the single-step WEAK tier: the CPU traps after every
// instruction the thread executes inside the scope, into a large SPARSE buffer (no
// fixed 64k cap). The demo calls TWO native leaves in one empty scope and ATTRIBUTES
// the captured addresses by range — telling the leaves apart from each other and
// from the runtime. Self-skips cleanly (exit 0) where single-step cannot run.

using System;
using Asmtest;

internal static class Program
{
    // add2(a,b)=a+b : mov rax,rdi ; add rax,rsi ; cmp rax,100 ; jle +3 ; dec rax ; ret
    static readonly byte[] ADD2 =
    {
        0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64,
        0x00, 0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3,
    };
    // sub2(a,b)=a-b : mov rax,rdi ; sub rax,rsi ; ret
    static readonly byte[] SUB2 = { 0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0xc3 };

    static unsafe int Main()
    {
        Console.WriteLine("== empty-ctor whole-window scope: using (new AsmTrace()) ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier — the portable x86-64 Linux default\n"
                          + "(the STRONG Intel-PT / CEILING AMD-LBR tiers are forward-look)\n");

        var add2 = NativeCode.FromBytes(ADD2);   // leaf A
        var sub2 = NativeCode.FromBytes(SUB2);   // leaf B (a distinct mapping)
        // Raw unmanaged function pointers (calli) — no Marshal delegate, no P/Invoke
        // transition stub, so the window under single-step is short and the leaves show.
        var fa = (delegate* unmanaged[Cdecl]<long, long, long>)add2.Base.ToPointer();
        var fb = (delegate* unmanaged[Cdecl]<long, long, long>)sub2.Base.ToPointer();
        fa(20, 22); fb(30, 12); // warm before the scope

        long r1 = 0, r2 = 0;
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false)) // EMPTY ctor: no region, auto-named here
        {
            r1 = fa(20, 22);   // leaf A runs
            r2 = fb(30, 12);   // leaf B runs
        }
        if (!ww.Armed)
        {
            Console.WriteLine($"# self-skip: {ww.SkipReason}");
            add2.Free(); sub2.Free();
            return 0;
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

        add2.Free();
        sub2.Free();
        return 0;
    }
}
