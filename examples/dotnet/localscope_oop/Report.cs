// examples/dotnet/localscope_oop — reporting + verdict. Classifies the captured ABSOLUTE
// address stream by origin (window frame vs each published leaf) to show the whole window
// was recorded across the process boundary, in order. Returns false only if the capture
// missed a region it should have named — so the lane exits nonzero on a real regression.

using System;
using Asmtest;

internal static class Report
{
    public static bool Print(HwTrace tr, NativeCode driver, NativeCode leafA, NativeCode leafB, long result)
    {
        ulong drvBase = (ulong)driver.Base.ToInt64(), drvEnd = drvBase + (ulong)driver.Length;
        ulong aBase = (ulong)leafA.Base.ToInt64(), aEnd = aBase + (ulong)leafA.Length;
        ulong bBase = (ulong)leafB.Base.ToInt64(), bEnd = bBase + (ulong)leafB.Length;

        long inDrv = 0, inA = 0, inB = 0, other = 0;
        long firstA = -1, firstB = -1;
        long i = 0;
        foreach (ulong at in tr.InsnOffsets())
        {
            if (at >= drvBase && at < drvEnd) inDrv++;
            else if (at >= aBase && at < aEnd) { inA++; if (firstA < 0) firstA = i; }
            else if (at >= bBase && at < bEnd) { inB++; if (firstB < 0) firstB = i; }
            else other++;
            i++;
        }

        Console.WriteLine($"frame returned {result} (expect 4 = A(7,3) then B(7,3)=7-3); the tracer captured "
                          + $"{tr.InsnsTotal()} instructions across the process boundary"
                          + (tr.Truncated() ? " (truncated)" : "") + $" in {tr.BlocksLen()} blocks.\n");

        Console.WriteLine("captured whole-window stream, attributed by origin (absolute addresses):");
        Console.WriteLine($"    window frame (driver)      : {inDrv,4}   <- movabs/call/ret");
        Console.WriteLine($"    leaf A  (published region) : {inA,4}   <- mov,add,ret");
        Console.WriteLine($"    leaf B  (published region) : {inB,4}   <- mov,sub,ret");
        Console.WriteLine($"    runtime/glue (stepped over): {other,4}\n");

        bool named = inDrv > 0 && inA > 0 && inB > 0;
        bool ordered = firstA >= 0 && firstB > firstA;
        if (!named)
        {
            Console.WriteLine("FAIL: the window did not capture the frame AND both published leaves");
            return false;
        }
        if (!ordered)
        {
            Console.WriteLine("FAIL: the window did not follow the calls in order (A before B)");
            return false;
        }
        Console.WriteLine("-> the whole window — the frame AND both channel-published leaves — was captured\n"
                          + "   in execution order, OUT OF PROCESS. This process's own thread was never armed\n"
                          + "   with EFLAGS.TF: a ptrace-stop is not gated by the tracee's signal mask, so this\n"
                          + "   survives code the in-process whole-window scope (localscope) is forbidden to step.");
        return true;
    }
}
