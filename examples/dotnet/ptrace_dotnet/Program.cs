// examples/dotnet/ptrace_dotnet — attach to a LIVE .NET runtime in ANOTHER process and single-
// step one real JIT'd method. The headline out-of-process capability no in-process AsmTrace
// example can touch: the traced method runs in a separate, GC'd, multi-threaded CoreCLR — the
// exact scenario in-process single-step is forbidden to do (the managed single-step footgun).
//
//   dotnet jit_dotnet.dll  (child; DOTNET_TieredCompilation=0 DOTNET_PerfMapEnabled=1) spins on
//                          Program::Add, so it JITs at a stable address written to the perf-map.
//   Ptrace.ProcPerfmapSymbol -> resolve Add ; PTRACE_ATTACH ; RunTo(entry) ; TraceAttached.
//
// CRITICAL: the child is launched with libc posix_spawn + reaped with raw waitpid, NOT
// System.Diagnostics.Process — .NET's Process installs a SIGCHLD child-reaper that races the
// ptrace waitpid and would hang the attach. All ptrace calls run on ONE dedicated OS thread; a
// kill-based watchdog bounds it so a moved/re-tiered address self-skips, never hangs. Self-skips
// (exit 0) where ptrace is denied / the SDK or method is absent. Flow proven in C by jit_trace.c.

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
        Console.WriteLine("== attach to a live .NET runtime and single-step a real JIT method ==\n");

        if (!Ptrace.Available())
        {
            Console.WriteLine($"# self-skip: ptrace unavailable: {Ptrace.SkipReason()}");
            return 0;
        }

        string csproj = FindRepoFile("examples/dotnet/jit_dotnet/jit_dotnet.csproj");
        if (csproj == null) { Console.WriteLine("# self-skip: jit_dotnet project not found"); return 0; }
        string dll = Build(csproj);
        if (dll == null) { Console.WriteLine("# self-skip: could not build jit_dotnet"); return 0; }

        // Launch the runtime WITHOUT System.Diagnostics.Process (its SIGCHLD reaper would race the
        // ptrace waitpid). posix_spawn + our own kill/waitpid keeps the child status ours alone.
        var env = new List<string>();
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
        {
            string k = (string)e.Key;
            if (k == "DOTNET_TieredCompilation" || k == "DOTNET_TC_QuickJitForLoops" || k == "DOTNET_PerfMapEnabled") continue;
            env.Add($"{k}={e.Value}");
        }
        env.Add("DOTNET_TieredCompilation=0");   // JIT Add once, at a stable address
        env.Add("DOTNET_TC_QuickJitForLoops=0");
        env.Add("DOTNET_PerfMapEnabled=1");       // write /tmp/perf-<pid>.map
        int pid = Spawn(new[] { "dotnet", dll }, env.ToArray());
        if (pid <= 0) { Console.WriteLine("# self-skip: could not spawn jit_dotnet"); return 0; }

        try
        {
            // Poll the child's perf-map until Program::Add is JIT'd + written (up to ~15s). The
            // library's ProcPerfmapSymbol does an EXACT match on the full symbol text, so first
            // read the map to find the full "int32 [jit_dotnet] Program::Add(...)[tier]" line.
            string full = null;
            (IntPtr Base, nuint Len)? sym = null;
            bool exited = false;
            for (int i = 0; i < 150; i++)
            {
                Thread.Sleep(100);
                if (waitpid(pid, out _, WNOHANG) == pid) { exited = true; break; }
                full = FullSymbol(pid, "Program::Add");
                if (full != null) { sym = Ptrace.ProcPerfmapSymbol(pid, full); if (sym.HasValue) break; }
            }
            if (exited) { Console.WriteLine("# self-skip: jit_dotnet exited early (SDK missing?)"); pid = 0; return 0; }
            if (!sym.HasValue) { Console.WriteLine("# self-skip: Program::Add not found in the child perf-map"); return 0; }

            var region = Ptrace.ProcRegionByAddr(pid, sym.Value.Base);
            Console.WriteLine($"resolved '{full}'\n  @ 0x{sym.Value.Base.ToInt64():x} ({sym.Value.Len} bytes) in "
                              + $"live pid {pid}" + (region.HasValue ? " (in an executable mapping)" : "") + ".\n");

            // Attach + run_to + trace, all on ONE dedicated OS thread; kill-timeout is the watchdog.
            var tr = HwTrace.Create(blocks: 128, instructions: 512);
            long result = 0; string fail = null; bool ok = false; byte[] codeBytes = null;
            int ttid = pid; // ttid == pid for CoreCLR (Main runs Add on the main thread)
            IntPtr mbase = sym.Value.Base; nuint mlen = sym.Value.Len;
            var worker = new Thread(() =>
            {
                bool attached = false;
                try
                {
                    if (ptrace(PTRACE_ATTACH, ttid, IntPtr.Zero, IntPtr.Zero) != 0)
                    { fail = "PTRACE_ATTACH denied (yama ptrace_scope?)"; return; }
                    if (waitpid(ttid, out _, 0) < 0) { fail = "waitpid after attach failed"; return; }
                    attached = true;
                    int rrc = Ptrace.RunTo(ttid, mbase);
                    if (rrc != 0) { fail = $"run_to failed (rc={rrc}) — the address may have moved/re-tiered"; return; }
                    result = Ptrace.TraceAttached(ttid, mbase, mlen, tr.Handle); // throws on a trace error
                    ok = true;
                    try { codeBytes = ReadProcMem(pid, mbase.ToInt64(), (int)mlen); } catch { }
                }
                catch (Exception e) { fail = e.Message; }
                finally { if (attached) ptrace(PTRACE_DETACH, ttid, IntPtr.Zero, IntPtr.Zero); }
            }) { IsBackground = true };
            worker.Start();
            if (!worker.Join(TimeSpan.FromSeconds(12)))
            {
                kill(pid, SIGKILL); // unblocks any waitpid inside run_to/trace_attached
                worker.Join(2000);
                fail ??= "watchdog timeout";
            }

            if (!ok) { Console.WriteLine($"# self-skip: {fail}"); return 0; }
            Report.Print(tr, result, mbase, codeBytes);
            tr.Free();
            return 0;
        }
        finally
        {
            if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, out _, 0); }
        }
    }

    // posix_spawnp a child, argv/envp as NULL-terminated C arrays; returns the pid (or -1).
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
        string outDir = Path.Combine(Path.GetTempPath(), "ptrace_dotnet_jd");
        var env = new List<string>();
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
            env.Add($"{e.Key}={e.Value}");
        // Via `sh -c … >/dev/null` so the build's own output does not clutter the example.
        string cmd = $"dotnet build \"{csproj}\" -c Release -o \"{outDir}\" >/dev/null 2>&1";
        int bpid = Spawn(new[] { "sh", "-c", cmd }, env.ToArray());
        if (bpid <= 0) return null;
        for (int i = 0; i < 1200; i++) { if (waitpid(bpid, out _, WNOHANG) == bpid) break; Thread.Sleep(100); }
        string dll = Path.Combine(outDir, "jit_dotnet.dll");
        return File.Exists(dll) ? dll : null;
    }

    static byte[] ReadProcMem(int pid, long addr, int len)
    {
        using var fs = new FileStream($"/proc/{pid}/mem", FileMode.Open, FileAccess.Read);
        fs.Seek(addr, SeekOrigin.Begin);
        var buf = new byte[len];
        int got = 0, r;
        while (got < len && (r = fs.Read(buf, got, len - got)) > 0) got += r;
        if (got < len) Array.Resize(ref buf, got);
        return buf;
    }
}
