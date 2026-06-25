# Language bindings

asm-test ships bindings for ten languages so you can drive the framework's two
engines from your own test suite: the **capture trampoline** (run a routine
through the real ABI and snapshot registers/flags) and the **emulator** (run it
in a virtual CPU, where faults are *data*, not a crash). This page shows
end-to-end usage for **Python**, **.NET**, and **Go**; the other seven (Rust,
C++, Zig, Node, Java, Ruby, Lua) follow the same shape. For how each package is
assembled and published, see [Packaging the bindings](packaging.md).

Every binding loads the shared library built from this repo and calls the
**binding ABI** — the macro-free entry points catalogued in the
[API reference](api-reference.md). The Python binding reads struct layout from
the `asmtest_abi.json` manifest; .NET and Go go through the opaque-handle
accessors (`asmtest_regs_*`, `asmtest_emu_*`), so no `regs_t` layout is mirrored
on their side.

## One-time setup

From the repository root, build the native library the bindings load:

```sh
make shared-emu    # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
make manifest      # asmtest_abi.json — required by the Python binding only
```

Your *routine under test* is any System V ABI function in a shared library.
Assemble yours with the
[`asm.h`](https://github.com/wilvk/asm-test/blob/main/include/asm.h) shim into
one:

```sh
cc -shared -fPIC -Iinclude -o libmyroutines.so myroutines.s
```

At run time the dynamic loader must find `libasmtest_emu` — point it at the
build directory with `LD_LIBRARY_PATH` (Linux) or `DYLD_LIBRARY_PATH` (macOS), or
set `ASMTEST_LIB` for Python. The emulator tier additionally needs **libunicorn**
(see [Emulator tier](emulator.md)).

## Python

The [Python binding](https://github.com/wilvk/asm-test/tree/main/bindings/python)
is pure `ctypes` — no compile step — and is the most complete: a packaged module,
`pytest`-native, with both a Tier-1 surface (`r.ret`, `r.abi_preserved`) and a
Tier-2 assertion layer. Pass it a `ctypes` function or a raw integer address.

```python
# test_myroutines.py
import ctypes
import asmtest
from asmtest.assertions import assert_ret, assert_abi_preserved, assert_flag, assert_fp

lib = ctypes.CDLL("./libmyroutines.so")   # your assembled routines

def test_add_signed():
    r = asmtest.capture(lib.add_signed, 40, 2)   # call through the real ABI
    assert_ret(r, 42)
    assert_abi_preserved(r)                       # callee-saved registers restored
    # Tier-1 style works too: assert r.ret == 42 and r.abi_preserved

def test_carry_flag():
    r = asmtest.capture(lib.set_carry)
    assert_flag(r, "CF", set=True)

def test_floating_point():
    r = asmtest.capture_fp(lib.fp_add, fargs=[1.5, 2.25])
    assert_fp(r, 3.75)

def test_under_emulator():           # faults become data, never a crash
    with asmtest.Emulator() as e:
        res = e.call(lib.add_signed, [40, 2])
    assert not res.faulted
    assert res.reg("rax") == 42
```

Run it:

```sh
pip install ./bindings/python pytest          # or: pip install asmtest (once published)
export ASMTEST_LIB=$PWD/build/libasmtest_emu.so
export ASMTEST_MANIFEST=$PWD/asmtest_abi.json
pytest
```

Capture helpers: `capture(fn, *args)` (up to 6 integer args),
`capture_fp(fn, iargs=…, fargs=…)`, and `capture_vec(fn, iargs=…, vargs=…)` with
the `asmtest.vec_f32(…)` / `vec_f64(…)` lane packers. The Tier-2 assertions live
in `asmtest.assertions` (`assert_ret`, `assert_abi_preserved`, `assert_flag`,
`assert_fp`, `assert_vec_f32`, `assert_no_fault`, `assert_reg`, …).

## .NET

The .NET binding ships a reusable library module,
[`Asmtest.cs`](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Asmtest.cs),
that keeps all P/Invoke (`DllImport`) inside and exposes the `Regs` / `Emu` /
`EmuResult` types plus an `Assert` helper — so your test code never declares a
native entry point. Add `Asmtest.cs` to your test project (until the NuGet
package ships), load your routine library with `NativeLibrary`, and assert with
any runner (xUnit shown):

```csharp
using System;
using System.Runtime.InteropServices;
using Xunit;
using Asm = Asmtest;   // alias so Asm.Assert doesn't collide with Xunit.Assert

public class MyRoutineTests {
    static readonly IntPtr Lib = NativeLibrary.Load("./libmyroutines.so");
    static IntPtr Fn(string name) => NativeLibrary.GetExport(Lib, name);

    [Fact] public void AddSigned() {
        using var r = new Asm.Regs();
        r.Capture6(Fn("add_signed"), 40, 2);    // call through the real ABI
        Asm.Assert.Ret(r, 42);
        Asm.Assert.AbiPreserved(r);              // callee-saved registers restored
    }

    [Fact] public void FpAdd() {
        using var r = new Asm.Regs();
        r.CaptureFp2(Fn("fp_add"), 1.5, 2.25);
        Asm.Assert.Fp(r, 3.75);
    }

    [Fact] public void UnderEmulator() {         // faults become data, never a crash
        using var e = new Asm.Emu();
        using var res = e.Call2(Fn("add_signed"), 40, 2);
        Asm.Assert.NoFault(res);
        Asm.Assert.EmuReg(res, "rax", 42);
    }
}
```

Run with the library path exported so `asmtest_emu` resolves by soname:

```sh
export LD_LIBRARY_PATH=$PWD/build      # DYLD_LIBRARY_PATH on macOS
dotnet test
```

The wrapper's `Corpus.Routine(name)` resolves the built-in fixtures;
`NativeLibrary` (as above) resolves your own routines.

## Go

The [Go binding](https://github.com/wilvk/asm-test/blob/main/bindings/go/asmtest.go)
is a real `go test` package (module `github.com/wilvk/asm-test/bindings/go`) with
Tier-2 assertions that take a `*testing.T`. Its `CorpusRoutine` resolves only the
built-in fixtures, so to test your own routine add a small `cgo` shim that returns
its address:

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

Run it:

```sh
export LD_LIBRARY_PATH=$PWD/build:$PWD       # DYLD_LIBRARY_PATH on macOS
CGO_ENABLED=1 go test ./...
```

The binding package's own `cgo` directives link `-lasmtest_emu -lasmtest_corpus`
relative to its source, so importing it pulls those in; you add only
`-lmyroutines` for your routines. Run the emulator case on an x86-64 target (the
guest is x86-64).

## Every binding has a reusable module

All ten bindings now expose a **reusable library module** that keeps the FFI
inside and presents an idiomatic surface — capture/emulator handles plus Tier-2
assertions — with a thin conformance runner consuming it (the same corpus, in
each language). So none of them require FFI declarations in your own test code:

| Language | Module | FFI mechanism | Consumer |
|---|---|---|---|
| Python | `asmtest/` package | `ctypes` | `pytest` suite |
| Go | `asmtest.go` | `cgo` | `conformance_test.go` |
| Rust | `src/` crate | `extern` + build script | `tests/` |
| C++ | `asmtest.hpp` | direct `#include` | `test_cpp.cpp` |
| Zig | `src/` module | `@cImport` | build step |
| Node | `asmtest.js` | `koffi` | `conformance.js` |
| Ruby | `asmtest.rb` | `Fiddle` | `conformance.rb` |
| Lua | `asmtest.lua` | LuaJIT `ffi` | `conformance.lua` |
| Java | `Asmtest.java` | FFM (Panama) | `Conformance.java` |
| .NET | `Asmtest.cs` | P/Invoke | `Program.cs` |

Every module deliberately avoids mirroring `regs_t`: Python reads field offsets
from the layout manifest, and the rest call the opaque-handle accessors. That
binding-ABI surface — the array-form capture entry points, the verdict shims, and
the opaque-handle accessors — is catalogued in the
[API reference](api-reference.md).

## Maturity

**Python** is the reference binding: packaged (`pyproject.toml` / wheel),
`pytest` fixtures, and both tiers — the most turnkey today. The others ship the
same reusable module and Tier-2 assertions but are **not yet published packages**
with per-platform native libraries bundled; that staging is tracked in
[Packaging the bindings](packaging.md). Today you consume them as shown above
(referencing the module and pointing at the built shared libs) — exactly how the
repo wires `make <lang>-test`.
