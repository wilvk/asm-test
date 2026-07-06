// examples/dotnet/region — the region-scoped scope.
//
//     using (new AsmTrace(code))  // scoped to one native routine
//     {
//         code.Call(...);         // its executed instructions are captured …
//     }                           // … and rendered on Dispose
//
// The same import + scope shape as the empty-ctor form, but scoped to a known
// native region, so you get EXACTLY that routine's executed assembly — no runtime
// noise. Runs live via the single-step WEAK tier; self-skips cleanly where it can't.

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

    static int Main()
    {
        Console.WriteLine("== region-scoped scope: using (new AsmTrace(code)) ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        var code = NativeCode.FromBytes(ADD2);
        code.Call(20, 22); // warm the call path before the window

        long r = 0;
        AsmTrace scope;
        using (scope = new AsmTrace(code, emit: false)) // scoped to this native region
        {
            r = code.Call(20, 22);
        }
        if (!scope.Armed)
        {
            Console.WriteLine($"# self-skip: {scope.SkipReason}");
            code.Free();
            return 0;
        }

        Console.WriteLine($"armed '{scope.Name}', add2(20,22) = {r}.");
        Console.WriteLine("rendered listing — EXACTLY the routine's executed instructions:");
        foreach (string line in (scope.Path ?? "").Split('\n'))
            if (line.Length > 0 && line[0] != ';')
                Console.WriteLine($"    {line}");
        Console.WriteLine($"truncated: {scope.Truncated}");
        Console.WriteLine("-> region-scoped gives the clean, isolated assembly path "
                          + "(the dec at 0xc is absent: add2(20,22)=42<=100, so the jle is taken).");

        code.Free();
        return 0;
    }
}
