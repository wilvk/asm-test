//! Hardware-tier native runtime tracing for Rust (single-step / Intel PT / AMD).
//!
//! This is the language-wrapper surface for the optional hardware-trace tier (see
//! `include/asmtest_hwtrace.h` and `docs/native-tracing.md`). It records the same
//! `asmtest_trace_t` offsets as the emulator ([`crate::Trace`]) and DynamoRIO
//! ([`crate::drtrace`]) tiers, but by observing the **real CPU** — and unlike the
//! DynamoRIO wrapper it needs no DynamoRIO install.
//!
//! Four backends share one API, selected by enum ([`Backend`]):
//!
//! * [`Backend::Singlestep`] — EFLAGS.TF single-step (`#DB` -> SIGTRAP). Exact and
//!   complete on **any x86-64 Linux** (Intel, any-Zen AMD, VM, CI, container): no
//!   PMU, no perf_event, no privilege, no decoder. This is the portable default and
//!   the one every binding's self-test exercises live; crucially it has **no
//!   thread-takeover constraint**, so (unlike the DynamoRIO tier) it runs cleanly
//!   inside a normal `cargo test` worker thread.
//! * [`Backend::IntelPt`] / [`Backend::CoreSight`] / [`Backend::AmdLbr`] — hardware
//!   branch-trace backends that self-skip off the specific bare-metal hardware they
//!   need.
//!
//! The tier links nothing extra: `libasmtest_hwtrace` is kept out of the core lib
//! and the `libasmtest_emu` superset, so — exactly like the optional in-line
//! assembler in `lib.rs` and the DynamoRIO wrapper in `drtrace.rs` — the symbols are
//! resolved at **run time** with the libc dynamic loader. The lib is found via env
//! `ASMTEST_HWTRACE_LIB`, else `<repo>/build/libasmtest_hwtrace.so`. When the lib is
//! absent, [`HwTrace::available`] reports `false` so callers self-skip cleanly
//! rather than panicking.
//!
//! ```ignore
//! use asmtest::hwtrace::{Backend, HwTrace, NativeCode};
//! if HwTrace::available_default() {
//!     HwTrace::init_default().unwrap();
//!     let code = NativeCode::from_bytes(&[0x48,0x89,0xf8,0x48,0x01,0xf0,0xc3]);
//!     let tr = HwTrace::new_trace(64, 64);
//!     tr.register("add", &code).unwrap();
//!     {
//!         let _r = tr.region("add");
//!         assert_eq!(code.call(20, 22), 42);
//!     }
//!     assert!(tr.covered(0));
//!     HwTrace::shutdown();
//! }
//! ```

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::OnceLock;

/// `ASMTEST_HW_OK` from `asmtest_hwtrace.h`; nonzero is an error.
const ASMTEST_HW_OK: c_int = 0;
const RTLD_NOW: c_int = 2; // same value on Linux and macOS

// The crate stays dependency-free, so the dynamic loader is reached directly
// (these are private to lib.rs, so re-declare them here per the binding's
// no-crates-io rule — same RTLD_NOW contract as drtrace.rs).
extern "C" {
    fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

/// The trace backend to select (mirrors `asmtest_trace_backend_t`).
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Backend {
    /// Intel PT — Intel-x86-64 bare metal only.
    IntelPt = 0,
    /// ARM CoreSight — specific AArch64 boards only.
    CoreSight = 1,
    /// AMD Zen 3 BRS / Zen 4 LbrExtV2 (16-deep).
    AmdLbr = 2,
    /// EFLAGS.TF `#DB` single-step: any x86-64 Linux, exact + complete. The
    /// portable default.
    Singlestep = 3,
}

/// The portable default backend used by the `*_default` helpers.
pub const DEFAULT_BACKEND: Backend = Backend::Singlestep;

/// Mirrors `asmtest_hwtrace_options_t`.
#[repr(C)]
struct Options {
    backend: c_int,
    aux_size: usize,
    data_size: usize,
    snapshot: c_int,
    object_hint: *const c_char,
}

// --- Resolved entry points (libasmtest_hwtrace) --------------------------- //
//
// Each is `None` when the lib can't be loaded or the symbol is missing, so every
// caller degrades to "unavailable" instead of crashing — matching `asm_fns()` and
// `dr_fns()`.

type AvailableFn = unsafe extern "C" fn(c_int) -> c_int;
type SkipReasonFn = unsafe extern "C" fn(c_int, *mut c_char, usize);
type InitFn = unsafe extern "C" fn(*const Options) -> c_int;
type ShutdownFn = unsafe extern "C" fn();
type RegisterRegionFn =
    unsafe extern "C" fn(*const c_char, *mut c_void, usize, *mut c_void) -> c_int;
type MarkerFn = unsafe extern "C" fn(*const c_char);
type ExecAllocFn =
    unsafe extern "C" fn(*const c_void, usize, *mut *mut c_void, *mut usize) -> c_int;
type ExecFreeFn = unsafe extern "C" fn(*mut c_void, usize);
type TraceNewFn = unsafe extern "C" fn(usize, usize) -> *mut c_void;
type TraceFreeFn = unsafe extern "C" fn(*mut c_void);
type TraceCoveredFn = unsafe extern "C" fn(*mut c_void, u64) -> c_int;
type TraceU64Fn = unsafe extern "C" fn(*mut c_void) -> u64;
type TraceIntFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type TraceAtFn = unsafe extern "C" fn(*mut c_void, usize) -> u64;

struct HwFns {
    available: Option<AvailableFn>,
    skip_reason: Option<SkipReasonFn>,
    init: Option<InitFn>,
    shutdown: Option<ShutdownFn>,
    register_region: Option<RegisterRegionFn>,
    begin: Option<MarkerFn>,
    end: Option<MarkerFn>,
    exec_alloc: Option<ExecAllocFn>,
    exec_free: Option<ExecFreeFn>,
    trace_new: Option<TraceNewFn>,
    trace_free: Option<TraceFreeFn>,
    trace_covered: Option<TraceCoveredFn>,
    blocks_len: Option<TraceU64Fn>,
    insns_total: Option<TraceU64Fn>,
    insns_len: Option<TraceU64Fn>,
    truncated: Option<TraceIntFn>,
    block_at: Option<TraceAtFn>,
    insn_at: Option<TraceAtFn>,
}

// The function pointers come from the process's own libraries and outlive any
// call; sharing them across threads is sound.
unsafe impl Sync for HwFns {}
unsafe impl Send for HwFns {}

/// The shared-object name (`.dylib` on macOS, `.so` elsewhere).
fn lib_name() -> &'static str {
    if cfg!(target_os = "macos") {
        "libasmtest_hwtrace.dylib"
    } else {
        "libasmtest_hwtrace.so"
    }
}

/// dlopen `libasmtest_hwtrace` from `ASMTEST_HWTRACE_LIB`, else `<repo>/build/<lib>`,
/// else the bare name (let the loader search). Returns a null handle if none load
/// — the resolver then leaves every pointer `None`.
fn open_lib() -> *mut c_void {
    let mut cands: Vec<String> = Vec::new();
    if let Ok(p) = std::env::var("ASMTEST_HWTRACE_LIB") {
        if !p.is_empty() {
            cands.push(p);
        }
    }
    // The crate lives at <repo>/bindings/rust, so the repo's build/ dir is two
    // levels up — mirroring drtrace.rs's default and Python's parents[3]/build.
    cands.push(format!("{}/../../build/{}", env!("CARGO_MANIFEST_DIR"), lib_name()));
    cands.push(lib_name().to_string());
    for c in cands {
        if let Ok(cs) = CString::new(c) {
            let h = unsafe { dlopen(cs.as_ptr(), RTLD_NOW) };
            if !h.is_null() {
                return h;
            }
        }
    }
    std::ptr::null_mut()
}

fn hw_fns() -> &'static HwFns {
    static FNS: OnceLock<HwFns> = OnceLock::new();
    FNS.get_or_init(|| {
        let handle = open_lib();
        if handle.is_null() {
            // No lib: every pointer stays None, available() returns false.
            return HwFns {
                available: None, skip_reason: None, init: None, shutdown: None,
                register_region: None, begin: None, end: None,
                exec_alloc: None, exec_free: None,
                trace_new: None, trace_free: None, trace_covered: None,
                blocks_len: None, insns_total: None, insns_len: None,
                truncated: None, block_at: None, insn_at: None,
            };
        }
        let sym = |name: &str| -> *mut c_void {
            match CString::new(name) {
                Ok(c) => unsafe { dlsym(handle, c.as_ptr()) },
                Err(_) => std::ptr::null_mut(),
            }
        };
        // transmute a resolved symbol to `Some(fn)`, or None when absent.
        macro_rules! load {
            ($name:literal, $ty:ty) => {{
                let p = sym($name);
                if p.is_null() {
                    None
                } else {
                    Some(unsafe { std::mem::transmute::<*mut c_void, $ty>(p) })
                }
            }};
        }
        HwFns {
            available: load!("asmtest_hwtrace_available", AvailableFn),
            skip_reason: load!("asmtest_hwtrace_skip_reason", SkipReasonFn),
            init: load!("asmtest_hwtrace_init", InitFn),
            shutdown: load!("asmtest_hwtrace_shutdown", ShutdownFn),
            register_region: load!("asmtest_hwtrace_register_region", RegisterRegionFn),
            begin: load!("asmtest_hwtrace_begin", MarkerFn),
            end: load!("asmtest_hwtrace_end", MarkerFn),
            exec_alloc: load!("asmtest_hwtrace_exec_alloc", ExecAllocFn),
            exec_free: load!("asmtest_hwtrace_exec_free", ExecFreeFn),
            trace_new: load!("asmtest_trace_new", TraceNewFn),
            trace_free: load!("asmtest_trace_free", TraceFreeFn),
            trace_covered: load!("asmtest_trace_covered", TraceCoveredFn),
            blocks_len: load!("asmtest_emu_trace_blocks_len", TraceU64Fn),
            insns_total: load!("asmtest_emu_trace_insns_total", TraceU64Fn),
            insns_len: load!("asmtest_emu_trace_insns_len", TraceU64Fn),
            truncated: load!("asmtest_emu_trace_truncated", TraceIntFn),
            block_at: load!("asmtest_emu_trace_block_at", TraceAtFn),
            insn_at: load!("asmtest_emu_trace_insn_at", TraceAtFn),
        }
    })
}

/// Host-native machine code in real executable (W^X) memory.
///
/// [`from_bytes`](NativeCode::from_bytes) maps executable memory at the bytes'
/// actual runtime address (so PC-relative and branch targets resolve); dropping
/// the value unmaps it. Free the [`NativeCode`] only **after** the region that
/// referenced it has been torn down.
pub struct NativeCode {
    base: *mut c_void,
    len: usize,
}

impl NativeCode {
    /// Map executable memory and copy `bytes` of host-native machine code into it.
    /// Panics only if the hwtrace lib is unavailable or the mapping fails — gate on
    /// [`HwTrace::available`] first (the tier is opt-in).
    pub fn from_bytes(bytes: &[u8]) -> NativeCode {
        let alloc = hw_fns()
            .exec_alloc
            .expect("libasmtest_hwtrace not loaded (HwTrace::available() is false)");
        let mut base: *mut c_void = std::ptr::null_mut();
        let mut len: usize = 0;
        let rc = unsafe {
            alloc(bytes.as_ptr() as *const c_void, bytes.len(), &mut base, &mut len)
        };
        assert!(rc == ASMTEST_HW_OK, "asmtest_hwtrace_exec_alloc failed: {rc}");
        NativeCode { base, len }
    }

    /// The executable entry address (offset 0) as a host pointer value.
    pub fn base(&self) -> usize {
        self.base as usize
    }

    /// Number of code bytes mapped.
    pub fn len(&self) -> usize {
        self.len
    }

    /// True if no bytes were mapped.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Call the code with no integer args, reading the result as a C long
    /// (the SysV integer ABI).
    pub fn call0(&self) -> i64 {
        let f: extern "C" fn() -> c_long = unsafe { std::mem::transmute(self.base) };
        f() as i64
    }

    /// Call the code with one integer arg.
    pub fn call1(&self, a: i64) -> i64 {
        let f: extern "C" fn(c_long) -> c_long = unsafe { std::mem::transmute(self.base) };
        f(a as c_long) as i64
    }

    /// Call the code with two integer args (the canonical `add2(a, b)` shape).
    pub fn call(&self, a: i64, b: i64) -> i64 {
        let f: extern "C" fn(c_long, c_long) -> c_long =
            unsafe { std::mem::transmute(self.base) };
        f(a as c_long, b as c_long) as i64
    }
}

impl Drop for NativeCode {
    fn drop(&mut self) {
        if !self.base.is_null() {
            if let Some(free) = hw_fns().exec_free {
                unsafe { free(self.base, self.len) };
            }
            self.base = std::ptr::null_mut();
        }
    }
}

/// A scoped trace region (RAII guard). Constructed by [`Trace::region`]: the ctor
/// calls `asmtest_hwtrace_begin(name)` and dropping it calls
/// `asmtest_hwtrace_end(name)`, so the begin/end markers stay balanced.
pub struct Region {
    name: CString,
}

impl Region {
    fn new(name: &str) -> Region {
        let cname = CString::new(name).expect("region name has interior NUL");
        if let Some(begin) = hw_fns().begin {
            unsafe { begin(cname.as_ptr()) };
        }
        Region { name: cname }
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        if let Some(end) = hw_fns().end {
            unsafe { end(self.name.as_ptr()) };
        }
    }
}

/// The process-wide hardware-trace tier: detect, initialize, tear down.
///
/// Bring the tier up once with [`init`](HwTrace::init) /
/// [`init_default`](HwTrace::init_default), allocate per-call recorders with
/// [`new_trace`](HwTrace::new_trace), register a [`NativeCode`] range, then read
/// back coverage. Take the tier down with [`shutdown`](HwTrace::shutdown).
pub struct HwTrace;

impl HwTrace {
    // ---- process-wide lifecycle ---- //

    /// True if the chosen `backend` can run on this host: the hwtrace lib loaded
    /// **and** `asmtest_hwtrace_available(backend)` reports the full detect chain
    /// passes. Never panics, so callers (and the test) can self-skip cleanly.
    pub fn available(backend: Backend) -> bool {
        match hw_fns().available {
            Some(f) => unsafe { f(backend as c_int) != 0 },
            None => false,
        }
    }

    /// [`available`](HwTrace::available) for the portable [`DEFAULT_BACKEND`]
    /// (single-step), which runs on any x86-64 Linux.
    pub fn available_default() -> bool {
        Self::available(DEFAULT_BACKEND)
    }

    /// A human-readable reason [`available`](HwTrace::available) is false (or
    /// `"available"`). Empty string when the lib is absent.
    pub fn skip_reason(backend: Backend) -> String {
        match hw_fns().skip_reason {
            Some(f) => {
                let mut buf = [0u8; 160];
                unsafe {
                    f(backend as c_int, buf.as_mut_ptr() as *mut c_char, buf.len());
                    std::ffi::CStr::from_ptr(buf.as_ptr() as *const c_char)
                        .to_string_lossy()
                        .into_owned()
                }
            }
            None => String::new(),
        }
    }

    /// [`skip_reason`](HwTrace::skip_reason) for the [`DEFAULT_BACKEND`].
    pub fn skip_reason_default() -> String {
        Self::skip_reason(DEFAULT_BACKEND)
    }

    /// Select `backend` and initialize the tier (`asmtest_hwtrace_init` with the
    /// other options defaulted: rings auto-sized, linear ring, no object hint).
    pub fn init(backend: Backend) -> Result<(), String> {
        let f = hw_fns().init.ok_or("libasmtest_hwtrace not loaded")?;
        let opts = Options {
            backend: backend as c_int,
            aux_size: 0,
            data_size: 0,
            snapshot: 0,
            object_hint: std::ptr::null(),
        };
        let rc = unsafe { f(&opts) };
        if rc != ASMTEST_HW_OK {
            return Err(format!("asmtest_hwtrace_init failed: {rc}"));
        }
        Ok(())
    }

    /// [`init`](HwTrace::init) with the portable [`DEFAULT_BACKEND`] (single-step).
    pub fn init_default() -> Result<(), String> {
        Self::init(DEFAULT_BACKEND)
    }

    /// Take the tier back down (`asmtest_hwtrace_shutdown`). A no-op if the lib
    /// never loaded.
    pub fn shutdown() {
        if let Some(f) = hw_fns().shutdown {
            unsafe { f() };
        }
    }

    // ---- per-trace ---- //

    /// Allocate an app-owned trace handle. Records the ordered instruction stream
    /// when `instructions > 0` and basic-block coverage when `blocks > 0`. NOTE:
    /// the C entry takes `(insns_cap, blocks_cap)`, so this forwards
    /// `asmtest_trace_new(instructions, blocks)`. Panics if the lib is unavailable
    /// or the allocation fails — gate on [`available`](HwTrace::available).
    pub fn new_trace(blocks: usize, instructions: usize) -> Trace {
        let f = hw_fns()
            .trace_new
            .expect("libasmtest_hwtrace not loaded (HwTrace::available() is false)");
        let h = unsafe { f(instructions, blocks) };
        assert!(!h.is_null(), "asmtest_trace_new failed");
        Trace { handle: h }
    }
}

/// An app-owned coverage recorder for a registered native region.
///
/// Create with [`HwTrace::new_trace`], register a [`NativeCode`] range, bracket a
/// call in a [`region`](Trace::region), then read back coverage. Dropping the value
/// frees the underlying trace handle.
pub struct Trace {
    handle: *mut c_void,
}

impl Trace {
    /// Register a non-overlapping native code range under `name`, recording
    /// coverage into this trace. The copy of `name` may be dropped after the call.
    pub fn register(&self, name: &str, code: &NativeCode) -> Result<(), String> {
        let f = hw_fns()
            .register_region
            .ok_or("libasmtest_hwtrace not loaded")?;
        let cname = CString::new(name).map_err(|e| e.to_string())?;
        let rc = unsafe { f(cname.as_ptr(), code.base, code.len, self.handle) };
        if rc != ASMTEST_HW_OK {
            return Err(format!("register_region({name:?}) failed: {rc}"));
        }
        Ok(())
    }

    /// Open a scoped trace region for `name`: the returned [`Region`] opens
    /// recording now (`asmtest_hwtrace_begin`) and closes it on drop
    /// (`asmtest_hwtrace_end`), keeping the markers balanced. Bracket exactly one
    /// registered routine per region (capture is a single process-global slot).
    pub fn region(&self, name: &str) -> Region {
        Region::new(name)
    }

    /// True if the basic block at byte-offset `off` (from the region entry) was
    /// entered.
    pub fn covered(&self, off: u64) -> bool {
        match hw_fns().trace_covered {
            Some(f) => unsafe { f(self.handle, off) != 0 },
            None => false,
        }
    }

    /// Number of distinct basic blocks recorded in this trace.
    pub fn blocks_len(&self) -> u64 {
        match hw_fns().blocks_len {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// Total instructions observed in the ordered stream (may exceed the stored
    /// `insns_len` if the trace's instruction capacity was reached).
    pub fn insns_total(&self) -> u64 {
        match hw_fns().insns_total {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// Number of instruction offsets actually stored (capped at the trace's insns
    /// capacity).
    pub fn insns_len(&self) -> u64 {
        match hw_fns().insns_len {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// True if the ordered instruction stream was truncated (more instructions
    /// executed than the trace's instruction capacity).
    pub fn truncated(&self) -> bool {
        match hw_fns().truncated {
            Some(f) => unsafe { f(self.handle) != 0 },
            None => false,
        }
    }

    /// The distinct basic-block start offsets recorded, in first-seen order.
    /// Empty when the lib is absent.
    pub fn block_offsets(&self) -> Vec<u64> {
        let fns = hw_fns();
        match (fns.blocks_len, fns.block_at) {
            (Some(len), Some(at)) => {
                let n = unsafe { len(self.handle) };
                (0..n).map(|i| unsafe { at(self.handle, i as usize) }).collect()
            }
            _ => Vec::new(),
        }
    }

    /// The ordered instruction-offset stream actually stored — each executed
    /// instruction's offset in execution order, up to the trace's insns capacity
    /// (insns_len, not the possibly-larger insns_total). Empty when the lib is
    /// absent.
    pub fn insn_offsets(&self) -> Vec<u64> {
        let fns = hw_fns();
        match (fns.insns_len, fns.insn_at) {
            (Some(len), Some(at)) => {
                let n = unsafe { len(self.handle) };
                (0..n).map(|i| unsafe { at(self.handle, i as usize) }).collect()
            }
            _ => Vec::new(),
        }
    }
}

impl Drop for Trace {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            if let Some(f) = hw_fns().trace_free {
                unsafe { f(self.handle) };
            }
            self.handle = std::ptr::null_mut();
        }
    }
}
