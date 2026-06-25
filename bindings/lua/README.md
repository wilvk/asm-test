# asm-test — Lua binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from **LuaJIT**, using
its `ffi`.

`ffi.cdef` declares the opaque-handle FFI helpers (`src/ffi.c`) almost verbatim
and `ffi.load` opens the shared libraries, so no C struct layout is mirrored:
`asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the
emulator (faults as data).

## Run

```sh
make lua-test         # from the repo root (needs the shared libs + LuaJIT)
make docker-lua       # or in an isolated container
```

The reusable module is [`asmtest.lua`](asmtest.lua) — it keeps all `ffi` inside
and returns a table with the `Regs` / `Emu` / `EmuResult` objects plus the
`assert_*` helpers. [`conformance.lua`](conformance.lua) is a thin consumer that
`require`s it and replays the corpus. `make lua-test` builds `libasmtest_emu` +
the routine fixture lib, then runs `conformance.lua` with `ASMTEST_LIB` /
`ASMTEST_CORPUS_LIB` set.

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

## Deferred

A published LuaRocks rock (and `busted` integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
