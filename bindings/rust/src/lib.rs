//! asm-test — Rust binding (Track R).
//!
//! Run, **capture**, and **emulate** assembly routines through the asm-test
//! framework's two engines, with results as plain typed structs and faults as
//! data. No external crates: the binding is `#[repr(C)]` mirrors of the C
//! structs plus `extern "C"` declarations of the binding-ABI entry points,
//! linked against the prebuilt shared libraries (see `build.rs`). `#[repr(C)]`
//! makes the layout match the C structs by construction; the conformance corpus
//! validates it end to end.
//!
//! ```ignore
//! let r = asmtest::capture(add_signed as *mut _, &[40, 2]);
//! assert_eq!(r.ret, 42);
//! assert!(asmtest::abi_preserved(&r));
//!
//! let emu = asmtest::Emulator::new().unwrap();
//! let res = emu.call(add_signed as *const _, &[40, 2]);
//! assert!(!res.faulted && res.regs.rax == 42);
//! ```
#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_long, c_void};

/// One 128-bit vector register, several lane views (mirrors `vec128_t`).
#[repr(C)]
#[derive(Clone, Copy)]
pub union Vec128 {
    pub u8_: [u8; 16],
    pub u32_: [u32; 4],
    pub u64_: [u64; 2],
    pub f32_: [f32; 4],
    pub f64_: [f64; 2],
}

impl Vec128 {
    pub fn zero() -> Self {
        Vec128 { u8_: [0; 16] }
    }
    pub fn from_f32(a: f32, b: f32, c: f32, d: f32) -> Self {
        Vec128 { f32_: [a, b, c, d] }
    }
    /// Read the four float32 lanes.
    pub fn f32(&self) -> [f32; 4] {
        unsafe { self.f32_ }
    }
}

// --- regs_t (capture snapshot), per architecture --------------------------- //
#[cfg(target_arch = "x86_64")]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Regs {
    pub ret: u64,
    pub rdx: u64,
    pub rbx: u64,
    pub rbp: u64,
    pub r12: u64,
    pub r13: u64,
    pub r14: u64,
    pub r15: u64,
    pub flags: u64,
    pub fret: f64,
    pub vec: [Vec128; 16],
}

#[cfg(target_arch = "aarch64")]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Regs {
    pub ret: u64,
    pub x19: u64,
    pub x20: u64,
    pub x21: u64,
    pub x22: u64,
    pub x23: u64,
    pub x24: u64,
    pub x25: u64,
    pub x26: u64,
    pub x27: u64,
    pub x28: u64,
    pub x29: u64,
    pub flags: u64,
    pub fret: f64,
    pub vec: [Vec128; 32],
}

impl Default for Regs {
    fn default() -> Self {
        // All fields are plain integers/floats/POD unions; zero is valid.
        unsafe { std::mem::zeroed() }
    }
}

impl Regs {
    /// True if condition flag bit(s) `mask` (e.g. [`CF`]) are set.
    pub fn flag_set(&self, mask: u64) -> bool {
        self.flags & mask != 0
    }

    // --- Tier-2 idiomatic assertions (panic with a clear message) --- //

    /// Assert the integer return value equals `expected`.
    pub fn assert_ret(&self, expected: u64) {
        assert!(self.ret == expected,
            "return value: got {}, want {}", self.ret, expected);
    }

    /// Assert every callee-saved register was restored.
    pub fn assert_abi_preserved(&self) {
        assert!(abi_preserved(self),
            "ABI not preserved: a callee-saved register was not restored");
    }

    /// Assert a callee-saved register was *not* restored (the negative case).
    pub fn assert_abi_clobbered(&self) {
        assert!(!abi_preserved(self),
            "expected an ABI violation, but all callee-saved registers were restored");
    }

    /// Assert condition flag bit(s) `mask` are set (or clear when `set` is false).
    pub fn assert_flag(&self, mask: u64, set: bool) {
        let got = self.flag_set(mask);
        assert!(got == set, "flag {:#x}: got {}, want {}", mask, got, set);
    }

    /// Assert the scalar double return equals `expected` exactly.
    pub fn assert_fp(&self, expected: f64) {
        assert!(self.fret == expected,
            "FP return: got {}, want {}", self.fret, expected);
    }
}

// Condition-flag bit masks (host arch), matching asmtest.h.
#[cfg(target_arch = "x86_64")]
pub const CF: u64 = 1 << 0;
#[cfg(target_arch = "x86_64")]
pub const PF: u64 = 1 << 2;
#[cfg(target_arch = "x86_64")]
pub const ZF: u64 = 1 << 6;
#[cfg(target_arch = "x86_64")]
pub const SF: u64 = 1 << 7;
#[cfg(target_arch = "x86_64")]
pub const OF: u64 = 1 << 11;

#[cfg(target_arch = "aarch64")]
pub const VF: u64 = 1 << 28;
#[cfg(target_arch = "aarch64")]
pub const CF: u64 = 1 << 29;
#[cfg(target_arch = "aarch64")]
pub const ZF: u64 = 1 << 30;
#[cfg(target_arch = "aarch64")]
pub const NF: u64 = 1 << 31;

// --- Emulator structs (x86-64 guest) --------------------------------------- //
#[repr(C)]
#[derive(Clone, Copy)]
pub struct EmuX86Regs {
    pub rax: u64,
    pub rbx: u64,
    pub rcx: u64,
    pub rdx: u64,
    pub rsi: u64,
    pub rdi: u64,
    pub rbp: u64,
    pub rsp: u64,
    pub r8: u64,
    pub r9: u64,
    pub r10: u64,
    pub r11: u64,
    pub r12: u64,
    pub r13: u64,
    pub r14: u64,
    pub r15: u64,
    pub rip: u64,
    pub rflags: u64,
    pub xmm: [Vec128; 16],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EmuResult {
    pub ok: bool,
    pub uc_err: c_int,
    pub faulted: bool,
    pub fault_addr: u64,
    pub fault_kind: c_int,
    pub regs: EmuX86Regs,
}

impl Default for EmuResult {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

impl EmuResult {
    /// Read an x86-64 guest register by name (rax, rbx, …, r15, rip, rflags).
    pub fn reg(&self, name: &str) -> u64 {
        match name {
            "rax" => self.regs.rax, "rbx" => self.regs.rbx, "rcx" => self.regs.rcx,
            "rdx" => self.regs.rdx, "rsi" => self.regs.rsi, "rdi" => self.regs.rdi,
            "rbp" => self.regs.rbp, "rsp" => self.regs.rsp, "r8" => self.regs.r8,
            "r9" => self.regs.r9, "r10" => self.regs.r10, "r11" => self.regs.r11,
            "r12" => self.regs.r12, "r13" => self.regs.r13, "r14" => self.regs.r14,
            "r15" => self.regs.r15, "rip" => self.regs.rip, "rflags" => self.regs.rflags,
            _ => 0,
        }
    }

    // --- Tier-2 idiomatic assertions (panic with a clear message) --- //

    /// Assert the run completed without an invalid memory access.
    pub fn assert_no_fault(&self) {
        assert!(!self.faulted,
            "unexpected fault at {:#x} (kind {})", self.fault_addr, self.fault_kind);
    }

    /// Assert the run hit an invalid memory access.
    pub fn assert_fault(&self) {
        assert!(self.faulted, "expected a fault, but the run completed cleanly");
    }

    /// Assert an x86-64 guest register equals `expected`.
    pub fn assert_reg(&self, name: &str, expected: u64) {
        let got = self.reg(name);
        assert!(got == expected, "register {}: got {}, want {}", name, got, expected);
    }
}

type emu_t = c_void;

extern "C" {
    fn asm_call_capture(out: *mut Regs, f: *mut c_void, args: *const c_long);
    fn asm_call_capture_fp(
        out: *mut Regs,
        f: *mut c_void,
        iargs: *const c_long,
        fargs: *const f64,
    );
    fn asm_call_capture_vec(
        out: *mut Regs,
        f: *mut c_void,
        iargs: *const c_long,
        vargs: *const Vec128,
    );
    fn asmtest_check_abi(r: *const Regs, msg: *mut c_char, n: usize) -> c_int;

    fn emu_open() -> *mut emu_t;
    fn emu_close(e: *mut emu_t);
    fn emu_call(
        e: *mut emu_t,
        f: *const c_void,
        code_len: usize,
        args: *const c_long,
        nargs: c_int,
        max_insns: u64,
        out: *mut EmuResult,
    ) -> bool;
}

fn args6(args: &[i64]) -> [c_long; 6] {
    let mut a = [0 as c_long; 6];
    for (i, v) in args.iter().take(6).enumerate() {
        a[i] = *v as c_long;
    }
    a
}

/// Call `f` through the integer ABI (up to 6 args) and capture register state.
pub fn capture(f: *mut c_void, args: &[i64]) -> Regs {
    let a = args6(args);
    let mut r = Regs::default();
    unsafe { asm_call_capture(&mut r, f, a.as_ptr()) };
    r
}

/// Call `f` marshalling up to 8 doubles into the FP argument registers.
pub fn capture_fp(f: *mut c_void, iargs: &[i64], fargs: &[f64]) -> Regs {
    let ia = args6(iargs);
    let mut fa = [0.0f64; 8];
    for (i, v) in fargs.iter().take(8).enumerate() {
        fa[i] = *v;
    }
    let mut r = Regs::default();
    unsafe { asm_call_capture_fp(&mut r, f, ia.as_ptr(), fa.as_ptr()) };
    r
}

/// Call `f` marshalling up to 8 128-bit vectors into the vector arg registers.
pub fn capture_vec(f: *mut c_void, iargs: &[i64], vargs: &[Vec128]) -> Regs {
    let ia = args6(iargs);
    let mut va = [Vec128::zero(); 8];
    for (i, v) in vargs.iter().take(8).enumerate() {
        va[i] = *v;
    }
    let mut r = Regs::default();
    unsafe { asm_call_capture_vec(&mut r, f, ia.as_ptr(), va.as_ptr()) };
    r
}

/// All callee-saved registers restored — via the native non-jumping verdict shim.
pub fn abi_preserved(r: &Regs) -> bool {
    unsafe { asmtest_check_abi(r, std::ptr::null_mut(), 0) == 0 }
}

/// A Unicorn x86-64 guest. Dropping it closes the handle.
pub struct Emulator {
    h: *mut emu_t,
}

impl Emulator {
    pub fn new() -> Option<Self> {
        let h = unsafe { emu_open() };
        if h.is_null() {
            None
        } else {
            Some(Emulator { h })
        }
    }

    /// Copy 64 bytes of `f` and run with integer args; faults are data.
    pub fn call(&self, f: *const c_void, args: &[i64]) -> EmuResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call(self.h, f, 64, ptr, args.len() as c_int, 0, &mut out);
        }
        out
    }
}

impl Drop for Emulator {
    fn drop(&mut self) {
        unsafe { emu_close(self.h) };
    }
}
