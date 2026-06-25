# asm-test — Lua binding

Run, **capture**, and **emulate** assembly routines through the
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

## Deferred

A published LuaRocks rock (and `busted` integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
