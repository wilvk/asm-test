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

Pass a routine as an **assembly string** instead of a pre-built address. The
Keystone assembler is built into `libasmtest_emu`, so this runs by default under
`make ruby-test`; `Emu#asm_available?` remains a defensive probe — it returns
false only against an older/leaner lib, so guard with it.

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

## Native-trace tier (DynamoRIO, optional)

[`drtrace.rb`](drtrace.rb) mirrors the Python `asmtest.drtrace`: in-process tracing
of **host-native** code via DynamoRIO. It loads a *separate* shared object,
`libasmtest_drapp` (env `ASMTEST_DRAPP_LIB`, else `<repo>/build/libasmtest_drapp.so`),
which dlopen()s `libdynamorio` lazily — so nothing here links DynamoRIO. The tier is
Linux-x86-64-only and opt-in; `NativeTrace.available?` returns false (no raise) when
the lib can't load or DynamoRIO is absent.

```ruby
require_relative "drtrace"
NT = Asmtest::DrTrace::NativeTrace
next unless NT.available?
NT.start(client: "./build/libasmtest_drclient.so", dynamorio_home: "/opt/DynamoRIO")
code = Asmtest::DrTrace::NativeCode.from_bytes([0x48,0x89,0xF8,0x48,0x01,0xF0,0xC3].pack("C*"))
tr = NT.create(blocks: 64, instructions: 0)   # NOTE: create, not new (handle is from C)
tr.register("add", code)
tr.region("add") { @r = code.call(20, 22) }    # begin/yield/ensure-end; markers stay balanced
raise unless @r == 42 && tr.covered?(0)
tr.unregister("add"); code.free; tr.free; NT.shutdown
```

The lifecycle entry point is `NativeTrace.start` (not `initialize`, which Ruby
reserves) and a trace is allocated with `NativeTrace.create` (not `new`, since the
handle comes from C). [`test_drtrace.rb`](test_drtrace.rb) is a standalone smoke test
(`ruby test_drtrace.rb`) that self-skips (`SKIP: ...`, exit 0) when the tier is
unavailable.

## Deferred

A published gem (and RSpec/minitest integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today.
