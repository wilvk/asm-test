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
  data: `faulted`, plus `fault_addr` / `fault_kind` (`EMU_FAULT_READ` …) for
  where and why one hit.

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

## Native-trace wrapper (DynamoRIO)

[`asmtest_drtrace.hpp`](asmtest_drtrace.hpp) is a separate header-only wrapper for
the optional in-process DynamoRIO native-trace tier (see
`include/asmtest_drtrace.h`), mirroring the Python `asmtest.drtrace` surface.
Where `asmtest::Trace` traces isolated *guest* bytes, `asmtest::NativeTrace`
traces *host-native* code as it runs inside this process.

Unlike `asmtest.hpp`, it links nothing at build time: it `dlopen`s
`libasmtest_drapp.so` at runtime (resolved via `$ASMTEST_DRAPP_LIB`, else
`<repo>/build/libasmtest_drapp.so`) and `dlsym`s the C API — so it builds even
when DynamoRIO is absent. `NativeTrace::available()` reports whether the tier can
actually run, so callers self-skip cleanly. Link only `-ldl`.

```cpp
#include "asmtest_drtrace.hpp"
using namespace asmtest;

if (!NativeTrace::available()) return 0;   // self-skip; no DynamoRIO present
NativeTrace::initialize();                 // dr_init + dr_start (throws on error)
NativeCode code = NativeCode::from_bytes(routine_bytes);
NativeTrace tr = NativeTrace::create(64);  // 64 block slots (add 2nd arg for insns)
tr.register_region("add", code);
{
    auto scope = tr.region("add");         // RAII begin/end markers
    long r = code.call(20, 22);            // call the host-native code (SysV ABI)
}
bool hit = tr.covered(0);                  // entry block reached?
NativeTrace::shutdown();
```

[`test_drtrace.cpp`](test_drtrace.cpp) is a standalone smoke test (a plain
`int main()`) mirroring the Python suite; it self-skips with a `SKIP:` line when
the tier is unavailable. Build it directly:

```sh
g++ -std=c++17 -I include bindings/cpp/test_drtrace.cpp -ldl -o test_drtrace
ASMTEST_DRAPP_LIB=$PWD/build/libasmtest_drapp.so ./test_drtrace
```

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
