# Go binding

The [Go binding](https://github.com/wilvk/asm-test/blob/main/bindings/go/asmtest.go)
is a real `go test` package (module `github.com/wilvk/asm-test/bindings/go`),
driving asm-test via `cgo`, with Tier-2 assertions that take a `*testing.T`.

`cgo` declares the opaque-handle FFI helpers and links the prebuilt shared
libraries, so no C struct layout is mirrored in Go: `asmtest_corpus_routine` for
addresses, `asmtest_capture6` / `_fp2` + `asmtest_regs_*` for capture (with
`asmtest_check_abi` as the ABI-preservation verdict), and `asmtest_emu_call2` +
accessors for the emulator (faults as data: `EmuResult.Faulted()`, plus
`FaultAddr()` / `FaultKind()` for where and why). The result handles are all
C-allocated, so they are exempt from cgo's pointer-passing rules. See
[Language bindings](../bindings.md) for the shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu      # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

Export the library path so `asmtest_emu` resolves at run time:

```sh
export LD_LIBRARY_PATH=$PWD/build:$PWD       # DYLD_LIBRARY_PATH on macOS
```

The binding package's own `cgo` directives link `-lasmtest_emu -lasmtest_corpus`
relative to its source, so importing it pulls those in; you add only
`-lmyroutines` for your own routines.

## Usage

`CorpusRoutine` resolves only the built-in fixtures, so to test your own routine
add a small `cgo` shim that returns its address:

```go
// myroutines_test.go
package myroutines

/*
#cgo LDFLAGS: -L${SRCDIR} -lmyroutines
extern long add_signed(long, long);
static void *addr_add_signed(void) { return (void *)add_signed; }
*/
import "C"

import (
    "testing"

    asmtest "github.com/wilvk/asm-test/bindings/go"
)

func TestAddSigned(t *testing.T) {
    r := asmtest.NewRegs()
    defer r.Free()
    r.Capture6(C.addr_add_signed(), 40, 2)   // void* -> asmtest.Routine
    asmtest.AssertRet(t, r, 42)
    asmtest.AssertABIPreserved(t, r)
}

func TestUnderEmulator(t *testing.T) {
    e := asmtest.NewEmu()
    defer e.Close()
    res := asmtest.NewEmuResult()
    defer res.Free()
    e.Call2(C.addr_add_signed(), 40, 2, res)
    asmtest.AssertNoFault(t, res)
    asmtest.AssertEmuReg(t, res, "rax", 42)   // the emulator guest is x86-64
}
```

The Tier-2 assertions (`AssertRet`, `AssertABIPreserved`, `AssertFlag`,
`AssertFP`, `AssertNoFault`, `AssertFault`, `AssertEmuReg`) take a small `TB`
interface that `*testing.T` satisfies, so they fail a test with a legible message.

## In-line assembler (optional)

Pass a routine as an **assembly string**. The assembler entry points live only in
the Keystone-carrying `libasmtest_emu_asm`; Go statically links the plain lib and
resolves them at run time through the dynamic loader, so `AsmAvailable()` is true
only when `ASMTEST_LIB` points at the assembler lib (`make go-asm-test`).

```go
func TestInlineAssembler(t *testing.T) {      // optional: pass the routine as text
    if !asmtest.AsmAvailable() {              // false against the plain libasmtest_emu
        t.Skip("assembler not in this build (run `make go-asm-test`)")
    }
    e := asmtest.NewEmu()
    defer e.Close()
    res := asmtest.NewEmuResult()
    defer res.Free()
    // Intel, up to six args; the error carries the Keystone diagnostic on a bad string.
    if err := e.CallAsm("mov rax, rdi; add rax, rsi; ret",
        []int64{40, 2}, asmtest.SyntaxIntel, 0, res); err != nil {
        t.Fatal(err)
    }
    asmtest.AssertEmuReg(t, res, "rax", 42)
    // Assemble-only, any arch — even a guest the x86 emulator can't run:
    arm64Ret, _ := asmtest.Assemble("ret", asmtest.ArchArm64, asmtest.SyntaxIntel, 0x00100000)
    _ = arm64Ret                              // C0 03 5F D6
}
```

`Assemble` covers x86-64/arm64/riscv64/arm32. Run the emulator and assembler
cases on an x86-64 target (the guest is x86-64).

## Run the tests

```sh
export LD_LIBRARY_PATH=$PWD/build:$PWD       # DYLD_LIBRARY_PATH on macOS
CGO_ENABLED=1 go test ./...
make go-asm-test                             # adds libasmtest_emu_asm so CallAsm/Assemble light up
```

`make go-test` (from the repo root) builds `libasmtest_emu` + the routine fixture
lib, then runs
[`conformance_test.go`](https://github.com/wilvk/asm-test/blob/main/bindings/go/conformance_test.go)
(the corpus replayed via `go test`). The emulator case uses the x86-64 guest, so
run it on an x86-64 target (e.g. `make docker-go DOCKER_PLATFORM=linux/amd64`).

## Maturity

A published Go module (with the native libs bundled per platform) is future work;
this is the Tier-1 + Tier-2 binding that proves the `cgo` path. See
[Packaging the bindings](../packaging.md).
