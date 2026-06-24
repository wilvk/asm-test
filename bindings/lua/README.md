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

`make lua-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.lua`](conformance.lua) (the corpus replayed in Lua) with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set.

## Deferred

A LuaRocks rock and a `busted` Tier-2 assertion layer are future work; this is
the Tier-1 binding that proves the LuaJIT `ffi` path.
