using System;
using Microsoft.Diagnostics.NETCore.Client;
// F4 GC-fence probe attach harness: attach the measuring CLR profiler to an ALREADY-RUNNING dotnet
// over its diagnostics port (the dotnet-diagnostic-<pid> Unix socket) — the only route available to
// the ptrace live-attach tier, and already proven to work (f4-attach-profiler-probe-findings.md).
//
// ORDERING (a design input, not a nuisance): this must run BEFORE the ptrace stepper. The attach
// travels the diagnostics IPC socket, which the runtime must be RUNNING to service, whereas the
// stepper stops its target. A sibling of examples/attachprof_probe/attacher, kept separate so the
// landed probe is not perturbed.
//   usage: gcfence_attacher <pid> <clsid> <profiler .so path> [timeout seconds]
class Attacher {
    static int Main(string[] args) {
        if (args.Length < 3) {
            Console.WriteLine("ATTACHER: usage: gcfence_attacher <pid> <clsid> <so-path> [timeout-s]");
            return 2;
        }
        int pid = int.Parse(args[0]);
        Guid clsid = Guid.Parse(args[1]);
        string path = args[2];
        int timeoutS = args.Length > 3 && int.TryParse(args[3], out int t) ? t : 30;
        Console.WriteLine($"ATTACHER: AttachProfiler pid={pid} clsid={clsid:B} path={path} timeout={timeoutS}s");
        Console.Out.Flush();
        try {
            var client = new DiagnosticsClient(pid);
            client.AttachProfiler(TimeSpan.FromSeconds(timeoutS), clsid, path);
            Console.WriteLine("ATTACHER: AttachProfiler ACCEPTED (runtime returned success)");
            return 0;
        } catch (Exception e) {
            Console.WriteLine("ATTACHER: AttachProfiler FAILED: " + e.GetType().Name + ": " + e.Message);
            return 1;
        }
    }
}
