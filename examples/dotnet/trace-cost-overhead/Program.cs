// examples/dotnet/trace-cost-overhead — the honest PRICE of each tracing tier.
//
// Every other .NET example here demonstrates ONE tier's CAPTURE (what it records). This one
// measures the COST: it runs the SAME native routine untraced, then under each tier, and prints
// a slowdown-multiplier + stop-count table so the price of fidelity is on the page, not implied.
//
//   untraced             — native speed, the baseline (no stops, no fork)
//   single-step (in-proc)— HwTrace.Init(SingleStep)+Register+Region: EFLAGS.TF, one SIGTRAP per
//                          INSTRUCTION. Exact, but ~µs/insn — the classic single-step tax.
//   block-step (OOP fork)— Ptrace.TraceCallBlockstep: forks a tracee, one stop per TAKEN BRANCH,
//                          and RECONSTRUCTS the identical per-instruction trace (same InsnsTotal)
//                          from far fewer stops — coarser granularity is the whole speedup.
//   OOP single-step      — Ptrace.TraceCall: forks a tracee, one stop per INSTRUCTION out of
//                          band. Per-instruction like in-proc single-step, but each stop is a
//                          plain ptrace stop, NOT a managed-runtime SIGTRAP — so despite the
//                          fork it can UNDERCUT in-proc TF. The table shows which wins here.
//   AMD LBR region       — HwTrace.Init(AmdLbr)+Region: out-of-band branch-record sampling, a
//                          handful of PMIs, near-native. Self-skips (n/a) off Zen 3+/LBR.
//
// The counted routine is a native spin loop (arg = iterations), so ONE call is measurable. The
// untraced baseline averages many calls for a stable per-call figure; the ptrace legs trace ONE
// call (as they fork per call). x-vs-untraced is the honest slowdown. Self-skips whole (exit 0)
// off the x86-64 single-step substrate; individual legs print n/a where their tier is absent.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using Asmtest;

internal static class Program
{
    // spin(rdi=iters): mov rax,0 ; L: add rax,rdi ; dec rdi ; jnz L ; ret. One taken back-edge
    // per iteration — real branches for block-step/LBR, and a per-instruction stream for
    // single-step. InsnsTotal per call = 3*iters + 2 (mov + iters*(add,dec,jnz) + ret).
    static readonly byte[] SPIN =
    {
        0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,  // 0x0: mov rax, 0
        0x48, 0x01, 0xF8,                          // 0x7: L: add rax, rdi
        0x48, 0xFF, 0xCF,                          // 0xa: dec rdi
        0x75, 0xF8,                                // 0xd: jnz L (-> 0x7)
        0xC3,                                      // 0xf: ret
    };

    const long Trips = 3000;      // native iterations per call: 3*3000+2 = 9002 insns
    const int MUntraced = 60000;  // baseline calls, averaged for a stable per-call time
    const int RepsSs = 2;         // in-process single-step: keep the TF window tiny + safe
    const int RepsBs = 3;         // block-step forks per call
    const int RepsOop = 3;        // OOP single-step forks per call

    static int Main()
    {
        Console.WriteLine("== trace-cost-overhead: the slowdown + stop-count of each tier, same routine ==\n");

        // The x86-64 single-step tier is the portable substrate this whole comparison stands on
        // (and the native blob is x86-64). Its absence means the wrong substrate — self-skip whole.
        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: "
                              + $"{HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        Console.WriteLine($"routine: native spin loop, {Trips:N0} iters/call = {3 * Trips + 2:N0} "
                          + "instructions/call.\n");

        var code = NativeCode.FromBytes(SPIN);
        var rows = new List<Report.Row>();
        long checksum = 0;

        // Warm the call + marshal + JIT paths so nothing first-times inside a traced window.
        for (int i = 0; i < 2000; i++) checksum += code.Call(Trips, 0);

        // ---- 1. untraced baseline: many calls, averaged ----------------------------------
        GC.Collect();
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < MUntraced; i++) checksum += code.Call(Trips, 0);
        sw.Stop();
        double baseMs = sw.Elapsed.TotalMilliseconds / MUntraced;
        rows.Add(Report.Row.Baseline(baseMs));

        // ---- 2. single-step, in-process (EFLAGS.TF, one SIGTRAP per instruction) ---------
        {
            HwTrace.Init(HwBackend.SingleStep);
            HwTrace tr = HwTrace.Create(blocks: 64, instructions: 1 << 15).Register("spin", code);
            GC.Collect(); // fresh gen0 budget so the tiny TF window allocates nothing
            var sws = Stopwatch.StartNew();
            tr.Region("spin", () => { for (int i = 0; i < RepsSs; i++) checksum += code.Call(Trips, 0); });
            sws.Stop();
            long insns = (long)tr.InsnsTotal() / RepsSs;   // per-call instructions stepped
            long blocks = (long)tr.BlocksLen();
            rows.Add(Report.Row.Traced("single-step (in-proc)", sws.Elapsed.TotalMilliseconds / RepsSs,
                                       insns, "insn-stops",
                                       $"{blocks} blocks; TF, 1 SIGTRAP / instruction"));
            tr.Free();
            HwTrace.Shutdown();
        }

        // ---- 3. block-step, out of process (fork; one stop per TAKEN branch) -------------
        if (Ptrace.BlockstepAvailable())
        {
            HwTrace tr = HwTrace.Create(blocks: 64, instructions: 1 << 16);
            var args = new long[] { Trips, 0 };
            // Counts from ONE call (fresh trace): InsnsTotal reconstructed == single-step's.
            checksum += Ptrace.TraceCallBlockstep(code.Base, (nuint)code.Length, args, tr.Handle);
            long insns = (long)tr.InsnsTotal();
            long blocks = (long)tr.BlocksLen();
            var swb = Stopwatch.StartNew();
            for (int i = 0; i < RepsBs; i++)
                checksum += Ptrace.TraceCallBlockstep(code.Base, (nuint)code.Length, args, tr.Handle);
            swb.Stop();
            rows.Add(Report.Row.Traced("block-step (OOP fork)", swb.Elapsed.TotalMilliseconds / RepsBs,
                                       blocks, "blocks",
                                       $"fork + 1 stop / taken branch; reconstructs {insns:N0} insns"));
            tr.Free();
        }
        else
        {
            rows.Add(Report.Row.Na("block-step (OOP fork)",
                                   "PTRACE_SINGLEBLOCK / Capstone unavailable"));
        }

        // ---- 4. OOP single-step (fork; one stop per instruction — the heaviest exact tier)
        if (Ptrace.Available())
        {
            HwTrace tr = HwTrace.Create(blocks: 64, instructions: 1 << 16);
            var args = new long[] { Trips, 0 };
            checksum += Ptrace.TraceCall(code.Base, (nuint)code.Length, args, tr.Handle);
            long insns = (long)tr.InsnsTotal();
            var swo = Stopwatch.StartNew();
            for (int i = 0; i < RepsOop; i++)
                checksum += Ptrace.TraceCall(code.Base, (nuint)code.Length, args, tr.Handle);
            swo.Stop();
            rows.Add(Report.Row.Traced("OOP single-step (fork)", swo.Elapsed.TotalMilliseconds / RepsOop,
                                       insns, "insn-stops",
                                       "fork + 1 stop / instruction, out of band (no managed SIGTRAP)"));
            tr.Free();
        }
        else
        {
            rows.Add(Report.Row.Na("OOP single-step (fork)", Ptrace.SkipReason()));
        }

        // ---- 5. AMD LBR region (out-of-band sampling; near-native) ------------------------
        if (HwTrace.Available(HwBackend.AmdLbr))
        {
            const int repsAmd = 200; // near-native, so average many for a stable figure
            HwTrace.Init(HwBackend.AmdLbr);
            HwTrace tr = HwTrace.Create(blocks: 64, instructions: 1 << 15).Register("spin", code);
            var swa = Stopwatch.StartNew();
            tr.Region("spin", () => { for (int i = 0; i < repsAmd; i++) checksum += code.Call(Trips, 0); });
            swa.Stop();
            long blocks = (long)tr.BlocksLen();
            rows.Add(Report.Row.Traced("AMD LBR region", swa.Elapsed.TotalMilliseconds / repsAmd,
                                       blocks > 0 ? blocks : -1, "blocks (sampled)",
                                       "out-of-band branch-record sampling, a few PMIs"));
            tr.Free();
            HwTrace.Shutdown();
        }
        else
        {
            rows.Add(Report.Row.Na("AMD LBR region", HwTrace.SkipReason(HwBackend.AmdLbr)));
        }

        Report.Print(rows, baseMs, checksum);
        code.Free();
        return 0;
    }
}
