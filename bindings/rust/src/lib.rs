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

/// Optional in-process DynamoRIO native-trace tier (`libasmtest_drapp`, loaded at
/// run time; self-skips when DynamoRIO is absent). See [`drtrace::NativeTrace`].
pub mod drtrace;

/// Optional hardware-trace tier (`libasmtest_hwtrace`, loaded at run time;
/// self-skips when unavailable). The single-step backend runs on any x86-64 Linux.
/// See [`hwtrace::HwTrace`].
pub mod hwtrace;

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
    /// Read the two float64 lanes (a scalar double return is `f64()[0]`).
    pub fn f64(&self) -> [f64; 2] {
        unsafe { self.f64_ }
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

/// `fault_kind` values (mirror `emu_fault_kind_t`): why an access was invalid.
pub const FAULT_NONE: c_int = 0;
pub const FAULT_READ: c_int = 1;
pub const FAULT_WRITE: c_int = 2;
pub const FAULT_FETCH: c_int = 3;

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

/// A memory-write watchpoint result (Track F). `#[repr(C)]` matches emu_watch_t.
#[repr(C)]
#[derive(Default)]
pub struct EmuWatch {
    pub violated: bool,
    pub addr: u64,
    pub size: u32,
    pub rip_off: u64,
}

/// A register-invariant guard result (Track F); matches emu_reg_guard_t.
#[repr(C)]
#[derive(Default)]
pub struct EmuRegGuard {
    pub violated: bool,
    pub got: u64,
    pub rip_off: u64,
}

/// Coverage-guided search result (Track E); matches emu_fuzz_stat_t.
#[repr(C)]
#[derive(Default)]
pub struct EmuFuzzStat {
    pub blocks_reached: u64,
    pub corpus_len: u64,
    pub iterations: u64,
}

/// Mutation-test result (Track E); matches emu_mutation_stat_t.
#[repr(C)]
#[derive(Default)]
pub struct EmuMutationStat {
    pub mutants: usize,
    pub killed: usize,
    pub survived: usize,
}

/// A 256-bit (AVX2) vector value (Track D), 32 contiguous bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vec256 {
    pub bytes: [u8; 32],
}

impl Vec256 {
    /// Pack four f64 lanes into a 256-bit vector.
    pub fn from_f64(lanes: [f64; 4]) -> Self {
        let mut v = Vec256 { bytes: [0; 32] };
        for (i, x) in lanes.iter().enumerate() {
            v.bytes[i * 8..i * 8 + 8].copy_from_slice(&x.to_le_bytes());
        }
        v
    }
    /// The four f64 lanes of the vector.
    pub fn f64(&self) -> [f64; 4] {
        let mut out = [0.0; 4];
        for (i, o) in out.iter_mut().enumerate() {
            *o = f64::from_le_bytes(self.bytes[i * 8..i * 8 + 8].try_into().unwrap());
        }
        out
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vec512 {
    pub bytes: [u8; 64],
}

impl Vec512 {
    /// Pack eight f64 lanes into a 512-bit vector.
    pub fn from_f64(lanes: [f64; 8]) -> Self {
        let mut v = Vec512 { bytes: [0; 64] };
        for (i, x) in lanes.iter().enumerate() {
            v.bytes[i * 8..i * 8 + 8].copy_from_slice(&x.to_le_bytes());
        }
        v
    }
    /// The eight f64 lanes of the vector.
    pub fn f64(&self) -> [f64; 8] {
        let mut out = [0.0; 8];
        for (i, o) in out.iter_mut().enumerate() {
            *o = f64::from_le_bytes(self.bytes[i * 8..i * 8 + 8].try_into().unwrap());
        }
        out
    }
}

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
    fn asm_call_capture_args(
        out: *mut Regs,
        f: *mut c_void,
        args: *const c_long,
        nargs: c_int,
    );
    fn asm_call_capture_sret(
        out: *mut Regs,
        f: *mut c_void,
        result: *mut c_void,
        args: *const c_long,
        nargs: c_int,
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

    // Mid-execution guards (Track F).
    fn emu_map(e: *mut emu_t, addr: u64, size: usize) -> bool;
    fn emu_watch_writes(e: *mut emu_t, addr: u64, size: usize, mode: c_int, out: *mut EmuWatch);
    fn emu_watch_clear(e: *mut emu_t);
    fn emu_guard_reg(e: *mut emu_t, name: *const c_char, want: u64, out: *mut EmuRegGuard) -> bool;
    fn emu_guard_reg_clear(e: *mut emu_t);
    // Coverage-guided fuzzing + mutation testing (Track E).
    #[allow(clippy::too_many_arguments)]
    fn emu_fuzz_cover1(e: *mut emu_t, code: *const c_void, len: usize, lo: c_long, hi: c_long,
                       iters: u64, seed: u64, uni: *mut c_void, st: *mut EmuFuzzStat) -> bool;
    fn emu_mutation_test1(e: *mut emu_t, code: *const c_void, len: usize, inputs: *const c_long,
                          n: usize, maxm: u64, seed: u64, st: *mut EmuMutationStat) -> usize;
    // AVX2 256-bit capture (Track D).
    fn asm_call_capture_vec256(out: *mut Vec256, f: *mut c_void, iargs: *const c_long, vargs: *const Vec256);
    fn asmtest_cpu_has_avx2() -> c_int;
    // AVX-512 512-bit capture (Track D).
    fn asm_call_capture_vec512(out: *mut Vec512, f: *mut c_void, iargs: *const c_long, vargs: *const Vec512);
    fn asmtest_cpu_has_avx512f() -> c_int;
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

/// Call `f` with an arbitrary number of integer args (wide arity): the first 6
/// go in registers, the rest spill onto the stack per the ABI.
pub fn capture_args(f: *mut c_void, args: &[i64]) -> Regs {
    let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
    let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
    let mut r = Regs::default();
    unsafe { asm_call_capture_args(&mut r, f, ptr, args.len() as c_int) };
    r
}

/// Call `f` returning a large (memory-class) struct via the hidden result
/// pointer (SysV rdi / AAPCS64 x8): the struct bytes are written into `result`
/// and the visible integer args follow the ABI.
pub fn capture_sret(f: *mut c_void, result: &mut [u8], args: &[i64]) -> Regs {
    let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
    let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
    let mut r = Regs::default();
    unsafe {
        asm_call_capture_sret(
            &mut r,
            f,
            result.as_mut_ptr() as *mut c_void,
            ptr,
            args.len() as c_int,
        )
    };
    r
}

/// Call `f` marshalling up to 8 doubles into the FP argument registers.
/// `iargs` fills the integer registers in the same call, so a mixed integer+FP
/// signature (e.g. `double mix_scale(long, double)`) is `capture_fp(f, &[n], &[x])`.
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

    /// Map a guest RW region [addr, addr+size) the routine can use (Track F).
    pub fn map(&self, addr: u64, size: usize) -> bool {
        unsafe { emu_map(self.h, addr, size) }
    }

    /// Arm a memory-write watchpoint over [addr, addr+size) (Track F): `mode` 1 =
    /// only (flag a write that escapes it), 0 = never (one that touches it). The
    /// guard persists across calls pointing at `out`, so `out` must outlive the
    /// run (keep it in a stable location until `watch_clear`).
    pub fn watch_writes(&self, addr: u64, size: usize, mode: i32, out: &mut EmuWatch) {
        unsafe { emu_watch_writes(self.h, addr, size, mode, out) };
    }
    pub fn watch_clear(&self) {
        unsafe { emu_watch_clear(self.h) }
    }

    /// Arm a register invariant (Track F), recorded into `out` (which must outlive
    /// the run). Returns false for an unknown register name.
    pub fn guard_reg(&self, name: &str, want: u64, out: &mut EmuRegGuard) -> bool {
        let cs = CString::new(name).unwrap();
        unsafe { emu_guard_reg(self.h, cs.as_ptr(), want, out) }
    }
    pub fn guard_reg_clear(&self) {
        unsafe { emu_guard_reg_clear(self.h) }
    }

    /// Coverage-guided input search over one-int-arg `code` (Track E).
    pub fn fuzz_cover(&self, code: &[u8], lo: i64, hi: i64, iters: u64) -> EmuFuzzStat {
        let mut st = EmuFuzzStat::default();
        unsafe {
            let uni = asmtest_emu_trace_new(0, 256);
            emu_fuzz_cover1(self.h, code.as_ptr() as *const c_void, code.len(),
                lo as c_long, hi as c_long, iters, 0xC0FFEE, uni, &mut st);
            asmtest_emu_trace_free(uni);
        }
        st
    }

    /// Bit-flip mutation testing of `code` against an input set (Track E).
    pub fn mutation_test(&self, code: &[u8], inputs: &[i64]) -> EmuMutationStat {
        let iv: Vec<c_long> = inputs.iter().map(|x| *x as c_long).collect();
        let ptr = if iv.is_empty() { std::ptr::null() } else { iv.as_ptr() };
        let mut st = EmuMutationStat::default();
        unsafe {
            emu_mutation_test1(self.h, code.as_ptr() as *const c_void, code.len(),
                ptr, inputs.len(), 0, 0xABCD, &mut st);
        }
        st
    }
}

/// True if the host CPU + OS support AVX2 (gate [`capture_vec256`]).
pub fn cpu_has_avx2() -> bool {
    unsafe { asmtest_cpu_has_avx2() != 0 }
}

/// AVX2 256-bit capture (Track D): run `f` with `vargs` (each a [`Vec256`]) and
/// return the ymm file as 16 [`Vec256`] (out[0] = the vector return).
pub fn capture_vec256(f: *mut c_void, vargs: &[Vec256]) -> [Vec256; 16] {
    let mut va = [Vec256 { bytes: [0; 32] }; 8];
    for (i, v) in vargs.iter().take(8).enumerate() {
        va[i] = *v;
    }
    let ia = [0 as c_long; 6];
    let mut out = [Vec256 { bytes: [0; 32] }; 16];
    unsafe { asm_call_capture_vec256(out.as_mut_ptr(), f, ia.as_ptr(), va.as_ptr()) };
    out
}

/// True if the host CPU + OS support AVX-512F (gate [`capture_vec512`]).
pub fn cpu_has_avx512f() -> bool {
    unsafe { asmtest_cpu_has_avx512f() != 0 }
}

/// AVX-512 512-bit capture (Track D): run `f` with `vargs` (each a [`Vec512`]) and
/// return the zmm file as 32 [`Vec512`] (out[0] = the vector return).
pub fn capture_vec512(f: *mut c_void, vargs: &[Vec512]) -> [Vec512; 32] {
    let mut va = [Vec512 { bytes: [0; 64] }; 8];
    for (i, v) in vargs.iter().take(8).enumerate() {
        va[i] = *v;
    }
    let ia = [0 as c_long; 6];
    let mut out = [Vec512 { bytes: [0; 64] }; 32];
    unsafe { asm_call_capture_vec512(out.as_mut_ptr(), f, ia.as_ptr(), va.as_ptr()) };
    out
}

impl Drop for Emulator {
    fn drop(&mut self) {
        unsafe { emu_close(self.h) };
    }
}

// --- In-line assembler (Keystone) — optional ------------------------------- //
//
// The assembler entry points live in libasmtest_emu (now the full superset:
// emulator + Keystone assembler + Capstone disassembler). To keep this crate
// dependency-free *and* link-clean, resolve them at run time with the libc dynamic
// loader: dlopen the lib named by `ASMTEST_LIB`, then dlsym. They normally resolve,
// so the helpers run by default; against an older/leaner Keystone-free lib the
// pointers stay `None` and the helpers report unavailability — matching the
// dlopen-based bindings (Ruby, Node, ...).

use std::ffi::{CStr, CString};
use std::sync::OnceLock;

extern "C" {
    fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

const RTLD_NOW: c_int = 2; // same value on Linux and macOS

type CallAsm6Fn = unsafe extern "C" fn(
    *mut emu_t, *const c_char, c_int, c_long, c_long, c_long, c_long, c_long,
    c_long, c_int, u64, *mut EmuResult,
) -> c_int;
type AsmBytesFn =
    unsafe extern "C" fn(c_int, c_int, *const c_char, u64, *mut u8, c_int) -> c_int;
type AsmErrFn = unsafe extern "C" fn() -> *const c_char;
type DisasFn =
    unsafe extern "C" fn(c_int, *const u8, usize, u64, u64, *mut c_char, usize) -> usize;
type DisasAvailFn = unsafe extern "C" fn() -> bool;

struct AsmFns {
    call_asm6: Option<CallAsm6Fn>,
    asm_bytes: Option<AsmBytesFn>,
    asm_err: Option<AsmErrFn>,
    disas: Option<DisasFn>,
    disas_avail: Option<DisasAvailFn>,
}
// The function pointers come from the process's own libraries and outlive any
// call; sharing them across threads is sound.
unsafe impl Sync for AsmFns {}
unsafe impl Send for AsmFns {}

fn asm_fns() -> &'static AsmFns {
    static FNS: OnceLock<AsmFns> = OnceLock::new();
    // dlopen/dlsym/transmute each carry their own `unsafe` block — a closure does
    // not inherit an enclosing one, so the unsafety is marked at each call.
    FNS.get_or_init(|| {
        let handle = match std::env::var("ASMTEST_LIB") {
            Ok(p) => match CString::new(p) {
                Ok(c) => unsafe { dlopen(c.as_ptr(), RTLD_NOW) },
                Err(_) => std::ptr::null_mut(),
            },
            Err(_) => std::ptr::null_mut(),
        };
        if handle.is_null() {
            return AsmFns {
                call_asm6: None, asm_bytes: None, asm_err: None,
                disas: None, disas_avail: None,
            };
        }
        let sym = |name: &str| -> *mut c_void {
            match CString::new(name) {
                Ok(c) => unsafe { dlsym(handle, c.as_ptr()) },
                Err(_) => std::ptr::null_mut(),
            }
        };
        let s1 = sym("asmtest_emu_call_asm6");
        let s2 = sym("asmtest_asm_bytes");
        let s3 = sym("asmtest_asm_last_error");
        let s4 = sym("emu_disas");
        let s5 = sym("emu_disas_available");
        unsafe {
            AsmFns {
                call_asm6: if s1.is_null() { None } else { Some(std::mem::transmute::<_, CallAsm6Fn>(s1)) },
                asm_bytes: if s2.is_null() { None } else { Some(std::mem::transmute::<_, AsmBytesFn>(s2)) },
                asm_err: if s3.is_null() { None } else { Some(std::mem::transmute::<_, AsmErrFn>(s3)) },
                disas: if s4.is_null() { None } else { Some(std::mem::transmute::<_, DisasFn>(s4)) },
                disas_avail: if s5.is_null() { None } else { Some(std::mem::transmute::<_, DisasAvailFn>(s5)) },
            }
        }
    })
}

/// Target architecture for [`assemble`] (mirrors `asm_arch_t`).
#[repr(i32)]
#[derive(Clone, Copy)]
pub enum AsmArch {
    X86_64 = 0,
    Arm64 = 1,
    Riscv64 = 2,
    Arm32 = 3,
}

/// Input assembly syntax (x86 only); mirrors `asm_syntax_t`.
#[repr(i32)]
#[derive(Clone, Copy)]
pub enum AsmSyntax {
    Intel = 0,
    Att = 1,
    Nasm = 2,
    Masm = 3,
    Gas = 4,
}

/// Whether the loaded native lib carries the in-line assembler (Keystone).
pub fn asm_available() -> bool {
    asm_fns().call_asm6.is_some()
}

/// The Keystone diagnostic from the most recent assemble on this thread
/// (`""` on success, or when the assembler is absent).
pub fn asm_error() -> String {
    match asm_fns().asm_err {
        Some(f) => unsafe { CStr::from_ptr(f()).to_string_lossy().into_owned() },
        None => String::new(),
    }
}

/// Whether the loaded native lib carries the disassembler (Capstone) — true by
/// default, since `libasmtest_emu` (the superset) carries it; false only if
/// `ASMTEST_LIB` points at an older/leaner lib.
pub fn disas_available() -> bool {
    match asm_fns().disas_avail {
        Some(f) => unsafe { f() },
        None => false,
    }
}

/// Disassemble the one instruction at byte `off` of `code` for `arch` (reuse the
/// [`AsmArch`] codes). `base` is the address the bytes run at (`EMU_CODE_BASE`)
/// so PC-relative operands resolve. Returns `"mnemonic operands"`, or `""` with
/// no disassembler present or on a decode miss.
pub fn disas(code: &[u8], off: u64, arch: AsmArch, base: u64) -> String {
    let f = match asm_fns().disas {
        Some(f) if disas_available() => f,
        _ => return String::new(),
    };
    let mut buf = [0u8; 160];
    let n = unsafe {
        f(arch as c_int, code.as_ptr(), code.len(), base, off,
          buf.as_mut_ptr() as *mut c_char, buf.len())
    };
    if n == 0 {
        return String::new();
    }
    unsafe {
        CStr::from_ptr(buf.as_ptr() as *const c_char)
            .to_string_lossy()
            .into_owned()
    }
}

impl Emulator {
    /// Assemble x86-64 `src` in `syntax` via Keystone and run it with the integer
    /// `args` (up to six), stopping after `max_insns` instructions (0 = run to
    /// `ret`). Returns the run's [`EmuResult`], or an `Err` carrying the Keystone
    /// diagnostic if `src` fails to assemble.
    pub fn call_asm(
        &self,
        src: &str,
        args: &[i64],
        syntax: AsmSyntax,
        max_insns: u64,
    ) -> Result<EmuResult, String> {
        let f = asm_fns()
            .call_asm6
            .ok_or("in-line assembler not in this build")?;
        let c = CString::new(src).map_err(|e| e.to_string())?;
        let mut a = [0 as c_long; 6];
        let n = args.len().min(6);
        for (i, v) in args.iter().take(6).enumerate() {
            a[i] = *v as c_long;
        }
        let mut out = EmuResult::default();
        let ok = unsafe {
            f(self.h, c.as_ptr(), syntax as c_int, a[0], a[1], a[2], a[3], a[4],
              a[5], n as c_int, max_insns, &mut out)
        };
        if ok == 0 {
            return Err(format!("in-line assembly failed: {}", asm_error()));
        }
        Ok(out)
    }
}

/// Assemble `src` for `arch`/`syntax` at load address `addr` and return the
/// machine-code bytes. Multi-arch (unlike [`Emulator::call_asm`], which runs on
/// the x86-64 guest). `Err` carries the Keystone diagnostic on failure.
pub fn assemble(
    src: &str,
    arch: AsmArch,
    syntax: AsmSyntax,
    addr: u64,
) -> Result<Vec<u8>, String> {
    let f = asm_fns()
        .asm_bytes
        .ok_or("in-line assembler not in this build")?;
    let c = CString::new(src).map_err(|e| e.to_string())?;
    let mut buf = vec![0u8; 256];
    let n = unsafe {
        f(arch as c_int, syntax as c_int, c.as_ptr(), addr, buf.as_mut_ptr(),
          buf.len() as c_int)
    };
    if n == 0 {
        return Err(format!("assemble failed: {}", asm_error()));
    }
    let mut n = n as usize;
    if n > buf.len() {
        buf = vec![0u8; n];
        n = unsafe {
            f(arch as c_int, syntax as c_int, c.as_ptr(), addr, buf.as_mut_ptr(),
              n as c_int)
        } as usize;
    }
    buf.truncate(n);
    Ok(buf)
}

// --- Extended x86 emu calls + cross-arch guests + trace -------------------- //
//
// The cross-arch guests (AArch64 / RISC-V / ARM32) run raw machine-code bytes,
// so they emulate on any host. Their per-arch result struct is read through the
// opaque accessors (no extra struct layout mirrored here); the x86-64 extended
// calls reuse the typed `EmuResult` above.

extern "C" {
    fn emu_call_fp(e: *mut emu_t, f: *const c_void, len: usize, ia: *const c_long,
                   ni: c_int, fa: *const f64, nf: c_int, mi: u64, out: *mut EmuResult) -> bool;
    fn emu_call_vec(e: *mut emu_t, f: *const c_void, len: usize, ia: *const c_long,
                    ni: c_int, va: *const Vec128, nv: c_int, mi: u64, out: *mut EmuResult) -> bool;
    fn emu_call_win64(e: *mut emu_t, f: *const c_void, len: usize, args: *const c_long,
                      nargs: c_int, mi: u64, out: *mut EmuResult) -> bool;
    fn emu_call_traced(e: *mut emu_t, f: *const c_void, len: usize, args: *const c_long,
                       nargs: c_int, mi: u64, out: *mut EmuResult, trace: *mut c_void) -> bool;

    fn asmtest_emu_result_faulted(r: *const c_void) -> c_int;
    fn asmtest_emu_trace_new(ic: usize, bc: usize) -> *mut c_void;
    fn asmtest_emu_trace_free(t: *mut c_void);
    fn asmtest_emu_trace_covered(t: *const c_void, off: u64) -> c_int;

    fn emu_arm64_open() -> *mut c_void;
    fn emu_arm64_close(e: *mut c_void);
    fn emu_arm64_call(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                      nargs: c_int, mi: u64, out: *mut c_void) -> bool;
    fn emu_arm64_call_traced(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                             nargs: c_int, mi: u64, out: *mut c_void, trace: *mut c_void) -> bool;
    fn asmtest_emu_arm64_result_new() -> *mut c_void;
    fn asmtest_emu_arm64_result_free(r: *mut c_void);
    fn asmtest_emu_arm64_reg(r: *const c_void, name: *const c_char) -> u64;

    fn emu_riscv_open() -> *mut c_void;
    fn emu_riscv_close(e: *mut c_void);
    fn emu_riscv_call(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                      nargs: c_int, mi: u64, out: *mut c_void) -> bool;
    fn emu_riscv_call_traced(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                             nargs: c_int, mi: u64, out: *mut c_void, trace: *mut c_void) -> bool;
    fn asmtest_emu_riscv_result_new() -> *mut c_void;
    fn asmtest_emu_riscv_result_free(r: *mut c_void);
    fn asmtest_emu_riscv_reg(r: *const c_void, name: *const c_char) -> u64;

    fn emu_arm_open() -> *mut c_void;
    fn emu_arm_close(e: *mut c_void);
    fn emu_arm_call(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                    nargs: c_int, mi: u64, out: *mut c_void) -> bool;
    fn emu_arm_call_traced(e: *mut c_void, code: *const c_void, len: usize, args: *const c_long,
                           nargs: c_int, mi: u64, out: *mut c_void, trace: *mut c_void) -> bool;
    fn asmtest_emu_arm_result_new() -> *mut c_void;
    fn asmtest_emu_arm_result_free(r: *mut c_void);
    fn asmtest_emu_arm_reg(r: *const c_void, name: *const c_char) -> u64;
}

impl Emulator {
    /// Run raw x86-64 machine-code `code` with up to six integer args.
    pub fn call_bytes(&self, code: &[u8], args: &[i64]) -> EmuResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call(self.h, code.as_ptr() as *const c_void, code.len(), ptr,
                     args.len() as c_int, 0, &mut out);
        }
        out
    }

    /// Run raw bytes marshalling doubles into the FP arg registers; the scalar
    /// double return is `res.regs.xmm[0].f64()[0]`.
    pub fn call_fp(&self, code: &[u8], iargs: &[i64], fargs: &[f64]) -> EmuResult {
        let ia: Vec<c_long> = iargs.iter().map(|x| *x as c_long).collect();
        let ip = if ia.is_empty() { std::ptr::null() } else { ia.as_ptr() };
        let fp = if fargs.is_empty() { std::ptr::null() } else { fargs.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call_fp(self.h, code.as_ptr() as *const c_void, code.len(), ip,
                        iargs.len() as c_int, fp, fargs.len() as c_int, 0, &mut out);
        }
        out
    }

    /// Run raw bytes marshalling 128-bit vectors into xmm0..7.
    pub fn call_vec(&self, code: &[u8], iargs: &[i64], vargs: &[Vec128]) -> EmuResult {
        let ia: Vec<c_long> = iargs.iter().map(|x| *x as c_long).collect();
        let ip = if ia.is_empty() { std::ptr::null() } else { ia.as_ptr() };
        let vp = if vargs.is_empty() { std::ptr::null() } else { vargs.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call_vec(self.h, code.as_ptr() as *const c_void, code.len(), ip,
                         iargs.len() as c_int, vp, vargs.len() as c_int, 0, &mut out);
        }
        out
    }

    /// Run raw bytes under the Microsoft x64 (Win64) convention.
    pub fn call_win64(&self, code: &[u8], args: &[i64]) -> EmuResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call_win64(self.h, code.as_ptr() as *const c_void, code.len(), ptr,
                           args.len() as c_int, 0, &mut out);
        }
        out
    }

    /// Like [`Emulator::call_bytes`], but record an execution trace / coverage.
    pub fn call_traced(&self, code: &[u8], args: &[i64], trace: &Trace) -> EmuResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let mut out = EmuResult::default();
        unsafe {
            emu_call_traced(self.h, code.as_ptr() as *const c_void, code.len(), ptr,
                            args.len() as c_int, 0, &mut out, trace.h);
        }
        out
    }
}

/// An opaque execution-trace / basic-block coverage recorder.
pub struct Trace {
    h: *mut c_void,
}

impl Trace {
    pub fn new() -> Self {
        Trace { h: unsafe { asmtest_emu_trace_new(4096, 4096) } }
    }
    /// True if the basic block at byte-offset `off` (from the routine entry) was entered.
    pub fn covered(&self, off: u64) -> bool {
        unsafe { asmtest_emu_trace_covered(self.h, off) != 0 }
    }
}

impl Default for Trace {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Trace {
    fn drop(&mut self) {
        unsafe { asmtest_emu_trace_free(self.h) };
    }
}

/// A cross-arch guest ISA.
#[derive(Clone, Copy, PartialEq)]
pub enum GuestArch {
    Arm64,
    Riscv,
    Arm,
}

/// A cross-arch run's outcome; registers are read by name. Dropping it frees the
/// underlying handle.
pub struct GuestResult {
    h: *mut c_void,
    arch: GuestArch,
}

impl GuestResult {
    pub fn faulted(&self) -> bool {
        unsafe { asmtest_emu_result_faulted(self.h) != 0 }
    }
    /// Guest register by name (e.g. "x0"/"sp", "a0"/"x10", "r0").
    pub fn reg(&self, name: &str) -> u64 {
        let c = match CString::new(name) {
            Ok(c) => c,
            Err(_) => return 0,
        };
        unsafe {
            match self.arch {
                GuestArch::Arm64 => asmtest_emu_arm64_reg(self.h, c.as_ptr()),
                GuestArch::Riscv => asmtest_emu_riscv_reg(self.h, c.as_ptr()),
                GuestArch::Arm => asmtest_emu_arm_reg(self.h, c.as_ptr()),
            }
        }
    }
}

impl Drop for GuestResult {
    fn drop(&mut self) {
        unsafe {
            match self.arch {
                GuestArch::Arm64 => asmtest_emu_arm64_result_free(self.h),
                GuestArch::Riscv => asmtest_emu_riscv_result_free(self.h),
                GuestArch::Arm => asmtest_emu_arm_result_free(self.h),
            }
        }
    }
}

/// A cross-arch Unicorn guest running raw machine-code bytes — emulated on any
/// host. Dropping it closes the handle.
pub struct Guest {
    h: *mut c_void,
    arch: GuestArch,
}

impl Guest {
    pub fn new(arch: GuestArch) -> Option<Self> {
        let h = unsafe {
            match arch {
                GuestArch::Arm64 => emu_arm64_open(),
                GuestArch::Riscv => emu_riscv_open(),
                GuestArch::Arm => emu_arm_open(),
            }
        };
        if h.is_null() {
            None
        } else {
            Some(Guest { h, arch })
        }
    }

    fn new_result(&self) -> *mut c_void {
        unsafe {
            match self.arch {
                GuestArch::Arm64 => asmtest_emu_arm64_result_new(),
                GuestArch::Riscv => asmtest_emu_riscv_result_new(),
                GuestArch::Arm => asmtest_emu_arm_result_new(),
            }
        }
    }

    /// Run raw machine-code `code` with integer args in the guest ABI registers.
    pub fn call(&self, code: &[u8], args: &[i64]) -> GuestResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let out = self.new_result();
        unsafe {
            let cp = code.as_ptr() as *const c_void;
            match self.arch {
                GuestArch::Arm64 => emu_arm64_call(self.h, cp, code.len(), ptr, args.len() as c_int, 0, out),
                GuestArch::Riscv => emu_riscv_call(self.h, cp, code.len(), ptr, args.len() as c_int, 0, out),
                GuestArch::Arm => emu_arm_call(self.h, cp, code.len(), ptr, args.len() as c_int, 0, out),
            };
        }
        GuestResult { h: out, arch: self.arch }
    }

    /// Like [`Guest::call`], but record an execution trace / coverage.
    pub fn call_traced(&self, code: &[u8], args: &[i64], trace: &Trace) -> GuestResult {
        let av: Vec<c_long> = args.iter().map(|x| *x as c_long).collect();
        let ptr = if av.is_empty() { std::ptr::null() } else { av.as_ptr() };
        let out = self.new_result();
        unsafe {
            let cp = code.as_ptr() as *const c_void;
            match self.arch {
                GuestArch::Arm64 => emu_arm64_call_traced(self.h, cp, code.len(), ptr,
                                                          args.len() as c_int, 0, out, trace.h),
                GuestArch::Riscv => emu_riscv_call_traced(self.h, cp, code.len(), ptr,
                                                          args.len() as c_int, 0, out, trace.h),
                GuestArch::Arm => emu_arm_call_traced(self.h, cp, code.len(), ptr,
                                                      args.len() as c_int, 0, out, trace.h),
            };
        }
        GuestResult { h: out, arch: self.arch }
    }
}

impl Drop for Guest {
    fn drop(&mut self) {
        unsafe {
            match self.arch {
                GuestArch::Arm64 => emu_arm64_close(self.h),
                GuestArch::Riscv => emu_riscv_close(self.h),
                GuestArch::Arm => emu_arm_close(self.h),
            }
        }
    }
}
