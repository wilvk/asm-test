// .NET data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the other language suites. P/Invoke with a DllImportResolver that maps the
// logical "asmtest_dataflow" to $ASMTEST_DATAFLOW_LIB (like the hwtrace binding).
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

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

        Console.WriteLine("1.." + _n);
        return _failed ? 1 : 0;
    }
}
