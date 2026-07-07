// examples/dotnet/descent_dotnet — call descent against a LIVE .NET runtime, out of process.
//
//   dotnet jit_dotnet.dll chain  (child; DOTNET_TieredCompilation=0 DOTNET_PerfMapEnabled=1) spins
//   on Program::Chain, which calls Program::Leaf twice — both JIT'd at stable, perf-map-resolvable
//   addresses. This attaches, RunTo(Chain), and TraceAttachedEx(Chain region) with a Descent that
//   allow-lists Leaf, so the stepper steps INTO Leaf as a nested frame.
//
// The out-of-process managed counterpart of the native `descent` example: an EXACT nested call
// tree (self-vs-inclusive counts) reconstructed while the method runs in ANOTHER, GC'd,
// multi-threaded CoreCLR — the scenario in-process single-step is forbidden to do.
//
// CRITICAL (same as ptrace_dotnet): the child is launched with libc posix_spawn + reaped with raw
// waitpid, NOT System.Diagnostics.Process — .NET's Process installs a SIGCHLD reaper that races
// the ptrace waitpid and hangs the attach. All ptrace calls run on ONE dedicated OS thread with a
// kill-based watchdog. Self-skips (exit 0) where ptrace is denied / the SDK or methods are absent.

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Asmtest;

internal static class Program
{
    const long PTRACE_ATTACH = 16, PTRACE_DETACH = 17;
    const int WNOHANG = 1, SIGKILL = 9;
    [DllImport("libc", SetLastError = true)] static extern long ptrace(long request, int pid, IntPtr addr, IntPtr data);
    [DllImport("libc", SetLastError = true)] static extern int waitpid(int pid, out int status, int options);
    [DllImport("libc", SetLastError = true)] static extern int kill(int pid, int sig);
    [DllImport("libc", SetLastError = true)]
    static extern int posix_spawnp(out int pid, string file, IntPtr fileActions, IntPtr attrp, IntPtr argv, IntPtr envp);

    static int Main()
    {
        Console.WriteLine("== call descent against a live .NET runtime (Chain -> Leaf, out of process) ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        string csproj = FindRepoFile("examples/dotnet/jit_dotnet/jit_dotnet.csproj");
        if (csproj == null) { Console.WriteLine("# self-skip: jit_dotnet project not found"); return 0; }
        string dll = Build(csproj);
        if (dll == null) { Console.WriteLine("# self-skip: could not build jit_dotnet"); return 0; }

        var env = new List<string>();
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
        {
            string k = (string)e.Key;
            if (k == "DOTNET_TieredCompilation" || k == "DOTNET_TC_QuickJitForLoops" || k == "DOTNET_PerfMapEnabled") continue;
            env.Add($"{k}={e.Value}");
        }
        env.Add("DOTNET_TieredCompilation=0");    // JIT Chain + Leaf once, at stable addresses
        env.Add("DOTNET_TC_QuickJitForLoops=0");
        env.Add("DOTNET_PerfMapEnabled=1");        // write /tmp/perf-<pid>.map
        int pid = Spawn(new[] { "dotnet", dll, "chain" }, env.ToArray());
        if (pid <= 0) { Console.WriteLine("# self-skip: could not spawn jit_dotnet"); return 0; }

        try
        {
            // Poll the perf-map until BOTH Chain and Leaf are JIT'd + written (up to ~15s).
            string chainFull = null, leafFull = null;
            (IntPtr Base, nuint Len)? chain = null, leaf = null;
            bool exited = false;
            for (int i = 0; i < 150; i++)
            {
                Thread.Sleep(100);
                if (waitpid(pid, out _, WNOHANG) == pid) { exited = true; break; }
                chainFull ??= FullSymbol(pid, "Program::Chain");
                leafFull ??= FullSymbol(pid, "Program::Leaf");
                if (chainFull != null) chain ??= Ptrace.ProcPerfmapSymbol(pid, chainFull);
                if (leafFull != null) leaf ??= Ptrace.ProcPerfmapSymbol(pid, leafFull);
                if (chain.HasValue) break;   // Chain is the region we MUST have; Leaf is best-effort
            }
            if (exited) { Console.WriteLine("# self-skip: jit_dotnet exited early (SDK missing?)"); pid = 0; return 0; }
            if (!chain.HasValue) { Console.WriteLine("# self-skip: Program::Chain not found in the child perf-map"); return 0; }

            Console.WriteLine($"resolved '{chainFull}'\n  @ 0x{chain.Value.Base.ToInt64():x} ({chain.Value.Len} bytes)"
                              + (leaf.HasValue ? $"; callee '{leafFull}' @ 0x{leaf.Value.Base.ToInt64():x} ({leaf.Value.Len} bytes)"
                                               : "; Leaf not resolved (edges only)")
                              + $" in live pid {pid}.\n");

            var tr = HwTrace.Create(blocks: 128, instructions: 1024);
            // DescendKnown + allow(Leaf) descends into Leaf as a frame; without Leaf, RecordEdges
            // still captures the (Chain -> callee) edges.
            var d = new Descent(leaf.HasValue ? DescentLevel.DescendKnown : DescentLevel.RecordEdges);
            d.SetMaxDepth(8);
            if (leaf.HasValue) d.AllowRegion(leaf.Value.Base, leaf.Value.Len);

            long result = 0; string fail = null; bool ok = false;
            int ttid = pid; // Main runs Chain on the main thread
            IntPtr cbase = chain.Value.Base; nuint clen = chain.Value.Len;
            var worker = new Thread(() =>
            {
                bool attached = false;
                try
                {
                    if (ptrace(PTRACE_ATTACH, ttid, IntPtr.Zero, IntPtr.Zero) != 0)
                    { fail = "PTRACE_ATTACH denied (yama ptrace_scope?)"; return; }
                    if (waitpid(ttid, out _, 0) < 0) { fail = "waitpid after attach failed"; return; }
                    attached = true;
                    int rrc = Ptrace.RunTo(ttid, cbase);
                    if (rrc != 0) { fail = $"run_to failed (rc={rrc}) — the address may have moved/re-tiered"; return; }
                    result = Ptrace.TraceAttachedEx(ttid, cbase, clen, tr.Handle, d);
                    ok = true;
                }
                catch (Exception e) { fail = e.Message; }
                finally { if (attached) ptrace(PTRACE_DETACH, ttid, IntPtr.Zero, IntPtr.Zero); }
            }) { IsBackground = true };
            worker.Start();
            if (!worker.Join(TimeSpan.FromSeconds(12)))
            {
                kill(pid, SIGKILL);
                worker.Join(2000);
                fail ??= "watchdog timeout";
            }

            if (!ok) { Console.WriteLine($"# self-skip: {fail}"); d.Dispose(); tr.Free(); return 0; }
            Report.Print(tr, d, result, chain.Value.Base, leaf);
            d.Dispose();
            tr.Free();
            return 0;
        }
        finally
        {
            if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, out _, 0); }
        }
    }

    static int Spawn(string[] argv, string[] envp)
    {
        IntPtr av = BuildCArray(argv), ev = BuildCArray(envp);
        try { return posix_spawnp(out int pid, argv[0], IntPtr.Zero, IntPtr.Zero, av, ev) == 0 ? pid : -1; }
        catch { return -1; }
        finally { FreeCArray(av, argv.Length); FreeCArray(ev, envp.Length); }
    }

    static IntPtr BuildCArray(string[] items)
    {
        IntPtr arr = Marshal.AllocHGlobal((items.Length + 1) * IntPtr.Size);
        for (int i = 0; i < items.Length; i++)
            Marshal.WriteIntPtr(arr, i * IntPtr.Size, Marshal.StringToHGlobalAnsi(items[i]));
        Marshal.WriteIntPtr(arr, items.Length * IntPtr.Size, IntPtr.Zero);
        return arr;
    }

    static void FreeCArray(IntPtr arr, int n)
    {
        for (int i = 0; i < n; i++) Marshal.FreeHGlobal(Marshal.ReadIntPtr(arr, i * IntPtr.Size));
        Marshal.FreeHGlobal(arr);
    }

    // The full perf-map symbol text whose name contains `substr` (last match = current body).
    static string FullSymbol(int pid, string substr)
    {
        string mp = $"/tmp/perf-{pid}.map";
        if (!File.Exists(mp)) return null;
        string best = null;
        try
        {
            foreach (string line in File.ReadLines(mp))
            {
                int a = line.IndexOf(' '); if (a < 0) continue;
                int b = line.IndexOf(' ', a + 1); if (b < 0) continue;
                string sym = line.Substring(b + 1).TrimEnd();
                if (sym.IndexOf(substr, StringComparison.Ordinal) >= 0) best = sym;
            }
        }
        catch { return null; }
        return best;
    }

    static string FindRepoFile(string rel)
    {
        string dir = AppContext.BaseDirectory;
        for (int i = 0; i < 12 && !string.IsNullOrEmpty(dir); i++)
        {
            string cand = Path.Combine(dir, rel.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(cand)) return cand;
            dir = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar));
        }
        return null;
    }

    // Build the jit_dotnet target once to a temp dir via posix_spawn (no Process -> no SIGCHLD
    // reaper). Bare console app -> offline restore from the SDK's bundled packs.
    static string Build(string csproj)
    {
        string outDir = Path.Combine(Path.GetTempPath(), "descent_dotnet_jd");
        var env = new List<string>();
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
            env.Add($"{e.Key}={e.Value}");
        string cmd = $"dotnet build \"{csproj}\" -c Release -o \"{outDir}\" >/dev/null 2>&1";
        int bpid = Spawn(new[] { "sh", "-c", cmd }, env.ToArray());
        if (bpid <= 0) return null;
        for (int i = 0; i < 1200; i++) { if (waitpid(bpid, out _, WNOHANG) == bpid) break; Thread.Sleep(100); }
        string dll = Path.Combine(outDir, "jit_dotnet.dll");
        return File.Exists(dll) ? dll : null;
    }
}
