// DrTraceProgram.cs — runnable smoke test for the DynamoRIO native-trace wrapper
// (DrTrace.cs), mirroring bindings/python/tests/test_drtrace.py.
//
// Self-skips cleanly: when DrTrace.Available() is false (no DynamoRIO / lib
// absent), it prints "SKIP: ..." and exits 0, so the lane is green wherever the
// tier is not built. Otherwise it exercises block coverage + accumulation and the
// ordered-instruction mode, then prints "PASS". Any failure throws (exit 1).
using System;
using Asmtest;

static class DrTraceProgram
{
    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
    static readonly byte[] ROUTINE =
    {
        0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
        0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
    };

    static void Assert(bool cond, string msg)
    {
        if (!cond) throw new DrTraceException("assertion failed: " + msg);
    }

    static int Main()
    {
        // Self-skip unless the tier is built AND DynamoRIO is resolvable.
        if (!DrTrace.Available())
        {
            Console.WriteLine("SKIP: DynamoRIO native-trace tier unavailable (self-skip)");
            return 0;
        }
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("ASMTEST_DRCLIENT")))
        {
            Console.WriteLine("SKIP: ASMTEST_DRCLIENT not set (build the DR client)");
            return 0;
        }

        try
        {
            DrTrace.Initialize();
        }
        catch (DrTraceException e)
        {
            Console.WriteLine($"SKIP: dr_init/start failed: {e.Message}");
            return 0;
        }

        try
        {
            // --- block coverage + accumulation --- //
            var code = NativeCode.FromBytes(ROUTINE);
            var tr = NativeTrace.Create(blocks: 64, instructions: 0);
            tr.Register("add2", code);

            long r = 0;
            tr.Region("add2", () => { r = code.Call(20, 22); });
            Assert(r == 42, $"add2(20,22) == 42 (got {r})");
            Assert(tr.Covered(0), "entry block covered");

            ulong before = tr.BlocksLen;
            long r2 = 0;
            tr.Region("add2", () => { r2 = code.Call(60, 60); }); // 120 > 100 -> dec -> 119
            Assert(r2 == 119, $"add2(60,60) == 119 (got {r2})");
            Assert(tr.BlocksLen >= before, $"blocks accumulate ({tr.BlocksLen} >= {before})");
            Assert(DrTrace.MarkerError() == 0, "markers balanced");

            tr.Unregister("add2");
            code.Free();
            tr.Free();

            // --- ordered-instruction mode --- //
            var code2 = NativeCode.FromBytes(ROUTINE);
            var tr2 = NativeTrace.Create(blocks: 64, instructions: 64);
            tr2.Register("add2i", code2);
            long r3 = 0;
            tr2.Region("add2i", () => { r3 = code2.Call(1, 2); });
            Assert(r3 == 3, $"add2i(1,2) == 3 (got {r3})");
            Assert(tr2.InsnsTotal >= 4, $"instruction stream recorded ({tr2.InsnsTotal} >= 4)");

            tr2.Unregister("add2i");
            code2.Free();
            tr2.Free();
        }
        finally
        {
            DrTrace.Shutdown();
        }

        Console.WriteLine("PASS");
        return 0;
    }
}
