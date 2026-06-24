# asm-test — Java binding

Run, **capture**, and **emulate** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Java, via the
**Foreign Function & Memory API** (Project Panama, `java.lang.foreign`) — no JNI,
no native glue.

Downcall handles target the opaque-handle FFI layer (`src/ffi.c`), so no C struct
layout is mirrored: `asmtest_corpus_routine` for addresses, `asmtest_capture6` /
`_fp2` + `asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the
emulator (faults as data).

## Run

```sh
make java-test        # from the repo root (needs the shared libs + JDK 21)
make docker-java      # or in an isolated container
```

`make java-test` builds `libasmtest_emu` + the routine fixture lib, compiles
[`Conformance.java`](Conformance.java), and runs it (the corpus replayed in
Java). FFM is a **preview** API in JDK 21, so it compiles with `--release 21
--enable-preview` and runs with `--enable-preview --enable-native-access=ALL-UNNAMED`
(stable in JDK 22+; drop the preview flags there).

## Deferred

A Maven/Gradle artifact, `jextract`-generated bindings, and a JUnit Tier-2
assertion layer are future work; this is the Tier-1 binding that proves the FFM
path.
