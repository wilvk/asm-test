# Java binding

The [Java binding](https://github.com/wilvk/asm-test/tree/main/bindings/java)
drives asm-test via the **Foreign Function & Memory API** (Project Panama,
`java.lang.foreign`) — no JNI, no native glue. The reusable module is
[`Asmtest.java`](https://github.com/wilvk/asm-test/blob/main/bindings/java/Asmtest.java)
— it keeps all FFM downcall handles inside and exposes the `Regs` / `Emu` /
`EmuResult` classes plus the `assert*` helpers.

Downcall handles target the opaque-handle FFI layer, so no C struct layout is
mirrored: `asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the emulator
(faults as data: `faulted()`, plus `faultAddr()` / `faultKind()` — a `FaultKind`
enum — for where and why one hit). See [Language bindings](index.md) for the
shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

FFM is **final since JDK 22**, so sources compile with `--release 22` and run
with `--enable-native-access=ALL-UNNAMED` (no preview flags). The classfiles then
load on any JDK 22 or newer; a JDK 21 (where FFM is still preview) is no longer
supported.

## Usage

```java
// Native capture through the real ABI.
try (Asmtest.Regs r = new Asmtest.Regs()) {
    r.capture6(fn, 40L, 2L);
    Asmtest.assertRet(r, 42);
    Asmtest.assertAbiPreserved(r);
}

// Emulator: faults are data, never a crash.
try (Asmtest.Emu e = new Asmtest.Emu();
     Asmtest.EmuResult res = e.call2(fn, 40L, 2L)) {
    Asmtest.assertNoFault(res);
    Asmtest.assertEmuReg(res, "rax", 42);   // the emulator guest is x86-64
}
```

## In-line assembler (optional)

Pass a routine as an **assembly string**. `libasmtest_emu` carries the Keystone
in-line assembler, so this works out of the box under `make java-test`.
`Emu#asmAvailable()` is a defensive probe — false only against an older/leaner
lib pointed at by `ASMTEST_LIB`.

```java
try (Asmtest.Emu e = new Asmtest.Emu()) {
    if (e.asmAvailable()) {
        // Intel, up to six args; throws AsmtestException (with the Keystone
        // diagnostic) if the string fails to assemble.
        try (Asmtest.EmuResult res = e.callAsm("mov rax, rdi; add rax, rsi; ret", 40L, 2L)) {
            // res.reg("rax") == 42
        }
        // AT&T syntax + an instruction cap:
        e.callAsm(src, new long[] {10, 20, 12}, Asmtest.AsmSyntax.ATT, 0).close();
        // Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
        byte[] a64 = Asmtest.assemble("ret", Asmtest.AsmArch.ARM64);
    }
}
```

## Function reference

Every public member of `Asmtest`, with an example and its options. The handle
types (`Regs`, `Emu`, `EmuResult`, `Trace`, `Guest`, `GuestResult`) are
`AutoCloseable` — use try-with-resources. A routine reference is a
`MemorySegment` (`corpusRoutine(name)` or your own FFM lookup); the
`callBytes`-family takes a `byte[]`.

### Resolving routines

```java
MemorySegment fn = Asmtest.corpusRoutine("add_signed");  // built-in (needs ASMTEST_CORPUS_LIB)
boolean hasAsm = new Asmtest.Emu().asmAvailable();        // in-line assembler present?
```

### Capture tier — `Regs`

```java
try (Asmtest.Regs r = new Asmtest.Regs()) {
    r.capture6(fn, 40L, 2L);                  // also capture6(fn) and capture6(fn,a0,a1,a2,a3,a4,a5)
    r.captureFp2(fn, 1.5, 2.25);              // two doubles; FP return in r.fret()
    r.captureVecF32(fn, new float[][]{{1,2,3,4}});  // up to 8 vectors (four float32 lanes each)
    long  ret = r.ret();                      // integer return (rax)
    double fr = r.fret();                     // scalar double return (xmm0)
    float[] v = r.vecF32(0);                  // four float32 lanes of vector register 0
    boolean cf = r.flagSet("CF");             // condition flag by name (CF/PF/ZF/SF/OF)
    boolean abi = r.abiPreserved();           // every callee-saved register restored
}
```

`capture6` is overloaded for 0, 2, or 6 integer args (missing default to 0).

### Emulator tier — `Emu` / `EmuResult`

```java
try (Asmtest.Emu e = new Asmtest.Emu()) {                  // x86-64 Unicorn guest
    Asmtest.EmuResult res = e.call2(fn, 40L, 2L);          // routine address + two int args
    res = e.callBytes(code, 40L, 2L);                      // raw bytes, up to 6 int args (varargs)
    res = e.callFp(code, new long[]{1}, new double[]{1.5});// doubles -> xmm0..7
    res = e.callVec(code, null, new float[][]{{1,2,3,4}}); // 128-bit vecs -> xmm0..7
    res = e.callWin64(code, 1L, 2L, 3L, 4L);               // Microsoft x64 (rcx,rdx,r8,r9)

    res.faulted();            // invalid access? (data, not a crash)
    res.faultAddr();          // where (valid when faulted)
    res.faultKind();          // Asmtest.FaultKind NONE/READ/WRITE/FETCH
    res.reg("rax");           // any GP register, plus "rip" / "rflags"
    res.xmmF64(0, 0);         // xmm lane as double (scalar FP return)
    res.xmmF32(0, 0);         // xmm lane as float32 (vector return)
}
```

`call2` is the only path taking a routine **address** (two int args); the
`callBytes`/`callFp`/`callVec`/`callWin64`/`callTraced` family takes raw `byte[]`.

### Execution trace / coverage — `Trace`

```java
try (Asmtest.Trace t = new Asmtest.Trace(4096, 4096)) {    // or new Trace() for the defaults
    Asmtest.EmuResult res = e.callTraced(code, new long[]{1, 2}, t);
    t.covered(0x0);           // was the basic block at this byte-offset entered?
}
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate **native** tier ([`DrTrace.java`](https://github.com/wilvk/asm-test/blob/main/bindings/java/DrTrace.java),
also FFM) traces host-native code as it runs **inside this JVM process** under
DynamoRIO, rather than through the Unicorn guest. Bring DynamoRIO up once with
`DrTrace.initialize()`, materialize machine code with `DrTrace.NativeCode`, mark a
region, call into it, and read back basic-block / instruction coverage. The tier
is opt-in: when the library or DynamoRIO is absent, `DrTrace.available()` returns
false so callers self-skip.

```java
// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
byte[] routine = {
    0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, 0x48, 0x3D,
    0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, (byte) 0xFF, (byte) 0xC8, (byte) 0xC3
};

if (!DrTrace.available()) return;             // self-skip without DynamoRIO
DrTrace.initialize();                         // up once; null client → $ASMTEST_DRCLIENT
try {
    // Instruction mode: trace executed code through a registered region.
    try (DrTrace.NativeCode code = DrTrace.NativeCode.fromBytes(routine);
         DrTrace.NativeTrace tr = DrTrace.NativeTrace.create(64, 64)) {  // 64 blocks, 64 insns
        tr.register("add2", code);
        tr.region("add2", () -> code.call(1, 2));  // run inside the region
        tr.covered(0);                             // entry block entered?

        tr.blockOffsets();    // distinct basic-block starts, first-seen order
        tr.insnOffsets();     // ordered instruction stream; jle-taken path:
                              //   {0x0, 0x3, 0x6, 0xc, 0x11}
    }

    // Symbol mode: trace a named exported function — no region/markers.
    try (DrTrace.NativeTrace tr = DrTrace.NativeTrace.create(64, 0)) {
        tr.registerSymbol("asmtest_symbol_demo", 256);
        long r = DrTrace.symbolDemo(3, 4);    // r == 10 (a*2+b), always-on recording
        tr.covered(0);                        // entry block entered?
    }
} finally {
    DrTrace.shutdown();
}
```

Linux x86-64 only; self-skips without DynamoRIO (and the JVM can be flaky for
in-process takeover — prefer Intel PT, see the central doc). Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier ([`HwTrace.java`](https://github.com/wilvk/asm-test/blob/main/bindings/java/HwTrace.java))
records the **same** `asmtest_trace_t` coverage from the real CPU, but needs no
separate engine install: it defaults to the **single-step** backend (the CPU's
`EFLAGS.TF` trap flag), so `HwTrace.available(...)` is true and it **traces live on
any x86-64 Linux** — CI and plain containers included — where the in-process
DynamoRIO tier can be flaky under the JVM. Intel PT and AMD LBR are picked
automatically on the bare-metal hardware that has them.

```java
// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
byte[] routine = {
    0x48, (byte) 0x89, (byte) 0xF8, 0x48, 0x01, (byte) 0xF0, 0x48, 0x3D,
    0x64, 0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, (byte) 0xFF, (byte) 0xC8, (byte) 0xC3
};

if (!HwTrace.available(HwTrace.SINGLESTEP)) return;   // self-skip off x86-64 Linux
HwTrace.init(HwTrace.SINGLESTEP);
try {
    HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(routine);
    HwTrace.NativeTrace tr = HwTrace.create(64, 64);   // blocks=64, instructions=64
    tr.register("add2", code);

    long[] r = {0};
    tr.region("add2", () -> r[0] = code.call(20, 22));  // 42; jle taken, dec skipped
    tr.insnOffsets();    // [0, 3, 6, 12, 17] — byte-for-byte the Unicorn/DynamoRIO/PT result
    tr.covered(0);       // entry basic block entered?
} finally {
    HwTrace.shutdown();
}
```

`HwTrace.resolve(HwTrace.BEST)` / `HwTrace.auto(HwTrace.BEST)` pick the host's
most-faithful available backend (Intel PT → AMD LBR → single-step), and
`HwTrace.resolveTiers` / `autoTier` extend the cascade across the DynamoRIO and
emulator tiers. An out-of-process `Ptrace` surface traces a method in a **separate**
process (fork-and-step, foreign-process attach + run-to-method, and `/proc`-map /
jitdump resolution) — the managed-runtime path. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

**Scoped tracing** — the try-with-resources scope (`HwTrace.AsmTrace`). It auto-names
the region from the call site (`StackWalker`), and `close()` renders the executed
assembly into `path()` (and to stdout unless `emit=false`); `truncated()` is the
thread-scope honesty bit.

```java
HwTrace.init(HwTrace.SINGLESTEP);
byte[] routine = { 0x48, (byte)0x89, (byte)0xF8, 0x48, 0x01, (byte)0xF0, (byte)0xC3 }; // add2; ret
HwTrace.NativeCode code = HwTrace.NativeCode.fromBytes(routine);
HwTrace.AsmTrace scope;
try (HwTrace.AsmTrace t = HwTrace.AsmTrace.scope(code, false)) { // auto-named "File.java:<line>"
    scope = t;
    code.call(20, 22);                                           // 42
}   // close(): end + render the executed assembly
String listing = scope.path();   // the disassembly that executed; scope.truncated() the thread bit
code.free();
```

### Cross-arch guests — `Guest` / `GuestResult`

```java
try (Asmtest.Guest g = new Asmtest.Guest("arm64")) {       // "arm64" | "riscv" | "arm"
    Asmtest.GuestResult gr = g.call(code, 40L, 2L);        // raw bytes, ints in guest ABI regs
    gr.reg("x0");             // by name: x0/sp, a0/x10, r0…
    gr.faulted();             // faults are data here too
    g.callTraced(code, new long[]{1}, t);                  // arm64 only (throws otherwise)
    gr.close();
}
```

### In-line assembler (optional)

```java
e.asmAvailable();                                          // assembler compiled in?
Asmtest.EmuResult res = e.callAsm("mov rax, rdi; ret", 42L);            // Intel, run to ret
res = e.callAsm(src, new long[]{10, 20}, Asmtest.AsmSyntax.ATT, 8);     // syntax + insn cap
byte[] bytes = Asmtest.assemble("ret", Asmtest.AsmArch.ARM64);          // text -> bytes, any arch
Asmtest.asmError();           // last Keystone diagnostic ("" on success)
```

* `Emu#callAsm(src, args…)` — convenience overload (Intel, run to `ret`); the full
  form is `callAsm(src, long[] args, AsmSyntax syntax, long maxInsns)`. ≤6 int
  args; throws `AsmtestException` (Keystone diagnostic) on a bad string.
* `assemble(src, arch[, syntax, addr])` — assemble-only; `arch` is
  `AsmArch.{X86_64,ARM64,RISCV64,ARM32}`, `syntax` is
  `AsmSyntax.{INTEL,ATT,NASM,MASM,GAS}` (the short overload defaults to Intel at
  `0x00100000`).

### Tier-2 assertions (throw `AsmtestException`)

```java
Asmtest.assertRet(r, 42);                  // r.ret() == 42
Asmtest.assertAbiPreserved(r);             // callee-saved restored
Asmtest.assertFlag(r, "CF", true);         // flag set/clear
Asmtest.assertFp(r, 3.75);                 // r.fret() == 3.75
Asmtest.assertVecF32(r, 0, new float[]{1,2,3,4});  // vector lanes of register 0
Asmtest.assertNoFault(res);                // emulator run clean
Asmtest.assertFault(res);                  // emulator run faulted
Asmtest.assertEmuReg(res, "rax", 42);      // x86 guest register
Asmtest.assertGuestReg(gr, "x0", 42);      // cross-arch guest register
Asmtest.assertCovered(t, 0x0);             // basic block entered
```

## Run the tests

```sh
make java-test        # from the repo root (needs the shared libs + JDK 22+)
make docker-java      # or in an isolated container
```

`make java-test` builds `libasmtest_emu` + the routine fixture lib, compiles both
sources, and runs
[`Conformance.java`](https://github.com/wilvk/asm-test/blob/main/bindings/java/Conformance.java)
— a thin consumer that replays the corpus through `Asmtest.java`.

## Maturity

A published Maven/Gradle artifact (and JUnit integration of the `assert*` helpers)
is future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../reference/packaging.md).
