// examples/dotnet/amd-snapshot — the DETERMINISTIC AMD boundary LBR snapshot.
//
//     var tr = HwTrace.Create(64, 64);
//     int rc = AmdSnapshot.Trace(code, exitOff: 0x11, () => code.Call(20, 22), tr);
//     // rc==OK -> tr.InsnsTotal() / tr.Covered(0): the EXACT in-region stream, entry block and all
//
// The sampled AMD survey (amdhot / amdlbr) runs the window at native speed and SAMPLES the
// branch stack — so a tiny single-shot routine that fires once and returns is honestly
// TRUNCATED: it never accumulates enough sample weight to be caught, and its entry block is
// lost. This example captures that SAME routine EXACTLY. It enables the LBR, plants a hardware
// execution breakpoint at base+exitOff (the region's final `ret`), runs the region once, and
// snapshots the frozen 16-entry branch stack at the boundary via bpf_get_branch_snapshot() —
// a deterministic capture, not a statistical one. The decoded in-region stream fills a
// HwTrace, so Covered(0) is true and the ENTRY BLOCK is reconstructed precisely where sampling
// would have dropped it.
//
// This is the AMD counterpart to `region` (single-step) and the exact sibling of the sampled
// WindowHot survey. It needs the far heavier substrate the sampled path does not — CAP_BPF +
// CAP_PERFMON + AMD LbrExtV2 + a BPF-toolchain build + Linux >= 6.10 — so on a box built
// without the BPF toolchain / without CAP_BPF it self-skips (exit 0). That clean self-skip is
// the correct outcome here; it is NOT a failure.

using System;
using Asmtest;

internal static class Program
{
    // add2(a,b)=a+b : mov rax,rdi ; add rax,rsi ; cmp rax,100 ; jle +3 ; dec rax ; ret
    // The final `ret` is at byte-offset 0x11 — the region's exit, where we snapshot the LBR.
    // The very routine the sampled survey truncates: it runs once and returns.
    static readonly byte[] ADD2 =
    {
        0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64,
        0x00, 0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3,
    };
    const nuint RET_OFF = 0x11;

    static int Main()
    {
        Console.WriteLine("== deterministic AMD boundary LBR snapshot: AmdSnapshot.Trace ==\n");

        // Gate on the snapshot substrate specifically — it is much heavier than the sampled
        // survey's Zen-LBR requirement (adds CAP_BPF + a BPF-toolchain build + kernel >= 6.10).
        if (!AmdSnapshot.Available())
        {
            Console.WriteLine($"# self-skip: {AmdSnapshot.SkipReason()}");
            return 0;
        }

        Console.WriteLine("substrate: AMD LbrExtV2 + BPF snapshot — plant a HW breakpoint at the region\n"
                          + "exit, run once, read the frozen branch stack. EXACT, not sampled.\n");

        var code = NativeCode.FromBytes(ADD2);
        var tr = HwTrace.Create(64, 64); // block + instruction recording both on

        long add2 = 0; // filled by the run callback the snapshot drives to the boundary
        int rc = AmdSnapshot.Trace(code, RET_OFF, () => add2 = code.Call(20, 22), tr);

        Report.Print(rc, tr, add2);

        tr.Free();
        code.Free();
        return 0;
    }
}
