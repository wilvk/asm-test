//! In-process native runtime tracing for Rust, backed by DynamoRIO.
//!
//! This is the language-wrapper surface for the optional DynamoRIO native-trace
//! tier (see `include/asmtest_drtrace.h` and `docs/native-tracing.md`). Where the
//! emulator tier ([`crate::Trace`]) traces isolated guest bytes, [`NativeTrace`]
//! traces host-native code as it runs **inside this Rust process**: initialize
//! DynamoRIO once at startup, materialize host-native machine code, mark a region,
//! call into it, and read back basic-block coverage / the instruction stream.
//!
//! The tier links nothing: `libasmtest_drapp` requires DynamoRIO and may be
//! absent, so — exactly like the optional in-line assembler in `lib.rs` — the
//! symbols are resolved at **run time** with the libc dynamic loader. The lib is
//! found via env `ASMTEST_DRAPP_LIB`, else `<repo>/build/libasmtest_drapp.so`.
//! When the lib (or `libdynamorio`) is absent, [`NativeTrace::available`] reports
//! `false` so callers self-skip cleanly rather than panicking.
//!
//! Advanced, Linux-x86-64-only, and opt-in.
//!
//! ```ignore
//! use asmtest::drtrace::{NativeTrace, NativeCode};
//! if NativeTrace::available() {
//!     NativeTrace::initialize_default().unwrap();
//!     let code = NativeCode::from_bytes(&[0x48,0x89,0xf8,0x48,0x01,0xf0,0xc3]);
//!     let tr = NativeTrace::new_trace(64, 0);
//!     tr.register("add", &code).unwrap();
//!     {
//!         let _r = tr.region("add");
//!         assert_eq!(code.call2(20, 22), 42);
//!     }
//!     assert!(tr.covered(0));
//!     NativeTrace::shutdown();
//! }
//! ```

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::OnceLock;

/// `ASMTEST_DR_OK` from `asmtest_drtrace.h`; nonzero is an error.
const ASMTEST_DR_OK: c_int = 0;
const RTLD_NOW: c_int = 2; // same value on Linux and macOS

// The crate stays dependency-free, so the dynamic loader is reached directly
// (these are private to lib.rs, so re-declare them here per the binding's
// no-crates-io rule — same RTLD_NOW contract).
extern "C" {
    fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
}

/// Mirrors `asmtest_drtrace_options_t`.
#[repr(C)]
struct Options {
    dynamorio_home: *const c_char,
    client_path: *const c_char,
    client_options: *const c_char,
    mode: c_int,
}

/// Mirrors `asmtest_exec_code_t`.
#[repr(C)]
#[derive(Clone, Copy)]
struct ExecCode {
    base: *mut c_void,
    len: usize,
}

// --- Resolved entry points (libasmtest_drapp) ----------------------------- //
//
// Each is `None` when the lib can't be loaded or the symbol is missing, so every
// caller degrades to "unavailable" instead of crashing — matching `asm_fns()`.

type AvailableFn = unsafe extern "C" fn() -> c_int;
type InitFn = unsafe extern "C" fn(*const Options) -> c_int;
type StartFn = unsafe extern "C" fn() -> c_int;
type StopFn = unsafe extern "C" fn() -> c_int;
type ShutdownFn = unsafe extern "C" fn();
type RegisterRegionFn =
    unsafe extern "C" fn(*const c_char, *mut c_void, usize, *mut c_void) -> c_int;
type UnregisterRegionFn = unsafe extern "C" fn(*const c_char) -> c_int;
type MarkerFn = unsafe extern "C" fn(*const c_char);
type MarkerErrorFn = unsafe extern "C" fn() -> c_int;
type ExecAllocFn = unsafe extern "C" fn(*const u8, usize, *mut ExecCode) -> c_int;
type ExecFreeFn = unsafe extern "C" fn(*mut ExecCode);
type TraceNewFn = unsafe extern "C" fn(usize, usize) -> *mut c_void;
type TraceFreeFn = unsafe extern "C" fn(*mut c_void);
type TraceCoveredFn = unsafe extern "C" fn(*mut c_void, u64) -> c_int;
type TraceU64Fn = unsafe extern "C" fn(*mut c_void) -> u64;
type TraceAtFn = unsafe extern "C" fn(*mut c_void, usize) -> u64;

struct DrFns {
    available: Option<AvailableFn>,
    init: Option<InitFn>,
    start: Option<StartFn>,
    #[allow(dead_code)] // exposed by the C API; not on the Python surface either
    stop: Option<StopFn>,
    shutdown: Option<ShutdownFn>,
    register_region: Option<RegisterRegionFn>,
    unregister_region: Option<UnregisterRegionFn>,
    trace_begin: Option<MarkerFn>,
    trace_end: Option<MarkerFn>,
    marker_error: Option<MarkerErrorFn>,
    exec_alloc: Option<ExecAllocFn>,
    exec_free: Option<ExecFreeFn>,
    trace_new: Option<TraceNewFn>,
    trace_free: Option<TraceFreeFn>,
    trace_covered: Option<TraceCoveredFn>,
    blocks_len: Option<TraceU64Fn>,
    insns_total: Option<TraceU64Fn>,
    insns_len: Option<TraceU64Fn>,
    block_at: Option<TraceAtFn>,
    insn_at: Option<TraceAtFn>,
}

// The function pointers come from the process's own libraries and outlive any
// call; sharing them across threads is sound.
unsafe impl Sync for DrFns {}
unsafe impl Send for DrFns {}

/// The shared-object name (`.dylib` on macOS, `.so` elsewhere).
fn lib_name() -> &'static str {
    if cfg!(target_os = "macos") {
        "libasmtest_drapp.dylib"
    } else {
        "libasmtest_drapp.so"
    }
}

/// dlopen `libasmtest_drapp` from `ASMTEST_DRAPP_LIB`, else `<repo>/build/<lib>`,
/// else the bare name (let the loader search). Returns a null handle if none load
/// — the resolver then leaves every pointer `None`.
fn open_lib() -> *mut c_void {
    let mut cands: Vec<String> = Vec::new();
    if let Ok(p) = std::env::var("ASMTEST_DRAPP_LIB") {
        if !p.is_empty() {
            cands.push(p);
        }
    }
    // The crate lives at <repo>/bindings/rust, so the repo's build/ dir is two
    // levels up — mirroring build.rs's default and Python's parents[3]/build.
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

fn dr_fns() -> &'static DrFns {
    static FNS: OnceLock<DrFns> = OnceLock::new();
    FNS.get_or_init(|| {
        let handle = open_lib();
        if handle.is_null() {
            // No lib: every pointer stays None, available() returns false.
            return DrFns {
                available: None, init: None, start: None, stop: None, shutdown: None,
                register_region: None, unregister_region: None, trace_begin: None,
                trace_end: None, marker_error: None, exec_alloc: None, exec_free: None,
                trace_new: None, trace_free: None, trace_covered: None,
                blocks_len: None, insns_total: None, insns_len: None,
                block_at: None, insn_at: None,
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
        DrFns {
            available: load!("asmtest_dr_available", AvailableFn),
            init: load!("asmtest_dr_init", InitFn),
            start: load!("asmtest_dr_start", StartFn),
            stop: load!("asmtest_dr_stop", StopFn),
            shutdown: load!("asmtest_dr_shutdown", ShutdownFn),
            register_region: load!("asmtest_dr_register_region", RegisterRegionFn),
            unregister_region: load!("asmtest_dr_unregister_region", UnregisterRegionFn),
            trace_begin: load!("asmtest_trace_begin", MarkerFn),
            trace_end: load!("asmtest_trace_end", MarkerFn),
            marker_error: load!("asmtest_dr_marker_error", MarkerErrorFn),
            exec_alloc: load!("asmtest_exec_alloc", ExecAllocFn),
            exec_free: load!("asmtest_exec_free", ExecFreeFn),
            trace_new: load!("asmtest_trace_new", TraceNewFn),
            trace_free: load!("asmtest_trace_free", TraceFreeFn),
            trace_covered: load!("asmtest_trace_covered", TraceCoveredFn),
            blocks_len: load!("asmtest_emu_trace_blocks_len", TraceU64Fn),
            insns_total: load!("asmtest_emu_trace_insns_total", TraceU64Fn),
            insns_len: load!("asmtest_emu_trace_insns_len", TraceU64Fn),
            block_at: load!("asmtest_emu_trace_block_at", TraceAtFn),
            insn_at: load!("asmtest_emu_trace_insn_at", TraceAtFn),
        }
    })
}

/// Host-native machine code in real executable (W^X) memory.
///
/// [`from_bytes`](NativeCode::from_bytes) maps executable memory at the bytes'
/// actual runtime address (so PC-relative and branch targets resolve); dropping
/// the value unmaps it. Free the [`NativeCode`] only **after** unregistering any
/// region that referenced it (regions cache a translation keyed by name).
pub struct NativeCode {
    code: ExecCode,
}

impl NativeCode {
    /// Map executable memory and copy `bytes` of host-native machine code into it.
    /// Panics only if the drapp lib is unavailable or the mapping fails — gate on
    /// [`NativeTrace::available`] first (the tier is opt-in).
    pub fn from_bytes(bytes: &[u8]) -> NativeCode {
        let alloc = dr_fns()
            .exec_alloc
            .expect("libasmtest_drapp not loaded (NativeTrace::available() is false)");
        let mut ec = ExecCode { base: std::ptr::null_mut(), len: 0 };
        let rc = unsafe { alloc(bytes.as_ptr(), bytes.len(), &mut ec) };
        assert!(rc == ASMTEST_DR_OK, "asmtest_exec_alloc failed: {rc}");
        NativeCode { code: ec }
    }

    /// The executable entry address (offset 0) as a host pointer value.
    pub fn base(&self) -> usize {
        self.code.base as usize
    }

    /// Number of code bytes mapped.
    pub fn len(&self) -> usize {
        self.code.len
    }

    /// True if no bytes were mapped.
    pub fn is_empty(&self) -> bool {
        self.code.len == 0
    }

    /// Call the code with no integer args, reading the result as a C long
    /// (the SysV integer ABI).
    pub fn call0(&self) -> i64 {
        let f: extern "C" fn() -> c_long = unsafe { std::mem::transmute(self.code.base) };
        f() as i64
    }

    /// Call the code with one integer arg.
    pub fn call1(&self, a: i64) -> i64 {
        let f: extern "C" fn(c_long) -> c_long =
            unsafe { std::mem::transmute(self.code.base) };
        f(a as c_long) as i64
    }

    /// Call the code with two integer args (the canonical `add2(a, b)` shape).
    pub fn call2(&self, a: i64, b: i64) -> i64 {
        let f: extern "C" fn(c_long, c_long) -> c_long =
            unsafe { std::mem::transmute(self.code.base) };
        f(a as c_long, b as c_long) as i64
    }

    /// Invoke through a function pointer, dispatching on `args.len()` (0..=2).
    /// More than two args panics — use [`call0`](NativeCode::call0) /
    /// [`call1`](NativeCode::call1) / [`call2`](NativeCode::call2) directly for a
    /// wider arity.
    pub fn call(&self, args: &[i64]) -> i64 {
        match args.len() {
            0 => self.call0(),
            1 => self.call1(args[0]),
            2 => self.call2(args[0], args[1]),
            n => panic!("NativeCode::call supports 0..=2 args, got {n}"),
        }
    }
}

impl Drop for NativeCode {
    fn drop(&mut self) {
        if !self.code.base.is_null() {
            if let Some(free) = dr_fns().exec_free {
                unsafe { free(&mut self.code) };
            }
            self.code.base = std::ptr::null_mut();
        }
    }
}

/// A scoped trace region (RAII guard). Constructed by [`NativeTrace::region`]:
/// the ctor calls `asmtest_trace_begin(name)` and dropping it calls
/// `asmtest_trace_end(name)`, so the begin/end markers stay balanced.
pub struct Region {
    name: CString,
}

impl Region {
    fn new(name: &str) -> Region {
        let cname = CString::new(name).expect("region name has interior NUL");
        if let Some(begin) = dr_fns().trace_begin {
            unsafe { begin(cname.as_ptr()) };
        }
        Region { name: cname }
    }
}

impl Drop for Region {
    fn drop(&mut self) {
        if let Some(end) = dr_fns().trace_end {
            unsafe { end(self.name.as_ptr()) };
        }
    }
}

/// An app-owned coverage recorder for a registered native region.
///
/// Create with [`new_trace`](NativeTrace::new_trace), bring DynamoRIO up once per
/// process with [`initialize`](NativeTrace::initialize) /
/// [`initialize_default`](NativeTrace::initialize_default), register a
/// [`NativeCode`] range, then read back coverage. Dropping the value frees the
/// underlying trace handle.
pub struct NativeTrace {
    handle: *mut c_void,
}

impl NativeTrace {
    // ---- process-wide lifecycle ---- //

    /// True if the tier can run: the drapp lib loaded **and**
    /// `asmtest_dr_available()` reports DynamoRIO is resolvable. Never panics, so
    /// callers (and the test) can self-skip cleanly.
    pub fn available() -> bool {
        match dr_fns().available {
            Some(f) => unsafe { f() != 0 },
            None => false,
        }
    }

    /// Bring DynamoRIO up in-process and take over: fill the options, run
    /// `asmtest_dr_init` then `asmtest_dr_start`. `client` is the path to
    /// `libasmtest_drclient.so` (a `None` passes a null pointer, so the C side
    /// falls back to env `ASMTEST_DRCLIENT`); `dynamorio_home` lets the C side
    /// find `libdynamorio` (else env `ASMTEST_DR_LIB` / rpath); `mode` is the
    /// process-init default recording mode (0 = blocks).
    pub fn initialize(
        client: Option<&str>,
        dynamorio_home: Option<&str>,
        client_options: Option<&str>,
        mode: i32,
    ) -> Result<(), String> {
        let fns = dr_fns();
        let init = fns.init.ok_or("libasmtest_drapp not loaded")?;
        let start = fns.start.ok_or("libasmtest_drapp not loaded")?;

        // Hold the CStrings alive across both calls; an absent option is a null
        // pointer (the C side reads env), so an empty &str also maps to null.
        let to_c = |s: Option<&str>| -> Result<Option<CString>, String> {
            match s {
                Some(v) if !v.is_empty() => CString::new(v).map(Some).map_err(|e| e.to_string()),
                _ => Ok(None),
            }
        };
        let home = to_c(dynamorio_home)?;
        let path = to_c(client)?;
        let copts = to_c(client_options)?;
        let p = |o: &Option<CString>| o.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());

        let opts = Options {
            dynamorio_home: p(&home),
            client_path: p(&path),
            client_options: p(&copts),
            mode: mode as c_int,
        };
        let rc = unsafe { init(&opts) };
        if rc != ASMTEST_DR_OK {
            return Err(format!("asmtest_dr_init failed: {rc}"));
        }
        let rc = unsafe { start() };
        if rc != ASMTEST_DR_OK {
            return Err(format!("asmtest_dr_start failed: {rc}"));
        }
        Ok(())
    }

    /// [`initialize`](NativeTrace::initialize) with every option defaulted (all
    /// `None`, mode 0): the C side reads `ASMTEST_DRCLIENT` / `ASMTEST_DR_LIB`.
    pub fn initialize_default() -> Result<(), String> {
        Self::initialize(None, None, None, 0)
    }

    /// Take DynamoRIO back down (`asmtest_dr_shutdown`). A no-op if the lib never
    /// loaded.
    pub fn shutdown() {
        if let Some(f) = dr_fns().shutdown {
            unsafe { f() };
        }
    }

    /// Count of illegal marker operations since init (end without begin, or a
    /// mismatched end). 0 means every marker was balanced. Returns 0 when the lib
    /// is absent.
    pub fn marker_error() -> i32 {
        match dr_fns().marker_error {
            Some(f) => unsafe { f() as i32 },
            None => 0,
        }
    }

    // ---- per-trace ---- //

    /// Allocate an app-owned trace handle. Records the ordered instruction stream
    /// when `instructions > 0` and basic-block coverage when `blocks > 0`. NOTE:
    /// the C entry takes `(insns_cap, blocks_cap)`, so this forwards
    /// `asmtest_trace_new(instructions, blocks)`. Panics if the lib is unavailable
    /// or the allocation fails — gate on [`available`](NativeTrace::available).
    pub fn new_trace(blocks: usize, instructions: usize) -> NativeTrace {
        let f = dr_fns()
            .trace_new
            .expect("libasmtest_drapp not loaded (NativeTrace::available() is false)");
        let h = unsafe { f(instructions, blocks) };
        assert!(!h.is_null(), "asmtest_trace_new failed");
        NativeTrace { handle: h }
    }

    /// Register a non-overlapping native code range under `name`, recording
    /// coverage into this trace. The copy of `name` may be dropped after the call.
    pub fn register(&self, name: &str, code: &NativeCode) -> Result<(), String> {
        let f = dr_fns()
            .register_region
            .ok_or("libasmtest_drapp not loaded")?;
        let cname = CString::new(name).map_err(|e| e.to_string())?;
        let rc = unsafe {
            f(cname.as_ptr(), code.code.base, code.code.len, self.handle)
        };
        if rc != ASMTEST_DR_OK {
            return Err(format!("register_region({name:?}) failed: {rc}"));
        }
        Ok(())
    }

    /// Drop the registration for `name` (makes the client drop its cached
    /// translation). Always unregister before freeing the [`NativeCode`].
    pub fn unregister(&self, name: &str) {
        if let (Some(f), Ok(cname)) = (dr_fns().unregister_region, CString::new(name)) {
            unsafe { f(cname.as_ptr()) };
        }
    }

    /// Open a scoped trace region for `name`: the returned [`Region`] opens
    /// recording now (`asmtest_trace_begin`) and closes it on drop
    /// (`asmtest_trace_end`), keeping the markers balanced.
    pub fn region(&self, name: &str) -> Region {
        Region::new(name)
    }

    /// True if the basic block at byte-offset `off` (from the region entry) was
    /// entered.
    pub fn covered(&self, off: u64) -> bool {
        match dr_fns().trace_covered {
            Some(f) => unsafe { f(self.handle, off) != 0 },
            None => false,
        }
    }

    /// Number of distinct basic blocks recorded in this trace.
    pub fn blocks_len(&self) -> u64 {
        match dr_fns().blocks_len {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// Total instructions in the ordered stream (when the trace was created with
    /// `instructions > 0`).
    pub fn insns_total(&self) -> u64 {
        match dr_fns().insns_total {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// The distinct basic-block start offsets recorded, in first-seen order.
    /// Empty when the lib is absent.
    pub fn block_offsets(&self) -> Vec<u64> {
        let fns = dr_fns();
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
        let fns = dr_fns();
        match (fns.insns_len, fns.insn_at) {
            (Some(len), Some(at)) => {
                let n = unsafe { len(self.handle) };
                (0..n).map(|i| unsafe { at(self.handle, i as usize) }).collect()
            }
            _ => Vec::new(),
        }
    }
}

impl Drop for NativeTrace {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            if let Some(f) = dr_fns().trace_free {
                unsafe { f(self.handle) };
            }
            self.handle = std::ptr::null_mut();
        }
    }
}
