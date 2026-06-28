# asm-test — Java binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Java, via the
**Foreign Function & Memory API** (Project Panama, `java.lang.foreign`) — no JNI,
no native glue.

Downcall handles target the opaque-handle FFI layer (`src/ffi.c`), so no C struct
layout is mirrored: `asmtest_corpus_routine` for addresses, `asmtest_capture6` /
`_fp2` + `asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the
emulator (faults as data: `faulted()`, plus `faultAddr()` / `faultKind()` —
a `FaultKind` enum — for where and why one hit).

## Run

```sh
make java-test        # from the repo root (needs the shared libs + JDK 21)
make docker-java      # or in an isolated container
```

The reusable module is [`Asmtest.java`](Asmtest.java) — it keeps all FFM downcall
handles inside and exposes the `Regs` / `Emu` / `EmuResult` classes plus the
`assert*` helpers. [`Conformance.java`](Conformance.java) is a thin consumer that
replays the corpus through it. `make java-test` builds `libasmtest_emu` + the
routine fixture lib, compiles both sources, and runs the conformance class. FFM
is a **preview** API in JDK 21, so it compiles with `--release 21
--enable-preview` and runs with `--enable-preview --enable-native-access=ALL-UNNAMED`
(stable in JDK 22+; drop the preview flags there).

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

## DynamoRIO native-trace tier (optional)

[`DrTrace.java`](DrTrace.java) is a separate, opt-in binding for the in-process
**DynamoRIO native-trace** tier (mirrors the Python `asmtest.drtrace`). Where
`Emu` traces isolated guest bytes through Unicorn, `DrTrace` traces host-native
code as it runs **inside this JVM**: initialize DynamoRIO once, materialize
host-native machine code (`NativeCode.fromBytes`), mark a region, call into it,
and read back basic-block coverage / the instruction stream (`NativeTrace`). It
loads one library, `libasmtest_drapp` (from `ASMTEST_DRAPP_LIB`, else
`<repo>/build/libasmtest_drapp.{so,dylib}`), which `dlopen`s `libdynamorio`
lazily — so this binding never links DynamoRIO.

The tier is advanced and **Linux-x86-64-only**; the library may be absent (no
DynamoRIO at build time), so `DrTrace.available()` never throws and the smoke
test [`DrTraceTest.java`](DrTraceTest.java) self-skips (prints `SKIP: ...`,
exits 0) when it can't run. Build the tier and client with
`make shared-drtrace drtrace-client DYNAMORIO_HOME=...` (or `make docker-drtrace`)
and export `ASMTEST_DRCLIENT`. Compile + run standalone:

```sh
javac --release 21 --enable-preview -d /tmp/jdt \
    bindings/java/DrTrace.java bindings/java/DrTraceTest.java
ASMTEST_DRAPP_LIB=$PWD/build/libasmtest_drapp.so \
    java --enable-preview --enable-native-access=ALL-UNNAMED -cp /tmp/jdt DrTraceTest
```

## Deferred

A published Maven/Gradle artifact (and JUnit integration of the `assert*`
helpers) is future work; the reusable module with Tier-2 assertions ships today.
