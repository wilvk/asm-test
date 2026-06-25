# asm-test — Python binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Python, and let
`pytest` validate the results. This is the reference binding for the
[multi-language bindings plan](../../docs/plans/multi-language-bindings-plan.md)
(Track P).

It is **pure ctypes** — no compile step, no `cffi`/`numpy` dependency. Struct
layouts are read from the `asmtest_abi.json` manifest (`make manifest`), so the
binding is automatically correct for whatever architecture the shared library
was built for. The C `_Static_assert`s guarantee the manifest matches the real
structs.

## Build the shared libraries

From the repository root:

```sh
make shared-emu   # libasmtest_emu.{so,dylib} — capture trampoline + emulator
make manifest     # asmtest_abi.json          — layout the binding consumes
```

The binding finds these next to the repo's `build/` dir automatically, or via
the `ASMTEST_LIB` and `ASMTEST_MANIFEST` environment variables.

## Usage

```python
import asmtest

# Native capture: run through the real ABI, inspect the register snapshot.
r = asmtest.capture(add_signed_addr, 40, 2)
assert r.ret == 42
assert r.abi_preserved            # via the native non-jumping verdict shim

r = asmtest.capture_fp(fp_add_addr, fargs=[1.5, 2.25])
assert r.fret == 3.75

# Emulator: faults are DATA, not a crash.
with asmtest.Emulator() as e:
    res = e.call(routine_addr, [40, 2])
    assert not res.faulted
    assert res.reg("rax") == 42
```

A routine reference is either an integer address or a ctypes function pointer
(e.g. `ctypes.CDLL("mylib.so").my_routine`).

## In-line assembler (optional)

Pass a routine as an **assembly string** instead of an address. Present only in
the Keystone-carrying `libasmtest_emu_asm` (`make python-asm-test` points
`ASMTEST_LIB` at it); `asmtest.asm_available()` is false against the plain lib.

```python
if asmtest.asm_available():
    with asmtest.Emulator() as e:
        res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])   # Intel, up to 6 args
        assert res.reg("rax") == 42
        # AT&T syntax + an instruction cap; raises asmtest.AsmtestError (carrying
        # the Keystone diagnostic) if the string fails to assemble.
        e.call_asm(src, [10, 20, 12], syntax=asmtest.Syntax.ATT, max_insns=0)
    # Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32), even guests the x86
    # emulator can't run; raises on a Keystone error.
    a64 = asmtest.assemble("ret", asmtest.Arch.ARM64)
```

## Crash safety

The **native** capture path runs the routine on real hardware in the Python
process: a buggy routine (bad store, stack smash) can crash the interpreter.
For untrusted or under-development routines, prefer the **emulator** path, which
turns invalid accesses into `EmuResult.faulted` data, or run native captures in
a `multiprocessing` child so a crash is contained.

## Tests

`make python-test` (from the repo root) builds the shared libs, the manifest,
and the conformance corpus, then runs the suite — including
[`tests/test_conformance.py`](tests/test_conformance.py), which replays the same
`corpus.json` the C reference emits and must reproduce every result.
