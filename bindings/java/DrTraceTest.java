/*
 * DrTraceTest.java — standalone smoke test for the DynamoRIO native-trace
 * binding (DrTrace.java), mirroring bindings/python/tests/test_drtrace.py.
 *
 * Self-skips unless the tier is built AND DynamoRIO is resolvable — i.e. unless
 * ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT (and ASMTEST_DR_LIB or DYNAMORIO_HOME)
 * point at a built libasmtest_drapp + client on a DynamoRIO-capable Linux
 * x86-64 host. On a skip it prints "SKIP: ..." and exits 0.
 *
 * Asserts are NOT used (the `assert` keyword is off by default); failures throw
 * or System.exit nonzero explicitly. On success it prints "PASS".
 *
 * Compile with `--release 22`, run with
 * `--enable-native-access=ALL-UNNAMED`.
 */
public final class DrTraceTest {

    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
    private static final byte[] ROUTINE = {
        0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, 0x48, 0x3D,
        0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, (byte) 0xFF, (byte) 0xC8, (byte) 0xC3
    };

    private static void check(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    public static void main(String[] args) {
        if (!DrTrace.available()) {
            Throwable e = DrTrace.loadError();
            System.out.println("SKIP: DynamoRIO native-trace tier unavailable (self-skip)"
                + (e != null ? ": " + e : ""));
            System.exit(0);
        }

        DrTrace.initialize(); // null client → C falls back to $ASMTEST_DRCLIENT
        try {
            blockCoverageAndAccumulation();
            instructionMode();
            symbolMode();
        } finally {
            DrTrace.shutdown();
        }
        System.out.println("PASS");
    }

    // Mirrors test_block_coverage_and_accumulation.
    private static void blockCoverageAndAccumulation() {
        DrTrace.NativeCode code = DrTrace.NativeCode.fromBytes(ROUTINE);
        DrTrace.NativeTrace tr = DrTrace.NativeTrace.create(64, 0);
        tr.register("add2", code);

        long[] r = new long[1];
        tr.region("add2", () -> r[0] = code.call(20, 22));
        check(r[0] == 42, "call(20,22): got " + r[0] + ", want 42");
        check(tr.covered(0), "entry block (offset 0) expected covered");

        long before = tr.blocksLen();
        tr.region("add2", () -> r[0] = code.call(60, 60)); // 120 > 100 → dec → 119, other block
        check(r[0] == 119, "call(60,60): got " + r[0] + ", want 119");
        check(tr.blocksLen() >= before, "blocksLen did not grow: " + tr.blocksLen() + " < " + before);
        check(DrTrace.markerError() == 0, "markerError: " + DrTrace.markerError());

        tr.unregister("add2");
        code.free();
        tr.free();
    }

    // Mirrors test_instruction_mode.
    private static void instructionMode() {
        DrTrace.NativeCode code = DrTrace.NativeCode.fromBytes(ROUTINE);
        DrTrace.NativeTrace tr = DrTrace.NativeTrace.create(64, 64);
        tr.register("add2i", code);

        long[] r = new long[1];
        tr.region("add2i", () -> r[0] = code.call(1, 2));
        check(r[0] == 3, "call(1,2): got " + r[0] + ", want 3");
        check(tr.insnsTotal() >= 4, "insnsTotal: got " + tr.insnsTotal() + ", want >= 4");

        long[] insns = tr.insnOffsets();
        long[] wantInsns = {0x0, 0x3, 0x6, 0xc, 0x11};
        check(java.util.Arrays.equals(insns, wantInsns),
            "insnOffsets: got " + java.util.Arrays.toString(insns)
                + ", want " + java.util.Arrays.toString(wantInsns));

        long[] blocks = tr.blockOffsets();
        boolean hasZero = false;
        for (long b : blocks) if (b == 0) { hasZero = true; break; }
        check(hasZero, "blockOffsets: " + java.util.Arrays.toString(blocks) + " does not contain 0");

        tr.unregister("add2i");
        code.free();
        tr.free();
    }

    // Mirrors test_symbol_mode: trace an exported function by name, no markers.
    private static void symbolMode() {
        DrTrace.NativeTrace tr = DrTrace.NativeTrace.create(64, 0);
        tr.registerSymbol("asmtest_symbol_demo", 256);

        long r = DrTrace.symbolDemo(3, 4); // no region/markers — always-on recording
        check(r == 10, "symbolDemo(3,4): got " + r + ", want 10");
        check(tr.covered(0), "entry block (offset 0) expected covered");
        check(DrTrace.markerError() == 0, "markerError: " + DrTrace.markerError());

        tr.unregister("asmtest_symbol_demo");
        tr.free();
    }
}
