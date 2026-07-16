using System;
using Microsoft.Diagnostics.NETCore.Client;
// F4 increment 1 attach harness: attach the GC-move-feed profiler to an ALREADY-RUNNING dotnet over
// its diagnostics port (the dotnet-diagnostic-<pid> Unix socket) — the only route available to the
// ptrace live-attach tier, since CORECLR_ENABLE_PROFILING is read at startup and this tier attaches
// to processes it did not launch. Proven in f4-attach-profiler-probe-findings.md.
//
// ORDERING (a design input, not a nuisance): this must run BEFORE the ptrace tracer. The attach
// travels the diagnostics IPC socket, which the runtime must be RUNNING to service, whereas the
// tracer stops its target. A sibling of examples/gcfence_probe/attacher, kept separate so the
// landed probes are not perturbed.
//   usage: gccanon_attacher <pid> <clsid> <profiler .so path> [timeout seconds]
class Attacher {
    static int Main(string[] args) {
        if (args.Length < 3) {
            Console.WriteLine("ATTACHER: usage: gccanon_attacher <pid> <clsid> <so-path> [timeout-s]");
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
