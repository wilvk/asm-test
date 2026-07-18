// Java data-flow binding smoke (Phase 6 + F7): GC-move canonicalizer + method
// resolver, mirroring the other language suites — and (F7) a REAL live attach to a
// victim process by pid. Project Panama FFM (final since JDK 22): compile with
// `--release 22`, run with `--enable-native-access=ALL-UNNAMED`.
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
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

    // --- F7: the LIVE-ATTACH producer entry points (src/dataflow_ptrace.c) --- //
    // The producer ships NO header on purpose (a value-trace PRODUCER is a tier, not
    // part of the shared sink API), so — exactly as its own C suite does — this
    // binding re-declares them, here as FFM FunctionDescriptors. Keep in step with
    // that file. No struct crosses by value; every argument is a scalar or a pointer,
    // so the SysV eightbyte classification that bites hand-flattened handles does not
    // arise. `long` in C is 64-bit on the Linux x86-64 this tier runs on -> JAVA_LONG.
    static MethodHandle attachPid;
    static MethodHandle attachPidTid;
    static MethodHandle attachJit;
    static MethodHandle attachPidVersioned;
    static MethodHandle valtraceNew;
    static MethodHandle valtraceFree;
    static MethodHandle valtraceSteps;
    static MethodHandle valtraceRecs;
    static MethodHandle valtraceAppend;

    // T4 — the versioned-decode code-image recorder (asmtest_codeimage.h): a userspace
    // PERF_RECORD_TEXT_POKE. Records a TIMESTAMPED timeline of a process's code bytes so
    // a live JIT that patches/frees/reuses an address mid-capture still decodes from the
    // bytes that were LIVE at trace time, not a late live snapshot. pid 0 records THIS
    // process. Its object (pic/codeimage.o) is already linked into libasmtest_dataflow.
    static MethodHandle ciAvailable;
    static MethodHandle ciSkipReason;
    static MethodHandle ciNew;
    static MethodHandle ciFree;
    static MethodHandle ciTrack;
    static MethodHandle ciNow;
    static MethodHandle ciBytesAt;
    static final int CI_OK = 0; // ok

    // L1 def-use graph + L2 slice (analysis pipeline, src/dataflow.c). The seed
    // crosses BY POINTER (asmtest_slice_forward_seed/_backward_seed): a by-value
    // at_val_rec_t StructLayout would need explicit paddingLayout members at
    // 20-23/45-47/52-55/68-71; the pointer form needs none of that — the seed is
    // just a 72-byte MemorySegment with JAVA_INT written at offset 64.
    static MethodHandle defuseBuild;
    static MethodHandle defuseFree;
    static MethodHandle sliceForwardSeed;
    static MethodHandle sliceBackwardSeed;
    static MethodHandle sliceFree;
    static MethodHandle sliceContains;

    // One operand read/write record (include/asmtest_valtrace.h at_val_rec_t),
    // 72 bytes. Offsets verified via offsetof on this build.
    static final long REC_SIZE = 72;
    static final long OFF_KIND = 0;
    static final long OFF_REG = 4;
    static final long OFF_ADDR = 32;
    static final long OFF_IS_WRITE = 42;
    static final long OFF_STEP = 64;

    // at_loc_kind_t: the location space of an operand.
    static final int LOC_REG = 0;     // a register (key = Capstone reg id)
    static final int LOC_MEM_ABS = 1; // memory at an absolute effective address (key = addr)
    static final int LOC_MEM_OFF = 2; // memory at a routine offset

    static final class Loc {
        final int kind;
        final long key;
        Loc(int kind, long key) { this.kind = kind; this.key = key; }
    }

    static Loc reg(long r) { return new Loc(LOC_REG, r); }
    static Loc mem(long addr) { return new Loc(LOC_MEM_ABS, addr); }

    static void writeRec(MemorySegment seg, long base, Loc loc, boolean isWrite) {
        seg.set(JAVA_INT, base + OFF_KIND, loc.kind);
        if (loc.kind == LOC_REG) seg.set(JAVA_INT, base + OFF_REG, (int) loc.key);
        else seg.set(JAVA_LONG, base + OFF_ADDR, loc.key);
        seg.set(JAVA_BYTE, base + OFF_IS_WRITE, (byte) (isWrite ? 1 : 0));
    }

    // A timestamped code-image timeline for one target process (asmtest_codeimage.h).
    // #track begins recording [base, base+length); #bytesAt answers "what bytes
    // were live at addr as of sequence `when`" — the decode a mid-capture re-JIT
    // needs. pid 0 records THIS process. base/addr cross as JAVA_LONG (raw
    // numeric address), not ADDRESS: the target may be a FOREIGN process, so
    // there is no Java-reachable MemorySegment to hand the linker for it.
    static final class CodeImage {
        MemorySegment h;

        CodeImage(int pid) throws Throwable {
            h = (MemorySegment) ciNew.invoke(pid);
            if (h.address() == 0) throw new IllegalStateException("asmtest_codeimage_new failed");
        }

        int track(long base, long length) throws Throwable {
            return (int) ciTrack.invoke(h, base, length);
        }

        long now() throws Throwable {
            return (long) ciNow.invoke(h);
        }

        // The bytes live at `addr` as of sequence `when` (0 = latest), or null if
        // the address was never tracked / had no version at-or-before `when`.
        byte[] bytesAt(long addr, long when) throws Throwable {
            MemorySegment outPtr = arena.allocate(JAVA_LONG);
            MemorySegment outLen = arena.allocate(JAVA_LONG);
            int rc = (int) ciBytesAt.invoke(h, addr, when, outPtr, outLen);
            if (rc != CI_OK) return null;
            long p = outPtr.get(JAVA_LONG, 0);
            long len = outLen.get(JAVA_LONG, 0);
            if (p == 0) return null;
            return MemorySegment.ofAddress(p).reinterpret(len).toArray(JAVA_BYTE);
        }

        void free() throws Throwable {
            if (h != null) {
                ciFree.invoke(h);
                h = null;
            }
        }
    }

    // A hand-built (or live-attach-filled) L0 value trace plus its cached L1
    // def-use graph and L2 slicer — mirrors the Python/Ruby/Lua/Zig/Rust/Go
    // ValueTrace.
    static final class ValueTrace {
        MemorySegment v;
        MemorySegment g;
        int nSteps;

        ValueTrace(long stepsCap, long recsCap) throws Throwable {
            v = (MemorySegment) valtraceNew.invoke(stepsCap, recsCap, 0L);
            if (v.address() == 0) throw new IllegalStateException("asmtest_valtrace_new failed");
        }

        // Adopts an existing native handle (e.g. one a live attach already
        // filled) instead of allocating a new one — the caller owns freeing
        // `existing`; call postAttach() to sync nSteps before slicing.
        ValueTrace(MemorySegment existing) {
            v = existing;
        }

        // Append one executed instruction at offset `off` reading `reads` and
        // writing `writes` (each an array of Loc). Read-set before write-set.
        void step(long off, Loc[] reads, Loc[] writes) throws Throwable {
            int n = reads.length + writes.length;
            MemorySegment recs = n > 0 ? arena.allocate(REC_SIZE * n) : MemorySegment.NULL;
            int i = 0;
            for (Loc loc : reads) writeRec(recs, (i++) * REC_SIZE, loc, false);
            for (Loc loc : writes) writeRec(recs, (i++) * REC_SIZE, loc, true);
            valtraceAppend.invoke(v, off, recs, (long) n);
            nSteps++;
            invalidateDefuse();
        }

        // A live producer appends behind our back (unlike step(), which counts
        // as it goes), so resync the step count and drop any stale def-use
        // graph.
        void postAttach() throws Throwable {
            nSteps = (int) (long) valtraceSteps.invoke(v);
            invalidateDefuse();
        }

        void invalidateDefuse() throws Throwable {
            if (g != null) {
                defuseFree.invoke(g);
                g = null;
            }
        }

        // The L1 last-writer def-use graph, built once and cached until the
        // next step()/attach invalidates it.
        MemorySegment defuse() throws Throwable {
            if (g == null) {
                g = (MemorySegment) defuseBuild.invoke(v);
                if (g.address() == 0) throw new IllegalStateException("asmtest_defuse_build failed");
            }
            return g;
        }

        int[] slice(int origin, boolean forward) throws Throwable {
            MemorySegment graph = defuse();
            MemorySegment seed = arena.allocate(REC_SIZE);
            seed.set(JAVA_INT, OFF_STEP, origin);
            MemorySegment s = (MemorySegment) (forward
                    ? sliceForwardSeed.invoke(graph, seed)
                    : sliceBackwardSeed.invoke(graph, seed));
            int[] buf = new int[nSteps];
            int cnt = 0;
            for (int i = 0; i < nSteps; i++) {
                if ((int) sliceContains.invoke(s, i) != 0) buf[cnt++] = i;
            }
            sliceFree.invoke(s);
            return java.util.Arrays.copyOf(buf, cnt);
        }

        // Steps influenced by the value defined at step `origin` (origin included).
        int[] forwardSlice(int origin) throws Throwable { return slice(origin, true); }

        // Steps that produced the value used at step `sink` (sink included).
        int[] backwardSlice(int sink) throws Throwable { return slice(sink, false); }

        void free() throws Throwable {
            if (v != null) {
                invalidateDefuse();
                valtraceFree.invoke(v);
                v = null;
            }
        }
    }

    // The producer's return codes, re-declared for the same reason.
    static final int PTRACE_OK = 0;      // a complete scoped trace
    static final int PTRACE_EINVAL = -1; // bad arguments
    static final int PTRACE_ENOSYS = -3; // off Linux x86-64 / no Capstone: tier absent
    static final int PTRACE_ETRACE = -4; // ptrace/wait failure (seccomp/yama)

    // ETRACE is NOT a skip. ptrace is a capability the lane can be GIVEN
    // (--cap-add=SYS_PTRACE / seccomp=unconfined), and the victim opts in via
    // PR_SET_PTRACER_ANY, so a refusal means the lane is misconfigured — be loud.
    static void checkRc(int rc, String desc) {
        if (rc == PTRACE_ETRACE)
            System.out.println("# " + desc + ": ptrace refused (ETRACE) — the lane needs "
                    + "--cap-add=SYS_PTRACE; this is NOT a valid skip");
        check(rc == PTRACE_OK, desc);
    }

    // A live victim: spawn it DETACHED, then learn its region base + its own pid from
    // the handshake file. a/b are OURS, so the expected result is a property of THIS
    // run, not a constant a stubbed wrapper could hardcode.
    //
    // DETACHED — the one thing Java must do differently here, and it reflects a real
    // property of the tier rather than a test trick. The JVM reaps child processes on
    // a dedicated "process reaper" THREAD sitting in a blocking waitpid() on each
    // child's pid. A ptrace-stop of a traced child is reportable to ANY thread in the
    // tracer's thread group, so that reaper RACES the producer's own waitpid: the
    // reaper consumes the stop notification, the producer blocks forever, and the lane
    // HANGS with the victim parked in state 't'. Observed exactly so before this was
    // detached — victim TracerPid = the FFM downcall's thread, another JVM thread in
    // do_wait, no progress. Python/Node/Ruby/... never hit it: none of them reap on a
    // concurrent thread while a blocking downcall is in flight.
    //
    // So the victim must NOT be a direct child of the JVM: `sh -c '... &'` backgrounds
    // it and sh exits, orphaning the victim to init, and the JVM's reaper only ever
    // sees sh. The attach still works because the victim calls PR_SET_PTRACER_ANY, so
    // Yama does not require the tracer to be an ancestor. THE SAME CAVEAT APPLIES TO
    // ANY MANAGED HOST attaching to its own child — .NET's Program.cs does likewise.
    static final class Victim {
        long base;
        long len;
        int pid;
        Path counterPath;

        Victim(String exe, String tag, int a, int b) throws Exception {
            counterPath = Path.of("/tmp/asmtest-df-java-" + tag + ".counter");
            Path hs = Path.of("/tmp/asmtest-df-java-" + tag + ".hs");
            Files.deleteIfExists(hs);
            Process sh = new ProcessBuilder(List.of("/bin/sh", "-c",
                    exe + " " + counterPath + " " + a + " " + b + " > " + hs + " 2>&1 &"))
                    .start();
            sh.waitFor(); // sh exits at once; the victim lives on, reparented to init
            String line = null;
            for (int i = 0; i < 500 && line == null; i++) {
                if (Files.exists(hs)) {
                    List<String> lines = Files.readAllLines(hs);
                    if (!lines.isEmpty()) line = lines.get(0);
                }
                if (line == null) Thread.sleep(10);
            }
            String[] f = line == null ? new String[0] : line.trim().split("\\s+");
            if (f.length != 3) throw new IllegalStateException("victim handshake failed: " + line);
            base = Long.parseUnsignedLong(f[0].substring("base=0x".length()), 16);
            len = Long.parseLong(f[1].substring("len=".length()));
            pid = Integer.parseInt(f[2].substring("pid=".length()));
        }

        long counter() throws Exception {
            byte[] b = Files.readAllBytes(counterPath);
            if (b.length < 8) return 0;
            return ByteBuffer.wrap(b, 0, 8).order(ByteOrder.LITTLE_ENDIAN).getLong();
        }

        // ProcessHandle reaches a non-child by pid (Java 9+); a Process cannot.
        void close() {
            ProcessHandle.of(pid).ifPresent(ProcessHandle::destroyForcibly);
        }
    }

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
        // F7 — the L0 sink handle (opaque: ADDRESS) + the live-attach entries.
        valtraceNew = linker.downcallHandle(lookup.find("asmtest_valtrace_new").get(),
                FunctionDescriptor.of(ADDRESS, JAVA_LONG, JAVA_LONG, JAVA_LONG));
        valtraceFree = linker.downcallHandle(lookup.find("asmtest_valtrace_free").get(),
                FunctionDescriptor.ofVoid(ADDRESS));
        valtraceSteps = linker.downcallHandle(lookup.find("asmtest_valtrace_steps").get(),
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        valtraceRecs = linker.downcallHandle(lookup.find("asmtest_valtrace_recs").get(),
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        // (pid, base, code_len, max_insns, long *result, valtrace *vt) -> int
        attachPid = linker.downcallHandle(
                lookup.find("asmtest_dataflow_ptrace_attach_pid").get(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_LONG,
                        ADDRESS, ADDRESS));
        // (pid, only_tid, base, code_len, max_insns, long *result, valtrace *vt) -> int
        attachPidTid = linker.downcallHandle(
                lookup.find("asmtest_dataflow_ptrace_attach_pid_tid").get(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG,
                        JAVA_LONG, ADDRESS, ADDRESS));
        // (pid, only_tid, base, code_len, img, when, max_insns, long *result,
        //  int *survived, valtrace *vt) -> int   — ten args: six in registers, four
        //  on the stack, so a dropped or reordered one lands garbage in `base`/`pid`.
        attachJit = linker.downcallHandle(
                lookup.find("asmtest_dataflow_ptrace_attach_jit").get(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG,
                        ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS, ADDRESS, ADDRESS));
        // (pid, base, code_len, max_insns, img, when, long *result, valtrace *vt) -> int
        attachPidVersioned = linker.downcallHandle(
                lookup.find("asmtest_dataflow_ptrace_attach_pid_versioned").get(),
                FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_LONG, JAVA_LONG, JAVA_LONG,
                        ADDRESS, JAVA_LONG, ADDRESS, ADDRESS));
        // T4 — the code-image recorder. base/addr are JAVA_LONG (see the CodeImage
        // class comment); img/out-pointers are ADDRESS (real Java-managed memory).
        ciAvailable = linker.downcallHandle(lookup.find("asmtest_codeimage_available").get(),
                FunctionDescriptor.of(JAVA_INT));
        ciSkipReason = linker.downcallHandle(lookup.find("asmtest_codeimage_skip_reason").get(),
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG));
        ciNew = linker.downcallHandle(lookup.find("asmtest_codeimage_new").get(),
                FunctionDescriptor.of(ADDRESS, JAVA_INT));
        ciFree = linker.downcallHandle(lookup.find("asmtest_codeimage_free").get(),
                FunctionDescriptor.ofVoid(ADDRESS));
        ciTrack = linker.downcallHandle(lookup.find("asmtest_codeimage_track").get(),
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_LONG));
        ciNow = linker.downcallHandle(lookup.find("asmtest_codeimage_now").get(),
                FunctionDescriptor.of(JAVA_LONG, ADDRESS));
        ciBytesAt = linker.downcallHandle(lookup.find("asmtest_codeimage_bytes_at").get(),
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_LONG, JAVA_LONG, ADDRESS, ADDRESS));
        valtraceAppend = linker.downcallHandle(lookup.find("asmtest_valtrace_append").get(),
                FunctionDescriptor.ofVoid(ADDRESS, JAVA_LONG, ADDRESS, JAVA_LONG));
        defuseBuild = linker.downcallHandle(lookup.find("asmtest_defuse_build").get(),
                FunctionDescriptor.of(ADDRESS, ADDRESS));
        defuseFree = linker.downcallHandle(lookup.find("asmtest_defuse_free").get(),
                FunctionDescriptor.ofVoid(ADDRESS));
        sliceForwardSeed = linker.downcallHandle(lookup.find("asmtest_slice_forward_seed").get(),
                FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS));
        sliceBackwardSeed = linker.downcallHandle(lookup.find("asmtest_slice_backward_seed").get(),
                FunctionDescriptor.of(ADDRESS, ADDRESS, ADDRESS));
        sliceFree = linker.downcallHandle(lookup.find("asmtest_slice_free").get(),
                FunctionDescriptor.ofVoid(ADDRESS));
        sliceContains = linker.downcallHandle(lookup.find("asmtest_slice_contains").get(),
                FunctionDescriptor.of(JAVA_INT, ADDRESS, JAVA_INT));

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

        defuseSliceSmoke();
        codeimageSmoke();
        liveAttachTests();

        System.out.println("1.." + n);
        if (failed) System.exit(1);
    }

    // T3 — def-use/slice round-trip over a hand-built register-move chain
    // (by-pointer seed, src/dataflow.c). Pure C, no ptrace, so it runs
    // unconditionally — exercises step()/append marshalling and both slicers
    // even where the live-attach section below self-skips.
    static void defuseSliceSmoke() throws Throwable {
        ValueTrace vt = new ValueTrace(64, 512);
        // A register chain r10 -> r11 -> r12 (mirrors the Python round-trip test).
        vt.step(0, new Loc[0], new Loc[] { reg(10) });
        vt.step(1, new Loc[] { reg(10) }, new Loc[] { reg(11) });
        vt.step(2, new Loc[] { reg(11) }, new Loc[] { reg(12) });
        int[] fwd = vt.forwardSlice(0);
        int[] bwd = vt.backwardSlice(2);
        check(java.util.Arrays.equals(fwd, new int[] { 0, 1, 2 }),
                "slice: forward_slice(0) over r10->r11->r12 == {0,1,2}");
        check(java.util.Arrays.equals(bwd, new int[] { 0, 1, 2 }),
                "slice: backward_slice(2) over r10->r11->r12 == {0,1,2}");
        vt.free();
    }

    // T4 — the code-image recorder (asmtest_codeimage.h): track a buffer in THIS
    // process and read back the exact bytes it snapshotted. Runs wherever
    // soft-dirty page tracking is available — no ptrace, so it runs even where
    // liveAttachTests() below self-skips.
    static void codeimageSmoke() throws Throwable {
        if ((int) ciAvailable.invoke() == 0) {
            MemorySegment why = arena.allocate(200);
            ciSkipReason.invoke(why, 200L);
            System.out.println("# SKIP codeimage: " + why.getString(0));
            return;
        }
        int n = 16;
        MemorySegment buf = arena.allocate(n);
        byte[] want = new byte[n];
        for (int i = 0; i < n; i++) {
            want[i] = (byte) (0xA0 + i);
            buf.set(JAVA_BYTE, i, want[i]);
        }
        CodeImage img = new CodeImage(0);
        int trc = img.track(buf.address(), n);
        check(trc == CI_OK, "codeimage: track() snapshots v0");
        long t0 = img.now();
        check(t0 > 0, "codeimage: now() advanced past 0 after track");
        byte[] got = img.bytesAt(buf.address(), t0);
        check(got != null && java.util.Arrays.equals(got, want),
                "codeimage: bytes_at() returns the exact tracked bytes");
        img.free();
    }

    // ----------------------------------------------------------------------
    // F7 — live-attach data flow: capture over a REAL attached pid.
    //
    // Every assertion is POSITIVE and keyed to something only a working capture can
    // produce (the region's return value, the exact step count, the survival report).
    // Nothing hides behind "if we captured anything" — an EMPTY capture IS the failure
    // signature, so a guard like that would skip exactly when it should shout.
    // ----------------------------------------------------------------------
    static void liveAttachTests() throws Throwable {
        // The tier is Linux x86-64 only (src/dataflow_ptrace.c's own #if). On such a
        // host the live tests MUST run: an unavailable tier there means the lib was
        // linked without Capstone — a build defect that has to be RED, not a skip.
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "");
        if (!os.contains("linux") || !(arch.equals("amd64") || arch.equals("x86_64"))) {
            System.out.println("# SKIP live-attach: not linux/x86_64 (the tier is Linux x86-64 only)");
            return;
        }
        String exe = System.getenv("ASMTEST_DATAFLOW_VICTIM");
        if (exe == null) {
            // The lane always exports this; missing means a misconfigured lane, and
            // silently skipping every live test is the hole this suite must not have.
            System.out.println("Bail out! ASMTEST_DATAFLOW_VICTIM unset; run `make dataflow-java-test`");
            System.exit(1);
        }

        // Probed, not a symbol-resolves check: EINVAL (real) vs ENOSYS (stub) — the
        // lookup above succeeds either way, so only the return code tells them apart.
        {
            MemorySegment v = (MemorySegment) valtraceNew.invoke(1L, 1L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            int rc = (int) attachPid.invoke(0, 0L, 0L, 0L, out, v);
            valtraceFree.invoke(v);
            check(rc != PTRACE_ENOSYS, "live: tier is real on linux/x86_64 (EINVAL, not ENOSYS)");
        }

        {
            Victim vic = new Victim(exe, "1", 7, 5);
            MemorySegment v = (MemorySegment) valtraceNew.invoke(64L, 512L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            checkRc((int) attachPid.invoke(vic.pid, vic.base, vic.len, 0L, out, v),
                    "live: attach_pid a FOREIGN running pid + stepped the region");
            // The region really executed IN the victim: rax = rdi + rsi.
            check(out.get(JAVA_LONG, 0) == 12, "live: attach_pid region returned 12 (rax = rdi + rsi)");
            // Exactly df_chain's six in-region instructions — not "some".
            check((long) valtraceSteps.invoke(v) == 6, "live: six in-region steps captured over the victim");
            check((long) valtraceRecs.invoke(v) > 0, "live: operand records captured");
            // SURVIVAL: we attached to a process we do not own; it must outlive the detach.
            long c0 = vic.counter();
            Thread.sleep(50);
            check(vic.counter() > c0, "live: victim SURVIVED the detach (counter advanced)");
            // T3: the memory def-use edge (step1 store -> step2 load) — only
            // reachable via the by-pointer slice seed (T1); the seven bindings
            // could never test this before. Wrap the already-filled `v` (do not
            // call .free() on this wrapper — valtraceFree below owns that).
            ValueTrace lvt = new ValueTrace(v);
            lvt.postAttach();
            int[] lfwd = lvt.forwardSlice(0);
            int[] lbwd = lvt.backwardSlice(4);
            check(java.util.Arrays.equals(lfwd, new int[] { 0, 1, 2, 3, 4 }),
                    "live: forward_slice(0) == {0,1,2,3,4} over df_chain, excludes ret");
            check(java.util.Arrays.equals(lbwd, new int[] { 0, 1, 2, 3, 4 }),
                    "live: backward_slice(4) == {0,1,2,3,4} -- the memory edge step1(store)->step2(load), "
                            + "excludes ret");
            valtraceFree.invoke(v);
            vic.close();
        }
        {
            // THE anti-hardcode control: a second victim, different args, same wrapper.
            Victim vic = new Victim(exe, "2", 17, 25);
            MemorySegment v = (MemorySegment) valtraceNew.invoke(64L, 512L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            checkRc((int) attachPid.invoke(vic.pid, vic.base, vic.len, 0L, out, v),
                    "live: attach_pid the second victim");
            check(out.get(JAVA_LONG, 0) == 42, "live: result TRACKS the victim's args (17+25=42)");
            check((long) valtraceSteps.invoke(v) == 6, "live: six steps on the second victim too");
            valtraceFree.invoke(v);
            vic.close();
        }
        {
            Victim vic = new Victim(exe, "3", 9, 4);
            MemorySegment v = (MemorySegment) valtraceNew.invoke(64L, 512L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            // only_tid 0: step whichever thread enters the region (here, the only one).
            checkRc((int) attachPidTid.invoke(vic.pid, 0, vic.base, vic.len, 0L, out, v),
                    "live: attach_pid_tid stepped the entering thread");
            check(out.get(JAVA_LONG, 0) == 13, "live: attach_pid_tid region returned 13 (9+4)");
            check((long) valtraceSteps.invoke(v) == 6, "live: attach_pid_tid captured six steps");
            valtraceFree.invoke(v);
            vic.close();
        }
        {
            Victim vic = new Victim(exe, "4", 20, 3);
            MemorySegment v = (MemorySegment) valtraceNew.invoke(64L, 512L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            MemorySegment survived = arena.allocate(JAVA_INT);
            checkRc((int) attachJit.invoke(vic.pid, 0, vic.base, vic.len, MemorySegment.NULL,
                    0L, 0L, out, survived, v), "live: attach_jit stepped the region");
            check(out.get(JAVA_LONG, 0) == 23, "live: attach_jit region returned 23 (20+3)");
            check((long) valtraceSteps.invoke(v) == 6, "live: attach_jit captured six steps");
            // The producer's OWN survival report — the house rule that a foreign
            // target is never killed, asserted from its side.
            check(survived.get(JAVA_INT, 0) == 1, "live: attach_jit reported the target as survived");
            long c0 = vic.counter();
            Thread.sleep(50);
            check(vic.counter() > c0, "live: attach_jit victim kept running after detach");
            valtraceFree.invoke(v);
            vic.close();
        }
        if ((int) ciAvailable.invoke() != 0) {
            // T4 — a real code-image threaded through attach_pid_versioned: build the
            // recorder over the victim's OWN published region, then decode the
            // capture against it. A non-NULL img must not break the capture or land
            // in the wrong argument slot (a dropped/misplaced pointer would corrupt
            // base/pid and the result assert below would catch it).
            Victim vic = new Victim(exe, "5", 11, 6);
            CodeImage img = new CodeImage(vic.pid);
            int trc = img.track(vic.base, vic.len);
            check(trc == CI_OK, "codeimage: track() over the victim's published region");
            MemorySegment v = (MemorySegment) valtraceNew.invoke(64L, 512L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            long when0 = img.now();
            checkRc((int) attachPidVersioned.invoke(vic.pid, vic.base, vic.len, 0L,
                    img.h, when0, out, v), "live: attach_pid_versioned with a real img");
            check(out.get(JAVA_LONG, 0) == 17,
                    "live: attach_pid_versioned result TRACKS the victim's args (11+6=17)");
            check((long) valtraceSteps.invoke(v) == 6,
                    "live: attach_pid_versioned captured six steps with a real img");
            img.free();
            valtraceFree.invoke(v);
            vic.close();
        } else {
            MemorySegment why = arena.allocate(200);
            ciSkipReason.invoke(why, 200L);
            System.out.println("# SKIP codeimage live: " + why.getString(0));
        }
        {
            // Negative control: the wrapper must surface the producer's rejections
            // rather than manufacture success.
            MemorySegment v = (MemorySegment) valtraceNew.invoke(8L, 8L, 0L);
            MemorySegment out = arena.allocate(JAVA_LONG);
            check((int) attachPid.invoke(12345, 0x1000L, 0L, 0L, out, v) == PTRACE_EINVAL,
                    "live: zero-length region is rejected (EINVAL)");
            check((int) attachPid.invoke(0, 0x1000L, 21L, 0L, out, v) == PTRACE_EINVAL,
                    "live: pid 0 is rejected (EINVAL)");
            check((int) attachPid.invoke(0x7FFFFFF0, 0x1000L, 21L, 0L, out, v) != PTRACE_OK,
                    "live: attaching to a nonexistent pid never returns OK");
            valtraceFree.invoke(v);
        }
    }
}
