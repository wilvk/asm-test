//! Tier-2 assertion methods: the pass paths succeed and the failure paths panic.
use std::os::raw::c_void;

// No CF on rv64: RISC-V has no condition-flags register (asmtest.h's
// ASMTEST_NO_FLAGS), so the constant is not defined for that arch.
#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
use asmtest::CF;
use asmtest::{self};

extern "C" {
    fn add_signed(a: i64, b: i64) -> i64;
    fn clobbers_rbx(a: i64, b: i64) -> i64;
    fn fp_add(a: f64, b: f64) -> f64;
}
// The carry fixtures have no rv64 analog and are omitted from the corpus there
// (examples/flags.s) — same gate test_capture.c uses via ASMTEST_NO_FLAGS.
#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
extern "C" {
    fn set_carry() -> i64;
    fn clear_carry() -> i64;
}

fn pm(addr: usize) -> *mut c_void {
    addr as *mut c_void
}

#[test]
fn pass_paths() {
    let r = asmtest::capture(pm(add_signed as usize), &[40, 2]);
    r.assert_ret(42);
    r.assert_abi_preserved();

    #[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
    {
        asmtest::capture(pm(set_carry as usize), &[]).assert_flag(CF, true);
        asmtest::capture(pm(clear_carry as usize), &[]).assert_flag(CF, false);
    }

    asmtest::capture_fp(pm(fp_add as usize), &[], &[1.5, 2.25]).assert_fp(3.75);

    asmtest::capture(pm(clobbers_rbx as usize), &[1, 2]).assert_abi_clobbered();

    let emu = asmtest::Emulator::new().expect("emu_open");
    let res = emu.call(add_signed as usize as *const c_void, &[40, 2]);
    res.assert_no_fault();
    res.assert_reg("rax", 42);
}

#[test]
#[should_panic(expected = "return value")]
fn ret_mismatch_panics() {
    asmtest::capture(pm(add_signed as usize), &[40, 2]).assert_ret(99);
}

#[test]
#[should_panic(expected = "ABI not preserved")]
fn abi_violation_panics() {
    asmtest::capture(pm(clobbers_rbx as usize), &[1, 2]).assert_abi_preserved();
}
