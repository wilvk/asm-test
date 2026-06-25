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
    Asmtest.assertABIPreserved(r);
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
