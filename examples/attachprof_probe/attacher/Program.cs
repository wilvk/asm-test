using System;
using Microsoft.Diagnostics.NETCore.Client;
// F4 attach harness: attach a CLR profiler to an ALREADY-RUNNING dotnet process over its
// diagnostics port (the dotnet-diagnostic-<pid> Unix socket), which is the only route available
// to the ptrace live-attach tier — CORECLR_ENABLE_PROFILING is startup-read and the tier does not
// launch its targets.
//
// DiagnosticsClient.AttachProfiler sends the IPC AttachProfiler command; the target runtime then
// dlopen()s the .so, calls DllGetClassObject, and drives ICorProfilerCallback3::InitializeForAttach.
// Everything interesting is logged by the profiler INSIDE the victim (see attachprof.cpp) — this
// program only reports whether the IPC command itself was accepted, and its hr/exception verbatim.
//   usage: attachprof_attacher <pid> <clsid> <profiler .so path> [timeout seconds]
class Attacher {
    static int Main(string[] args) {
        if (args.Length < 3) {
            Console.WriteLine("ATTACHER: usage: attachprof_attacher <pid> <clsid> <so-path> [timeout-s]");
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
            // ServerErrorException carries the runtime's rejection (e.g. profiler already attached,
            // or a CORPROF_E_* hr); print type + message so the driver can classify the kill.
            Console.WriteLine("ATTACHER: AttachProfiler FAILED: " + e.GetType().Name + ": " + e.Message);
            return 1;
        }
    }
}
