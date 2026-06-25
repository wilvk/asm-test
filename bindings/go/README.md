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

Pass a routine as an **assembly string**. The assembler entry points live only
in the Keystone-carrying `libasmtest_emu_asm`; Go statically links the plain lib
and resolves them at run time through the dynamic loader, so `AsmAvailable()` is
true only when `ASMTEST_LIB` points at the assembler lib (`make go-asm-test`).

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

## Deferred

A published Go module (with the native libs bundled per platform) is future work;
this is the Tier-1 + Tier-2 binding that proves the `cgo` path.
