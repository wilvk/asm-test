# Python binding

The [Python binding](https://github.com/wilvk/asm-test/tree/main/bindings/python)
is the **reference binding** and the most complete: pure `ctypes` (no compile
step, no `cffi`/`numpy` dependency), a packaged module, `pytest`-native, with both
a Tier-1 surface (`r.ret`, `r.abi_preserved`) and a Tier-2 assertion layer. Pass
it a `ctypes` function or a raw integer address.

Struct layouts are read from the `asmtest_abi.json` manifest (`make manifest`),
so the binding is automatically correct for whatever architecture the shared
library was built for; the C `_Static_assert`s guarantee the manifest matches the
real structs. See [Language bindings](../bindings.md) for the shared architecture
and the [capabilities-at-a-glance table](../bindings.md#capabilities-at-a-glance).

## Setup

From the repository root, build the native library and the layout manifest:

```sh
make shared-emu   # libasmtest_emu.{so,dylib} — capture trampoline + emulator
make manifest     # asmtest_abi.json          — layout the Python binding consumes
```

Install the package (editable or once published):

```sh
pip install ./bindings/python pytest          # or: pip install asmtest (once published)
```

The binding finds the shared lib and manifest next to the repo's `build/` dir
automatically, or via the `ASMTEST_LIB` and `ASMTEST_MANIFEST` environment
variables:

```sh
export ASMTEST_LIB=$PWD/build/libasmtest_emu.so
export ASMTEST_MANIFEST=$PWD/asmtest_abi.json
```

## Usage

```python
# test_myroutines.py
import ctypes
import pytest
import asmtest
from asmtest.assertions import (
    assert_ret, assert_abi_preserved, assert_flag, assert_fp, assert_fault,
)

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

def test_bad_access_is_data():       # a faulting routine is a verdict, not a SIGSEGV
    with asmtest.Emulator() as e:
        res = e.call(lib.deref, [0])              # dereferences a null pointer arg
    assert_fault(res)
    assert res.fault_addr == 0                    # plus res.fault_kind for the cause
```

A routine reference is either an integer address or a `ctypes` function pointer
(e.g. `ctypes.CDLL("mylib.so").my_routine`).

Capture helpers: `capture(fn, *args)` (up to 6 integer args),
`capture_fp(fn, iargs=…, fargs=…)`, and `capture_vec(fn, iargs=…, vargs=…)` with
the `asmtest.vec_f32(…)` / `vec_f64(…)` lane packers. The Tier-2 assertions live
in `asmtest.assertions` — `assert_ret`, `assert_abi_preserved`, `assert_flag`,
`assert_fp`, `assert_vec_f32`, `assert_no_fault`, `assert_fault`, `assert_reg`,
and friends.

## In-line assembler (optional)

Pass a routine as an **assembly string** instead of an address. Present only in
the Keystone-carrying `libasmtest_emu_asm` (`make python-asm-test` points
`ASMTEST_LIB` at it); `asmtest.asm_available()` is false against the plain lib and
the assembler calls self-skip.

```python
def test_inline_assembler():
    if not asmtest.asm_available():  # only with libasmtest_emu_asm (make python-asm-test)
        pytest.skip("assembler not in this build")
    with asmtest.Emulator() as e:
        res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])   # Intel, ≤6 args
        assert res.reg("rax") == 42
        # AT&T syntax + a cap on executed instructions; a bad source string raises
        # asmtest.AsmtestError carrying the Keystone diagnostic.
        e.call_asm("mov %rdi,%rax; add %rsi,%rax; ret", [10, 32],
                   syntax=asmtest.Syntax.ATT, max_insns=2)
    # Assemble-only, any arch — even a guest the x86 emulator can't run:
    assert asmtest.assemble("ret", asmtest.Arch.ARM64) == b"\xc0\x03\x5f\xd6"
```

The in-line assembler adds `asmtest.asm_available()`, `Emulator.call_asm(…)`, the
multi-arch `asmtest.assemble(…)` (x86-64/arm64/riscv64/arm32), the `Arch` /
`Syntax` enums, and the `AsmtestError` raised on a Keystone failure.

## Run the tests

```sh
pytest
# For the in-line assembler tests, point ASMTEST_LIB at the Keystone-carrying lib
# (or just run the wired-up target, which does it for you):
make python-asm-test
```

`make python-test` (from the repo root) builds the shared libs, the manifest, and
the conformance corpus, then runs the suite — including
`tests/test_conformance.py`, which replays the same `corpus.json` the C reference
emits and must reproduce every result.

## Crash safety

The **native** capture path runs the routine on real hardware in the Python
process: a buggy routine (bad store, stack smash) can crash the interpreter. For
untrusted or under-development routines, prefer the **emulator** path, which turns
invalid accesses into `EmuResult.faulted` data, or run native captures in a
`multiprocessing` child so a crash is contained.

## Maturity

Python is the reference binding: packaged (`pyproject.toml` / wheel), `pytest`
fixtures, and both tiers — the most turnkey today. For how the package is
assembled and published, see [Packaging the bindings](../packaging.md).
