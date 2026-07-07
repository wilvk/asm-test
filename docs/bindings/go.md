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
[Language bindings](index.md) for the shared architecture.

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

Pass a routine as an **assembly string**. The assembler entry points live in the
Keystone-carrying `libasmtest_emu` (now the full superset), which Go resolves at
run time through the dynamic loader, so `AsmAvailable()` is true by default.
`AsmAvailable()` stays a defensive probe: it goes false only if `ASMTEST_LIB`
points at an older/leaner lib without the assembler.

```go
func TestInlineAssembler(t *testing.T) {      // optional: pass the routine as text
    if !asmtest.AsmAvailable() {              // false only against an older/leaner lib
        t.Skip("assembler not in this build (run `make go-test`)")
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

## Function reference

Every exported identifier of `package asmtest`, with an example and its options.
Each handle (`Regs`, `Emu`, `EmuResult`, `Trace`, `Guest`, `GuestResult`) is
C-allocated by a `New…` constructor and released with `Free`/`Close` — pair them
with `defer`. The emulator calls take an explicit `*EmuResult` (or `*GuestResult`)
out-param rather than returning one, so you can reuse a handle across runs.

### Resolving routines

```go
fn := asmtest.CorpusRoutine("add_signed")   // built-in fixture -> asmtest.Routine (nil if unknown)
ok := asmtest.AsmAvailable()                // is the in-line assembler in this build?
```

`Routine` is an opaque `unsafe.Pointer`. Your own routines come from a tiny `cgo`
shim returning the function address (see [Usage](#usage)).

### Capture tier — `Regs`

```go
r := asmtest.NewRegs(); defer r.Free()
r.Capture6(fn, 40, 2)                        // up to 6 integer args (variadic; missing default 0)
r.CaptureFP2(fn, 1.5, 2.25)                  // two double args; FP return in r.FRet()
r.CaptureVecF32(fn, [][4]float32{{1,2,3,4}}) // up to 8 128-bit vectors (four float32 lanes each)
ret := r.Ret()                               // int64 integer return (rax)
fr  := r.FRet()                              // float64 scalar return (xmm0)
v   := r.VecF32(0)                           // [4]float32 lanes of vector register 0
cf  := r.FlagSet("CF")                       // condition flag by name (CF/PF/ZF/SF/OF)
abi := r.ABIPreserved()                      // every callee-saved register restored
```

### Emulator tier — `Emu` / `EmuResult`

```go
e := asmtest.NewEmu(); defer e.Close()       // x86-64 Unicorn guest
res := asmtest.NewEmuResult(); defer res.Free()
e.Call2(fn, 40, 2, res)                          // routine address + two int args
e.CallBytes(code, []int64{40, 2}, res)           // raw machine-code bytes, up to 6 int args
e.CallFP(code, []int64{1}, []float64{1.5}, res)  // doubles -> xmm0..7 (return: res.XmmF64(0,0))
e.CallVec(code, nil, [][4]float32{{1,2,3,4}}, res) // 128-bit vecs -> xmm0..7
e.CallWin64(code, []int64{1,2,3,4}, res)         // Microsoft x64 (rcx, rdx, r8, r9)
```

* `Call2` takes a routine **address** (reads a 64-byte code window); two int args.
* `CallBytes`/`CallFP`/`CallVec`/`CallWin64`/`CallTraced` take raw `[]byte` machine
  code and run it whole. A `nil` arg slice is treated as empty.

Read the outcome — faults are data, not a crash:

```go
res.Faulted()            // bool: hit an invalid access?
res.FaultAddr()          // uint64: where (valid when Faulted)
res.FaultKind()          // FaultNone / FaultRead / FaultWrite / FaultFetch
res.X86Reg("rax")        // any GP register, plus "rip" / "rflags"
res.XmmF64(0, 0)         // xmm lane as float64 (scalar FP return)
res.XmmF32(0, 0)         // xmm lane as float32 (vector return)
```

### Execution trace / coverage — `Trace`

```go
tr := asmtest.NewTrace(4096, 4096); defer tr.Free()   // insns / blocks buffer caps
e.CallTraced(code, []int64{1, 2}, tr, res)            // record while running
tr.Covered(0x0)          // bool: was the basic block at this byte-offset entered?
tr.InsnsTotal()          // uint64: instructions executed (counts past the cap)
tr.BlocksLen()           // uint64: distinct basic blocks recorded
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate, opt-in tier from the emulator `Trace` above: instead of running guest
bytes under Unicorn, it traces **host-native code as it runs inside this Go
process** under DynamoRIO. It lives in `libasmtest_drapp`, dlopen'd at run time, so
`NativeTraceAvailable()` self-skips cleanly when the lib (or DynamoRIO) is absent.
Bring the tier up with `NativeTraceInitializeDefault()` (env-driven) and tear it
down with `NativeTraceShutdown()`; materialize bytes into a `NativeCode`, allocate
a `NewNativeTrace(blocks, instructions)`, `Register` the range, then call into it
inside a `Region` (balanced begin/end). Read coverage back in the same
`Covered`/`BlocksLen`/`InsnsTotal` shape, plus `BlockOffsets()`/`InsnOffsets()`.

```go
func TestNativeTrace(t *testing.T) {
    if !asmtest.NativeTraceAvailable() {       // no libasmtest_drapp / DynamoRIO -> self-skip
        t.Skip("DynamoRIO native-trace tier unavailable")
    }
    if err := asmtest.NativeTraceInitializeDefault(); err != nil {
        t.Skipf("dr_init/start failed: %v", err)
    }
    defer asmtest.NativeTraceShutdown()

    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
    routine := []byte{
        0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
        0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
    }
    code, err := asmtest.NativeCodeFromBytes(routine)
    if err != nil {
        t.Fatal(err)
    }
    defer code.Free()

    // Instruction mode: blocks=64, instructions=64 (both buffers recorded).
    tr := asmtest.NewNativeTrace(64, 64)
    defer tr.Free()
    if err := tr.Register("add2", code); err != nil {
        t.Fatal(err)
    }
    defer tr.Unregister("add2")

    var r int64
    tr.Region("add2", func() { r = code.Call(60, 60) })   // 120 > 100 -> dec -> 119
    if r != 119 || !tr.Covered(0) {                       // entry block entered
        t.Fatalf("Call=%d, Covered(0)=%v", r, tr.Covered(0))
    }
    _ = tr.BlockOffsets()                                 // distinct block starts, first-seen order
    _ = tr.InsnOffsets()                                  // jle-taken path -> [0 3 6 0xc 0x11]
}
```

Symbol mode traces an exported function by NAME with no region or markers —
recording is always on for `[entry, entry+maxLen)`:

```go
tr := asmtest.NewNativeTrace(64, 0)                       // blocks=64, instructions=0
defer tr.Free()
if err := tr.RegisterSymbol("asmtest_symbol_demo", 256); err != nil {
    t.Fatal(err)
}
defer tr.Unregister("asmtest_symbol_demo")
if got := asmtest.SymbolDemo(3, 4); got != 10 {          // exported fixture: a*2+b
    t.Fatalf("SymbolDemo(3,4)=%d, want 10", got)
}
if !tr.Covered(0) {
    t.Error("entry block (offset 0) expected covered")
}
```

Linux x86-64 only; self-skips without DynamoRIO. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier records the **same** `asmtest_trace_t` coverage from the real
CPU, but needs no separate engine install: it defaults to the **single-step**
backend (the CPU's `EFLAGS.TF` trap flag), so `HwTraceAvailable(SingleStep)` is true
and it **traces live on any x86-64 Linux** — CI and plain containers included —
where the DynamoRIO tier needs a DynamoRIO install. Intel PT and AMD LBR are picked
automatically on the bare-metal hardware that has them.

```go
func TestHwTrace(t *testing.T) {
    if !asmtest.HwTraceAvailable(asmtest.SingleStep) {     // self-skip off x86-64 Linux
        t.Skipf("single-step backend unavailable: %s",
            asmtest.HwTraceSkipReason(asmtest.SingleStep))
    }
    if err := asmtest.HwTraceInit(asmtest.SingleStep); err != nil {
        t.Skipf("hwtrace init: %v", err)
    }
    defer asmtest.HwTraceShutdown()

    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
    code, _ := asmtest.HwNativeCodeFromBytes([]byte{
        0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
        0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3})
    defer code.Free()

    tr := asmtest.NewHwTrace(64, 64)                       // blocks=64, instructions=64
    defer tr.Free()
    tr.Register("add2", code)

    var r int64
    tr.Region("add2", func() { r = code.Call(20, 22) })    // 42; jle taken, dec skipped
    // tr.InsnOffsets() == [0 3 6 0xc 0x11] — byte-for-byte the Unicorn/DynamoRIO/PT result
    if r != 42 || !tr.Covered(0) {
        t.Fatalf("Call=%d Covered(0)=%v", r, tr.Covered(0))
    }
}
```

`asmtest.HwTraceAuto(BEST)` / `HwTraceResolve(...)` pick the host's most-faithful
available backend (Intel PT → AMD LBR → single-step), and `ResolveTiers` / `AutoTier`
extend the cascade across the DynamoRIO and emulator tiers. An out-of-process
`Ptrace` surface (`PtraceTraceCall`, `PtraceRunTo`, …) traces a method in a
**separate** process — the managed-runtime path. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

**Scoped tracing** — the closure *import + scope* form (`Scope`). It auto-names the
region from the call site (`runtime.Caller`), pins the goroutine with
`runtime.LockOSThread` for the region, renders the executed assembly on close, and
returns a `ScopedResult`. Work fanned out via `go func()` runs on another OS thread and
is silently untraced (a disclosed gap); `res.Truncated` flags a cross-thread close.

```go
_ = asmtest.HwTraceInit(asmtest.SingleStep)
code, _ := asmtest.HwNativeCodeFromBytes([]byte{0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3}) // add2; ret
tr := asmtest.NewHwTrace(64, 256)
var r int64
res := tr.Scope(code, false, func() { r = code.Call(20, 22) }) // auto-named "file.go:<line>"
// res.Path holds the disassembly that executed; res.Truncated is the thread-scope bit
```

### Cross-arch guests — `Guest` / `GuestResult`

These run **raw machine-code bytes** on any host.

```go
g := asmtest.NewGuest("arm64"); defer g.Close()       // "arm64" | "riscv" | "arm" (nil if unknown)
gr := asmtest.NewGuestResult("arm64"); defer gr.Free()
g.Call(code, []int64{40, 2}, gr)                      // ints in x0..x5 / a0..a7 / r0..r3
g.CallFP(code, nil, []float64{1.5}, gr)               // doubles into the FP arg regs
g.CallVec(code, nil, [][4]float32{{1,2,3,4}}, gr)     // arm64 / arm only (RISC-V has no vec file)
g.CallTraced(code, []int64{1}, tr, gr)               // same Trace recorder
gr.Reg("x0")             // register by name: x0/sp/pc/nzcv, a0/x10/ra/sp, r0/lr/pc/cpsr
gr.Faulted()             // faults are data here too
gr.VecF64(0, 0)          // FP/vector lane (v / q / f register file)
```

### In-line assembler (optional) — `Emu.CallAsm` / `Assemble`

```go
if asmtest.AsmAvailable() {
    err := e.CallAsm("mov rax, rdi; add rax, rsi; ret",
                     []int64{40, 2}, asmtest.SyntaxIntel, 0, res)   // assemble x86-64 + run
    // err carries the Keystone diagnostic on a bad string; 0 maxInsns runs to ret.
    bytes, err := asmtest.Assemble("ret", asmtest.ArchArm64, asmtest.SyntaxIntel, 0x00100000)
    _ = asmtest.AsmError()    // last Keystone diagnostic on this thread ("" on success)
}
```

* `(*Emu).CallAsm(src, args, syntax, maxInsns, out) error` — assemble x86-64 `src`
  and run it (≤6 int args). `syntax` is `SyntaxIntel`/`SyntaxAtt`/…; `maxInsns: 0`
  runs to `ret`. Returns a non-nil error on a Keystone failure or Keystone-free
  build.
* `Assemble(src, arch, syntax, addr) ([]byte, error)` — assemble-only, any of
  `ArchX8664`/`ArchArm64`/`ArchRiscv64`/`ArchArm32`; `addr` is the base load
  address.
* `AsmSyntax` covers `SyntaxIntel`/`Att`/`Nasm`/`Masm`/`Gas`.

### Tier-2 assertions

Each takes a `TB` (which `*testing.T` satisfies) and fails the test with a
legible message.

```go
asmtest.AssertRet(t, r, 42)                  // r.Ret() == 42
asmtest.AssertABIPreserved(t, r)             // callee-saved restored
asmtest.AssertFlag(t, r, "CF", true)         // flag set/clear
asmtest.AssertFP(t, r, 3.75)                 // r.FRet() == 3.75
asmtest.AssertVecF32(t, r, 0, [4]float32{1,2,3,4})  // vector lanes of register 0
asmtest.AssertNoFault(t, res)                // emulator run clean
asmtest.AssertFault(t, res)                  // emulator run faulted
asmtest.AssertEmuReg(t, res, "rax", 42)      // x86 guest register
asmtest.AssertGuestReg(t, gr, "x0", 42)      // cross-arch guest register
asmtest.AssertCovered(t, tr, 0x0)            // basic block entered
```

## Run the tests

```sh
export LD_LIBRARY_PATH=$PWD/build:$PWD       # DYLD_LIBRARY_PATH on macOS
CGO_ENABLED=1 go test ./...                  # libasmtest_emu already carries CallAsm/Assemble
```

`make go-test` (from the repo root) builds `libasmtest_emu` + the routine fixture
lib, then runs
[`conformance_test.go`](https://github.com/wilvk/asm-test/blob/main/bindings/go/conformance_test.go)
(the corpus replayed via `go test`). The emulator case uses the x86-64 guest, so
run it on an x86-64 target (e.g. `make docker-go DOCKER_PLATFORM=linux/amd64`).

## Maturity

A published Go module (with the native libs bundled per platform) is future work;
this is the Tier-1 + Tier-2 binding that proves the `cgo` path. See
[Packaging the bindings](../reference/packaging.md).

:::{note}
The Track D/E/F surface — `WatchWrites`, `GuardReg`, `FuzzCover`, `MutationTest`,
and `CaptureVec256` — ships in `asmtest.go` and is mapped in the
[shared capability table](index.md); the semantics of each are documented in full
on the [Python reference page](python.md), which this binding mirrors
name-for-name.
:::
