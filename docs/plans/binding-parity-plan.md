# asm-test — Binding Coverage Parity Plan

Closes the gaps where the C core exposes an emulator/assembler capability that **no
language binding can reach**, because [`src/ffi.c`](../../src/ffi.c) never grew an
opaque-handle wrapper for it — and anchors every tier in the shared conformance
corpus so the bindings stay honest.

> Status legend: **done** / **planned**. Update as tracks land.

---

## Context: the gaps (as of the vector-capture commit)

A review found the ten bindings (Python, Go, Rust, C++, .NET, Java, Lua, Node, Ruby,
Zig) at full parity on three tiers — integer/FP/vector **capture**, the **x86-64
emulator**, and the **inline assembler** — but several core capabilities were
unreachable from any binding, and the corpus was behind the bindings it governs:

1. **Multi-arch emulator guests** (arm64 / riscv / arm). The `emu_arm64_*` /
   `emu_riscv_*` / `emu_arm_*` engines and their register structs existed in
   [`asmtest_emu.h`](../../include/asmtest_emu.h) / [`asmtest_abi.json`](../../asmtest_abi.json),
   but the FFI exposed only **x86** accessors, so every binding's emulator tier was
   x86-only.
2. **Emulator FP/vector args + >2 int args** — `emu_call_fp` / `emu_call_vec` / 6-arg
   `emu_call` existed; bindings had only the 2-int `asmtest_emu_call2`.
3. **Execution trace / coverage** — `emu_call_traced` + `emu_trace_t` — no FFI, no binding.
4. **Win64 emulation** — `emu_call_win64` — unreachable from bindings.
5. **Assembler tier not anchored in the corpus** — every binding tested it, but
   `conformance.c` / `corpus.json` had no asm cases.
6. **C++ case parity** — `test_cpp.cpp` lacked the standalone `sum_via_rbx` /
   `clear_carry` cases every other binding had.

**Design decisions (validated against the code):**
- The cross-arch guests run **raw machine-code bytes**, not a host function pointer,
  so they emulate on any host. The new corpus cases use **pre-assembled byte
  literals** (generated once, checked in) — the base emu tier needs no Keystone, and
  RISC-V (absent from many Keystone builds) still runs under Unicorn.
- All result structs share the same leading layout (`ok`/`uc_err`/`faulted`/
  `fault_addr`/`fault_kind`), so the existing `asmtest_emu_result_*` accessors are
  reused for every guest; only per-arch **register** reads are new.
- FFI split preserved: struct-field reads in `ffi.c` (no Unicorn dep); anything that
  drives the emulator in `emu.c`, next to `asmtest_emu_call2`.

---

## Track A — C core FFI surface — **done**

- `src/ffi.c`: opaque per-arch result handles + register accessors
  (`asmtest_emu_{arm64,riscv,arm}_result_new/_free/_reg`, the vector-lane readers),
  and the opaque execution-trace handle (`asmtest_emu_trace_new/_free/_covered/…`).
- `src/emu.c`: scalar-arg wrappers for the dynamic bindings — `asmtest_emu_call6`
  (wide int), `_call_fp2`, `_call_vec_f32`, `_call_win64_6`, `_call6_traced`.
- `include/asmtest_emu.h`: prototypes for all of the above.
- Validated end-to-end: all eight new guest/byte sequences assembled and run through
  the emulator returning the expected results on the build host.

## Track B — Shared corpus (single source of truth) — **done**

- `bindings/conformance/conformance.c` + regenerated `corpus.json`: new cases driven
  through the opaque-handle FFI, host-portable (raw bytes):
  - `emu_arm64.add`, `emu_riscv.add`, `emu_arm.add` (cross-arch int);
  - `emu.wide_int` (call6), `emu.fp_add` (call_fp2), `emu.vec_add4f` (call_vec_f32),
    `emu.win64_add` (call_win64_6);
  - `emu_arm64.trace_sel` — a two-block select proving block coverage as data.
- Asm anchor (#5): `asm.add_signed` / `asm.att_3arg` / `asm.bad_source` /
  `asm.arm64_bytes` emitted into `corpus.json` unconditionally; executed under a new
  `make conformance-asm` build (`-DASMTEST_ENABLE_ASM`, links Keystone), keeping the
  base `make conformance` Keystone-free.
- `make conformance` → 18 cases pass / 22 emitted; `make conformance-asm` → 22 pass.

## Track C — All ten bindings — **in progress**

Each binding gains, in its reusable module: cross-arch guest runners (`arm64`/`riscv`/
`arm`, with FP/vector where the ISA has them), the extended x86 emu calls (wide int /
FP / vector / Win64), an execution-trace handle, and per-arch result reads by name;
its conformance runner replays the new corpus cases. C++ also gains the two missing
capture cases (#6).

| Binding | Module + runner | Tested here |
|---|---|---|
| Python | done | `make python-test` / `python-asm-test` ✓ |
| Go | done | toolchain absent on build host — mirrors tested surface |
| C++ | planned | `make cpp-test` |
| Java | planned | `make java-test` |
| Ruby | planned | `make ruby-test` |
| Rust | planned | toolchain absent — mirrors tested surface |
| Zig | planned | toolchain absent |
| Node | planned | toolchain absent |
| Lua | planned | LuaJIT absent |
| .NET | planned | toolchain absent |

## Track D — Docs — **planned**

- `docs/bindings.md`: extend the "Capabilities at a glance" table + prose.
- `docs/api-reference.md`: catalog the new `asmtest_emu_*` entry points.
- `docs/emulator.md`: the cross-arch guest + trace/win64 binding paths.
- `CHANGELOG.md`: one entry for the expanded binding ABI.

---

## Verification

1. `make shared-emu shared-emu-asm manifest conformance` — build + base corpus.
2. `make conformance-asm` — execute the asm tier.
3. `make <lang>-test` / `make <lang>-asm-test` for each binding whose toolchain is
   present; the new case names appear with identical expected literals in every
   runner and in `corpus.json`.
