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
