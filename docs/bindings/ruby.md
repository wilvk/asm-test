# Ruby binding

The [Ruby binding](https://github.com/wilvk/asm-test/tree/main/bindings/ruby)
drives asm-test using the **stdlib `Fiddle`** FFI — no gem to install. The
reusable module is
[`asmtest.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/asmtest.rb)
— it keeps all Fiddle FFI inside and exposes `Asmtest::Regs` / `Emu` / `EmuResult`
plus the `Asmtest.assert_*` helpers.

It uses the opaque-handle FFI layer, so no C struct layout is mirrored in Ruby:
`asmtest_corpus_routine` returns a routine address, `asmtest_capture6` / `_fp2` +
`asmtest_regs_*` accessors cover the capture tier, and `asmtest_emu_call2` +
accessors cover the emulator (faults as data: `faulted?`, plus `fault_addr` /
`fault_kind` — cf. `Asmtest::FaultKind` — for where and why one hit). See
[Language bindings](../bindings.md) for the shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

Point the module at the built libs via `ASMTEST_LIB` / `ASMTEST_CORPUS_LIB`.

## Usage

```ruby
require_relative "asmtest"

# Native capture through the real ABI.
r = Asmtest::Regs.new
r.capture6(fn, 40, 2)
Asmtest.assert_ret(r, 42)
Asmtest.assert_abi_preserved(r)

# Emulator: faults are data, never a crash.
e = Asmtest::Emu.new
res = e.call2(fn, 40, 2)
Asmtest.assert_no_fault(res)
Asmtest.assert_emu_reg(res, "rax", 42)
```

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

## Run the tests

```sh
make ruby-test        # from the repo root (needs the shared libs + Ruby)
make docker-ruby      # or in an isolated container
make ruby-asm-test    # points ASMTEST_LIB at libasmtest_emu_asm
```

`make ruby-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/conformance.rb)
— a thin consumer that `require_relative`s `asmtest.rb` and replays the corpus —
pointing `ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` at the built libs.

## Maturity

A published gem (and RSpec/minitest integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../packaging.md).
