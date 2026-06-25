# asm-test — Ruby binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Ruby, using the
**stdlib `Fiddle`** FFI — no gem to install.

It uses the opaque-handle FFI layer (`src/ffi.c`), so no C struct layout is
mirrored in Ruby: `asmtest_corpus_routine` returns a routine address,
`asmtest_capture6` / `_fp2` + `asmtest_regs_*` accessors cover the capture tier,
and `asmtest_emu_call2` + accessors cover the emulator (faults as data:
`faulted?`, plus `fault_addr` / `fault_kind` — cf. `Asmtest::FaultKind` — for
where and why one hit).

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

## In-line assembler (optional)

Pass a routine as an **assembly string** instead of a pre-built address. Present
only in the Keystone-carrying `libasmtest_emu_asm` (`make ruby-asm-test` points
`ASMTEST_LIB` at it); `Emu#asm_available?` is false against the plain lib, so
guard with it.

```ruby
e = Asmtest::Emu.new
if e.asm_available?
  res = e.call_asm("mov rax, rdi; add rax, rsi; ret", [40, 2])   # Intel, up to 6 args
  res.reg("rax")                                                 # => 42
  # AT&T syntax + an instruction cap; raises Asmtest::Error (with the Keystone
  # diagnostic) if the string fails to assemble.
  e.call_asm(src, [10, 20, 12], syntax: Asmtest::SYNTAX[:att], max_insns: 0)
end
# Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32), even guests the x86
# emulator can't run; raises on a Keystone error.
bytes = Asmtest.assemble("ret", arch: Asmtest::ARCH[:arm64])
```

## Deferred

A published gem (and RSpec/minitest integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
