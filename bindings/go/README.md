# asm-test — Go binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from **Go**, via `cgo`.

`cgo` declares the opaque-handle FFI helpers (`src/ffi.c` + `emu.c`) and links the
prebuilt shared libraries, so no C struct layout is mirrored in Go:
`asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture (with `asmtest_check_abi` as the ABI-preservation
verdict), and `asmtest_emu_call2` + accessors for the emulator (faults as data:
`EmuResult.Faulted()`, plus `FaultAddr()` / `FaultKind()` for where and why).
The result handles are all C-allocated, so they are exempt from cgo's
pointer-passing rules.

Tier-2 idiomatic assertions (`AssertRet`, `AssertABIPreserved`, `AssertFlag`,
`AssertFP`, `AssertNoFault`, `AssertFault`, `AssertEmuReg`) take a small `TB` interface that
`*testing.T` satisfies, so they fail a test with a legible message — and are
themselves testable (the suite proves each one bites on bad input).

## Run

```sh
make go-test          # from the repo root (needs the shared libs + the Go toolchain)
make docker-go        # or in an isolated container
```

`make go-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance_test.go`](conformance_test.go) (the corpus replayed via `go test`)
with `CGO_LDFLAGS` and `LD_LIBRARY_PATH` / `DYLD_LIBRARY_PATH` pointing at the
build directory. The emulator case uses the x86-64 guest, so run it on an x86-64
target (e.g. `make docker-go DOCKER_PLATFORM=linux/amd64`).

## In-line assembler (optional)

Pass a routine as an **assembly string**. The assembler entry points live in the
Keystone-carrying `libasmtest_emu` (now the full superset), which Go resolves at
run time through the dynamic loader, so `AsmAvailable()` is true by default. It
stays a defensive probe — false only if `ASMTEST_LIB` points at an older/leaner
lib without the assembler.

```go
if asmtest.AsmAvailable() {
    e := asmtest.NewEmu(); defer e.Close()
    out := asmtest.NewEmuResult(); defer out.Free()
    // Intel, up to six args; err carries the Keystone diagnostic if it didn't assemble.
    if err := e.CallAsm("mov rax, rdi; add rax, rsi; ret",
        []int64{40, 2}, asmtest.SyntaxIntel, 0, out); err == nil {
        _ = out.X86Reg("rax") // 42
    }
    // Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32), even guests the
    // x86 emulator can't run.
    a64, _ := asmtest.Assemble("ret", asmtest.ArchArm64, asmtest.SyntaxIntel, 0x00100000)
    _ = a64
}
```

## Native trace (DynamoRIO, optional)

[`drtrace.go`](drtrace.go) mirrors the Python wrapper
(`bindings/python/asmtest/drtrace.py`): where the emulator traces isolated guest
bytes, `NativeTrace` traces host-native code as it runs **inside this Go process**
under DynamoRIO. The tier lives in a separate lib, `libasmtest_drapp`, which is
built only when DynamoRIO is present — so, exactly like the in-line assembler
above, this file does **not** link it. It `dlopen`s the lib at run time (from
`$ASMTEST_DRAPP_LIB`, else `<repo>/build/libasmtest_drapp.so`) and `dlsym`s the
entry points; if the lib (or `libdynamorio` inside it) can't be resolved,
`NativeTraceAvailable()` returns false and callers self-skip. Linux x86-64 only.

```go
if asmtest.NativeTraceAvailable() {
    // client="" -> NULL -> the C side reads $ASMTEST_DRCLIENT.
    if err := asmtest.NativeTraceInitializeDefault(); err == nil {
        defer asmtest.NativeTraceShutdown()
        // mov rax,rdi; add rax,rsi; ret
        code, _ := asmtest.NativeCodeFromBytes([]byte{0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3})
        defer code.Free()
        tr := asmtest.NewNativeTrace(64, 0) // blocks=64, instructions=0
        defer tr.Free()
        tr.Register("add", code)
        var r int64
        tr.Region("add", func() { r = code.Call(20, 22) }) // Begin/End balanced
        _ = r              // 42
        _ = tr.Covered(0)  // entry block entered
        tr.Unregister("add")
    }
}
```

The `make docker-drtrace` lane runs the C + Python tiers in a DynamoRIO container;
[`drtrace_test.go`](drtrace_test.go) self-skips wherever DynamoRIO is absent (so
it never fails on AMD hosts, VMs, or standard CI).

## Deferred

A published Go module (with the native libs bundled per platform) is future work;
this is the Tier-1 + Tier-2 binding that proves the `cgo` path.
