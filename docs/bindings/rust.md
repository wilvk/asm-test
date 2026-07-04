# Rust binding

The [Rust binding](https://github.com/wilvk/asm-test/tree/main/bindings/rust)
drives asm-test with results as plain typed structs and faults as data.

No crates.io dependencies: the binding is `#[repr(C)]` mirrors of the C structs
plus `extern "C"` declarations of the binding-ABI entry points, linked against the
prebuilt shared libraries (`build.rs` locates them). `#[repr(C)]` makes the struct
layout match the C structs by construction, and the no-GC model means sharing a
buffer by pointer is trivial and lifetime-checked. See
[Language bindings](index.md) for the shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator
```

`build.rs` links against `build/` by default (override with `ASMTEST_LIB_DIR`).

## Usage

```rust
use std::os::raw::c_void;
extern "C" { fn add_signed(a: i64, b: i64) -> i64; }

let r = asmtest::capture(add_signed as usize as *mut c_void, &[40, 2]);
assert_eq!(r.ret, 42);
assert!(asmtest::abi_preserved(&r));   // via the native verdict shim

let emu = asmtest::Emulator::new().unwrap();
let res = emu.call(add_signed as usize as *const c_void, &[40, 2]);
assert!(!res.faulted && res.regs.rax == 42);   // faults are data, not a crash
```

`capture` / `capture_fp` / `capture_vec` return a `Regs` snapshot (`ret`, `flags`,
`fret`, `vec` lanes, `flag_set(mask)`); `Emulator::call` returns an `EmuResult`
whose `faulted` / `fault_addr` / `fault_kind` (cf. `FAULT_READ` …) surface invalid
accesses — where and why, not just that one hit.

## In-line assembler (optional)

Pass a routine as an **assembly string**. The assembler lives in the
Keystone-carrying `libasmtest_emu` (now the full superset); the dependency-free
crate resolves it at run time via the libc loader, so `asm_available()` is true by
default. It stays a defensive probe — false only if `ASMTEST_LIB` points at an
older/leaner lib without the assembler.

```rust
use asmtest::{AsmArch, AsmSyntax};
if asmtest::asm_available() {
    let emu = asmtest::Emulator::new().unwrap();
    // Intel, up to six args; Err(String) carries the Keystone diagnostic.
    let res = emu
        .call_asm("mov rax, rdi; add rax, rsi; ret", &[40, 2], AsmSyntax::Intel, 0)
        .unwrap();
    assert_eq!(res.regs.rax, 42);
    // Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32).
    let a64 = asmtest::assemble("ret", AsmArch::Arm64, AsmSyntax::Intel, 0x0010_0000).unwrap();
    assert_eq!(a64, vec![0xC0, 0x03, 0x5F, 0xD6]);
}
```

## Function reference

The full crate surface, with an example and its options. A routine reference is a
raw pointer (`add_signed as *mut c_void` for capture, `as *const c_void` for the
emulator); the byte-array paths take `&[u8]` machine code. `Regs`, `EmuResult`,
and `EmuX86Regs` are `#[repr(C)]` mirrors, so you read their fields directly;
`Emulator`, `Trace`, `Guest`, and `GuestResult` are RAII handles freed on `Drop`.

### Capture tier — free functions + `Regs`

```rust
let r = asmtest::capture(fn_ptr, &[40, 2]);                 // up to 6 integer args
let r = asmtest::capture_fp(fn_ptr, &[1], &[1.5, 2.25]);    // ints + up to 8 doubles
let v = asmtest::Vec128::from_f32(1.0, 2.0, 3.0, 4.0);      // pack a 128-bit vector
let r = asmtest::capture_vec(fn_ptr, &[], &[v]);            // up to 8 vectors

r.ret;                       // u64 integer return (rax)
r.fret;                      // f64 scalar return (xmm0)
r.flags;                     // raw flags word
r.flag_set(asmtest::CF);     // bool: flag bit set? (masks CF/PF/ZF/SF/OF)
r.vec[0].f32();              // [f32; 4] lanes of vector register 0
r.vec[0].f64();              // [f64; 2] lanes
asmtest::abi_preserved(&r);  // every callee-saved register restored (verdict shim)
```

* `capture(f, args)` / `capture_fp(f, iargs, fargs)` / `capture_vec(f, iargs,
  vargs)` — args past the register count are ignored (6 int / 8 fp / 8 vec).
* `Vec128` has `zero()`, `from_f32(a,b,c,d)`, and the `f32()` / `f64()` lane
  readers, plus the raw union views (`u8_`, `u32_`, `u64_`, `f32_`, `f64_`).
* The flag masks `CF`/`PF`/`ZF`/`SF`/`OF` are `pub const u64` for the host arch.

### Emulator tier — `Emulator` / `EmuResult`

```rust
let emu = asmtest::Emulator::new().unwrap();                // None if emu_open fails
let res = emu.call(fn_ptr, &[40, 2]);                       // routine addr, copies 64 bytes
let res = emu.call_bytes(&code, &[40, 2]);                  // raw bytes, up to 6 int args
let res = emu.call_fp(&code, &[1], &[1.5]);                 // doubles -> xmm0..7
let res = emu.call_vec(&code, &[], &[v]);                   // 128-bit vecs -> xmm0..7
let res = emu.call_win64(&code, &[1, 2, 3, 4]);            // Microsoft x64 (rcx,rdx,r8,r9)

res.faulted;                 // bool: invalid access (not a crash)
res.fault_addr;              // u64: where (valid when faulted)
res.fault_kind;              // FAULT_NONE / FAULT_READ / FAULT_WRITE / FAULT_FETCH
res.reg("rax");              // any GP register by name, plus "rip" / "rflags"
res.regs.rax;                // …or the field directly (EmuX86Regs)
res.regs.xmm[0].f64()[0];    // scalar FP return; .f32()[lane] for a vector return
```

`call` takes a routine **address** (copies 64 bytes); `call_bytes` / `call_fp` /
`call_vec` / `call_win64` / `call_traced` take `&[u8]` and run it whole.

### Execution trace / coverage — `Trace`

```rust
let t = asmtest::Trace::new();                  // 4096 insn / 4096 block buffers
let res = emu.call_traced(&code, &[1, 2], &t);  // record while running
t.covered(0x0);                                 // bool: basic block at byte-offset entered?
```

### Native tracing — `NativeTrace` (optional, DynamoRIO)

The optional in-process [native-trace tier](../guides/tracing/native-tracing.md) traces
**host-native** code as it runs inside this Rust process, rather than guest bytes
in the emulator. Bring DynamoRIO up once, materialize machine code with
`NativeCode`, mark a region, call into it, and read back basic-block coverage and
the ordered instruction stream. The tier links nothing and resolves
`libasmtest_drapp` at run time, so always gate on `NativeTrace::available()` —
without DynamoRIO it returns `false` and you self-skip.

```rust
use asmtest::drtrace::{NativeCode, NativeTrace};

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE: [u8; 18] = [
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
    0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
];

if NativeTrace::available() {
    NativeTrace::initialize_default().unwrap();   // dr_init + dr_start, once per process

    // ---- instruction mode (64 blocks, 64 insns) ----
    let code = NativeCode::from_bytes(&ROUTINE);
    let tr = NativeTrace::new_trace(64, 64);
    tr.register("add2", &code).unwrap();
    {
        let _r = tr.region("add2");               // RAII: trace_begin now, trace_end on drop
        assert_eq!(code.call2(1, 2), 3);
    }
    assert!(tr.covered(0));                        // entry basic block entered
    tr.block_offsets();                            // distinct block starts, first-seen order
    // the jle-taken path yields [0x0, 0x3, 0x6, 0xc, 0x11]
    assert_eq!(tr.insn_offsets(), vec![0x0, 0x3, 0x6, 0xc, 0x11]);
    tr.unregister("add2");

    // ---- symbol mode (trace an exported function by name, no markers) ----
    let sym = NativeTrace::new_trace(64, 0);
    sym.register_symbol("asmtest_symbol_demo", 256).unwrap();
    assert_eq!(NativeTrace::symbol_demo(3, 4), 10);   // always-on recording, no region
    assert!(sym.covered(0));
    sym.unregister("asmtest_symbol_demo");

    NativeTrace::shutdown();
}
```

Linux x86-64 only; self-skips without DynamoRIO. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier (`asmtest::hwtrace`) records the **same** `asmtest_trace_t`
coverage from the real CPU, but needs no separate engine install: it defaults to
the **single-step** backend (the CPU's `EFLAGS.TF` trap flag), so
`HwTrace::available(Backend::Singlestep)` is true and it **traces live on any
x86-64 Linux** — CI and plain containers included — where the DynamoRIO tier needs a
DynamoRIO install (and so runs as `examples/drtrace.rs`, not a `cargo test`). Intel
PT and AMD LBR are picked automatically on the bare-metal hardware that has them, so
this one runs as an ordinary integration test.

```rust
use asmtest::hwtrace::{Backend, HwTrace, NativeCode, Policy};

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE: [u8; 18] = [
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00, 0x00, 0x00,
    0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
];

if HwTrace::available(Backend::Singlestep) {      // self-skip off x86-64 Linux
    HwTrace::init(Backend::Singlestep).unwrap();
    let code = NativeCode::from_bytes(&ROUTINE);
    let tr = HwTrace::new_trace(64, 64);          // blocks=64, instructions=64
    tr.register("add2", &code).unwrap();
    let r = {
        let _r = tr.region("add2");               // EFLAGS.TF armed for begin..call..end
        code.call(20, 22)                         // 42; jle taken, dec skipped
    };
    assert_eq!(r, 42);
    // Byte-for-byte the Unicorn / DynamoRIO / Intel PT result for this fixture.
    assert_eq!(tr.insn_offsets(), vec![0x0, 0x3, 0x6, 0xc, 0x11]);
    assert!(tr.covered(0) && !tr.truncated());
    HwTrace::shutdown();
}
```

`HwTrace::resolve(Policy::Best)` / `HwTrace::auto(Policy::Best)` pick the host's
most-faithful available backend (Intel PT → AMD LBR → single-step), and a cross-tier
resolver (`TracePolicy`) extends the cascade across the DynamoRIO and emulator
tiers. An out-of-process `Ptrace` surface traces a method in a **separate** process
(fork-and-step, foreign-process attach + run-to-method, and `/proc`-map / jitdump
resolution) — the managed-runtime path. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

**Scoped tracing** — the *import + scope* form (`HwTrace::scope`). It auto-names the
region from the call site (`#[track_caller]` + `Location::caller`); `close()` ends the
scope, renders the executed assembly, and returns the listing (`Drop` closes it,
emitting to stdout, otherwise). `was_truncated()` is the thread-scope honesty bit.

```rust
use asmtest::hwtrace::{Backend, HwTrace, NativeCode};

HwTrace::init(Backend::Singlestep).unwrap();
let code = NativeCode::from_bytes(&[0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0xC3]); // add2; ret
let scope = HwTrace::scope(&code, /*emit=*/false); // auto-named "file.rs:<line>"
let r = code.call(20, 22);                          // 42
let listing = scope.close();                        // the disassembly that executed
assert_eq!(r, 42);
HwTrace::shutdown();
```

### Cross-arch guests — `Guest` / `GuestResult`

```rust
use asmtest::GuestArch;
let g = asmtest::Guest::new(GuestArch::Arm64).unwrap();   // Arm64 | Riscv | Arm
let res = g.call(&code, &[40, 2]);                        // raw bytes, guest ABI regs
let res = g.call_traced(&code, &[1], &t);                 // AArch64 only
res.faulted();               // faults are data here too
res.reg("x0");               // by name: x0/sp/pc/nzcv, a0/x10/ra/sp, r0/lr/pc/cpsr
```

### In-line assembler (optional)

```rust
use asmtest::{AsmArch, AsmSyntax};
asmtest::asm_available();                                  // assembler compiled in?
let res = emu.call_asm("mov rax, rdi; ret", &[42], AsmSyntax::Intel, 0)?;  // x86-64 + run
let bytes = asmtest::assemble("ret", AsmArch::Arm64, AsmSyntax::Intel, 0x0010_0000)?;
asmtest::asm_error();                                     // last Keystone diagnostic ("" on success)
```

* `Emulator::call_asm(src, args, syntax, max_insns) -> Result<EmuResult, String>`
  — assemble x86-64 `src` and run it (≤6 int args). `max_insns: 0` runs to `ret`;
  `Err` carries the Keystone diagnostic.
* `assemble(src, arch, syntax, addr) -> Result<Vec<u8>, String>` — assemble-only,
  any `AsmArch` (`X86_64`/`Arm64`/`Riscv64`/`Arm32`); `addr` is the base load
  address. `AsmSyntax` is `Intel`/`Att`/`Nasm`/`Masm`/`Gas`.

### Tier-2 assertions (methods that panic on failure)

```rust
r.assert_ret(42);                  // r.ret == 42
r.assert_abi_preserved();          // callee-saved restored
r.assert_abi_clobbered();          // the negative case
r.assert_flag(asmtest::CF, true);  // flag set/clear (by mask)
r.assert_fp(3.75);                 // r.fret == 3.75
res.assert_no_fault();             // emulator run clean
res.assert_fault();                // emulator run faulted
res.assert_reg("rax", 42);         // guest register equals expected
```

## Crash safety

The native capture path runs the routine in-process; a buggy routine can abort the
test binary. The `Emulator` path turns invalid accesses into `EmuResult` data
instead — prefer it for untrusted or under-development routines.

## Run the tests

```sh
make rust-test      # from the repo root: builds the shared libs + a fixture lib, runs cargo test
```

[`tests/conformance.rs`](https://github.com/wilvk/asm-test/blob/main/bindings/rust/tests/conformance.rs)
replays the conformance corpus and reproduces every case.

## Maturity

The published-crate story (a `bindgen`-generated `-sys` crate, `cibuildwheel`-style
prebuilt libs, crates.io) and a Tier-2 idiomatic assertion layer are future work;
this is the Tier-1 binding that proves the no-GC native path. See
[Packaging the bindings](../reference/packaging.md).
