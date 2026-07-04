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
[Language bindings](index.md) for the shared architecture.

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

## Function reference

Every public name in the `Asmtest` module, with an example and its options.
Handles (`Regs`, `EmuResult`, `Trace`, `Guest`, `GuestResult`) are released with
`#free`; an `Emu`/`Guest` with `#close`. A routine reference is the pointer from
`Asmtest.corpus_routine(name)` (or your own `Fiddle` address); the
`call_bytes`-family takes a binary `String` of machine code.

### Resolving routines

```ruby
fn = Asmtest.corpus_routine("add_signed")   # built-in fixture (needs ASMTEST_CORPUS_LIB)
Asmtest::Emu.new.asm_available?             # is the in-line assembler in this build?
```

### Capture tier — `Regs`

```ruby
r = Asmtest::Regs.new
r.capture6(fn, 40, 2)                        # up to 6 integer args (default 0)
r.capture_fp2(fn, 1.5, 2.25)                 # two doubles; FP return in r.fret
r.capture_vec_f32(fn, [[1, 2, 3, 4]])        # up to 8 vectors (four float32 lanes each)
r.ret                     # integer return (rax)
r.fret                    # scalar double return (xmm0)
r.vec_f32(0)              # [l0,l1,l2,l3] lanes of vector register 0
r.flag_set?("CF")         # condition flag by name (CF/PF/ZF/SF/OF)
r.abi_preserved?          # every callee-saved register restored
r.free
```

### Emulator tier — `Emu` / `EmuResult`

```ruby
e = Asmtest::Emu.new                                       # x86-64 Unicorn guest
res = e.call2(fn, 40, 2)                                   # routine address + two int args
res = e.call_bytes(code, [40, 2])                          # raw bytes (String), up to 6 int args
res = e.call_fp(code, iargs: [1], fargs: [1.5])            # doubles -> xmm0..7
res = e.call_vec(code, iargs: [], vargs: [[1, 2, 3, 4]])   # 128-bit vecs -> xmm0..7
res = e.call_win64(code, [1, 2, 3, 4])                     # Microsoft x64 (rcx,rdx,r8,r9)

res.faulted?              # invalid access? (data, not a crash)
res.fault_addr            # where (valid when faulted)
res.fault_kind            # an Asmtest::FaultKind value (NONE/READ/WRITE/FETCH)
res.reg("rax")            # any GP register, plus "rip" / "rflags"
res.xmm_f64(0, 0)         # xmm lane as double (scalar FP return)
res.xmm_f32(0, 0)         # xmm lane as float32 (vector return)
e.close
```

`call2` is the only path taking a routine **address** (two int args); the
`call_bytes`/`call_fp`/`call_vec`/`call_win64`/`call_traced` family takes a binary
`String`. `call_fp`/`call_vec` take keyword args (`iargs:`/`fargs:`/`vargs:`).

### Execution trace / coverage — `Trace`

```ruby
t = Asmtest::Trace.new(4096, 4096)           # insns / blocks buffer caps
res = e.call_traced(code, [1, 2], t)         # record while running
t.covered?(0x0)           # was the basic block at this byte-offset entered?
t.free
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate, opt-in tier in
[`drtrace.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/drtrace.rb)
(`Asmtest::DrTrace`) traces **host-native** code as it runs *inside this Ruby
process*, backed by DynamoRIO rather than the Unicorn emulator. Bring DynamoRIO
up once with `NativeTrace.start`, materialize machine code with
`NativeCode.from_bytes`, mark a region, call into it, then read back basic-block
coverage and the ordered instruction stream. `NativeTrace.available?` reports
whether the tier can run so callers self-skip cleanly.

```ruby
require_relative "drtrace"

NativeTrace = Asmtest::DrTrace::NativeTrace
NativeCode  = Asmtest::DrTrace::NativeCode

return unless NativeTrace.available?         # self-skip without DynamoRIO

NativeTrace.start(client: "./build/libasmtest_drclient.so",
                  dynamorio_home: "/opt/DynamoRIO")

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
code = NativeCode.from_bytes(
  [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
   0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3].pack("C*"))

tr = NativeTrace.create(blocks: 64, instructions: 64)
tr.register("add", code)
tr.region("add") { code.call(20, 22) }       # => 42, runs the jle-taken path
tr.covered?(0)            # entry block entered?
tr.block_offsets         # distinct block starts, first-seen order
tr.insn_offsets          # ordered instruction stream => [0x0, 0x3, 0x6, 0xc, 0x11]

# Symbol mode: trace a named exported function with NO begin/end markers —
# recording is always-on over [entry, entry + max_len).
tr2 = NativeTrace.create(blocks: 64)
tr2.register_symbol("asmtest_symbol_demo", 256)
NativeTrace.symbol_demo(3, 4)                 # => 10, called with no region/markers
tr2.covered?(0)           # symbol-mode block entered?

NativeTrace.shutdown
```

Linux x86-64 only; self-skips without DynamoRIO; full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier in
[`hwtrace.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/hwtrace.rb)
(`Asmtest::HwTrace`) records the **same** `asmtest_trace_t` coverage from the real
CPU, but needs no separate engine install: it defaults to the **single-step**
backend (the CPU's `EFLAGS.TF` trap flag), so `HwTrace.available?(SINGLESTEP)` is
true and it **traces live on any x86-64 Linux** — CI and plain containers included —
where the DynamoRIO tier needs a DynamoRIO install. Intel PT and AMD LBR are picked
automatically on the bare-metal hardware that has them.

```ruby
require_relative "hwtrace"

HwTrace    = Asmtest::HwTrace
NativeCode = Asmtest::HwTrace::NativeCode
SINGLESTEP = Asmtest::HwTrace::SINGLESTEP

return unless HwTrace.available?(SINGLESTEP)   # self-skip off x86-64 Linux
HwTrace.init(SINGLESTEP)

# mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
code = NativeCode.from_bytes(
  [0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
   0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3].pack("C*"))

trace = HwTrace.create(blocks: 64, instructions: 64)
trace.register("add2", code)
trace.region("add2") { code.call(20, 22) }     # => 42; jle taken, dec skipped
trace.insn_offsets        # => [0x0, 0x3, 0x6, 0xc, 0x11] — == Unicorn/DynamoRIO/PT
trace.covered?(0)         # entry block entered?

HwTrace.shutdown
```

`HwTrace.resolve(BEST)` / `HwTrace.auto(BEST)` pick the host's most-faithful
available backend (Intel PT → AMD LBR → single-step), and `HwTrace.resolve_tiers` /
`auto_tier` extend the cascade across the DynamoRIO and emulator tiers. An
out-of-process `Ptrace` surface traces a method in a **separate** process
(fork-and-step, foreign-process attach + run-to-method, and `/proc`-map / jitdump
resolution) — the managed-runtime path. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

**Scoped tracing** — the block *import + scope* form (`#scope`). It auto-names the
region from the call site (`caller_locations`), renders the executed assembly on
close, and returns a `ScopeResult`; the `ensure` closes the region even if the block
raises. `.truncated` flags a close on a different OS thread (Ractors / the 3.3+ M:N
scheduler — fibers never migrate).

```ruby
# (HwTrace / NativeCode / SINGLESTEP aliased as above)
HwTrace.init(SINGLESTEP)
code = NativeCode.from_bytes([0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3])  # add2; ret
tr = HwTrace.create(blocks: 64, instructions: 256)
res = tr.scope(code, emit: false) { code.call(20, 22) }   # auto-named "file.rb:<line>"
puts res.path        # the disassembly that executed; res.truncated is the thread-scope bit
```

### Cross-arch guests — `Guest` / `GuestResult`

```ruby
g = Asmtest::Guest.new("arm64")              # "arm64" | "riscv" | "arm"
gr = g.call(code, [40, 2])                   # raw bytes, ints in the guest ABI regs
gr.reg("x0")              # by name: x0/sp, a0/x10, r0…
gr.faulted?               # faults are data here too
g.call_traced(code, [1], t)                  # arm64 only (raises otherwise)
gr.free; g.close
```

### In-line assembler (optional)

```ruby
e.asm_available?                             # assembler compiled in?
res = e.call_asm("mov rax, rdi; ret", [42])                    # assemble x86-64 + run
e.call_asm(src, [10, 20], syntax: Asmtest::SYNTAX[:att], max_insns: 8)
bytes = Asmtest.assemble("ret", arch: Asmtest::ARCH[:arm64])   # text -> bytes, any arch
Asmtest.asm_error         # last Keystone diagnostic ("" on success)
```

* `Emu#call_asm(src, args = [], syntax: 0, max_insns: 0)` — assemble x86-64 `src`
  and run it (≤6 int args). `syntax` is an `Asmtest::SYNTAX[...]` code; raises
  `Asmtest::Error` (Keystone diagnostic) on a bad string.
* `Asmtest.assemble(src, arch: 0, syntax: 0, addr: 0x00100000)` — assemble-only,
  returning a binary `String`. `arch` / `syntax` are `Asmtest::ARCH[...]` /
  `SYNTAX[...]` codes; `addr` is the base load address.

### Tier-2 assertions (raise `Asmtest::Error`)

```ruby
Asmtest.assert_ret(r, 42)                  # r.ret == 42
Asmtest.assert_abi_preserved(r)            # callee-saved restored
Asmtest.assert_flag(r, "CF", true)         # flag set/clear
Asmtest.assert_fp(r, 3.75)                 # r.fret == 3.75
Asmtest.assert_vec_f32(r, 0, [1, 2, 3, 4]) # vector lanes of register 0
Asmtest.assert_no_fault(res)               # emulator run clean
Asmtest.assert_fault(res)                  # emulator run faulted
Asmtest.assert_emu_reg(res, "rax", 42)     # x86 guest register
```

## Run the tests

```sh
make ruby-test        # from the repo root (needs the shared libs + Ruby)
make docker-ruby      # or in an isolated container
```

`make ruby-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.rb`](https://github.com/wilvk/asm-test/blob/main/bindings/ruby/conformance.rb)
— a thin consumer that `require_relative`s `asmtest.rb` and replays the corpus —
pointing `ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` at the built libs.

## Maturity

A published gem (and RSpec/minitest integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../reference/packaging.md).
