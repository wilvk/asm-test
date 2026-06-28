# asm-test — Lua binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from **LuaJIT**, using
its `ffi`.

`ffi.cdef` declares the opaque-handle FFI helpers (`src/ffi.c`) almost verbatim
and `ffi.load` opens the shared libraries, so no C struct layout is mirrored:
`asmtest_corpus_routine` for addresses, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` for capture, and `asmtest_emu_call2` + accessors for the
emulator (faults as data: `:faulted()`, plus `:fault_addr()` / `:fault_kind()` —
against the `FaultKind` table — for where and why one hit).

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

Pass a routine as an **assembly string**. The Keystone assembler is built into
`libasmtest_emu`, so this runs by default under `make lua-test`;
`Emu:asm_available()` remains a defensive probe, false only against an
older/leaner lib.

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

## Native tracing (DynamoRIO, optional)

[`drtrace.lua`](drtrace.lua) is the Lua mirror of Python's `asmtest.drtrace`: it
traces **host-native** code as it runs inside this LuaJIT process, backed by the
optional DynamoRIO tier. It loads `libasmtest_drapp` (from `ASMTEST_DRAPP_LIB`,
else `<repo>/build/`) and returns `NativeTrace` / `NativeCode`. The tier is
advanced, Linux-x86-64-only, and opt-in; `NativeTrace.available()` self-skips
cleanly when it can't run (no DynamoRIO / lib absent).

```lua
local drtrace = require("drtrace")
if drtrace.NativeTrace.available() then
  drtrace.NativeTrace.initialize{ client = "./build/libasmtest_drclient.so" }
  local code = drtrace.NativeCode.from_bytes("\x48\x89\xf8\x48\x01\xf0\xc3")  -- mov rax,rdi; add rax,rsi; ret
  local tr = drtrace.NativeTrace.new(64)
  tr:register("add", code)
  local r; tr:region("add", function() r = code:call(20, 22) end)  -- r == 42, tr:covered(0)
  drtrace.NativeTrace.shutdown()
end
```

[`test_drtrace.lua`](test_drtrace.lua) is the standalone test (mirrors
`tests/test_drtrace.py`); it self-skips (`SKIP: ...`, exit 0) when the tier is
unavailable:

```sh
ASMTEST_DRAPP_LIB=../../build/libasmtest_drapp.so luajit test_drtrace.lua
```

## Deferred

A published LuaRocks rock (and `busted` integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
