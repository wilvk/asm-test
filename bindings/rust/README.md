# asm-test — Rust binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Rust, with results
as plain typed structs and faults as data.

No crates.io dependencies: the binding is `#[repr(C)]` mirrors of the C structs
plus `extern "C"` declarations of the binding-ABI entry points, linked against
the prebuilt shared libraries (`build.rs` locates them). `#[repr(C)]` makes the
struct layout match the C structs by construction, and the no-GC model means
sharing a buffer by pointer is trivial and lifetime-checked.

## Build the shared libraries

```sh
make shared-emu     # from the repo root: libasmtest_emu.{so,dylib}
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

`capture` / `capture_fp` / `capture_vec` return a `Regs` snapshot (`ret`,
`flags`, `fret`, `vec` lanes, `flag_set(mask)`); `Emulator::call` returns an
`EmuResult` whose `faulted` / `fault_addr` / `fault_kind` (cf. `FAULT_READ` …)
surface invalid accesses — where and why, not just that one hit.

## In-line assembler (optional)

Pass a routine as an **assembly string**. The assembler lives only in the
Keystone-carrying `libasmtest_emu_asm`; the dependency-free crate links the plain
lib and resolves it at run time via the libc loader, so `asm_available()` is true
only when `ASMTEST_LIB` points at the assembler lib (`make rust-asm-test`).

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

## Crash safety

The native capture path runs the routine in-process; a buggy routine can abort
the test binary. The `Emulator` path turns invalid accesses into `EmuResult`
data instead — prefer it for untrusted or under-development routines.

## Tests

`make rust-test` (from the repo root) builds the shared libs + a routine fixture
lib, then runs `cargo test`. [`tests/conformance.rs`](tests/conformance.rs)
replays the conformance corpus and reproduces every case.

## Deferred

The published-crate story (a `bindgen`-generated `-sys` crate, `cibuildwheel`-style
prebuilt libs, crates.io) and a Tier-2 idiomatic assertion layer are future work;
this is the Tier-1 binding that proves the no-GC native path.
