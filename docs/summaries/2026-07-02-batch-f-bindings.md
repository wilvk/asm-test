# Batch F — language-binding fixes (findings #26–#32)

Date: 2026-07-02. Scope: `bindings/` only (no `src/` or `asmtest_abi.json` edits).
Docker lanes: base image + `make docker-<lang>` per language.

## #26 (High) Rust `Guest::call_traced` overflowed smaller Arm/Riscv result buffers

- Validated: real. `call_traced` unconditionally called `emu_arm64_call_traced`
  against a per-arch result buffer (Arm 360B / Riscv 808B vs arm64 816B) and the
  wrong engine handle — heap corruption from safe Rust.
- Changed: `bindings/rust/src/lib.rs` — added `extern` decls for
  `emu_riscv_call_traced` / `emu_arm_call_traced` (both already exist in `src/emu.c`
  and `include/asmtest_emu.h`) and made `call_traced` `match self.arch` exactly like
  `call`. Added regression test `guest_call_traced_per_arch` (traced RISC-V add reads
  back 42) in `bindings/rust/tests/conformance.rs`.
- Verified: `make docker-rust` PASS (both the initial run with the lib.rs fix and a
  re-run including the new test).

## #27 (High) Python `EmuResult.reg()` raised KeyError for 15/18 x86 registers

- Validated: real. `reg()` resolved via `offset("emu_x86_regs_t", name)` but the
  manifest publishes only rax/rip/rflags/xmm, so rbx/rcx/…/r15 raised KeyError.
- Changed: `bindings/python/asmtest/core.py` — resolve GP names by index off the
  published `rax` offset (the file is a contiguous `uint64[18]`, same trick as
  `src/ffi.c asmtest_emu_x86_reg`). No generator/manifest change needed, so no
  `scripts/`/`asmtest_abi.json` edit. Unknown names now raise a clear KeyError.
- Test: added `test_reg_all_documented_names_resolve` in
  `bindings/python/tests/test_asm.py` (all 18 names resolve; rax==42; rip/rsp != 0).
- Verified: `make docker-python` PASS (the resolve-all test confirmed no KeyError).

## #28 (Med) Node hwtrace numeric addresses truncated to 32 bits by koffi

- Validated: real. koffi coerces a JS Number pointer arg with Int32Value (32-bit);
  real 64-bit JIT addresses were silently truncated.
- Changed: `bindings/node/hwtrace.js` — added `_addr(v)` helper (numeric → BigInt,
  lossless Int64Value path; External/BigInt/null pass through) and applied it to the
  void* address args of `runTo`, `traceAttached`, `traceAttachedVersioned`,
  `procRegionByAddr`, `CodeImage.track`, `CodeImage.bytesAt`. `jitdumpFind` now
  returns `codeAddr`/`codeSize` as BigInt so a 64-bit address survives intact.
  Updated `test_hwtrace.js` codeAddr/codeSize asserts to BigInt.
- Verified: `node --check` PASS; `make docker-node` PASS (core lane). The hwtrace
  tier tests run under the separate hwtrace-bindings lane; logic validated by review.

## #29 (Med) Lua 64-bit read-backs funneled through `tonumber()` (lossy >= 2^53)

- Validated: real. A Lua number is a double (53-bit mantissa); register/address
  read-backs >= 2^53 rounded, and distinct values could compare equal.
- Changed: added a `u64(v)` helper (returns a Lua number when it fits the mantissa,
  else the boxed `uint64_t` cdata which LuaJIT compares/prints exactly) in both
  `bindings/lua/asmtest.lua` and `bindings/lua/hwtrace.lua`. Applied to the register/
  address accessors: `Regs:ret`, `EmuResult:fault_addr`, `EmuResult:reg`,
  `Watch:addr`, `Watch:rip_off`, `RegGuard:got`, `GuestResult:reg`; and in hwtrace
  `NativeCode:call`, the ptrace result read-backs, proc region base/len, jitdump
  fields, and codeimage event addr/len/timestamp. API-compatible for values < 2^53.
- Verified: `make docker-lua` PASS (33/33; assert_ret/assert_emu_reg exercise `u64`).

## #30 (Med) C++ hwtrace/drtrace wrappers called null fn pointers on missing lib

- Validated: real. On dlopen failure the handle table's fn pointers are null, yet
  every non-available() entry point called through them → SIGSEGV, contradicting the
  documented no-crash/self-skip contract.
- Changed: `bindings/cpp/asmtest_hwtrace.hpp` and `bindings/cpp/asmtest_drtrace.hpp`
  — added `detail::require()` (returns the loaded api or throws std::runtime_error
  with the skip_reason message) and routed every action entry point through it
  (init/create/register/NativeCode::from_bytes/RegionScope begin/Ptrace + CodeImage
  ops / NativeTrace initialize/marker_error/symbol_demo/register…). `shutdown()` is a
  safe no-op when the lib never loaded (so an unconditional teardown after a
  self-skip cannot crash). available()/skip_reason()/resolve/auto keep using api().
- Verified: `make docker-cpp` PASS.

## #31 (Med) Java allocated per-call buffers from a never-closed shared Arena

- Validated: real. One static `Arena.ofShared()` backed every transient copy
  (code/args/C-strings/scratch), never closed → native RSS grows with call count.
- Changed: `bindings/java/Asmtest.java`, `HwTrace.java`, `DrTrace.java` — the
  marshalling helpers (`bytesSeg/longsSeg/doublesSeg/vecsSeg/str`) now take a per-call
  `Arena`, and each call site wraps the downcall in
  `try (Arena a = Arena.ofConfined())`. The shared `ARENA` is kept only for the
  process-lifetime library lookup. DrTrace's `NativeCode` retains the exec_code_t
  struct (read by base()/length()/free()), so it gets its own `Arena.ofShared()`
  closed in `free()` rather than leaking; the transient input buffer is confined.
- Verified: `make docker-java` PASS (33/33; all call paths compile + run).

## #32 (Low) Python hwtrace available()/skip probes raised OSError on missing lib

- Validated: real. `available()`/`skip_reason()` went through `_get()->_load()` which
  raises OSError, so the documented self-skip idiom threw instead of skipping.
- Changed: `bindings/python/asmtest/hwtrace.py` — added `_try_get()` (folds a load
  OSError into None) and used it in `HwTrace`/`Ptrace`/`CodeImage.available()` +
  `skip_reason()` and the eBPF `bpf_available()`/`bpf_skip_reason()` probes: return
  False / "libasmtest_hwtrace not found" instead of raising. init/from_bytes still
  raise via `_get()`.
- Verified: `make docker-python` PASS (core lane; probe logic self-skips cleanly).

## Note on finding #9 (Lua vec_f32)

Left as-is: the C side (`asmtest_regs_vec_f32`) already clamps the index, so no
Lua-side clamp was added.
