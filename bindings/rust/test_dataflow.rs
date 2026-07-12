// Rust data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the other language suites. Direct FFI (extern "C" + #[repr(C)]); built
// standalone with `rustc test_dataflow.rs -L <build> -l asmtest_dataflow`.
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

#[repr(C)]
struct GcMove {
    old_base: u64,
    new_base: u64,
    len: u64,
    step: u32,
}

#[repr(C)]
struct Method {
    addr: u64,
    size: u64,
    name: *const c_char,
    version: u64,
}

extern "C" {
    fn asmtest_gcmove_canon(moves: *const GcMove, nmoves: usize, step: u32, phys: u64) -> u64;
    fn asmtest_method_resolve_pc(methods: *const Method, nmethods: usize, pc: u64) -> c_int;
}

static N: AtomicU32 = AtomicU32::new(0);
static FAILED: AtomicBool = AtomicBool::new(false);

fn check(cond: bool, desc: &str) {
    let n = N.fetch_add(1, Ordering::SeqCst) + 1;
    println!("{} {} - {}", if cond { "ok" } else { "not ok" }, n, desc);
    if !cond {
        FAILED.store(true, Ordering::SeqCst);
    }
}

fn gcmove(moves: &[(u64, u64, u64, u32)], step: u32, phys: u64) -> u64 {
    let arr: Vec<GcMove> = moves
        .iter()
        .map(|&(o, n, l, s)| GcMove { old_base: o, new_base: n, len: l, step: s })
        .collect();
    let ptr = if arr.is_empty() { std::ptr::null() } else { arr.as_ptr() };
    unsafe { asmtest_gcmove_canon(ptr, arr.len(), step, phys) }
}

fn method(methods: &[(u64, u64, &str, u64)], pc: u64) -> c_int {
    let names: Vec<CString> = methods.iter().map(|&(_, _, n, _)| CString::new(n).unwrap()).collect();
    let arr: Vec<Method> = methods
        .iter()
        .enumerate()
        .map(|(i, &(a, sz, _, v))| Method { addr: a, size: sz, name: names[i].as_ptr(), version: v })
        .collect();
    let ptr = if arr.is_empty() { std::ptr::null() } else { arr.as_ptr() };
    let r = unsafe { asmtest_method_resolve_pc(ptr, arr.len(), pc) };
    drop(names); // keep the CStrings alive through the call
    r
}

fn main() {
    // GC-move canonicalizer
    check(gcmove(&[], 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    let mv = [(0x1000, 0x2000, 0x100, 5)];
    check(gcmove(&mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove(&mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove(&mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove(&mv, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded");
    check(gcmove(&mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove(&mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    let mv2 = [(0x1000, 0x2000, 0x100, 3), (0x2000, 0x3000, 0x100, 6)];
    check(gcmove(&mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // method resolver
    let ms = [(0x1000, 0x40, "Foo", 3), (0x2000, 0x20, "Bar", 1), (0x3000, 0, "Baz", 2)];
    check(method(&ms, 0x1000) == 0, "method: Foo range start");
    check(method(&ms, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method(&ms, 0x1040) == -1, "method: one past Foo -> none");
    check(method(&ms, 0x2010) == 1, "method: Bar range");
    check(method(&ms, 0x3000) == 2, "method: Baz point match");
    check(method(&ms, 0x3001) == -1, "method: Baz is point-only");
    let rj = [(0x1000, 0x40, "Foo", 1), (0x1000, 0x40, "Foo", 5)];
    check(method(&rj, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method(&[], 0x1000) == -1, "method: empty map -> -1");

    println!("1..{}", N.load(Ordering::SeqCst));
    if FAILED.load(Ordering::SeqCst) {
        std::process::exit(1);
    }
}
