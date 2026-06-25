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
enum — for where and why one hit). See [Language bindings](../bindings.md) for the
shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

FFM is a **preview** API in JDK 21, so sources compile with `--release 21
--enable-preview` and run with `--enable-preview --enable-native-access=ALL-UNNAMED`
(stable in JDK 22+; drop the preview flags there).

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

Pass a routine as an **assembly string**. Present only in the Keystone-carrying
`libasmtest_emu_asm` (`make java-asm-test` points `ASMTEST_LIB` at it);
`Emu#asmAvailable()` is false against the plain lib.

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
make java-test        # from the repo root (needs the shared libs + JDK 21)
make docker-java      # or in an isolated container
make java-asm-test    # points ASMTEST_LIB at libasmtest_emu_asm
```

`make java-test` builds `libasmtest_emu` + the routine fixture lib, compiles both
sources, and runs
[`Conformance.java`](https://github.com/wilvk/asm-test/blob/main/bindings/java/Conformance.java)
— a thin consumer that replays the corpus through `Asmtest.java`.

## Maturity

A published Maven/Gradle artifact (and JUnit integration of the `assert*` helpers)
is future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../packaging.md).
