// examples/dotnet — the scoped-trace facility working for real on this machine.
//
// The star is the aspirational EMPTY-ctor form:
//
//     using (new AsmTrace())      // no NativeCode, no [base,len) — zero config
//     {
//         HotPath(...);           // whatever runs here is captured …
//     }                           // … and its executed asm rendered on Dispose
//
// This is the §Z0/§Z1 "zero-config whole-window" scope. On THIS machine (an AMD
// Zen 5 with no Intel PT) it runs via the single-step WEAK tier: the CPU traps
// after every instruction the thread executes inside the scope, the handler
// records each ABSOLUTE address, and render_window disassembles them from live
// memory.
//
// The example runs the SAME scope two ways so the honest trade-off is visible:
//   (1) the empty ctor  new AsmTrace()      — captures EVERYTHING the thread ran,
//       which for a managed caller is the .NET runtime's own JIT'd code (honest,
//       and noisy — "you get everything, including the runtime");
//   (2) the region ctor new AsmTrace(code)  — the same import + same scope shape,
//       but scoped to one native routine, so you get EXACTLY its assembly.
//
// Build/run: `make hwtrace-dotnet-example` (or, with the shared lib built,
// `dotnet run --project examples/dotnet/asmscope.csproj`). It self-skips cleanly
// (exit 0) where the single-step backend cannot run.

using System;
using System.Runtime.InteropServices;
using Asmtest;

internal static class Program
{
    // mov rax,rdi ; add rax,rsi ; cmp rax,100 ; jle +3 ; dec rax ; ret
    // add2(20,22) = 42 (<=100 so the jle is taken and the dec at 0xc is skipped).
    static readonly byte[] ROUTINE =
    {
        0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64,
        0x00, 0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3,
    };

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate long Add2(long a, long b);

    static int Main()
    {
        Console.WriteLine("== asm-test: scoped in-process tracing, live on this machine ==\n");

        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0; // honest self-skip — never a hard failure
        }
        HwTrace.Init(HwBackend.SingleStep);
        Console.WriteLine("backend: single-step WEAK tier (no Intel PT on this AMD host)\n");

        var code = NativeCode.FromBytes(ROUTINE);
        var add2 = Marshal.GetDelegateForFunctionPointer<Add2>(code.Base);
        add2(20, 22); // warm the managed->native transition before the scopes

        // ---- (1) the aspirational EMPTY ctor: using (new AsmTrace()) ---- //
        Console.WriteLine("(1) using (new AsmTrace())  — zero config, whole-window\n");
        long r1 = 0;
        AsmTrace ww;
        using (ww = new AsmTrace(emit: false)) // EMPTY ctor: no region, auto-named here
        {
            r1 = add2(20, 22);
        }
        if (ww.Armed)
        {
            int n = CountInsns(ww.Path);
            Console.WriteLine($"    armed, auto-named '{ww.Name}', call returned {r1}.");
            Console.WriteLine($"    captured {n} instructions of real executed machine code"
                              + (ww.Truncated ? " (window overflowed — truncated)" : "") + ".");
            Console.WriteLine("    a sample of the disassembly (this is the .NET runtime's own");
            Console.WriteLine("    JIT'd code executing the call — the honest whole-window cost):");
            PrintSample(ww.Path, 6);
            Console.WriteLine("    -> the empty ctor faithfully captures WHATEVER ran on the thread.\n");
        }
        else
        {
            Console.WriteLine($"    self-skip: {ww.SkipReason}\n");
        }

        // ---- (2) the same scope, scoped to one native routine ---- //
        Console.WriteLine("(2) using (new AsmTrace(code))  — same import + scope, one routine\n");
        long r2 = 0;
        AsmTrace rg;
        using (rg = new AsmTrace(code, emit: false))
        {
            r2 = add2(20, 22);
        }
        if (rg.Armed)
        {
            Console.WriteLine($"    armed, auto-named '{rg.Name}', call returned {r2}.");
            Console.WriteLine("    rendered listing — EXACTLY the routine's executed instructions:");
            foreach (string line in (rg.Path ?? "").Split('\n'))
                if (line.Length > 0 && line[0] != ';')
                    Console.WriteLine($"      {line}");
            Console.WriteLine($"    truncated: {rg.Truncated}");
            Console.WriteLine("    -> region-scoped gives the clean, isolated assembly path.");
        }
        else
        {
            Console.WriteLine($"    self-skip: {rg.SkipReason}");
        }

        code.Free();
        return 0;
    }

    static int CountInsns(string path)
    {
        int n = 0;
        foreach (string line in (path ?? "").Split('\n'))
            if (line.Length > 0 && line[0] != ';') n++;
        return n;
    }

    static void PrintSample(string path, int count)
    {
        int shown = 0;
        foreach (string line in (path ?? "").Split('\n'))
        {
            if (line.Length == 0 || line[0] == ';') continue;
            int tab = line.IndexOf('\t');
            Console.WriteLine($"        {(tab >= 0 ? line.Substring(tab + 1) : line)}");
            if (++shown >= count) break;
        }
    }
}
