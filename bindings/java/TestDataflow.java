// Java data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the other language suites. Project Panama FFM (final since JDK 22): compile
// with `--release 22`, run with `--enable-native-access=ALL-UNNAMED`.
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

public class TestDataflow {
    static int n = 0;
    static boolean failed = false;

    static void check(boolean cond, String desc) {
        n++;
        System.out.println((cond ? "ok " : "not ok ") + n + " - " + desc);
        if (!cond) failed = true;
    }

    static MethodHandle canon;
    static MethodHandle resolve;
    static Arena arena;

    // gcmove: {u64 old, new, len; u32 step} = 32 bytes. `moves` rows are [old,new,len,step].
    static long gcmove(long[][] moves, int step, long phys) throws Throwable {
        MemorySegment seg = MemorySegment.NULL;
        if (moves.length > 0) {
            seg = arena.allocate(32L * moves.length);
            for (int i = 0; i < moves.length; i++) {
                long b = i * 32L;
                seg.set(JAVA_LONG, b, moves[i][0]);
                seg.set(JAVA_LONG, b + 8, moves[i][1]);
                seg.set(JAVA_LONG, b + 16, moves[i][2]);
                seg.set(JAVA_INT, b + 24, (int) moves[i][3]);
            }
        }
        return (long) canon.invoke(seg, (long) moves.length, step, phys);
    }

    // method: {u64 addr, size; const char* name; u64 version} = 32 bytes.
    static int method(Object[][] methods, long pc) throws Throwable {
        MemorySegment seg = MemorySegment.NULL;
        if (methods.length > 0) {
            seg = arena.allocate(32L * methods.length);
            for (int i = 0; i < methods.length; i++) {
                long b = i * 32L;
                MemorySegment name = arena.allocateFrom((String) methods[i][2]);
                seg.set(JAVA_LONG, b, (long) methods[i][0]);
                seg.set(JAVA_LONG, b + 8, (long) methods[i][1]);
                seg.set(ADDRESS, b + 16, name);
                seg.set(JAVA_LONG, b + 24, (long) methods[i][3]);
            }
        }
        return (int) resolve.invoke(seg, (long) methods.length, pc);
    }

    public static void main(String[] args) throws Throwable {
        String path = System.getenv("ASMTEST_DATAFLOW_LIB");
        if (path == null) path = "build/libasmtest_dataflow.so";
        arena = Arena.ofConfined();
        Linker linker = Linker.nativeLinker();
        SymbolLookup lookup = SymbolLookup.libraryLookup(path, arena);
        canon = linker.downcallHandle(lookup.find("asmtest_gcmove_canon").get(),
                FunctionDescriptor.of(JAVA_LONG, ADDRESS, JAVA_LONG, JAVA_INT, JAVA_LONG));
        resolve = linker.downcallHandle(lookup.find("asmtest_method_resolve_pc").get(),
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_LONG));

        // GC-move canonicalizer
        check(gcmove(new long[][] {}, 0, 0x1234L) == 0x1234L, "gcmove: empty move set is identity");
        long[][] mv = { { 0x1000, 0x2000, 0x100, 5 } };
        check(gcmove(mv, 3, 0x1010L) == 0x2010L, "gcmove: pre-move addr forwards to final");
        check(gcmove(mv, 3, 0x1000L) == 0x2000L, "gcmove: object base forwards");
        check(gcmove(mv, 3, 0x10FFL) == 0x20FFL, "gcmove: last byte of half-open window forwards");
        check(gcmove(mv, 3, 0x1100L) == 0x1100L, "gcmove: one past the window not forwarded");
        check(gcmove(mv, 5, 0x1010L) == 0x1010L, "gcmove: at-move-step observation not forwarded");
        check(gcmove(mv, 3, 0x3000L) == 0x3000L, "gcmove: out-of-range addr unchanged");
        long[][] mv2 = { { 0x1000, 0x2000, 0x100, 3 }, { 0x2000, 0x3000, 0x100, 6 } };
        check(gcmove(mv2, 1, 0x1010L) == 0x3010L, "gcmove: two compactions compose to final");

        // method resolver
        Object[][] ms = { { 0x1000L, 0x40L, "Foo", 3L }, { 0x2000L, 0x20L, "Bar", 1L }, { 0x3000L, 0L, "Baz", 2L } };
        check(method(ms, 0x1000L) == 0, "method: Foo range start");
        check(method(ms, 0x103FL) == 0, "method: Foo last byte (half-open)");
        check(method(ms, 0x1040L) == -1, "method: one past Foo -> none");
        check(method(ms, 0x2010L) == 1, "method: Bar range");
        check(method(ms, 0x3000L) == 2, "method: Baz point match");
        check(method(ms, 0x3001L) == -1, "method: Baz is point-only");
        Object[][] rj = { { 0x1000L, 0x40L, "Foo", 1L }, { 0x1000L, 0x40L, "Foo", 5L } };
        check(method(rj, 0x1010L) == 1, "method: tiered re-JIT newest version wins");
        check(method(new Object[][] {}, 0x1000L) == -1, "method: empty map -> -1");

        System.out.println("1.." + n);
        if (failed) System.exit(1);
    }
}
