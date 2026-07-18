// .NET data-flow binding smoke (Phase 6 + F7): GC-move canonicalizer + method
// resolver, mirroring the other language suites — and (F7) a REAL live attach to a
// victim process by pid. P/Invoke with a DllImportResolver that maps the logical
// "asmtest_dataflow" to $ASMTEST_DATAFLOW_LIB (like the hwtrace binding).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

static class Program
{
    const string LIB = "asmtest_dataflow";

    [StructLayout(LayoutKind.Sequential)]
    struct GcMove
    {
        public ulong old_base, new_base, len;
        public uint step;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Method
    {
        public ulong addr, size;
        public IntPtr name;
        public ulong version;
    }

    [DllImport(LIB)]
    static extern ulong asmtest_gcmove_canon(GcMove[] moves, nuint nmoves, uint step, ulong phys);

    [DllImport(LIB)]
    static extern int asmtest_method_resolve_pc(Method[] methods, nuint nmethods, ulong pc);

    // --- F7: the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c) --- //
    // The producer ships NO header on purpose (a value-trace PRODUCER is a tier, not
    // part of the shared sink API), so — exactly as its own C suite does — this
    // binding re-declares them. Keep in step with that file. The L0 sink handle is
    // opaque (IntPtr); no struct crosses by value; C `long` is 64-bit on the Linux
    // x86-64 this tier runs on, so `result` is a long*, marshalled as `out long`.
    [DllImport(LIB)]
    static extern IntPtr asmtest_valtrace_new(nuint stepsCap, nuint recsCap, nuint wideCap);

    [DllImport(LIB)]
    static extern void asmtest_valtrace_free(IntPtr v);

    [DllImport(LIB)]
    static extern nuint asmtest_valtrace_steps(IntPtr v);

    [DllImport(LIB)]
    static extern nuint asmtest_valtrace_recs(IntPtr v);

    [DllImport(LIB)]
    static extern int asmtest_dataflow_ptrace_attach_pid(
        int pid, ulong baseAddr, nuint codeLen, ulong maxInsns, out long result, IntPtr vt);

    [DllImport(LIB)]
    static extern int asmtest_dataflow_ptrace_attach_pid_tid(
        int pid, int onlyTid, ulong baseAddr, nuint codeLen, ulong maxInsns,
        out long result, IntPtr vt);

    // Ten args: six in registers, four on the stack — a dropped or reordered one
    // lands garbage in baseAddr/pid, which the result/survived asserts below catch.
    [DllImport(LIB)]
    static extern int asmtest_dataflow_ptrace_attach_jit(
        int pid, int onlyTid, ulong baseAddr, nuint codeLen, IntPtr img, ulong when,
        ulong maxInsns, out long result, out int survived, IntPtr vt);

    // L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). The seed
    // crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed), which
    // sidesteps C#'s non-blittable-`bool` problem entirely: the seed and record
    // buffers are raw IntPtr into Marshal.AllocHGlobal, not a typed struct.
    [DllImport(LIB)]
    static extern void asmtest_valtrace_append(IntPtr v, ulong off, IntPtr recs, nuint n);

    [DllImport(LIB)]
    static extern IntPtr asmtest_defuse_build(IntPtr v);

    [DllImport(LIB)]
    static extern void asmtest_defuse_free(IntPtr g);

    [DllImport(LIB)]
    static extern IntPtr asmtest_slice_forward_seed(IntPtr g, IntPtr seed);

    [DllImport(LIB)]
    static extern IntPtr asmtest_slice_backward_seed(IntPtr g, IntPtr seed);

    [DllImport(LIB)]
    static extern void asmtest_slice_free(IntPtr s);

    [DllImport(LIB)]
    static extern int asmtest_slice_contains(IntPtr s, uint step);

    // One operand read/write record (include/asmtest_valtrace.h at_val_rec_t),
    // 72 bytes. Offsets verified via offsetof on this build.
    const int RecSize = 72;
    const int OffKind = 0;
    const int OffReg = 4;
    const int OffAddr = 32;
    const int OffIsWrite = 42;
    const int OffStep = 64;

    // at_loc_kind_t: the location space of an operand.
    const int LOC_REG = 0;     // a register (key = Capstone reg id)
    const int LOC_MEM_ABS = 1; // memory at an absolute effective address (key = addr)
    const int LOC_MEM_OFF = 2; // memory at a routine offset

    readonly struct Loc
    {
        public readonly int Kind;
        public readonly ulong Key;
        public Loc(int kind, ulong key) { Kind = kind; Key = key; }
    }

    static Loc Reg(ulong r) => new Loc(LOC_REG, r);
    static Loc Mem(ulong addr) => new Loc(LOC_MEM_ABS, addr);

    static IntPtr AllocZeroed(int size)
    {
        IntPtr p = Marshal.AllocHGlobal(size);
        for (int i = 0; i < size; i++) Marshal.WriteByte(p, i, 0);
        return p;
    }

    static void WriteRec(IntPtr buf, int off, Loc loc, bool isWrite)
    {
        Marshal.WriteInt32(buf, off + OffKind, loc.Kind);
        if (loc.Kind == LOC_REG) Marshal.WriteInt32(buf, off + OffReg, (int)loc.Key);
        else Marshal.WriteInt64(buf, off + OffAddr, (long)loc.Key);
        Marshal.WriteByte(buf, off + OffIsWrite, (byte)(isWrite ? 1 : 0));
    }

    // A hand-built (or live-attach-filled) L0 value trace plus its cached L1
    // def-use graph and L2 slicer — mirrors the Python/Ruby/Lua/Zig/Rust/Go/Java
    // ValueTrace.
    sealed class ValueTrace
    {
        public IntPtr V;
        IntPtr _g;
        int _nSteps;

        public ValueTrace(nuint stepsCap, nuint recsCap)
        {
            V = asmtest_valtrace_new(stepsCap, recsCap, 0);
            if (V == IntPtr.Zero) throw new InvalidOperationException("asmtest_valtrace_new failed");
        }

        // Append one executed instruction at offset `off` reading `reads` and
        // writing `writes` (each an array of Loc). Read-set before write-set.
        public void Step(ulong off, Loc[] reads, Loc[] writes)
        {
            int n = reads.Length + writes.Length;
            IntPtr recs = n > 0 ? AllocZeroed(RecSize * n) : IntPtr.Zero;
            try
            {
                int i = 0;
                foreach (var loc in reads) WriteRec(recs, (i++) * RecSize, loc, false);
                foreach (var loc in writes) WriteRec(recs, (i++) * RecSize, loc, true);
                asmtest_valtrace_append(V, off, recs, (nuint)n);
            }
            finally
            {
                if (recs != IntPtr.Zero) Marshal.FreeHGlobal(recs);
            }
            _nSteps++;
            InvalidateDefuse();
        }

        // A live producer appends behind our back (unlike Step, which counts as
        // it goes), so resync the step count and drop any stale def-use graph.
        public void PostAttach()
        {
            _nSteps = (int)asmtest_valtrace_steps(V);
            InvalidateDefuse();
        }

        void InvalidateDefuse()
        {
            if (_g != IntPtr.Zero)
            {
                asmtest_defuse_free(_g);
                _g = IntPtr.Zero;
            }
        }

        // The L1 last-writer def-use graph, built once and cached until the next
        // Step()/attach invalidates it.
        IntPtr Defuse()
        {
            if (_g == IntPtr.Zero)
            {
                _g = asmtest_defuse_build(V);
                if (_g == IntPtr.Zero) throw new InvalidOperationException("asmtest_defuse_build failed");
            }
            return _g;
        }

        int[] SliceSteps(uint origin, bool forward)
        {
            IntPtr g = Defuse();
            IntPtr seed = AllocZeroed(RecSize);
            try
            {
                Marshal.WriteInt32(seed, OffStep, (int)origin);
                IntPtr s = forward ? asmtest_slice_forward_seed(g, seed) : asmtest_slice_backward_seed(g, seed);
                var list = new List<int>();
                for (int i = 0; i < _nSteps; i++)
                    if (asmtest_slice_contains(s, (uint)i) != 0) list.Add(i);
                asmtest_slice_free(s);
                return list.ToArray();
            }
            finally
            {
                Marshal.FreeHGlobal(seed);
            }
        }

        // Steps influenced by the value defined at step `origin` (origin included).
        public int[] ForwardSlice(uint origin) => SliceSteps(origin, true);

        // Steps that produced the value used at step `sink` (sink included).
        public int[] BackwardSlice(uint sink) => SliceSteps(sink, false);

        public void Free()
        {
            if (V != IntPtr.Zero)
            {
                InvalidateDefuse();
                asmtest_valtrace_free(V);
                V = IntPtr.Zero;
            }
        }
    }

    // The producer's return codes, re-declared for the same reason.
    const int PTRACE_OK = 0;      // a complete scoped trace
    const int PTRACE_EINVAL = -1; // bad arguments
    const int PTRACE_ENOSYS = -3; // off Linux x86-64 / no Capstone: the tier is absent
    const int PTRACE_ETRACE = -4; // ptrace/wait failure (seccomp/yama)

    static int _n = 0;
    static bool _failed = false;

    static void Check(bool cond, string desc)
    {
        _n++;
        Console.WriteLine((cond ? "ok " : "not ok ") + _n + " - " + desc);
        if (!cond) _failed = true;
    }

    static ulong Gcmove(ulong[][] moves, uint step, ulong phys)
    {
        if (moves.Length == 0) return asmtest_gcmove_canon(null, 0, step, phys);
        var arr = new GcMove[moves.Length];
        for (int i = 0; i < moves.Length; i++)
            arr[i] = new GcMove { old_base = moves[i][0], new_base = moves[i][1], len = moves[i][2], step = (uint)moves[i][3] };
        return asmtest_gcmove_canon(arr, (nuint)arr.Length, step, phys);
    }

    static int MethodResolve((ulong addr, ulong size, string name, ulong ver)[] methods, ulong pc)
    {
        if (methods.Length == 0) return asmtest_method_resolve_pc(null, 0, pc);
        var arr = new Method[methods.Length];
        var strs = new List<IntPtr>();
        try
        {
            for (int i = 0; i < methods.Length; i++)
            {
                IntPtr name = Marshal.StringToHGlobalAnsi(methods[i].name);
                strs.Add(name);
                arr[i] = new Method { addr = methods[i].addr, size = methods[i].size, name = name, version = methods[i].ver };
            }
            return asmtest_method_resolve_pc(arr, (nuint)arr.Length, pc);
        }
        finally
        {
            foreach (var p in strs) Marshal.FreeHGlobal(p);
        }
    }

    static int Main()
    {
        NativeLibrary.SetDllImportResolver(typeof(Program).Assembly, (name, asm, path) =>
        {
            if (name == LIB)
            {
                var env = Environment.GetEnvironmentVariable("ASMTEST_DATAFLOW_LIB");
                if (!string.IsNullOrEmpty(env)) return NativeLibrary.Load(env);
            }
            return IntPtr.Zero;
        });

        // GC-move canonicalizer
        Check(Gcmove(new ulong[][] { }, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
        var mv = new ulong[][] { new ulong[] { 0x1000, 0x2000, 0x100, 5 } };
        Check(Gcmove(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
        Check(Gcmove(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
        Check(Gcmove(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
        Check(Gcmove(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded");
        Check(Gcmove(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
        Check(Gcmove(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
        var mv2 = new ulong[][] { new ulong[] { 0x1000, 0x2000, 0x100, 3 }, new ulong[] { 0x2000, 0x3000, 0x100, 6 } };
        Check(Gcmove(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

        // method resolver
        var ms = new (ulong, ulong, string, ulong)[] { (0x1000, 0x40, "Foo", 3), (0x2000, 0x20, "Bar", 1), (0x3000, 0, "Baz", 2) };
        Check(MethodResolve(ms, 0x1000) == 0, "method: Foo range start");
        Check(MethodResolve(ms, 0x103F) == 0, "method: Foo last byte (half-open)");
        Check(MethodResolve(ms, 0x1040) == -1, "method: one past Foo -> none");
        Check(MethodResolve(ms, 0x2010) == 1, "method: Bar range");
        Check(MethodResolve(ms, 0x3000) == 2, "method: Baz point match");
        Check(MethodResolve(ms, 0x3001) == -1, "method: Baz is point-only");
        var rj = new (ulong, ulong, string, ulong)[] { (0x1000, 0x40, "Foo", 1), (0x1000, 0x40, "Foo", 5) };
        Check(MethodResolve(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins");
        Check(MethodResolve(new (ulong, ulong, string, ulong)[] { }, 0x1000) == -1, "method: empty map -> -1");

        DefuseSliceSmoke();
        LiveAttachTests();

        Console.WriteLine("1.." + _n);
        return _failed ? 1 : 0;
    }

    // T2 — def-use/slice surface (by-pointer seed, src/dataflow.c). Pure C, no
    // ptrace, so it runs unconditionally. Not a counted TAP assertion (T3 adds
    // those, plus the live-captured memory-edge case) — this is a build-time
    // proof that the wrapper actually slices correctly, not just that it links.
    static void DefuseSliceSmoke()
    {
        var vt = new ValueTrace(64, 512);
        // A register chain r10 -> r11 -> r12 (mirrors the Python round-trip test).
        vt.Step(0, Array.Empty<Loc>(), new[] { Reg(10) });
        vt.Step(1, new[] { Reg(10) }, new[] { Reg(11) });
        vt.Step(2, new[] { Reg(11) }, new[] { Reg(12) });
        int[] fwd = vt.ForwardSlice(0);
        int[] bwd = vt.BackwardSlice(2);
        if (fwd.Length != 3 || bwd.Length != 3)
        {
            throw new InvalidOperationException(
                $"DefuseSliceSmoke: forward=[{string.Join(",", fwd)}] "
                + $"backward=[{string.Join(",", bwd)}], want len 3 each");
        }
        vt.Free();
    }

    // ----------------------------------------------------------------------
    // F7 — live-attach data flow: capture over a REAL attached pid.
    //
    // Every assertion is POSITIVE and keyed to something only a working capture can
    // produce (the region's return value, the exact step count, the survival report).
    // Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
    // signature, so a guard like that would skip exactly when it should shout.
    // ----------------------------------------------------------------------

    // ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
    // (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
    // PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
    static void CheckRc(int rc, string desc)
    {
        if (rc == PTRACE_ETRACE)
            Console.WriteLine("# " + desc + ": ptrace refused (ETRACE) — the lane needs "
                + "--cap-add=SYS_PTRACE; this is NOT a valid skip");
        Check(rc == PTRACE_OK, desc);
    }

    // A live victim: spawn it DETACHED, then learn its region base + its own pid from
    // the handshake file. a/b are OURS, so the expected result is a property of THIS
    // run, not a constant a stubbed wrapper could hardcode.
    //
    // DETACHED, for the same reason the Java lane is (see TestDataflow.java's Victim):
    // a managed runtime reaps its children from its OWN SIGCHLD machinery, and a
    // ptrace-stop of a traced child is reportable to any thread in the tracer's thread
    // group — so the runtime's reaper races the producer's waitpid, eats the stop, and
    // the producer blocks forever. `sh -c '... &'` backgrounds the victim and sh exits,
    // so the victim reparents to init and the runtime never waits on it. The attach
    // still works because the victim calls PR_SET_PTRACER_ANY, so Yama does not require
    // the tracer to be an ancestor. This is a real caveat for ANY managed host attaching
    // to its own child, not a quirk of the test.
    sealed class Victim
    {
        public ulong Base;
        public nuint Len;
        public int Pid;
        public string CounterPath;

        public Victim(string exe, string tag, int a, int b)
        {
            CounterPath = "/tmp/asmtest-df-dotnet-" + tag + ".counter";
            string hs = "/tmp/asmtest-df-dotnet-" + tag + ".hs";
            if (File.Exists(hs)) File.Delete(hs);
            var sh = new Process();
            sh.StartInfo.FileName = "/bin/sh";
            sh.StartInfo.ArgumentList.Add("-c");
            sh.StartInfo.ArgumentList.Add(exe + " " + CounterPath + " " + a + " " + b
                                          + " > " + hs + " 2>&1 &");
            sh.StartInfo.UseShellExecute = false;
            sh.Start();
            sh.WaitForExit(); // sh exits at once; the victim lives on, reparented to init
            string line = null;
            for (int i = 0; i < 500 && line == null; i++)
            {
                if (File.Exists(hs))
                {
                    string[] lines = File.ReadAllLines(hs);
                    if (lines.Length > 0) line = lines[0];
                }
                if (line == null) Thread.Sleep(10);
            }
            string[] f = (line ?? "").Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (f.Length != 3) throw new InvalidOperationException("victim handshake failed: " + line);
            Base = Convert.ToUInt64(f[0].Substring("base=0x".Length), 16);
            Len = (nuint)ulong.Parse(f[1].Substring("len=".Length));
            Pid = int.Parse(f[2].Substring("pid=".Length));
        }

        public ulong Counter()
        {
            byte[] b = File.ReadAllBytes(CounterPath);
            return b.Length < 8 ? 0 : BitConverter.ToUInt64(b, 0);
        }

        // GetProcessById reaches a non-child by pid; Kill sends SIGKILL via kill(2).
        public void Close()
        {
            try { Process.GetProcessById(Pid).Kill(); } catch { }
        }
    }

    static void LiveAttachTests()
    {
        // The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a
        // host the live tests MUST run: an unavailable tier there means the lib was
        // linked without Capstone — a build defect that has to be RED, not a skip.
        if (!(RuntimeInformation.IsOSPlatform(OSPlatform.Linux)
              && RuntimeInformation.ProcessArchitecture == Architecture.X64))
        {
            Console.WriteLine("# SKIP live-attach: not linux/x64 (the tier is Linux x86-64 only)");
            return;
        }
        string exe = Environment.GetEnvironmentVariable("ASMTEST_DATAFLOW_VICTIM");
        if (exe == null)
        {
            // The lane always exports this; missing means a misconfigured lane, and
            // silently skipping every live test is the hole this suite must not have.
            Console.WriteLine("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-dotnet-test`");
            Environment.Exit(1);
        }

        // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub) — the
        // P/Invoke binds either way, so only the return code tells them apart.
        {
            IntPtr v = asmtest_valtrace_new(1, 1, 0);
            int rc = asmtest_dataflow_ptrace_attach_pid(0, 0, 0, 0, out long _, v);
            asmtest_valtrace_free(v);
            Check(rc != PTRACE_ENOSYS, "live: tier is real on linux/x64 (EINVAL, not ENOSYS)");
        }

        {
            var vic = new Victim(exe, "1", 7, 5);
            IntPtr v = asmtest_valtrace_new(64, 512, 0);
            CheckRc(asmtest_dataflow_ptrace_attach_pid(vic.Pid, vic.Base, vic.Len, 0, out long result, v),
                "live: attach_pid a FOREIGN running pid + stepped the region");
            // The region really executed IN the victim: rax = rdi + rsi.
            Check(result == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)");
            // Exactly df_chain's six in-region instructions — not "some".
            Check(asmtest_valtrace_steps(v) == 6, "live: six in-region steps captured over the victim");
            Check(asmtest_valtrace_recs(v) > 0, "live: operand records captured");
            // SURVIVAL: we attached to a process we do not own; it must outlive the detach.
            ulong c0 = vic.Counter();
            Thread.Sleep(50);
            Check(vic.Counter() > c0, "live: victim SURVIVED the detach (counter advanced)");
            asmtest_valtrace_free(v);
            vic.Close();
        }
        {
            // THE anti-hardcode control: a second victim, different args, same wrapper.
            var vic = new Victim(exe, "2", 17, 25);
            IntPtr v = asmtest_valtrace_new(64, 512, 0);
            CheckRc(asmtest_dataflow_ptrace_attach_pid(vic.Pid, vic.Base, vic.Len, 0, out long result, v),
                "live: attach_pid the second victim");
            Check(result == 42, "live: result TRACKS the victim's args (17+25=42)");
            Check(asmtest_valtrace_steps(v) == 6, "live: six steps on the second victim too");
            asmtest_valtrace_free(v);
            vic.Close();
        }
        {
            var vic = new Victim(exe, "3", 9, 4);
            IntPtr v = asmtest_valtrace_new(64, 512, 0);
            // onlyTid 0: step whichever thread enters the region (here, the only one).
            CheckRc(asmtest_dataflow_ptrace_attach_pid_tid(vic.Pid, 0, vic.Base, vic.Len, 0, out long result, v),
                "live: attach_pid_tid stepped the entering thread");
            Check(result == 13, "live: attach_pid_tid region returned 13 (9+4)");
            Check(asmtest_valtrace_steps(v) == 6, "live: attach_pid_tid captured six steps");
            asmtest_valtrace_free(v);
            vic.Close();
        }
        {
            var vic = new Victim(exe, "4", 20, 3);
            IntPtr v = asmtest_valtrace_new(64, 512, 0);
            CheckRc(asmtest_dataflow_ptrace_attach_jit(vic.Pid, 0, vic.Base, vic.Len, IntPtr.Zero,
                    0, 0, out long result, out int survived, v),
                "live: attach_jit stepped the region");
            Check(result == 23, "live: attach_jit region returned 23 (20+3)");
            Check(asmtest_valtrace_steps(v) == 6, "live: attach_jit captured six steps");
            // The producer's OWN survival report — the house rule that a foreign
            // target is never killed, asserted from its side.
            Check(survived == 1, "live: attach_jit reported the target as survived");
            ulong c0 = vic.Counter();
            Thread.Sleep(50);
            Check(vic.Counter() > c0, "live: attach_jit victim kept running after detach");
            asmtest_valtrace_free(v);
            vic.Close();
        }
        {
            // Negative control: the wrapper must surface the producer's rejections
            // rather than manufacture success.
            IntPtr v = asmtest_valtrace_new(8, 8, 0);
            Check(asmtest_dataflow_ptrace_attach_pid(12345, 0x1000, 0, 0, out long _, v) == PTRACE_EINVAL,
                "live: zero-length region is rejected (EINVAL)");
            Check(asmtest_dataflow_ptrace_attach_pid(0, 0x1000, 21, 0, out long _, v) == PTRACE_EINVAL,
                "live: pid 0 is rejected (EINVAL)");
            Check(asmtest_dataflow_ptrace_attach_pid(0x7FFFFFF0, 0x1000, 21, 0, out long _, v) != PTRACE_OK,
                "live: attaching to a nonexistent pid never returns OK");
            asmtest_valtrace_free(v);
        }
    }
}
