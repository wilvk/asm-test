# C++ binding

The asm-test C headers are **C++-consumable as-is** (they carry `extern "C"`
guards), so a C++ project can `#include "asmtest.h"` / `"asmtest_emu.h"` and use
the framework — `TEST`/`ASSERT_*`, the capture trampoline, the emulator —
directly. The [C++ binding](https://github.com/wilvk/asm-test/tree/main/bindings/cpp)
adds an *optional* header-only convenience layer
([`asmtest.hpp`](https://github.com/wilvk/asm-test/blob/main/bindings/cpp/asmtest.hpp));
there is no separate binding to build. See [Language bindings](../bindings.md) for
the shared architecture.

Unlike the dynamic-FFI bindings, C++ gates the optional emulator and assembler at
**build time** with `-DASMTEST_ENABLE_EMU` / `-DASMTEST_ENABLE_ASM`, so a
Keystone-free build compiles the assembler out entirely.

## `asmtest.hpp`

Thin RAII + typed ergonomics over the C API:

```cpp
#include "asmtest.hpp"
extern "C" long add_signed(long, long);
using namespace asmtest;

TEST(mymath, add) {
    regs_t r = capture((void*)add_signed, {40, 2});   // initializer-list args
    ASSERT_EQ(r.ret, 42);
    ASSERT_TRUE(abi_preserved(r));                     // verdict predicate
}
```

- `capture` / `capture_fp` / `capture_vec` — pass args as `{...}`, get a `regs_t`.
- `vec_f32(...)` — build a 128-bit vector argument lane-wise.
- `abi_preserved(r)` / `flag_set(r, ASMTEST_CF)` — predicates over the snapshot
  (`abi_preserved` uses the native non-jumping verdict shim).
- `asmtest::Emu` — an RAII guard over `emu_t` (define `ASMTEST_ENABLE_EMU` and
  link the emulator). `Emu::call(...)` returns an `emu_result_t` whose faults are
  data: `faulted`, plus `fault_addr` / `fault_kind` (`EMU_FAULT_READ` …) for where
  and why one hit.

## In-line assembler (optional)

Define `ASMTEST_ENABLE_ASM` (alongside `ASMTEST_ENABLE_EMU`) and link
`assemble.o` + `keystone` to pass a routine as an **assembly string**. Unlike the
dlopen bindings, the C++ header links the assembler directly, so it is always
available once those objects are linked (the `make cpp-test` suite does so).

```cpp
#define ASMTEST_ENABLE_EMU
#define ASMTEST_ENABLE_ASM
#include "asmtest.hpp"
using namespace asmtest;

Emu e;
// Intel, up to six args; throws asmtest::asm_error (with the Keystone diagnostic).
emu_result_t r = e.call_asm("mov rax, rdi; add rax, rsi; ret", {40, 2});
// AT&T syntax + an instruction cap:
e.call_asm(src, {10, 20, 12}, ASM_SYNTAX_ATT, /*max_insns=*/0);
// Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
std::vector<std::uint8_t> a64 = assemble("ret", ASM_ARM64);
```

## Function reference

Every member of `namespace asmtest`, with an example and its options. The capture
helpers are always available (`#include "asmtest.hpp"`); the `Emu` family needs
`-DASMTEST_ENABLE_EMU`, and `call_asm` / `assemble` need `-DASMTEST_ENABLE_ASM`.
Results are the plain C value-structs, so you read their fields directly
(`r.ret`, `res.regs.rax`, `res.regs.xmm[0].f64[0]`). A routine reference is a
`void *`; the emulator's `call_bytes`-family takes a `(code, len)` pair.

### Capture tier — free functions

```cpp
regs_t r = capture((void*)fn, {40, 2});               // up to 6 integer args
regs_t r = capture_fp((void*)fn, {1}, {1.5, 2.25});   // ints + up to 8 doubles
vec128_t v = vec_f32(1, 2, 3, 4);                     // pack a 128-bit vector arg
regs_t r = capture_vec((void*)fn, {}, {v});           // up to 8 vectors

r.ret;                       // integer return (rax)
r.fret;                      // scalar double return (xmm0)
r.vec[0].f32[0];             // a lane of vector register 0 (.f32[]/.f64[]/.u8[]…)
flag_set(r, ASMTEST_CF);     // condition flag bit set? (ASMTEST_CF/PF/ZF/SF/OF)
abi_preserved(r);            // every callee-saved register restored (verdict shim)
```

* `capture` / `capture_fp` / `capture_vec` take `std::initializer_list` args;
  extras past the register count are dropped, missing ones default to 0.
* `flag_set(r, mask)` / `abi_preserved(r)` are predicates over a `regs_t`.

### Emulator tier — `Emu` (define `ASMTEST_ENABLE_EMU`)

```cpp
Emu e;                                          // RAII x86-64 guest (move-only)
emu_result_t r = e.call((void*)fn, {40, 2});    // routine addr; opts: max_insns=0, code_len=64
emu_result_t r = e.call_bytes(code, len, {40, 2});          // raw bytes, up to 6 int args
emu_result_t r = e.call_fp(code, len, {1}, {1.5});          // doubles -> xmm0..7
emu_result_t r = e.call_vec(code, len, {}, {v});            // 128-bit vecs -> xmm0..7
emu_result_t r = e.call_win64(code, len, {1, 2, 3, 4});     // Microsoft x64 (rcx,rdx,r8,r9)

r.faulted;                   // invalid access? (data, not a crash)
r.fault_addr;                // where (valid when faulted)
r.fault_kind;                // EMU_FAULT_NONE / READ / WRITE / FETCH
r.regs.rax;                  // any GP register, plus r.regs.rip / r.regs.rflags
r.regs.xmm[0].f64[0];        // scalar FP return; .f32[lane] for a vector return
```

* `call(fn, args, max_insns=0, code_len=64)` is the only address path (copies
  `code_len` bytes). The `call_bytes`-family takes raw `(code, len)`.
* Every method takes a trailing `max_insns` budget (`0` = run to `ret`).

### Execution trace / coverage — `Trace`

```cpp
Trace t(/*insns_cap=*/4096, /*blocks_cap=*/4096);
emu_result_t r = e.call_traced(code, len, t.get(), {1, 2});  // record while running
t.covered(0x0);              // basic block at byte-offset entered?
t.insns_total();             // instructions executed (counts past the cap)
t.blocks_len();              // distinct basic blocks recorded
```

`t.get()` hands the underlying `emu_trace_t*` to `Emu::call_traced` (or a guest's).

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate, opt-in tier in `asmtest_drtrace.hpp` traces **host-native code as it
runs inside this process** under DynamoRIO, rather than inside the emulator's
virtual CPU. It is header-only, links nothing at build time (it `dlopen`s
`libasmtest_drapp` at run time and needs only `-ldl`), and self-skips cleanly when
DynamoRIO is absent. Bring DynamoRIO up once, materialize host-native bytes with
`NativeCode`, mark a region, call into it, and read back coverage.

```cpp
#include "asmtest_drtrace.hpp"
using namespace asmtest;

if (!NativeTrace::available()) return;     // self-skip when DynamoRIO is absent
NativeTrace::initialize();                 // dr_init + dr_start (once, per process)

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
std::vector<std::uint8_t> bytes = {
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3};
NativeCode code = NativeCode::from_bytes(bytes);

NativeTrace tr = NativeTrace::create(/*blocks=*/64, /*instructions=*/64);
tr.register_region("add", code);
{
    auto s = tr.region("add");             // RAII begin/end marker pair
    long r = code.call(1, 2);              // r == 3
}
tr.covered(0);                             // entry basic block entered?

// Instruction mode (allocated above with instructions > 0): the jle-taken path
// (1 + 2 <= 100, dec skipped) yields the exact ordered stream
//   tr.insn_offsets() == {0x0, 0x3, 0x6, 0xc, 0x11}
tr.block_offsets();                        // distinct block starts, first-seen order
tr.insn_offsets();                         // ordered instruction offsets (std::vector<uint64_t>)
```

Symbol mode traces a **named exported function** with no begin/end markers —
recording is always-on over the resolved range, so the fixture is called without a
region scope and coverage still lands:

```cpp
NativeTrace tr2 = NativeTrace::create(/*blocks=*/64);
tr2.register_symbol("asmtest_symbol_demo", 256);
long sr = NativeTrace::symbol_demo(3, 4);  // == 10, no region scope
tr2.covered(0);                            // recorded with no manual marker

NativeTrace::shutdown();                   // dr_app_stop_and_cleanup (back to native)
```

Linux x86-64 only, self-skips when DynamoRIO is absent; full reference in
[Native runtime tracing](../native-tracing.md).

### Cross-arch guests (raw bytes, any host)

```cpp
Arm64Emu g;                                            // also RiscvEmu, ArmEmu
emu_arm64_result_t r = g.call(code, len, {40, 2});     // ints in x0..x5
emu_arm64_result_t r = g.call_traced(code, len, t.get(), {1});  // arm64 only
r.regs.x[0];                 // arm64 x0..x30; r.regs.sp / pc / nzcv; r.regs.v[0].f64[0]
// RiscvEmu: r.regs.x[10] (a0) / r.regs.pc / r.regs.f[0].f64[0]
// ArmEmu:   r.regs.r[0] / r.regs.r[13] (sp) / r.regs.q[0].f64[0]
```

Each guest is move-disabled RAII over its `emu_<arch>_t`; `call` takes the same
`(code, len, args, max_insns=0)` shape.

### In-line assembler (define `ASMTEST_ENABLE_ASM`)

```cpp
emu_result_t r = e.call_asm("mov rax, rdi; ret", {42});        // x86-64 + run
e.call_asm(src, {10, 20}, ASM_SYNTAX_ATT, /*max_insns=*/8);    // syntax + insn cap
std::vector<std::uint8_t> bytes = assemble("ret", ASM_ARM64);  // text -> bytes, any arch
```

* `Emu::call_asm(src, args={}, syntax=ASM_SYNTAX_INTEL, max_insns=0)` — assemble
  x86-64 `src` and run (≤6 int args). Throws `asmtest::asm_error` (Keystone
  diagnostic) on a bad string.
* `assemble(src, arch=ASM_X86_64, syntax=ASM_SYNTAX_INTEL, addr=EMU_CODE_BASE)` —
  assemble-only; `arch` is `ASM_X86_64`/`ASM_ARM64`/`ASM_RISCV64`/`ASM_ARM32`,
  `syntax` one of `ASM_SYNTAX_INTEL`/`ATT`/`NASM`/`MASM`/`GAS`.

### Tier-2 assertions (throw `asmtest::assertion_error`)

```cpp
assert_ret(r, 42);                 // r.ret == 42
assert_abi_preserved(r);           // callee-saved restored
assert_flag(r, ASMTEST_CF, true);  // flag set/clear (by mask)
assert_fp(r, 3.75);                // r.fret == 3.75
```

## Consuming it

Link the framework like any C consumer — via pkg-config:

```sh
c++ -std=c++17 $(pkg-config --cflags asmtest) my_suite.cpp \
    $(pkg-config --libs asmtest) -o my_suite
```

For the emulator conveniences, add `-DASMTEST_ENABLE_EMU` and link the emulator
shared lib (`pkg-config --libs asmtest-emu`). Point your include path at the
binding directory to pick up `asmtest.hpp`. The same works from CMake via
`find_package(PkgConfig)` + `pkg_check_modules(ASMTEST REQUIRED asmtest)`.

## Run the tests

```sh
make cpp-test       # from the repo root
```

It builds and runs
[`test_cpp.cpp`](https://github.com/wilvk/asm-test/blob/main/bindings/cpp/test_cpp.cpp),
an example suite that exercises capture (int / FP / SIMD / flags / ABI), the
verdict predicates, and the RAII emulator from C++. The asm path builds as a
separate Keystone-carrying target so the base `cpp-test` stays Keystone-free.
