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

The [.NET binding](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Program.cs)
drives the opaque-handle FFI layer through **P/Invoke** (`DllImport`). The
reusable pattern: declare the entry points, load your routine library with
`NativeLibrary`, and assert with any runner (xUnit shown). Fields are read
through accessors, so no struct layout is mirrored.

```csharp
using System;
using System.Runtime.InteropServices;
using Xunit;

static class Native {
    const string EMU = "asmtest_emu";
    [DllImport(EMU)] public static extern IntPtr asmtest_regs_new();
    [DllImport(EMU)] public static extern void   asmtest_regs_free(IntPtr r);
    [DllImport(EMU)] public static extern void   asmtest_capture6(IntPtr o, IntPtr fn,
                         long a0, long a1, long a2, long a3, long a4, long a5);
    [DllImport(EMU)] public static extern void   asmtest_capture_fp2(IntPtr o, IntPtr fn, double f0, double f1);
    [DllImport(EMU)] public static extern ulong  asmtest_regs_ret(IntPtr r);
    [DllImport(EMU)] public static extern double asmtest_regs_fret(IntPtr r);
    [DllImport(EMU)] public static extern int    asmtest_regs_flag_set(IntPtr r, string name);
    [DllImport(EMU)] public static extern int    asmtest_check_abi(IntPtr r, IntPtr msg, nuint n);
}

public class MyRoutineTests {
    static readonly IntPtr Lib = NativeLibrary.Load("./libmyroutines.so");
    static IntPtr Fn(string name) => NativeLibrary.GetExport(Lib, name);

    [Fact] public void AddSigned() {
        IntPtr r = Native.asmtest_regs_new();
        try {
            Native.asmtest_capture6(r, Fn("add_signed"), 40, 2, 0, 0, 0, 0);
            Assert.Equal(42UL, Native.asmtest_regs_ret(r));
            Assert.Equal(0, Native.asmtest_check_abi(r, IntPtr.Zero, 0)); // 0 == ABI preserved
        } finally { Native.asmtest_regs_free(r); }
    }

    [Fact] public void FpAdd() {
        IntPtr r = Native.asmtest_regs_new();
        try {
            Native.asmtest_capture_fp2(r, Fn("fp_add"), 1.5, 2.25);
            Assert.Equal(3.75, Native.asmtest_regs_fret(r));
        } finally { Native.asmtest_regs_free(r); }
    }
}
```

Run with the library path exported so `asmtest_emu` resolves by soname:

```sh
export LD_LIBRARY_PATH=$PWD/build      # DYLD_LIBRARY_PATH on macOS
dotnet test
```

For the emulator tier the same source shows the handle dance:
`emu_open()` → `asmtest_emu_call2(e, fn, a0, a1, res)` →
`asmtest_emu_result_faulted(res)` / `asmtest_emu_x86_reg(res, "rax")`. The
shipped `Program.cs` resolves built-in fixtures via `asmtest_corpus_routine`;
swap in `NativeLibrary` (as above) to test your own routines.

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

## Binding maturity

- **Python** is the reference binding: packaged (`pyproject.toml` / wheel),
  `pytest` fixtures, and both Tier-1 and Tier-2 layers. The most turnkey for a
  real project today.
- **.NET and Go** are Tier-1 + Tier-2 bindings that prove the P/Invoke and `cgo`
  paths. They work and ship idiomatic assertions, but are not yet published
  packages with per-platform native libraries bundled — that staging is tracked
  in [Packaging the bindings](packaging.md). The patterns above are exactly how
  the repo wires `make dotnet-test` and `make go-test`.

All three deliberately avoid mirroring `regs_t`: Python reads field offsets from
the layout manifest, while .NET and Go call the opaque-handle accessors. That
binding-ABI surface — the array-form capture entry points, the verdict shims, and
the opaque-handle accessors — is catalogued in the
[API reference](api-reference.md).
