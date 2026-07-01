/*
 * Hot.java — the live-trace target for the HotSpot lane of jit_trace.c.
 *
 * A uniquely-named static method (asmtjit) kept hot in a tight loop so HotSpot's C2
 * compiles it to a standalone optimized nmethod at a stable address. The harness runs
 * the JVM with -XX:-TieredCompilation (one C2 body, no tier churn) and
 * -XX:CompileCommand=dontinline,Hot.asmtjit (so the body stays a real, separately
 * callable nmethod the run_to breakpoint can land on), then dumps the perf-map with
 * `jcmd <pid> Compiler.perfmap`, attaches, and single-steps one invocation.
 *
 * `static` matters: with no receiver there is no inline-cache check, so the method's
 * verified entry sits at the nmethod's code_begin — exactly the address the perf-map
 * reports — which is where the harness sets its breakpoint.
 *
 * Run with the "bcl" argument (the java-bcl lane) to instead keep a real JDK LIBRARY
 * method hot — java.lang.Math.floorDiv — see main() below.
 */
public class Hot {
    static int asmtjit(int a, int b) { return a + b; }

    public static void main(String[] args) {
        // "bcl": trace a real JDK library method (java.lang.Math.floorDiv) rather than user
        // code. The contrast with .NET is the point — HotSpot JITs JDK methods on demand BY
        // DEFAULT (no ReadyToRun-style precompilation to disable), so once this loop makes
        // floorDiv C2-hot it resolves in Compiler.perfmap and single-steps like asmtjit. The
        // harness adds -XX:CompileCommand=dontinline,java/lang/Math.floorDiv to keep it a
        // standalone nmethod; floorDiv allocates nothing, so the GC never runs mid-trace.
        if (args.length > 0 && args[0].equals("bcl")) {
            int s = 1;
            for (int i = 1; ; i++) {
                s += Math.floorDiv(i, 3);
                if (s > 1_000_000) s = 1; // keep it int32 and bounded; never exits on its own
            }
        }

        int s = 0;
        // Run forever; the harness kills us once it has traced one invocation.
        for (;;) {
            s = asmtjit(s, 1);
            if (s > 1_000_000) s = 0; // keep it int32 and bounded; never exits on its own
        }
    }
}
