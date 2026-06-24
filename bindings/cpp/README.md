# asm-test — C++ binding

The asm-test C headers are **C++-consumable as-is** (they carry `extern "C"`
guards), so a C++ project can `#include "asmtest.h"` / `"asmtest_emu.h"` and use
the framework — `TEST`/`ASSERT_*`, the capture trampoline, the emulator —
directly. This directory adds an *optional* header-only convenience layer; there
is no separate binding to build.

## `asmtest.hpp`

Thin RAII + typed ergonomics over the C API
([asmtest.hpp](asmtest.hpp)):

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
  data.

## Consuming it

Link the framework like any C consumer — via pkg-config:

```sh
c++ -std=c++17 $(pkg-config --cflags asmtest) my_suite.cpp \
    $(pkg-config --libs asmtest) -o my_suite
```

For the emulator conveniences, add `-DASMTEST_ENABLE_EMU` and link the emulator
shared lib (`pkg-config --libs asmtest-emu`). Point your include path at this
directory to pick up `asmtest.hpp`. The same works from CMake via
`find_package(PkgConfig)` + `pkg_check_modules(ASMTEST REQUIRED asmtest)`.

## Tests

`make cpp-test` (from the repo root) builds and runs
[test_cpp.cpp](test_cpp.cpp), an example suite that exercises capture (int / FP /
SIMD / flags / ABI), the verdict predicates, and the RAII emulator from C++.
