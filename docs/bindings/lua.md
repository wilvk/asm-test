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
[Language bindings](index.md) for the shared architecture.

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

## Function reference

Every name the module returns, with an example and its options. Objects are made
by the module's constructor functions (`asmtest.Regs()`, `asmtest.Emu()`, …) and
called with `:` method syntax; release a handle with `:free()` (or `:close()` for
an `Emu`/`Guest`). A routine reference is the pointer from
`asmtest.corpus_routine(name)` (or your own `ffi` address); the `call_bytes`-family
takes a Lua string of machine code.

### Resolving routines

```lua
local fn = asmtest.corpus_routine("add_signed")   -- built-in (needs ASMTEST_CORPUS_LIB)
asmtest.Emu():asm_available()                      -- is the in-line assembler in this build?
```

### Capture tier — `Regs`

```lua
local r = asmtest.Regs()
r:capture6(fn, 40, 2)                       -- up to 6 integer args (nil -> 0)
r:capture_fp2(fn, 1.5, 2.25)                -- two doubles; FP return in r:fret()
r:capture_vec_f32(fn, {{1, 2, 3, 4}})       -- up to 8 vectors (four float32 lanes each)
r:ret()                   -- integer return (rax)
r:fret()                  -- scalar double return (xmm0)
r:vec_f32(0)              -- {l1,l2,l3,l4} lanes of vector register 0 (1-indexed table)
r:flag_set("CF")          -- condition flag by name (CF/PF/ZF/SF/OF)
r:abi_preserved()         -- every callee-saved register restored
r:free()
```

### Emulator tier — `Emu` / `EmuResult`

```lua
local e = asmtest.Emu()                                  -- x86-64 Unicorn guest
local res = e:call2(fn, 40, 2)                           -- routine address + two int args
res = e:call_bytes(code, {40, 2})                        -- raw bytes (string), up to 6 int args
res = e:call_fp(code, { fargs = {1.5, 2.25} })           -- doubles -> xmm0..7
res = e:call_vec(code, { vargs = {{1, 2, 3, 4}} })       -- 128-bit vecs -> xmm0..7
res = e:call_win64(code, {1, 2, 3, 4})                   -- Microsoft x64 (rcx,rdx,r8,r9)

res:faulted()             -- invalid access? (data, not a crash)
res:fault_addr()          -- where (valid when faulted)
res:fault_kind()          -- an asmtest.FaultKind value (NONE/READ/WRITE/FETCH)
res:reg("rax")            -- any GP register, plus "rip" / "rflags"
res:xmm_f64(0, 0)         -- xmm lane as double (scalar FP return)
res:xmm_f32(0, 0)         -- xmm lane as float32 (vector return)
e:close()
```

`call2` is the only path taking a routine **address** (two int args); the
`call_bytes`/`call_fp`/`call_vec`/`call_win64`/`call_traced` family takes a Lua
string. `call_fp`/`call_vec` take an options table (`{ fargs = … }` /
`{ vargs = … }`).

### Execution trace / coverage — `Trace`

```lua
local t = asmtest.Trace(4096, 4096)         -- insns / blocks buffer caps
local res = e:call_traced(code, {1, 2}, t)  -- record while running
t:covered(0x0)            -- was the basic block at this byte-offset entered?
t:free()
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

Where `Trace` records isolated guest bytes under the emulator, the optional
[native-trace tier](../guides/tracing/native-tracing.md) traces **host-native code as it runs
inside this LuaJIT process**: DynamoRIO is brought up once, machine code is
materialized into executable (W^X) memory, a region is marked, you call into it,
and basic-block coverage / the ordered instruction stream is read back. It lives
in a separate module,
[`drtrace.lua`](https://github.com/wilvk/asm-test/blob/main/bindings/lua/drtrace.lua),
and is Linux-x86-64-only and opt-in: `NativeTrace.available()` reports whether it
can run so callers self-skip cleanly.

```lua
local drtrace = require("drtrace")
local NativeTrace, NativeCode = drtrace.NativeTrace, drtrace.NativeCode

-- Self-skip unless the tier is built and DynamoRIO is resolvable.
if not NativeTrace.available() then return end
NativeTrace.initialize()                  -- dr_init + dr_start (opts table optional)

-- mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
local code = NativeCode.from_bytes(string.char(
  0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
  0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3))

-- Instruction mode: blocks AND the ordered instruction stream (insns 2nd arg).
local tr = NativeTrace.new(64, 64)        -- (blocks, instructions)
tr:register("add", code)
tr:region("add", function() code:call(20, 22) end)   -- balanced begin/end markers
tr:covered(0)                             -- entry block entered?

-- block_offsets()/insn_offsets() are 1-based Lua tables; the jle-taken path here
-- yields {0x0, 0x3, 0x6, 0xc, 0x11}.
local blocks = tr:block_offsets()
local insns  = tr:insn_offsets()

-- Symbol mode: trace a named exported function by NAME, no region/markers.
local tr2 = NativeTrace.new(64)
tr2:register_symbol("asmtest_symbol_demo", 256)
drtrace.symbol_demo(3, 4)                 -- a*2+b == 10; always-on recording
tr2:covered(0)

NativeTrace.shutdown()
```

Linux x86-64 only; self-skips without DynamoRIO. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier
([`hwtrace.lua`](https://github.com/wilvk/asm-test/blob/main/bindings/lua/hwtrace.lua))
records the **same** `asmtest_trace_t` coverage from the real CPU, but needs no
separate engine install: it defaults to the **single-step** backend (the CPU's
`EFLAGS.TF` trap flag), so `HwTrace.available(SINGLESTEP)` is true and it **traces
live on any x86-64 Linux** — CI and plain containers included — where the DynamoRIO
tier needs a DynamoRIO install. Intel PT and AMD LBR are picked automatically on the
bare-metal hardware that has them.

```lua
local hwtrace = require("hwtrace")
local HwTrace, NativeCode = hwtrace.HwTrace, hwtrace.NativeCode
local SINGLESTEP = hwtrace.SINGLESTEP

if not HwTrace.available(SINGLESTEP) then return end  -- self-skip off x86-64 Linux
HwTrace.init(SINGLESTEP)

-- mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
local code = NativeCode.from_bytes(string.char(
  0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
  0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3))

local tr = HwTrace.create(64, 64)           -- (blocks, instructions)
tr:register("add2", code)
tr:region("add2", function() code:call(20, 22) end)  -- 42; jle taken, dec skipped
-- tr:insn_offsets() == {0x0, 0x3, 0x6, 0xc, 0x11} (1-based Lua table)
tr:covered(0)                               -- entry block entered?
tr:free(); code:free()

HwTrace.shutdown()
```

`HwTrace.resolve(BEST)` / `HwTrace.auto(BEST)` pick the host's most-faithful
available backend (Intel PT → AMD LBR → single-step). A cross-tier resolver
(spanning the DynamoRIO and emulator tiers) and an out-of-process `Ptrace` surface
— which traces a method in a **separate** process (fork-and-step, foreign-process
attach + run-to-method, and `/proc`-map / jitdump resolution), the managed-runtime
path — round out the tier. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Cross-arch guests — `Guest` / `GuestResult`

```lua
local g = asmtest.Guest("arm64")            -- "arm64" | "riscv" | "arm"
local gr = g:call(code, {40, 2})            -- raw bytes, ints in the guest ABI regs
gr:reg("x0")              -- by name: x0/sp, a0/x10, r0…
gr:faulted()              -- faults are data here too
g:call_traced(code, {1}, t)                 -- arm64 only (asserts otherwise)
gr:free(); g:close()
```

### In-line assembler (optional)

```lua
e:asm_available()                           -- assembler compiled in?
local res = e:call_asm("mov rax, rdi; ret", {42})                  -- assemble x86-64 + run
e:call_asm(src, {10, 20}, { syntax = asmtest.Syntax.ATT, max_insns = 8 })
local bytes = asmtest.assemble("ret", asmtest.Arch.ARM64)          -- text -> bytes, any arch
asmtest.asm_error()       -- last Keystone diagnostic ("" on success)
```

* `Emu:call_asm(src, args, opts)` — assemble x86-64 `src` and run it; `args` is a
  table of ≤6 ints, `opts` is `{ syntax = asmtest.Syntax.*, max_insns = 0 }`.
  `error()`s with the Keystone diagnostic on a bad string.
* `asmtest.assemble(src, arch, syntax, addr)` — assemble-only, returning a Lua
  string. `arch` is `asmtest.Arch.{X86_64,ARM64,RISCV64,ARM32}`; `syntax` is
  `asmtest.Syntax.{INTEL,ATT,NASM,MASM,GAS}`; `addr` defaults to `0x00100000`.

### Tier-2 assertions (`error()` on failure)

```lua
asmtest.assert_ret(r, 42)                  -- r:ret() == 42
asmtest.assert_abi_preserved(r)            -- callee-saved restored
asmtest.assert_flag(r, "CF", true)         -- flag set/clear
asmtest.assert_fp(r, 3.75)                 -- r:fret() == 3.75
asmtest.assert_vec_f32(r, 0, {1, 2, 3, 4}) -- vector lanes of register 0
asmtest.assert_no_fault(res)               -- emulator run clean
asmtest.assert_fault(res)                  -- emulator run faulted
asmtest.assert_emu_reg(res, "rax", 42)     -- x86 guest register
asmtest.assert_guest_reg(gr, "x0", 42)     -- cross-arch guest register
asmtest.assert_covered(t, 0x0)             -- basic block entered
```

## Run the tests

```sh
make lua-test         # from the repo root (needs the shared libs + LuaJIT)
make docker-lua       # or in an isolated container
```

`make lua-test` builds `libasmtest_emu` + the routine fixture lib, then runs
[`conformance.lua`](https://github.com/wilvk/asm-test/blob/main/bindings/lua/conformance.lua)
— a thin consumer that `require`s `asmtest.lua` and replays the corpus — with
`ASMTEST_LIB` / `ASMTEST_CORPUS_LIB` set.

## Maturity

A published LuaRocks rock (and `busted` integration of the `assert_*` helpers) is
future work; the reusable module with Tier-2 assertions ships today. See
[Packaging the bindings](../reference/packaging.md).
