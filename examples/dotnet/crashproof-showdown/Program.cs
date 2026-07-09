// examples/dotnet/crashproof-showdown — the FATAL single-step boundary as an OBSERVED fact.
//
// The SAME hostile managed block — spawn a thread and join it, INSIDE the trace window — run
// two ways, back to back, LIVE:
//
//   FATAL  (in-process, EFLAGS.TF): using (new AsmTrace()) { SpawnThreadInWindow(); }
//   SAFE   (out-of-process ptrace): AsmTrace.Window(() => SpawnThreadInWindow());
//
// examples/dotnet/localscope_oop_managed shows only the SURVIVING half; single-method only
// asserts the safe posture. Nothing else runs the FAILING half live and reads its exit code.
// This does. The fatal leg cannot run here — a force-kill would take THIS process with it — so
// it runs in a re-exec'd CHILD (Environment.ProcessPath + "--fatal-child") whose death is
// reaped as an exit code: 133 == 128 + SIGTRAP(5). The parent then runs the identical block
// in-process through AsmTrace.Window (single-stepped out of band, never TF-armed) and it does
// NOT crash — same code, opposite outcome.
//
// WHY the fatal leg dies: glibc pthread_create blocks SIGTRAP, so a TF-armed runtime thread
// that spawns a thread in-window takes a MASKED #DB and the kernel force-kills the process. An
// in-process whole-window single-step CANNOT survive it; a ptrace-stop is not gated by the
// tracee's signal mask, so the out-of-process form does. Self-skips (exit 0) where single-step
// or ptrace is unavailable.

using System;
using Asmtest;

internal static class Program
{
    // The hostile block, IDENTICAL in both legs: spawn a thread and join it inside the window.
    // The .Start() reaches glibc pthread_create, which blocks SIGTRAP — fatal to a TF-armed
    // thread, survivable to an out-of-band ptrace stepper. NoInlining so the shape is stable.
    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    static long SpawnThreadInWindow()
    {
        long acc = 0;
        var t = new System.Threading.Thread(() =>
        {
            for (int i = 0; i < 100_000; i++) acc += i;
        });
        t.Start();
        t.Join();
        return acc;
    }

    static int Main(string[] args)
    {
        // The re-exec'd child: run ONLY the fatal leg, then let the process die (or, if
        // single-step self-skipped here, exit 0 so the parent can say so honestly).
        if (System.Array.IndexOf(args, "--fatal-child") >= 0)
            return RunFatalChild();

        Console.WriteLine("== crashproof showdown: the SAME thread-spawning block, traced two ways ==\n");
        Console.WriteLine("FATAL  in-process  new AsmTrace()      (EFLAGS.TF — expected force-kill, exit 133)");
        Console.WriteLine("SAFE   out-of-proc AsmTrace.Window()   (ptrace stepper — expected to survive)\n");

        // Both legs are gated: the fatal leg needs the single-step tier to arm; the safe leg
        // needs the out-of-process ptrace stepper. Self-skip the WHOLE example if either is out.
        if (!HwTrace.Available(HwBackend.SingleStep))
        {
            Console.WriteLine($"# self-skip: single-step backend unavailable: {HwTrace.SkipReason(HwBackend.SingleStep)}");
            return 0;
        }
        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: out-of-process ptrace stepper unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        // ---- FATAL leg: re-exec self as the child, wait, read how it died. ----
        int childCode = RunFatalLegInChild();

        // Warm up thread spawning so that no JIT compilation occurs inside the trace window
        SpawnThreadInWindow();

        // ---- SAFE leg: the IDENTICAL block, in THIS process, stepped out of band. Cannot crash. ----
        AsmTrace w = AsmTrace.Window(() => SpawnThreadInWindow());  // already-closed; do NOT `using`

        // ---- INLINE OOP leg: the IDENTICAL block, using the new inline out-of-process using scope. Cannot crash. ----
        Console.WriteLine("\nINLINE OOP leg: using (new AsmTrace(outOfProcess: true)) (expected to survive)");
        using (var t = new AsmTrace(outOfProcess: true))
        {
            if (t.Armed)
            {
                SpawnThreadInWindow();
                Console.WriteLine($"   inline OOP scope: SURVIVED, captured {t.Addresses.Length} instructions"
                                + (t.Truncated ? " (truncated)" : "") + ".");
            }
            else
            {
                Console.WriteLine($"   inline OOP scope self-skipped: {t.SkipReason}");
            }
        }

        return Report.Print(childCode, w);
    }

    // Re-exec this same program with "--fatal-child" and reap its exit code. Because a force-kill
    // is asynchronous and takes the whole process, the failing leg MUST be a separate process so
    // its death becomes an observable exit code instead of ending the parent.
    static int RunFatalLegInChild()
    {
        var psi = new System.Diagnostics.ProcessStartInfo { UseShellExecute = false };

        string exe = Environment.ProcessPath;
        // When launched via `dotnet run`/`dotnet exec`, ProcessPath is the shared muxer — it
        // needs the managed entry .dll as its first arg. An apphost (bin/…/crashproof-showdown)
        // re-execs directly. Handle both so the child path is reached either way.
        if (exe != null && exe.EndsWith("dotnet", StringComparison.Ordinal))
        {
            psi.FileName = exe;
            psi.ArgumentList.Add(System.Reflection.Assembly.GetEntryAssembly().Location);
        }
        else
        {
            psi.FileName = exe ?? System.Reflection.Assembly.GetEntryAssembly().Location;
        }
        psi.ArgumentList.Add("--fatal-child");

        Console.WriteLine("-- FATAL leg: re-exec'ing self as --fatal-child (its death is reaped as an exit code) --");
        using (var child = System.Diagnostics.Process.Start(psi))
        {
            child.WaitForExit();
            return child.ExitCode;
        }
    }

    // The child. Arm the in-process whole-window single-step scope and run the hostile block.
    // This is the leg EXPECTED to die (exit 133). If single-step self-skips here, the block runs
    // uninstrumented and the child exits 0 — the parent interprets that honestly.
    static int RunFatalChild()
    {
        Console.WriteLine("   [child] arming in-process whole-window (new AsmTrace()) on this thread…");
        Console.Out.Flush();  // make the marker visible before a possible force-kill

        using (var t = new AsmTrace())  // whole-window, in-process, EFLAGS.TF-armed
        {
            if (!t.Armed)
            {
                Console.WriteLine($"   [child] # self-skip: {t.SkipReason}");
                Console.Out.Flush();
                return 0;  // no arm -> no TF -> the block below runs safely; parent reads 0
            }
            Console.WriteLine("   [child] armed — spawning a thread IN-WINDOW (glibc masks SIGTRAP → force-kill)…");
            Console.Out.Flush();
            SpawnThreadInWindow();
        }

        // Reaching here means we were NOT force-killed — the predicted fatal boundary did not
        // fire on this host. The parent reads exit 0 and reports that honestly.
        Console.WriteLine("   [child] survived (no force-kill) — closing the window normally.");
        Console.Out.Flush();
        return 0;
    }
}
