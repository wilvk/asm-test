//! Replay the conformance corpus (Track 0.4) from Rust.
//!
//! The Rust analog of the C reference runner: drive each canonical routine
//! through the binding-ABI entry points and reproduce the expected results. The
//! cases mirror `bindings/conformance/corpus.json` (the C reference emits that
//! file; these literals must match it). Passing proves the Rust binding wired
//! the ABI up correctly. Run via `make rust-test`.
use std::os::raw::c_void;

use asmtest::{self, Vec128, CF};

// Canonical routines under test (examples/{add,flags,fp,simd}.s), linked from
// the fixture lib via build.rs. Signatures are nominal — we only take addresses.
extern "C" {
    fn add_signed(a: i64, b: i64) -> i64;
    fn sum_via_rbx(a: i64, b: i64) -> i64;
    fn clobbers_rbx(a: i64, b: i64) -> i64;
    fn set_carry() -> i64;
    fn clear_carry() -> i64;
    fn fp_add(a: f64, b: f64) -> f64;
    fn vec_add4f();
    fn read_fault(p: *const i64) -> i64;
    fn int_to_double(n: i64) -> f64;
}

fn pm(addr: usize) -> *mut c_void {
    addr as *mut c_void
}

#[test]
fn add_signed_basic() {
    let r = asmtest::capture(pm(add_signed as usize), &[40, 2]);
    assert_eq!(r.ret, 42);
    assert!(asmtest::abi_preserved(&r));
}

#[test]
fn sum_via_rbx_abi_preserved() {
    let r = asmtest::capture(pm(sum_via_rbx as usize), &[20, 22]);
    assert_eq!(r.ret, 42);
    assert!(asmtest::abi_preserved(&r));
}

#[test]
fn clobbers_rbx_abi_violation_detected() {
    let r = asmtest::capture(pm(clobbers_rbx as usize), &[1, 2]);
    assert!(!asmtest::abi_preserved(&r), "violation should be reported");
}

#[test]
fn set_carry_cf_set() {
    let r = asmtest::capture(pm(set_carry as usize), &[]);
    assert!(r.flag_set(CF));
}

#[test]
fn clear_carry_cf_clear() {
    let r = asmtest::capture(pm(clear_carry as usize), &[]);
    assert!(!r.flag_set(CF));
}

#[test]
fn fp_add_basic() {
    let r = asmtest::capture_fp(pm(fp_add as usize), &[], &[1.5, 2.25]);
    assert_eq!(r.fret, 3.75);
}

#[test]
fn vec_add4f_basic() {
    let vargs = [
        Vec128::from_f32(1.0, 2.0, 3.0, 4.0),
        Vec128::from_f32(10.0, 20.0, 30.0, 40.0),
    ];
    let r = asmtest::capture_vec(pm(vec_add4f as usize), &[], &vargs);
    assert_eq!(r.vec[0].f32(), [11.0, 22.0, 33.0, 44.0]);
}

// Emulator x86-64 guest runs host-compiled bytes — valid only on an x86-64 host.
#[cfg(target_arch = "x86_64")]
#[test]
fn emu_add_signed() {
    let emu = asmtest::Emulator::new().expect("emu_open failed");
    let res = emu.call(add_signed as usize as *const c_void, &[40, 2]);
    assert!(!res.faulted);
    assert_eq!(res.regs.rax, 42);
}

// read_fault dereferences an unmapped address: the fault is data — where
// (fault_addr) and why (fault_kind) — not a crash.
#[cfg(target_arch = "x86_64")]
#[test]
fn emu_read_fault() {
    const FAULT_ADDR: u64 = 0x00DEAD00;
    let emu = asmtest::Emulator::new().expect("emu_open failed");
    let res = emu.call(read_fault as usize as *const c_void, &[FAULT_ADDR as i64]);
    res.assert_fault();
    assert_eq!(res.fault_addr, FAULT_ADDR);
    assert_eq!(res.fault_kind, asmtest::FAULT_READ);
}

// int_to_double lands (double)42 in xmm0, so the XMM file is readable beyond the
// GP registers; a clean run also keeps rflags live (x86 holds bit 1 set).
#[cfg(target_arch = "x86_64")]
#[test]
fn emu_xmm_and_rflags() {
    let emu = asmtest::Emulator::new().expect("emu_open failed");
    let res = emu.call(int_to_double as usize as *const c_void, &[42]);
    assert!(!res.faulted);
    assert_eq!(res.regs.xmm[0].f64()[0], 42.0);
    assert_ne!(res.regs.rflags & 0x2, 0);
}

// In-line assembler (Keystone): only when the loaded lib carries it (run via
// `make rust-asm-test`, which points ASMTEST_LIB at libasmtest_emu_asm).
#[cfg(target_arch = "x86_64")]
#[test]
fn inline_assembler() {
    use asmtest::{AsmArch, AsmSyntax};
    if !asmtest::asm_available() {
        eprintln!("skip: in-line assembler not in this build");
        return;
    }
    let emu = asmtest::Emulator::new().expect("emu_open failed");

    let res = emu
        .call_asm("mov rax, rdi; add rax, rsi; ret", &[40, 2], AsmSyntax::Intel, 0)
        .expect("assemble+run");
    assert!(!res.faulted);
    assert_eq!(res.regs.rax, 42);

    // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
    let att = emu
        .call_asm(
            "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
            &[10, 20, 12],
            AsmSyntax::Att,
            0,
        )
        .expect("assemble+run (AT&T)");
    assert_eq!(att.regs.rax, 42);

    // Failure path: a bad string is an Err carrying the diagnostic.
    assert!(emu
        .call_asm("mov rax, nonsense_token", &[], AsmSyntax::Intel, 0)
        .is_err());

    // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
    let a64 = asmtest::assemble("ret", AsmArch::Arm64, AsmSyntax::Intel, 0x0010_0000)
        .expect("assemble arm64");
    assert_eq!(a64, vec![0xC0, 0x03, 0x5F, 0xD6]);
}
