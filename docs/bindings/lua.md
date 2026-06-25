# Lua binding

The [Lua binding](https://github.com/wilvk/asm-test/tree/main/bindings/lua) drives
asm-test from **LuaJIT**, using its `ffi`. The reusable module is
[`asmtest.lua`](https://github.com/wilvk/asm-test/blob/main/bindings/lua/asmtest.lua)
— it keeps all `ffi` inside and returns a table with the `Regs` / `Emu` /
`EmuResult` objects plus the `assert_*` helpers.

`ffi.cdef` declares the opaque-handle FFI helpers almost verbatim and `ffi.load`
opens the shared libraries, so no C struct layout is mirrored:
`asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the emulator
(faults as data: `:faulted()`, plus `:fault_addr()` / `:fault_kind()` — against
the `FaultKind` table — for where and why one hit). See
[Language bindings](../bindings.md) for the shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

Point the module at the built libs via `ASMTEST_LIB` / `ASMTEST_CORPUS_LIB`.

## Usage

```lua
local asmtest = require("asmtest")

-- Native capture through the real ABI.
local r = asmtest.Regs()
r:capture6(fn, 40, 2)
asmtest.assert_ret(r, 42)
asmtest.assert_abi_preserved(r)

-- Emulator: faults are data, never a crash.
local e = asmtest.Emu()
local res = e:call2(fn, 40, 2)
asmtest.assert_no_fault(res)
asmtest.assert_emu_reg(res, "rax", 42)
```

## In-line assembler (optional)

Pass a routine as an **assembly string**. Present only in the Keystone-carrying
`libasmtest_emu_asm` (`make lua-asm-test` points `ASMTEST_LIB` at it);
`Emu:asm_available()` is false against the plain lib.

```lua
local e = asmtest.Emu()
if e:asm_available() then
  -- Intel, up to six args; error()s (with the Keystone diagnostic) on a bad string.
  local res = e:call_asm("mov rax, rdi; add rax, rsi; ret", {40, 2})  -- res:reg("rax") == 42
  -- AT&T syntax + an instruction cap:
  e:call_asm(src, {10, 20, 12}, { syntax = asmtest.Syntax.ATT, max_insns = 0 })
  -- Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
  local a64 = asmtest.assemble("ret", asmtest.Arch.ARM64)
end
```

## Run the tests

```sh
make lua-test         # from the repo root (needs the shared libs + LuaJIT)
make docker-lua       # or in an isolated container
make lua-asm-test     # points ASMTEST_LIB at libasmtest_emu_asm
```

`make lua-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.lua`](https://github.com/wilvk/asm-test/blob/main/bindings/lua/conformance.lua)
— a thin consumer that `require`s `asmtest.lua` and replays the corpus — with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set.

## Maturity

A published LuaRocks rock (and `busted` integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../packaging.md).
