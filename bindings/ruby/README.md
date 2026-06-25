# asm-test — Ruby binding

Run, **capture**, and **emulate** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Ruby, using the
**stdlib `Fiddle`** FFI — no gem to install.

It uses the opaque-handle FFI layer (`src/ffi.c`), so no C struct layout is
mirrored in Ruby: `asmtest_corpus_routine` returns a routine address,
`asmtest_capture6` / `_fp2` + `asmtest_regs_*` accessors cover the capture tier,
and `asmtest_emu_call2` + accessors cover the emulator (faults as data).

## Run

```sh
make ruby-test        # from the repo root (needs the shared libs + Ruby)
make docker-ruby      # or in an isolated container
```

The reusable module is [`asmtest.rb`](asmtest.rb) — it keeps all Fiddle FFI
inside and exposes `Asmtest::Regs` / `Emu` / `EmuResult` plus the `Asmtest.assert_*`
helpers. [`conformance.rb`](conformance.rb) is a thin consumer that
`require_relative`s it and replays the corpus. `make ruby-test` builds
`libasmtest_emu` + the routine fixture lib, then runs `conformance.rb`, pointing
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` at them.

## Deferred

A published gem (and RSpec/minitest integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
