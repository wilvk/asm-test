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

Pass a routine as an **assembly string** instead of an address. The
Keystone assembler ships in `libasmtest_emu` (the superset lib), so it runs by
default under `make python-test`. `asmtest.asm_available()` stays as a defensive
probe — it returns false only if `ASMTEST_LIB` points at an older/leaner lib
without Keystone, in which case the assembler calls self-skip.

```python
def test_inline_assembler():
    if not asmtest.asm_available():  # carried by libasmtest_emu; false only on an older/leaner lib
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

## Function reference

Every public entry point in the `asmtest` package, with a worked example and its
options. A *routine reference* (`fn`) is an `int` address or a `ctypes` function
pointer for the capture tier and `Emulator`; the cross-arch guests and the
raw-bytes paths take a `bytes` object of machine code instead.

### Module setup

```python
ctx = asmtest.load()        # process-wide Context (created lazily on first use)
ctx.version                 # native lib version string, e.g. "1.0.0"
ctx.arch                    # host arch the shared lib was built for, e.g. "x86_64"
ctx.has_emu                 # bool: emulator tier present (libasmtest_emu)
ctx.has_asm                 # bool: in-line assembler present (carried by libasmtest_emu)
ctx.flags                   # {"CF": mask, "ZF": mask, …} for this arch
ctx.size("regs_t")          # struct size from the manifest
ctx.offset("regs_t", "ret") # field offset from the manifest
```

`load()` returns the shared [`Context`]; you rarely call it directly — every other
function calls it for you. Use it to introspect the build (`has_emu`/`has_asm`
gate optional tiers) or read manifest layout.

### Capture tier

```python
r = asmtest.capture(fn, 40, 2)            # up to 6 integer args -> Regs
r = asmtest.capture_fp(fn, iargs=[1], fargs=[1.5, 2.25])  # ints + up to 8 doubles
r = asmtest.capture_vec(fn, vargs=[asmtest.vec_f32(1,2,3,4)])  # up to 8 128-bit vecs
out = asmtest.capture_vec256(fn, vargs=[v256])  # AVX2: 32-byte vecs, if cpu_has_avx2()
v = asmtest.vec_f32(1.0, 2.0, 3.0, 4.0)   # pack 4 float32 lanes -> 16-byte arg
v = asmtest.vec_f64(1.0, 2.0)             # pack 2 float64 lanes -> 16-byte arg
```

* `capture(fn, *args)` — calls `fn` through the integer System V ABI. Extra args
  past six are ignored (the trampoline has six integer slots).
* `capture_fp(fn, iargs=(), fargs=())` — `iargs` go in the GP registers, `fargs`
  (up to 8 `double`s) in xmm0..7. The scalar double return is `r.fret`.
* `capture_vec(fn, iargs=(), vargs=())` — each `vargs` entry is exactly 16 bytes
  (use the packers, or any `bytes`); a `ValueError` is raised otherwise. The
  vector return is `r.vec_f32(0)` / `r.vec_f64(0)`.
* `capture_vec256(fn, vargs=())` — AVX2 256-bit capture (Track D): each `vargs`
  entry is exactly 32 `bytes` into ymm0..7; returns a list of 16 × 32-byte lanes
  (`out[0]` is the return). x86-64 + AVX2 only — gate on `asmtest.cpu_has_avx2()`.

### `Regs` — a capture snapshot

```python
r.ret              # integer return value (rax), as unsigned
r.fret             # scalar double return (xmm0)
r.flags            # raw flags word
r.flag_set("CF")   # bool: is condition flag CF/PF/ZF/SF/OF set?
r.abi_preserved    # bool: every callee-saved register restored (native verdict shim)
r.vec_f32(0)       # [l0,l1,l2,l3]  four float32 lanes of vector register 0
r.vec_f64(0)       # [l0,l1]        two float64 lanes of vector register 0
r.vec_bytes(0)     # raw 16 bytes of vector register 0
```

`vec_*` take a vector-register index (default `0`, the return lane). `flag_set`
resolves the name against the host arch's mask from the manifest, so no flag
constants are needed.

### Emulator tier

```python
with asmtest.Emulator() as e:                     # Unicorn x86-64 guest
    res = e.call(fn, [40, 2])                      # int args -> EmuResult
    res = e.call_fp(fn, iargs=[1], fargs=[1.5])    # doubles into xmm0..7
    res = e.call_vec(fn, vargs=[asmtest.vec_f32(1,2,3,4)])  # 128-bit vecs
    res = e.call_win64(fn, [1, 2, 3, 4])           # Microsoft x64 convention (rcx,rdx,r8,r9)
    res = e.call(rawbytes, [1, 2])                 # `fn` may be raw machine-code bytes
```

Shared options on every `Emulator.call*`:

* `args` / `iargs` / `fargs` / `vargs` — argument lists (same shapes as the
  capture tier).
* `code_len=64` — how many bytes of `fn` to copy into the guest when `fn` is an
  address (ignored when `fn` is `bytes`, which is run whole).
* `max_insns=0` — instruction budget; `0` runs to `ret`. A nonzero cap stops a
  runaway loop and is how you bound untrusted code.

### `EmuResult` — an emulator outcome (faults are data)

```python
res.ok                 # bool: ran and returned cleanly
res.faulted            # bool: hit an invalid memory access (not a crash)
res.fault_addr         # guest address of the fault (meaningful when faulted)
res.fault_kind         # 0 none / 1 read / 2 write / 3 fetch
res.reg("rax")         # any GP register, plus "rip" / "rflags"
res.xmm_f64(0, 0)      # lane (0..1) of xmm register 0 as double (scalar FP return)
res.xmm_f32(0, 0)      # lane (0..3) of xmm register 0 as float32 (vector return)
res.ran                # bool: the FFI call itself dispatched
```

### Execution trace / coverage

```python
with asmtest.Emulator() as e, asmtest.Trace(insns_cap=4096, blocks_cap=4096) as t:
    res = e.call_traced(fn, [1, 2], trace=t)   # record while running
    t.covered(0x0)        # bool: was the basic block at this byte-offset entered?
    t.insns_total         # instructions executed (counts past the buffer cap)
    t.blocks_len          # number of distinct basic blocks recorded
```

* `Trace(insns_cap=4096, blocks_cap=4096)` — buffer capacities; entries past a
  cap are dropped and counted (`insns_total` still grows).
* `Emulator.call_traced(fn, args=(), trace=None, code_len=64, max_insns=0)` —
  like `call`, plus a `trace` to record into (`None` runs untraced).

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate **native** tier (`asmtest.drtrace`) traces host-native machine code as
it runs **inside this Python process**, backed by in-process DynamoRIO — the same
`asmtest_trace_t` coverage shape as the emulator `Trace` above, but on the real
CPU. Bring DynamoRIO up once, materialize host-native bytes with `NativeCode`,
mark a region, call into it, and read back basic-block (and optionally
instruction) coverage. Build the libraries with
`make shared-drtrace drtrace-client DYNAMORIO_HOME=...` and point the env at the
client (`ASMTEST_DRCLIENT`).

```python
from asmtest.drtrace import NativeTrace, NativeCode

if not NativeTrace.available():        # not built, or DynamoRIO not resolvable
    pytest.skip("DynamoRIO native-trace tier unavailable")

NativeTrace.initialize(client="build/libasmtest_drclient.so",
                       dynamorio_home="/opt/DynamoRIO")
# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
code = NativeCode.from_bytes(
    b"\x48\x89\xf8\x48\x01\xf0\x48\x3d\x64\x00\x00\x00\x7e\x03\x48\xff\xc8\xc3")

# Marker mode + instruction recording: bracket the call with region(name).
trace = NativeTrace.new(blocks=64, instructions=64)   # instructions>0 -> insn mode
trace.register("add", code)
with trace.region("add"):
    result = code.call(20, 22)         # 42 <= 100, so the jle is taken
assert result == 42 and trace.covered(0)              # entry block entered
trace.block_offsets()                  # distinct block starts, first-seen order
trace.insn_offsets()                   # ordered insn stream == [0x0, 0x3, 0x6, 0xc, 0x11]

# Symbol mode: trace a named exported function with no begin/end markers —
# recording is always-on over [entry, entry+max_len), so just call it.
trace2 = NativeTrace.new(blocks=64)
trace2.register_symbol("asmtest_symbol_demo", 256)
assert NativeTrace.symbol_demo(3, 4) == 10            # a*2 + b, no region()
assert trace2.covered(0)

NativeTrace.shutdown()
```

This tier is Linux x86-64 only and self-skips when DynamoRIO is absent; the full
reference (lifecycle, env vars, the managed-runtime caveat) lives in
[Native runtime tracing](../native-tracing.md).

### Mid-execution guards (Track F)

Assert a property *while* a routine runs, not just on its result (x86-64 guest).
A guard is armed on the handle and persists across calls until cleared.

```python
with asmtest.Emulator() as e:
    w = e.watch_writes(0x400000, 8, asmtest.EMU_WATCH_ONLY)  # writes must stay in [base, base+8)
    e.call(fn, [arg]); e.watch_clear()
    w.violated            # bool: a write escaped the region (w.addr / w.rip_off locate it)

    g = e.guard_reg("rbx", 0)          # rbx must read 0 at every basic-block entry
    e.call(fn, [arg]); e.guard_reg_clear()
    g.violated            # bool: corrupted mid-routine, even if restored before return
```

* `watch_writes(addr, size, mode)` — `EMU_WATCH_ONLY` flags a write that escapes
  the region; `EMU_WATCH_NEVER` one that touches it. `watch_clear()` disarms.
* `guard_reg(name, want)` / `guard_reg_clear()` — a callee-saved / stack-pointer
  invariant that catches corruption even when restored by return; raises
  `ValueError` for an unknown register name.

### Coverage-guided fuzzing & mutation testing (Track E)

Both drive a one-int-arg routine (raw `bytes`) entirely inside the emulator, so a
pathological input or a broken mutant cannot crash the host; both are seeded, so
runs reproduce.

```python
with asmtest.Emulator() as e:
    fz = e.fuzz_cover(code, -50, 50, iters=2000)   # keep inputs that grow block coverage
    fz.blocks_reached; fz.corpus_len               # reach + the coverage-expanding corpus

    mt = e.mutation_test(code, [-7, 0, 9])         # flip bits; run each mutant + the original
    mt.mutants; mt.killed; mt.survived             # survivors == a test-gap signal
```

* `fuzz_cover(code, lo, hi, iters, seed=0xC0FFEE, blocks_cap=256)` — draws inputs
  in `[lo, hi]`, keeping those that expand the basic-block union; returns a
  `FuzzStat`. Reaches blocks a handful of fixed vectors miss.
* `mutation_test(code, inputs, max_mutants=0, seed=0xABCD)` — runs every
  single-bit flip (`max_mutants=0`) or a seeded sample; returns a `MutationStat`
  (a stronger input set kills more). See `tests/test_fuzz.py`.

### Cross-arch guests (AArch64 / RISC-V / ARM32)

These run **raw machine-code bytes** on any host, so they need no host routine.

```python
with asmtest.GuestEmulator("arm64") as g:         # "arm64" | "riscv" | "arm"
    res = g.call(code, [40, 2])                    # ints in x0..x5 / a0..a7 / r0..r3
    res = g.call_fp(code, fargs=[1.5, 2.25])       # doubles in the FP arg regs
    res = g.call_vec(code, vargs=[asmtest.vec_f64(1,2)])  # arm64/arm only (RISC-V has none)
    res = g.call_traced(code, [1], trace=t)        # same Trace recorder
    res.reg("x0")          # register by name: x0../sp/pc/nzcv, a0../x10/ra/sp, r0../lr/pc/cpsr
    res.fault_addr; res.fault_kind                 # faults are data here too
    res.vec_f64(0, 0)      # FP/vector lane (v/q/f register file)
```

`GuestEmulator(arch)` raises `ValueError` for an unknown arch and `RuntimeError`
if the emulator tier is absent. `call_vec` on the RISC-V guest raises (no vector
file). Every `call*` takes the same `max_insns=0` budget.

### In-line assembler (carried by `libasmtest_emu`)

```python
asmtest.asm_available()                            # bool: assembler compiled in
with asmtest.Emulator() as e:
    res = e.call_asm("mov rax, rdi; ret", [42])    # assemble x86-64 + run -> EmuResult
    res = e.call_asm("mov %rdi,%rax; ret", [42],
                     syntax=asmtest.Syntax.ATT, max_insns=8)
code = asmtest.assemble("ret", asmtest.Arch.ARM64) # text -> bytes, any arch
asmtest.asm_error()                                # last Keystone diagnostic ("" on success)
```

* `Emulator.call_asm(src, args=(), syntax=0, max_insns=0)` — assemble x86-64
  `src` and run it; up to six integer `args`. `syntax` is `Syntax.INTEL` (0) or
  `Syntax.ATT` (1). Raises `AsmtestError` (carrying the Keystone diagnostic) on a
  bad string, or if the assembler isn't in the build.
* `assemble(src, arch=Arch.X86_64, syntax=Syntax.INTEL, addr=0x00100000)` —
  assemble-only, returning the machine-code `bytes`. `arch` is one of
  `Arch.X86_64 | ARM64 | RISCV64 | ARM32`; `addr` is the base load address (matters
  for PC-relative encodings). Works for guests the x86 emulator can't run.
* `Arch` / `Syntax` are integer-code enums; `AsmtestError` is the raised type.
* Both tiers ship in `libasmtest_emu` (the superset) — a single load gives you the
  assembler *and* the disassembler (below).

### Disassembler (carried by `libasmtest_emu`)

Turn an emulator fault/trace/coverage **offset** into the instruction text at it
(Capstone). It self-skips to `""` against an older/leaner lib without Capstone, so
the same call is safe either way — branch on `disas_available()`.

```python
asmtest.disas_available()                          # bool: disassembler compiled in
asmtest.disas(b"\x48\x31\xc0", 0)                  # -> "xor rax, rax"
asmtest.disas(code, off, arch=asmtest.Arch.ARM64, base=0x00100000)
```

* `disas(code, off=0, arch=Arch.X86_64, base=0x00100000)` — decode the one
  instruction at byte `off`; `base` is the load address, so PC-relative operands
  resolve. Returns `"mnemonic operands"`, or `""` with no disassembler / when the
  bytes do not decode. Pair it with a fault's `res.reg("rip")` (minus the load
  base) to name the offending instruction.
* Carried by `libasmtest_emu` (the superset includes Capstone), so it is present
  out of the box with `make shared-emu`.

### Tier-2 assertions (`asmtest.assertions`)

Thin wrappers that raise `AssertionError` with a legible message, so a `pytest`
suite reads naturally.

```python
from asmtest.assertions import (
    assert_ret, assert_abi_preserved, assert_abi_clobbered, assert_flag,
    assert_fp, assert_vec_f32, assert_no_fault, assert_fault, assert_reg,
)
assert_ret(r, 42)                       # r.ret == 42
assert_abi_preserved(r)                 # all callee-saved restored
assert_abi_clobbered(r)                 # the negative case (expect a violation)
assert_flag(r, "CF", set=True)          # flag set (set=False to require clear)
assert_fp(r, 3.75)                      # r.fret == 3.75 exactly
assert_vec_f32(r, [1, 2, 3, 4], index=0)# the four float32 lanes of vector `index`
assert_no_fault(res)                    # emulator run completed cleanly
assert_fault(res)                       # emulator run faulted
assert_reg(res, "rax", 42)              # a guest register equals expected
```

## Run the tests

```sh
pytest
# The in-line assembler and disassembler tiers ship in libasmtest_emu, so they
# run by default under `make python-test`.
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
