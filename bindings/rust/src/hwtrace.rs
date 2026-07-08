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
/// `ASMTEST_HW_EUNAVAIL` from `asmtest_hwtrace.h`: no hardware-trace backend is
/// available on this host (the `< 0` return of [`HwTrace::auto`]).
pub const ASMTEST_HW_EUNAVAIL: c_int = -3;
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

/// The backend auto-selection policy (mirrors `asmtest_hwtrace_policy_t`), passed
/// to [`HwTrace::resolve`] / [`HwTrace::auto`].
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Policy {
    /// The most faithful available backend.
    Best = 0,
    /// The same, but skipping the one fixed-window backend (AMD LBR); re-resolve
    /// under this after a trace comes back truncated.
    CeilingFree = 1,
}

// --- Cross-tier orchestrator (asmtest_trace_auto.h) ----------------------- //
//
// The front-end OVER all three trace tiers (hardware + DynamoRIO + emulator), not
// just the hardware backends above. It walks the full descending-fidelity cascade
// — Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight -> emulator — and
// returns the available options for the caller to bracket with that tier's API.

/// A trace tier, most-faithful to least (mirrors `asmtest_trace_tier_t`).
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tier {
    /// HW branch trace / single-step on the real CPU.
    HwTrace = 0,
    /// In-process software DBI (DynamoRIO) on the real CPU.
    DynamoRio = 1,
    /// Unicorn virtual CPU tracing an isolated guest.
    Emulator = 2,
}

/// Execution fidelity of a tier (mirrors `asmtest_trace_fidelity_t`). The single
/// `Native` -> `Virtual` transition is the line [`TracePolicy::NativeOnly`] gates.
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Fidelity {
    /// Runs the real bytes on the real CPU in this process.
    Native = 0,
    /// Runs an isolated guest on an emulated CPU.
    Virtual = 1,
}

/// The cross-tier auto-selection policy bitmask (mirrors the `ASMTEST_TRACE_*`
/// flags), passed to [`HwTrace::resolve_tiers`] / [`HwTrace::auto_tier`].
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TracePolicy {
    /// The most-faithful available choice; the emulator floor is allowed.
    Best = 0,
    /// The same, but dropping the one fixed-window backend (AMD LBR).
    CeilingFree = 1,
    /// Forbid the native->emulator crossing: resolve only the real-CPU tiers.
    NativeOnly = 2,
}

/// A resolved cross-tier trace option (the safe wrapper over `asmtest_trace_choice_t`):
/// which [`Tier`] to use, which hardware [`Backend`] within it (meaningful only when
/// `tier == Tier::HwTrace`; otherwise `0`/ignore, exposed as the raw `i32`), and the
/// [`Fidelity`] class so a caller can see at a glance whether a choice crosses the
/// native->emulator line.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TierChoice {
    /// The trace tier of this choice.
    pub tier: Tier,
    /// The hardware backend enum, valid only when `tier == Tier::HwTrace`.
    pub backend: i32,
    /// The execution fidelity (native vs emulated) of this choice.
    pub fidelity: Fidelity,
}

/// Marshal a raw `asmtest_trace_choice_t` (three C ints) into the safe
/// [`TierChoice`]. The `tier`/`fidelity` ints come straight from the C enums, so
/// any value outside the known set is a contract break and panics.
fn tier_choice_of(c: TraceChoice) -> TierChoice {
    let tier = match c.tier {
        0 => Tier::HwTrace,
        1 => Tier::DynamoRio,
        2 => Tier::Emulator,
        other => panic!("unexpected trace tier enum {other}"),
    };
    let fidelity = match c.fidelity {
        0 => Fidelity::Native,
        1 => Fidelity::Virtual,
        other => panic!("unexpected trace fidelity enum {other}"),
    };
    TierChoice { tier, backend: c.backend, fidelity }
}

/// Mirrors `asmtest_hwtrace_options_t`.
#[repr(C)]
struct Options {
    backend: c_int,
    aux_size: usize,
    data_size: usize,
    snapshot: c_int,
    object_hint: *const c_char,
}

/// Mirrors `asmtest_trace_choice_t` (three int-sized enum fields, no padding).
#[repr(C)]
#[derive(Clone, Copy)]
struct TraceChoice {
    tier: c_int,
    backend: c_int,
    fidelity: c_int,
}

/// Mirrors `asmtest_jitdump_entry_t` (four `u64` fields, no padding) — a JIT
/// method as recorded in a jitdump `JIT_CODE_LOAD` record.
#[repr(C)]
struct JitEntryRaw {
    code_addr: u64,
    code_size: u64,
    timestamp: u64,
    code_index: u64,
}

// --- Resolved entry points (libasmtest_hwtrace) --------------------------- //
//
// Each is `None` when the lib can't be loaded or the symbol is missing, so every
// caller degrades to "unavailable" instead of crashing — matching `asm_fns()` and
// `dr_fns()`.

type AvailableFn = unsafe extern "C" fn(c_int) -> c_int;
type SkipReasonFn = unsafe extern "C" fn(c_int, *mut c_char, usize);
type ResolveFn = unsafe extern "C" fn(c_int, *mut c_int, usize) -> usize;
type AutoFn = unsafe extern "C" fn(c_int) -> c_int;
type TraceResolveFn =
    unsafe extern "C" fn(std::os::raw::c_uint, *mut TraceChoice, usize) -> usize;
type TraceAutoFn = unsafe extern "C" fn(std::os::raw::c_uint, *mut TraceChoice) -> c_int;
type InitFn = unsafe extern "C" fn(*const Options) -> c_int;
type ShutdownFn = unsafe extern "C" fn();
type RegisterRegionFn =
    unsafe extern "C" fn(*const c_char, *mut c_void, usize, *mut c_void) -> c_int;
type MarkerFn = unsafe extern "C" fn(*const c_char);
// Scoped-tracing shared core (§0/§1): error-returning begin + render-on-close.
type TryBeginFn = unsafe extern "C" fn(*const c_char) -> c_int;
type RenderFn = unsafe extern "C" fn(*const c_char, *mut c_char, usize) -> c_int;
type ExecAllocFn =
    unsafe extern "C" fn(*const c_void, usize, *mut *mut c_void, *mut usize) -> c_int;
type ExecFreeFn = unsafe extern "C" fn(*mut c_void, usize);
type TraceNewFn = unsafe extern "C" fn(usize, usize) -> *mut c_void;
type TraceFreeFn = unsafe extern "C" fn(*mut c_void);
type TraceCoveredFn = unsafe extern "C" fn(*mut c_void, u64) -> c_int;
type TraceU64Fn = unsafe extern "C" fn(*mut c_void) -> u64;
type TraceIntFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type TraceAtFn = unsafe extern "C" fn(*mut c_void, usize) -> u64;

// §1 registry-free lazy-arm call + handle-keyed render (the call_scoped path).
// The 8-byte scope handle is passed to render_scope BY VALUE — native #[repr(C)]
// struct-by-value (no packing, unlike the Ruby/Java bindings). `gen` is spelled
// `generation` (a reserved keyword in edition 2024).
#[repr(C)]
#[derive(Clone, Copy)]
struct HwScope {
    idx: u32,
    generation: u32,
}
type CallScopedExFn = unsafe extern "C" fn(
    *mut c_void,   // base
    usize,         // len
    *mut c_void,   // trace (asmtest_trace_t*)
    *mut c_void,   // fn
    *const c_long, // args
    c_int,         // nargs
    *mut c_long,   // result_out
    *mut HwScope,  // scope out
) -> c_int;
type RenderScopeFn = unsafe extern "C" fn(HwScope, *mut c_char, usize) -> c_int;

// --- Out-of-process / foreign-process tracing (asmtest_ptrace.h) ---------- //
//
// The same `libasmtest_hwtrace` already loaded above also ships the out-of-band
// ptrace toolkit: single-step a forked or externally-attached target out of
// band, and resolve the (base,len) to trace from the OS — /proc/<pid>/maps, a
// JIT perf-map, or a binary jitdump. Same resolve-at-run-time idiom; each entry
// is `None` when the symbol is absent so callers self-skip.

type PtraceAvailableFn = unsafe extern "C" fn() -> c_int;
type PtraceSkipReasonFn = unsafe extern "C" fn(*mut c_char, usize);
type PtraceTraceCallFn = unsafe extern "C" fn(
    *const c_void,
    usize,
    *const c_long,
    c_int,
    *mut c_long,
    *mut c_void,
) -> c_int;
type PtraceTraceAttachedFn =
    unsafe extern "C" fn(c_int, *const c_void, usize, *mut c_long, *mut c_void) -> c_int;
type PtraceRunToFn = unsafe extern "C" fn(c_int, *const c_void) -> c_int;
type ProcRegionByAddrFn =
    unsafe extern "C" fn(c_int, *const c_void, *mut *mut c_void, *mut usize) -> c_int;
type ProcPerfmapSymbolFn =
    unsafe extern "C" fn(c_int, *const c_char, *mut *mut c_void, *mut usize) -> c_int;
type JitdumpFindFn = unsafe extern "C" fn(
    *const c_char,
    c_int,
    *const c_char,
    *mut JitEntryRaw,
    *mut u8,
    usize,
    *mut usize,
) -> c_int;
type PtraceTraceAttachedVersionedFn = unsafe extern "C" fn(
    c_int,
    *const c_void,
    usize,
    *mut c_void,
    u64,
    *mut c_long,
    *mut c_void,
) -> c_int;

// --- Call descent (asmtest_descent_t) — asmtest_ptrace.h ------------------ //
//
// Descend into the call-outs the ptrace stepper would otherwise step over, at
// four opt-in levels ([`DescentLevel`]). Descent records into a SEPARATE opaque
// handle read through the scalar accessors below; the flat `asmtest_trace_t`
// stays the single-region frame-0 view. Same `libasmtest_hwtrace`, same
// resolve-at-run-time idiom; each entry is `None` when the symbol is absent so
// callers self-skip.
//
// This binding is ALLOW-SET-ONLY: it wraps `allow_region`/`deny_region` but NOT
// the two capturing-upcall setters (`set_resolver`/`set_denylist`) — a Rust
// `extern "C"` fn pointer cannot host a GC-safe capturing closure, so those two
// symbols are exempted in `scripts/bindings-parity-allow.txt` rather than wrapped.

type DescentNewFn = unsafe extern "C" fn(c_int) -> *mut c_void;
type DescentFreeFn = unsafe extern "C" fn(*mut c_void);
type DescentSetU32Fn = unsafe extern "C" fn(*mut c_void, u32);
type DescentSetU64Fn = unsafe extern "C" fn(*mut c_void, u64);
type DescentRegionFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> c_int;
type DescentLenFn = unsafe extern "C" fn(*mut c_void) -> usize;
type DescentAtU64Fn = unsafe extern "C" fn(*mut c_void, usize) -> u64;
type DescentAtU32Fn = unsafe extern "C" fn(*mut c_void, usize) -> u32;
type DescentAtI32Fn = unsafe extern "C" fn(*mut c_void, usize) -> i32;
type DescentCountFn = unsafe extern "C" fn(*mut c_void, usize) -> usize;
type DescentAt2Fn = unsafe extern "C" fn(*mut c_void, usize, usize) -> u64;
type DescentFlagFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type PtraceTraceCallExFn = unsafe extern "C" fn(
    *const c_void,
    usize,
    *const c_long,
    c_int,
    *mut c_long,
    *mut c_void,
    *mut c_void,
) -> c_int;
type PtraceTraceAttachedExFn = unsafe extern "C" fn(
    c_int,
    *const c_void,
    usize,
    *mut c_long,
    *mut c_void,
    *mut c_void,
) -> c_int;
type PtraceTraceAttachedVersionedExFn = unsafe extern "C" fn(
    c_int,
    *const c_void,
    usize,
    *mut c_void,
    u64,
    *mut c_long,
    *mut c_void,
    *mut c_void,
) -> c_int;

// --- Time-aware code-image recorder (asmtest_codeimage.h) ----------------- //
//
// The userspace PERF_RECORD_TEXT_POKE: a timestamped code-image timeline that
// snapshots a (possibly foreign) region's bytes and re-snapshots only the pages
// that changed, so a branch-trace decoder can ask "what bytes were live at addr
// as of sequence `when`". Same `libasmtest_hwtrace`, same resolve-at-run-time
// idiom; each entry is `None` when the symbol is absent so callers self-skip.

type CodeImageAvailableFn = unsafe extern "C" fn() -> c_int;
type CodeImageSkipReasonFn = unsafe extern "C" fn(*mut c_char, usize);
type CodeImageNewFn = unsafe extern "C" fn(c_int) -> *mut c_void;
type CodeImageFreeFn = unsafe extern "C" fn(*mut c_void);
type CodeImageTrackFn = unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> c_int;
type CodeImageRefreshFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type CodeImageNowFn = unsafe extern "C" fn(*mut c_void) -> u64;
type CodeImageBytesAtFn = unsafe extern "C" fn(
    *mut c_void,
    *const c_void,
    u64,
    *mut *const u8,
    *mut usize,
) -> c_int;
type CodeImageBpfAvailableFn = unsafe extern "C" fn() -> c_int;
type CodeImageBpfSkipReasonFn = unsafe extern "C" fn(*mut c_char, usize);
type CodeImageWatchBpfFn = unsafe extern "C" fn(*mut c_void) -> c_int;
type CodeImagePollBpfFn = unsafe extern "C" fn(*mut c_void, c_int) -> c_int;
type CodeImageNextFn = unsafe extern "C" fn(*mut c_void, *mut CodeEventRaw) -> c_int;

/// Mirrors `asmtest_codeimage_event_t` (40 bytes, no padding) — a code-emission
/// event from the optional eBPF detector.
#[repr(C)]
struct CodeEventRaw {
    addr: u64,
    len: u64,
    timestamp: u64,
    pid: u32,
    tid: u32,
    kind: u32,
    fd: i32,
}

struct HwFns {
    available: Option<AvailableFn>,
    skip_reason: Option<SkipReasonFn>,
    resolve: Option<ResolveFn>,
    auto: Option<AutoFn>,
    trace_resolve: Option<TraceResolveFn>,
    trace_auto: Option<TraceAutoFn>,
    init: Option<InitFn>,
    shutdown: Option<ShutdownFn>,
    register_region: Option<RegisterRegionFn>,
    begin: Option<MarkerFn>,
    end: Option<MarkerFn>,
    try_begin: Option<TryBeginFn>,
    render: Option<RenderFn>,
    call_scoped_ex: Option<CallScopedExFn>,
    render_scope: Option<RenderScopeFn>,
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
    // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
    ptrace_available: Option<PtraceAvailableFn>,
    ptrace_skip_reason: Option<PtraceSkipReasonFn>,
    ptrace_trace_call: Option<PtraceTraceCallFn>,
    // BTF block-step tier: same shapes as the per-instruction trio above.
    ptrace_blockstep_available: Option<PtraceAvailableFn>,
    ptrace_trace_call_blockstep: Option<PtraceTraceCallFn>,
    ptrace_trace_attached_blockstep: Option<PtraceTraceAttachedFn>,
    ptrace_trace_attached: Option<PtraceTraceAttachedFn>,
    ptrace_run_to: Option<PtraceRunToFn>,
    ptrace_trace_attached_versioned: Option<PtraceTraceAttachedVersionedFn>,
    proc_region_by_addr: Option<ProcRegionByAddrFn>,
    proc_perfmap_symbol: Option<ProcPerfmapSymbolFn>,
    jitdump_find: Option<JitdumpFindFn>,
    // asmtest_ptrace.h — call descent (asmtest_descent_t): edges + nested frames.
    // Allow-set-only: no set_resolver/set_denylist (see the alias block above).
    descent_new: Option<DescentNewFn>,
    descent_free: Option<DescentFreeFn>,
    descent_set_max_depth: Option<DescentSetU32Fn>,
    descent_set_insn_budget: Option<DescentSetU64Fn>,
    descent_set_watchdog_ms: Option<DescentSetU32Fn>,
    descent_use_default_denylist: Option<DescentFreeFn>,
    descent_allow_region: Option<DescentRegionFn>,
    descent_deny_region: Option<DescentRegionFn>,
    descent_edges_len: Option<DescentLenFn>,
    descent_edge_site: Option<DescentAtU64Fn>,
    descent_edge_target: Option<DescentAtU64Fn>,
    descent_edge_depth: Option<DescentAtU32Fn>,
    descent_frames_len: Option<DescentLenFn>,
    descent_frame_base: Option<DescentAtU64Fn>,
    descent_frame_len: Option<DescentAtU64Fn>,
    descent_frame_depth: Option<DescentAtU32Fn>,
    descent_frame_parent: Option<DescentAtI32Fn>,
    descent_frame_insn_count: Option<DescentCountFn>,
    descent_frame_insn_at: Option<DescentAt2Fn>,
    descent_frame_block_count: Option<DescentCountFn>,
    descent_frame_block_at: Option<DescentAt2Fn>,
    descent_truncated: Option<DescentFlagFn>,
    descent_depth_capped: Option<DescentFlagFn>,
    ptrace_trace_call_ex: Option<PtraceTraceCallExFn>,
    ptrace_trace_attached_ex: Option<PtraceTraceAttachedExFn>,
    ptrace_trace_attached_versioned_ex: Option<PtraceTraceAttachedVersionedExFn>,
    // asmtest_codeimage.h — time-aware code-image recorder.
    ci_available: Option<CodeImageAvailableFn>,
    ci_skip_reason: Option<CodeImageSkipReasonFn>,
    ci_new: Option<CodeImageNewFn>,
    ci_free: Option<CodeImageFreeFn>,
    ci_track: Option<CodeImageTrackFn>,
    ci_refresh: Option<CodeImageRefreshFn>,
    ci_now: Option<CodeImageNowFn>,
    ci_bytes_at: Option<CodeImageBytesAtFn>,
    ci_bpf_available: Option<CodeImageBpfAvailableFn>,
    ci_bpf_skip_reason: Option<CodeImageBpfSkipReasonFn>,
    ci_watch_bpf: Option<CodeImageWatchBpfFn>,
    ci_poll_bpf: Option<CodeImagePollBpfFn>,
    ci_next: Option<CodeImageNextFn>,
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

/// The candidate string that satisfied the last successful [`open_lib`], captured
/// so [`HwTrace::library_path`] can report which one the loader used.
static RESOLVED_PATH: OnceLock<Option<String>> = OnceLock::new();

/// dlopen `libasmtest_hwtrace` from `ASMTEST_HWTRACE_LIB`, else `<repo>/build/<lib>`,
/// else the bare name (let the loader search). Returns a null handle if none load
/// — the resolver then leaves every pointer `None`. Records the winning candidate
/// string in `RESOLVED_PATH` (env override first, else the build/ default), so the
/// self-report reflects the exact search order without changing it.
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
        if let Ok(cs) = CString::new(c.clone()) {
            let h = unsafe { dlopen(cs.as_ptr(), RTLD_NOW) };
            if !h.is_null() {
                let _ = RESOLVED_PATH.set(Some(c));
                return h;
            }
        }
    }
    let _ = RESOLVED_PATH.set(None);
    std::ptr::null_mut()
}

fn hw_fns() -> &'static HwFns {
    static FNS: OnceLock<HwFns> = OnceLock::new();
    FNS.get_or_init(|| {
        let handle = open_lib();
        if handle.is_null() {
            // No lib: every pointer stays None, available() returns false.
            return HwFns {
                available: None, skip_reason: None, resolve: None, auto: None,
                trace_resolve: None, trace_auto: None,
                init: None, shutdown: None,
                register_region: None, begin: None, end: None,
                try_begin: None, render: None,
                call_scoped_ex: None, render_scope: None,
                exec_alloc: None, exec_free: None,
                trace_new: None, trace_free: None, trace_covered: None,
                blocks_len: None, insns_total: None, insns_len: None,
                truncated: None, block_at: None, insn_at: None,
                ptrace_available: None, ptrace_skip_reason: None,
                ptrace_trace_call: None, ptrace_trace_attached: None,
                ptrace_blockstep_available: None, ptrace_trace_call_blockstep: None,
                ptrace_trace_attached_blockstep: None,
                ptrace_run_to: None, ptrace_trace_attached_versioned: None,
                proc_region_by_addr: None, proc_perfmap_symbol: None,
                jitdump_find: None,
                descent_new: None, descent_free: None,
                descent_set_max_depth: None, descent_set_insn_budget: None,
                descent_set_watchdog_ms: None, descent_allow_region: None,
                descent_use_default_denylist: None,
                descent_deny_region: None, descent_edges_len: None,
                descent_edge_site: None, descent_edge_target: None,
                descent_edge_depth: None, descent_frames_len: None,
                descent_frame_base: None, descent_frame_len: None,
                descent_frame_depth: None, descent_frame_parent: None,
                descent_frame_insn_count: None, descent_frame_insn_at: None,
                descent_frame_block_count: None, descent_frame_block_at: None,
                descent_truncated: None, descent_depth_capped: None,
                ptrace_trace_call_ex: None, ptrace_trace_attached_ex: None,
                ptrace_trace_attached_versioned_ex: None,
                ci_available: None, ci_skip_reason: None,
                ci_new: None, ci_free: None, ci_track: None,
                ci_refresh: None, ci_now: None, ci_bytes_at: None,
                ci_bpf_available: None, ci_bpf_skip_reason: None,
                ci_watch_bpf: None, ci_poll_bpf: None, ci_next: None,
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
            resolve: load!("asmtest_hwtrace_resolve", ResolveFn),
            auto: load!("asmtest_hwtrace_auto", AutoFn),
            trace_resolve: load!("asmtest_trace_resolve", TraceResolveFn),
            trace_auto: load!("asmtest_trace_auto", TraceAutoFn),
            init: load!("asmtest_hwtrace_init", InitFn),
            shutdown: load!("asmtest_hwtrace_shutdown", ShutdownFn),
            register_region: load!("asmtest_hwtrace_register_region", RegisterRegionFn),
            begin: load!("asmtest_hwtrace_begin", MarkerFn),
            end: load!("asmtest_hwtrace_end", MarkerFn),
            try_begin: load!("asmtest_hwtrace_try_begin", TryBeginFn),
            render: load!("asmtest_hwtrace_render", RenderFn),
            call_scoped_ex: load!("asmtest_hwtrace_call_scoped_ex", CallScopedExFn),
            render_scope: load!("asmtest_hwtrace_render_scope", RenderScopeFn),
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
            ptrace_available: load!("asmtest_ptrace_available", PtraceAvailableFn),
            ptrace_skip_reason: load!("asmtest_ptrace_skip_reason", PtraceSkipReasonFn),
            ptrace_trace_call: load!("asmtest_ptrace_trace_call", PtraceTraceCallFn),
            ptrace_blockstep_available: load!("asmtest_ptrace_blockstep_available", PtraceAvailableFn),
            ptrace_trace_call_blockstep: load!("asmtest_ptrace_trace_call_blockstep", PtraceTraceCallFn),
            ptrace_trace_attached_blockstep: load!("asmtest_ptrace_trace_attached_blockstep", PtraceTraceAttachedFn),
            ptrace_trace_attached: load!("asmtest_ptrace_trace_attached", PtraceTraceAttachedFn),
            ptrace_run_to: load!("asmtest_ptrace_run_to", PtraceRunToFn),
            ptrace_trace_attached_versioned: load!(
                "asmtest_ptrace_trace_attached_versioned",
                PtraceTraceAttachedVersionedFn
            ),
            proc_region_by_addr: load!("asmtest_proc_region_by_addr", ProcRegionByAddrFn),
            proc_perfmap_symbol: load!("asmtest_proc_perfmap_symbol", ProcPerfmapSymbolFn),
            jitdump_find: load!("asmtest_jitdump_find", JitdumpFindFn),
            // Call descent (asmtest_descent_t) — allow-set-only: set_resolver /
            // set_denylist are intentionally NOT loaded (see the alias block).
            descent_new: load!("asmtest_descent_new", DescentNewFn),
            descent_free: load!("asmtest_descent_free", DescentFreeFn),
            descent_set_max_depth: load!("asmtest_descent_set_max_depth", DescentSetU32Fn),
            descent_set_insn_budget: load!("asmtest_descent_set_insn_budget", DescentSetU64Fn),
            descent_set_watchdog_ms: load!("asmtest_descent_set_watchdog_ms", DescentSetU32Fn),
            descent_use_default_denylist: load!("asmtest_descent_use_default_denylist", DescentFreeFn),
            descent_allow_region: load!("asmtest_descent_allow_region", DescentRegionFn),
            descent_deny_region: load!("asmtest_descent_deny_region", DescentRegionFn),
            descent_edges_len: load!("asmtest_descent_edges_len", DescentLenFn),
            descent_edge_site: load!("asmtest_descent_edge_site", DescentAtU64Fn),
            descent_edge_target: load!("asmtest_descent_edge_target", DescentAtU64Fn),
            descent_edge_depth: load!("asmtest_descent_edge_depth", DescentAtU32Fn),
            descent_frames_len: load!("asmtest_descent_frames_len", DescentLenFn),
            descent_frame_base: load!("asmtest_descent_frame_base", DescentAtU64Fn),
            descent_frame_len: load!("asmtest_descent_frame_len", DescentAtU64Fn),
            descent_frame_depth: load!("asmtest_descent_frame_depth", DescentAtU32Fn),
            descent_frame_parent: load!("asmtest_descent_frame_parent", DescentAtI32Fn),
            descent_frame_insn_count: load!("asmtest_descent_frame_insn_count", DescentCountFn),
            descent_frame_insn_at: load!("asmtest_descent_frame_insn_at", DescentAt2Fn),
            descent_frame_block_count: load!("asmtest_descent_frame_block_count", DescentCountFn),
            descent_frame_block_at: load!("asmtest_descent_frame_block_at", DescentAt2Fn),
            descent_truncated: load!("asmtest_descent_truncated", DescentFlagFn),
            descent_depth_capped: load!("asmtest_descent_depth_capped", DescentFlagFn),
            ptrace_trace_call_ex: load!("asmtest_ptrace_trace_call_ex", PtraceTraceCallExFn),
            ptrace_trace_attached_ex: load!(
                "asmtest_ptrace_trace_attached_ex",
                PtraceTraceAttachedExFn
            ),
            ptrace_trace_attached_versioned_ex: load!(
                "asmtest_ptrace_trace_attached_versioned_ex",
                PtraceTraceAttachedVersionedExFn
            ),
            ci_available: load!("asmtest_codeimage_available", CodeImageAvailableFn),
            ci_skip_reason: load!("asmtest_codeimage_skip_reason", CodeImageSkipReasonFn),
            ci_new: load!("asmtest_codeimage_new", CodeImageNewFn),
            ci_free: load!("asmtest_codeimage_free", CodeImageFreeFn),
            ci_track: load!("asmtest_codeimage_track", CodeImageTrackFn),
            ci_refresh: load!("asmtest_codeimage_refresh", CodeImageRefreshFn),
            ci_now: load!("asmtest_codeimage_now", CodeImageNowFn),
            ci_bytes_at: load!("asmtest_codeimage_bytes_at", CodeImageBytesAtFn),
            ci_bpf_available: load!("asmtest_codeimage_bpf_available", CodeImageBpfAvailableFn),
            ci_bpf_skip_reason: load!(
                "asmtest_codeimage_bpf_skip_reason",
                CodeImageBpfSkipReasonFn
            ),
            ci_watch_bpf: load!("asmtest_codeimage_watch_bpf", CodeImageWatchBpfFn),
            ci_poll_bpf: load!("asmtest_codeimage_poll_bpf", CodeImagePollBpfFn),
            ci_next: load!("asmtest_codeimage_next", CodeImageNextFn),
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

/// Build a call-site region name `basename:line` (basename dodges the 64-char C
/// name ceiling and full-path aliasing under Core §0.4's by-name registry).
fn scope_name(file: &str, line: u32) -> String {
    let base = file.rsplit('/').next().unwrap_or(file);
    let n = format!("{base}:{line}");
    if n.len() > 63 {
        // ASCII path:line, so a byte slice of the tail is on a char boundary.
        n[n.len() - 63..].to_string()
    } else {
        n
    }
}

/// The outcome of [`HwTrace::call_scoped`]: the traced call's return value
/// (`result`, `None` on a self-skip), the executed body's disassembly (`path`,
/// empty when no decoder is present), the thread-scope honesty bit (`truncated`),
/// and the raw `ASMTEST_HW_*` status (`rc`, `0` == OK). Mirrors the Python/Ruby
/// `CallScopedResult`.
#[derive(Debug, Clone)]
pub struct CallScopedResult {
    /// The call's return value (SysV integer ABI), or `None` on a self-skip.
    pub result: Option<i64>,
    /// The executed body disassembly (empty when the decoder is absent).
    pub path: String,
    /// True if the capture overflowed or the scope closed on another thread.
    pub truncated: bool,
    /// The `ASMTEST_HW_*` status (`0` == OK).
    pub rc: i32,
}

/// A scoped trace over the register-then-begin/end pair with the shared-core
/// render-on-close. Constructed by [`HwTrace::scope`]: it auto-names from the call
/// site via `#[track_caller]` + [`Location::caller`](std::panic::Location::caller),
/// brackets `try_begin`/`end` (a nonzero `try_begin` is a clean self-skip), and on
/// close renders the executed assembly. Call [`close`](ScopedTrace::close) to end the
/// scope and take the rendered listing; otherwise `Drop` ends it (emitting to stdout
/// when `emit`). The C core flags the trace `truncated` on a cross-thread close
/// (§0.2/§1), surfaced via [`truncated`](ScopedTrace::truncated).
pub struct ScopedTrace {
    name: CString,
    handle: *mut c_void,
    emit: bool,
    active: bool,
    closed: bool,
    truncated: bool,
    path: String,
}

impl ScopedTrace {
    fn with_name(name: &str, code: &NativeCode, emit: bool) -> ScopedTrace {
        let cname = CString::new(name).expect("scope name has interior NUL");
        let f = hw_fns();
        let handle = f
            .trace_new
            .map(|g| unsafe { g(256, 64) })
            .unwrap_or(std::ptr::null_mut());
        if let Some(reg) = f.register_region {
            unsafe {
                reg(cname.as_ptr(), code.base() as *mut c_void, code.len(), handle);
            }
        }
        let active = if let Some(tb) = f.try_begin {
            unsafe { tb(cname.as_ptr()) == 0 } // nonzero -> clean self-skip
        } else if let Some(b) = f.begin {
            unsafe { b(cname.as_ptr()) };
            true
        } else {
            false
        };
        ScopedTrace { name: cname, handle, emit, active, closed: false, truncated: false, path: String::new() }
    }

    /// True if the scope armed (a backend was available and `try_begin` succeeded).
    pub fn armed(&self) -> bool {
        self.active
    }

    /// The auto-generated (or explicit) region name.
    pub fn name(&self) -> &str {
        self.name.to_str().unwrap_or("")
    }

    /// True if the close hopped OS threads / the capture overflowed (Core §0.2/§1).
    /// Meaningful after [`close`](ScopedTrace::close) (or once dropped).
    pub fn was_truncated(&self) -> bool {
        self.truncated
    }

    /// End the scope, render the executed assembly, and return the listing.
    pub fn close(mut self) -> String {
        self.finish();
        std::mem::take(&mut self.path)
    }

    fn finish(&mut self) {
        if self.closed {
            return;
        }
        self.closed = true;
        let f = hw_fns();
        if let Some(end) = f.end {
            unsafe { end(self.name.as_ptr()) };
        }
        if let Some(render) = f.render {
            let need = unsafe { render(self.name.as_ptr(), std::ptr::null_mut(), 0) };
            if need > 0 {
                let mut buf = vec![0u8; need as usize + 1];
                unsafe {
                    render(self.name.as_ptr(), buf.as_mut_ptr() as *mut c_char, buf.len());
                }
                self.path = String::from_utf8_lossy(&buf[..need as usize]).into_owned();
            }
        }
        if let Some(tr) = f.truncated {
            if !self.handle.is_null() {
                self.truncated = unsafe { tr(self.handle) != 0 };
            }
        }
        if self.emit && !self.path.is_empty() {
            print!("{}", self.path);
        }
        if let Some(free) = f.trace_free {
            if !self.handle.is_null() {
                unsafe { free(self.handle) };
                self.handle = std::ptr::null_mut();
            }
        }
    }
}

impl Drop for ScopedTrace {
    fn drop(&mut self) {
        self.finish();
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

    /// The `libasmtest_hwtrace` candidate string this process actually dlopen()ed —
    /// the `ASMTEST_HWTRACE_LIB` override if set, else the `<repo>/build/` default
    /// (the resolver's search order, env-override first). Returns `None` when no
    /// candidate loaded. Counterpart of the Python wrapper's
    /// `hwtrace.library_path()`, letting a clean-room test assert which candidate
    /// satisfied the load.
    ///
    /// NOTE: this binding is a **source distribution** — the consumer builds/links
    /// libasmtest themselves, so there is no bundled `native/` directory to point
    /// at. The value is the exact string handed to `dlopen`, resolved at RUN TIME
    /// (this tier is dlopen-based, not one of the crate's link-time core libs).
    pub fn library_path() -> Option<String> {
        // Ensure the loader has run so RESOLVED_PATH is populated, then report it.
        let _ = hw_fns();
        RESOLVED_PATH.get().cloned().flatten()
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

    /// This host's hardware-trace fallback cascade: the available backends (as their
    /// `asmtest_trace_backend_t` enum values), most-faithful first
    /// (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring `policy`. Empty only
    /// off x86-64 Linux (single-step is the floor there) or when the lib is absent.
    /// `CeilingFree` drops the ceiling-bounded backend (AMD LBR).
    pub fn resolve(policy: Policy) -> Vec<i32> {
        match hw_fns().resolve {
            Some(f) => {
                let mut out = [0i32; 4];
                let n = unsafe { f(policy as c_int, out.as_mut_ptr(), out.len()) };
                out[..n].to_vec()
            }
            None => Vec::new(),
        }
    }

    /// The single most-preferred available backend under `policy` (a backend enum
    /// `>= 0`, ready to [`init`](HwTrace::init)), or [`ASMTEST_HW_EUNAVAIL`] (`< 0`)
    /// when no hardware-trace backend is available on this host. NOTE: `auto` is not
    /// a Rust keyword, so this keeps the C name.
    pub fn auto(policy: Policy) -> i32 {
        match hw_fns().auto {
            Some(f) => unsafe { f(policy as c_int) },
            None => ASMTEST_HW_EUNAVAIL,
        }
    }

    /// This host's full CROSS-TIER fallback cascade (`asmtest_trace_resolve`),
    /// most-faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step ->
    /// CoreSight -> emulator, each included only if its tier is available, honoring
    /// `policy`. Returns a [`TierChoice`] per option. [`TracePolicy::NativeOnly`]
    /// drops the emulator floor (no native->emulator fidelity crossing);
    /// [`TracePolicy::CeilingFree`] drops AMD LBR. Empty when the lib is absent.
    pub fn resolve_tiers(policy: TracePolicy) -> Vec<TierChoice> {
        match hw_fns().trace_resolve {
            Some(f) => {
                let mut out = [TraceChoice { tier: 0, backend: 0, fidelity: 0 }; 8];
                let n = unsafe {
                    f(policy as std::os::raw::c_uint, out.as_mut_ptr(), out.len())
                };
                out[..n].iter().map(|c| tier_choice_of(*c)).collect()
            }
            None => Vec::new(),
        }
    }

    /// The single most-preferred available cross-tier choice under `policy`
    /// (`asmtest_trace_auto`) as a [`TierChoice`], or `None` when the cascade is
    /// empty (only off a native host under [`TracePolicy::NativeOnly`]) or the lib
    /// is absent. NOTE: `auto` is not a Rust keyword, but this keeps `_tier` to
    /// distinguish it from the hardware-tier [`auto`](HwTrace::auto).
    pub fn auto_tier(policy: TracePolicy) -> Option<TierChoice> {
        let f = hw_fns().trace_auto?;
        let mut out = TraceChoice { tier: 0, backend: 0, fidelity: 0 };
        let rc = unsafe { f(policy as std::os::raw::c_uint, &mut out) };
        if rc != ASMTEST_HW_OK {
            return None;
        }
        Some(tier_choice_of(out))
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

    /// A scoped trace around a traced native region — the *import + scope* surface.
    /// `code` is the region being traced (registered under a `basename:line`
    /// call-site name captured via `#[track_caller]`); the returned [`ScopedTrace`]
    /// renders the executed assembly on close. Requires the tier to be up
    /// ([`init`](HwTrace::init)); self-skips cleanly where no backend is available.
    #[track_caller]
    pub fn scope(code: &NativeCode, emit: bool) -> ScopedTrace {
        let loc = std::panic::Location::caller();
        let name = scope_name(loc.file(), loc.line());
        ScopedTrace::with_name(&name, code, emit)
    }

    /// Trace ONE native call the managed-safe way: arm the single-step window,
    /// call `code(args…)` through the SysV integer ABI, and disarm — all in native
    /// code (`asmtest_hwtrace_call_scoped_ex`), a tighter window than [`scope`] (its
    /// FFI dispatch of `code.call` is stepped, though region-filtered). REGISTRY-FREE
    /// — consumes no `MAX_REGIONS` slot — so it is safe in a tight loop. Integer args
    /// (0-6). Requires the tier to be up ([`init`](HwTrace::init)); self-skips
    /// (`result` `None`, negative `rc`) where no single-step backend is available.
    pub fn call_scoped(code: &NativeCode, args: &[i64]) -> CallScopedResult {
        let f = hw_fns();
        let (Some(call), Some(new), Some(free)) = (f.call_scoped_ex, f.trace_new, f.trace_free)
        else {
            return CallScopedResult { result: None, path: String::new(), truncated: false, rc: ASMTEST_HW_EUNAVAIL };
        };
        let handle = unsafe { new(256, 64) }; // insns=256, blocks=64
        if handle.is_null() {
            return CallScopedResult { result: None, path: String::new(), truncated: false, rc: ASMTEST_HW_EUNAVAIL };
        }
        // `long` on Linux x86-64 is i64; args ride through as `*const c_long`.
        let argv: Vec<c_long> = args.iter().map(|&a| a as c_long).collect();
        let argp = if argv.is_empty() { std::ptr::null() } else { argv.as_ptr() };
        let mut result: c_long = 0;
        let mut scope = HwScope { idx: 0, generation: 0 };
        let rc = unsafe {
            call(
                code.base() as *mut c_void,
                code.len(),
                handle,
                code.base() as *mut c_void,
                argp,
                argv.len() as c_int,
                &mut result,
                &mut scope,
            )
        };
        if rc != ASMTEST_HW_OK {
            unsafe { free(handle) };
            return CallScopedResult { result: None, path: String::new(), truncated: false, rc: rc as i32 };
        }
        let mut path = String::new();
        if let Some(render) = f.render_scope {
            let need = unsafe { render(scope, std::ptr::null_mut(), 0) };
            if need > 0 {
                let mut buf = vec![0u8; need as usize + 1];
                unsafe { render(scope, buf.as_mut_ptr() as *mut c_char, buf.len()) };
                path = String::from_utf8_lossy(&buf[..need as usize]).into_owned();
            }
        }
        let truncated = f.truncated.map(|t| unsafe { t(handle) } != 0).unwrap_or(false);
        unsafe { free(handle) };
        CallScopedResult { result: Some(result as i64), path, truncated, rc: rc as i32 }
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

/// `ASMTEST_PTRACE_OK` from `asmtest_ptrace.h`; nonzero is an error.
const ASMTEST_PTRACE_OK: c_int = 0;
/// `ASMTEST_PTRACE_ENOENT` from `asmtest_ptrace.h`: region / symbol / method not
/// found (the `None`-returning resolve path).
pub const ASMTEST_PTRACE_ENOENT: c_int = -7;

/// A JIT method resolved from a jitdump (the safe wrapper over
/// `asmtest_jitdump_entry_t`): its load address, byte size, the JIT's
/// timestamp/index, and — unlike the text perf-map — optionally the recorded
/// native code bytes.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct JitMethod {
    /// Load address (the base to trace).
    pub code_addr: u64,
    /// Code length in bytes.
    pub code_size: u64,
    /// Record timestamp (load order; the latest re-JIT wins).
    pub timestamp: u64,
    /// The JIT's unique index for this code.
    pub code_index: u64,
    /// The recorded native code bytes, when requested (else empty).
    pub code: Vec<u8>,
}

/// Out-of-process / foreign-process tracing (`asmtest_ptrace.h`): single-step a
/// forked or externally-attached target out of band, and resolve the code region
/// to trace from the OS — `/proc/<pid>/maps`, a JIT perf-map, or a binary
/// jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is
/// unavailable and in-process DynamoRIO cannot seize the runtime's threads).
/// Linux x86-64.
///
/// Wraps the same `libasmtest_hwtrace` the [`HwTrace`] tier loads, so the same
/// self-skip applies: gate on [`Ptrace::available`] (it is `false` when the lib
/// or the backend is absent) before tracing.
pub struct Ptrace;

impl Ptrace {
    /// True if the out-of-process single-step tracer can run on this host (the
    /// lib loaded **and** `asmtest_ptrace_available()` reports a Linux x86-64
    /// host). Never panics, so callers (and the test) self-skip cleanly.
    pub fn available() -> bool {
        match hw_fns().ptrace_available {
            Some(f) => unsafe { f() != 0 },
            None => false,
        }
    }

    /// A human-readable reason [`available`](Ptrace::available) is false. Empty
    /// string when the lib is absent.
    pub fn skip_reason() -> String {
        match hw_fns().ptrace_skip_reason {
            Some(f) => {
                let mut buf = [0u8; 160];
                unsafe {
                    f(buf.as_mut_ptr() as *mut c_char, buf.len());
                    std::ffi::CStr::from_ptr(buf.as_ptr() as *const c_char)
                        .to_string_lossy()
                        .into_owned()
                }
            }
            None => String::new(),
        }
    }

    /// Fork a tracee that calls `code` (a [`NativeCode`] range) with up to six
    /// integer `args`, single-step it OUT OF PROCESS, and fill `trace`; returns
    /// the routine's return value (the child's RAX at the `ret`). Panics on a
    /// fork/ptrace failure or when the lib is absent — gate on
    /// [`available`](Ptrace::available) first.
    pub fn trace_call(code: &NativeCode, args: &[i64], trace: &Trace) -> i64 {
        let f = hw_fns()
            .ptrace_trace_call
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let cargs: Vec<c_long> = args.iter().map(|&a| a as c_long).collect();
        let mut result: c_long = 0;
        let rc = unsafe {
            f(
                code.base,
                code.len,
                cargs.as_ptr(),
                cargs.len() as c_int,
                &mut result,
                trace.handle,
            )
        };
        assert!(rc == ASMTEST_PTRACE_OK, "asmtest_ptrace_trace_call failed: {rc}");
        result as i64
    }

    /// True if the BTF block-step variant (`PTRACE_SINGLEBLOCK` — one `#DB` per
    /// TAKEN branch instead of one per instruction) can run here: x86-64 Linux
    /// with a functional `PTRACE_SINGLEBLOCK` and Capstone for the intra-block
    /// reconstruction. Hang-proof, cached probe; callers self-skip on `false`.
    pub fn blockstep_available() -> bool {
        match hw_fns().ptrace_blockstep_available {
            Some(f) => unsafe { f() != 0 },
            None => false,
        }
    }

    /// Block-step variant of [`trace_call`](Ptrace::trace_call): drives
    /// `PTRACE_SINGLEBLOCK` (`DEBUGCTL.BTF`), stopping once per TAKEN branch and
    /// reconstructing the intra-block instructions with Capstone — the same
    /// insns/blocks stream as `trace_call` at a fraction of the stops. Probe
    /// first with [`blockstep_available`](Ptrace::blockstep_available). Complete
    /// at moderate overhead, NOT cheap: each block still costs a full ptrace
    /// round-trip. Panics on a failure or when the lib is absent.
    pub fn trace_call_blockstep(code: &NativeCode, args: &[i64], trace: &Trace) -> i64 {
        let f = hw_fns()
            .ptrace_trace_call_blockstep
            .expect("libasmtest_hwtrace not loaded (Ptrace::blockstep_available() is false)");
        let cargs: Vec<c_long> = args.iter().map(|&a| a as c_long).collect();
        let mut result: c_long = 0;
        let rc = unsafe {
            f(
                code.base,
                code.len,
                cargs.as_ptr(),
                cargs.len() as c_int,
                &mut result,
                trace.handle,
            )
        };
        assert!(
            rc == ASMTEST_PTRACE_OK,
            "asmtest_ptrace_trace_call_blockstep failed: {rc}"
        );
        result as i64
    }

    /// Trace a region `[base, base+len)` in a SEPARATE, already-ptrace-stopped
    /// process `pid` (the caller owns PTRACE_ATTACH/DETACH); reads the target's
    /// bytes via `process_vm_readv` and returns the region's return value (the
    /// target's RAX at the `ret`). Panics on a ptrace failure or when the lib is
    /// absent — gate on [`available`](Ptrace::available) first.
    pub fn trace_attached(pid: i32, base: usize, len: usize, trace: &Trace) -> i64 {
        let f = hw_fns()
            .ptrace_trace_attached
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let mut result: c_long = 0;
        let rc = unsafe {
            f(pid as c_int, base as *const c_void, len, &mut result, trace.handle)
        };
        assert!(rc == ASMTEST_PTRACE_OK, "asmtest_ptrace_trace_attached failed: {rc}");
        result as i64
    }

    /// Block-step variant of [`trace_attached`](Ptrace::trace_attached): one `#DB`
    /// per TAKEN branch (intra-block instructions reconstructed with Capstone), same
    /// contract otherwise — the rootless managed-runtime completeness fallback at a
    /// fraction of the stops. Probe first with
    /// [`blockstep_available`](Ptrace::blockstep_available).
    pub fn trace_attached_blockstep(pid: i32, base: usize, len: usize, trace: &Trace) -> i64 {
        let f = hw_fns()
            .ptrace_trace_attached_blockstep
            .expect("libasmtest_hwtrace not loaded (Ptrace::blockstep_available() is false)");
        let mut result: c_long = 0;
        let rc = unsafe {
            f(pid as c_int, base as *const c_void, len, &mut result, trace.handle)
        };
        assert!(
            rc == ASMTEST_PTRACE_OK,
            "asmtest_ptrace_trace_attached_blockstep failed: {rc}"
        );
        result as i64
    }

    /// Like [`trace_attached`](Ptrace::trace_attached), but decode the region
    /// against TIME-CORRECT bytes from a [`CodeImage`] recorder instead of a single
    /// live `process_vm_readv` snapshot: for a JIT whose code at `base` was patched,
    /// freed, or had its address reused during the run, passing the recorder plus the
    /// logical timestamp `when` (`0` = latest) the region was live at makes block
    /// normalization use the bytes that were actually executing. `img` must already
    /// be tracking a region covering `[base, base+len)`
    /// ([`CodeImage::track`]); the call returns
    /// [`ASMTEST_PTRACE_ENOENT`] otherwise. Returns the routine's return value (the
    /// target's RAX at the `ret`). Panics on a ptrace failure or when the lib is
    /// absent — gate on [`available`](Ptrace::available) first.
    pub fn trace_attached_versioned(
        pid: i32,
        base: usize,
        len: usize,
        img: &CodeImage,
        when: u64,
        trace: &Trace,
    ) -> i64 {
        let f = hw_fns()
            .ptrace_trace_attached_versioned
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let mut result: c_long = 0;
        let rc = unsafe {
            f(
                pid as c_int,
                base as *const c_void,
                len,
                img.handle,
                when,
                &mut result,
                trace.handle,
            )
        };
        assert!(
            rc == ASMTEST_PTRACE_OK,
            "asmtest_ptrace_trace_attached_versioned failed: {rc}"
        );
        result as i64
    }

    /// Descending variant of [`trace_call`](Ptrace::trace_call): thread a
    /// [`Descent`] handle through the single-step loop so the call-outs the tracer
    /// would step over are recorded as edges (level >= 1) and descended as nested
    /// frames (level >= 2). `trace` (the flat frame-0 view) may be `None` to record
    /// only into `descent`.
    ///
    /// CRITICAL: `region` is the traced region's byte length, NOT the whole
    /// allocation — pass it (`Some(len)`) when the call target is an in-blob sibling
    /// that must stay OUTSIDE the traced region, or a call into that sibling
    /// mis-records as recursion. `None` defaults to the whole [`NativeCode`] length.
    /// Panics on a fork/ptrace failure or when the lib is absent — gate on
    /// [`available`](Ptrace::available) first.
    pub fn trace_call_ex(
        code: &NativeCode,
        args: &[i64],
        trace: Option<&Trace>,
        descent: &Descent,
        region: Option<usize>,
    ) -> i64 {
        let f = hw_fns()
            .ptrace_trace_call_ex
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let cargs: Vec<c_long> = args.iter().map(|&a| a as c_long).collect();
        let mut result: c_long = 0;
        let th = trace.map_or(std::ptr::null_mut(), |t| t.handle);
        let len = region.unwrap_or(code.len);
        let rc = unsafe {
            f(
                code.base,
                len,
                cargs.as_ptr(),
                cargs.len() as c_int,
                &mut result,
                th,
                descent.handle,
            )
        };
        assert!(rc == ASMTEST_PTRACE_OK, "asmtest_ptrace_trace_call_ex failed: {rc}");
        result as i64
    }

    /// Descending variant of [`trace_attached`](Ptrace::trace_attached) for an
    /// externally-attached, ptrace-stopped process `pid` (the caller owns
    /// PTRACE_ATTACH/DETACH). Threads a [`Descent`] handle through the loop; `trace`
    /// (frame 0) may be `None` to record only into `descent`. Panics on a ptrace
    /// failure or when the lib is absent — gate on [`available`](Ptrace::available).
    pub fn trace_attached_ex(
        pid: i32,
        base: usize,
        len: usize,
        trace: Option<&Trace>,
        descent: &Descent,
    ) -> i64 {
        let f = hw_fns()
            .ptrace_trace_attached_ex
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let mut result: c_long = 0;
        let th = trace.map_or(std::ptr::null_mut(), |t| t.handle);
        let rc = unsafe {
            f(pid as c_int, base as *const c_void, len, &mut result, th, descent.handle)
        };
        assert!(rc == ASMTEST_PTRACE_OK, "asmtest_ptrace_trace_attached_ex failed: {rc}");
        result as i64
    }

    /// Descending variant of
    /// [`trace_attached_versioned`](Ptrace::trace_attached_versioned): decode the
    /// region against TIME-CORRECT bytes from a [`CodeImage`] at logical timestamp
    /// `when` (`0` = latest) while threading a [`Descent`] handle through the loop.
    /// `trace` (frame 0) may be `None` to record only into `descent`. Panics on a
    /// ptrace failure or when the lib is absent — gate on
    /// [`available`](Ptrace::available) first.
    pub fn trace_attached_versioned_ex(
        pid: i32,
        base: usize,
        len: usize,
        img: &CodeImage,
        when: u64,
        trace: Option<&Trace>,
        descent: &Descent,
    ) -> i64 {
        let f = hw_fns()
            .ptrace_trace_attached_versioned_ex
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let mut result: c_long = 0;
        let th = trace.map_or(std::ptr::null_mut(), |t| t.handle);
        let rc = unsafe {
            f(
                pid as c_int,
                base as *const c_void,
                len,
                img.handle,
                when,
                &mut result,
                th,
                descent.handle,
            )
        };
        assert!(
            rc == ASMTEST_PTRACE_OK,
            "asmtest_ptrace_trace_attached_versioned_ex failed: {rc}"
        );
        result as i64
    }

    /// Run an already-attached, ptrace-stopped target `pid` forward until it reaches
    /// `addr` (a software breakpoint that fires when the program itself next calls
    /// in), leaving it stopped there ready for [`trace_attached`](Ptrace::trace_attached)
    /// — the step that makes a resolved JIT method traceable when you don't control
    /// call timing. Returns the status: `ASMTEST_PTRACE_OK`, or `ASMTEST_PTRACE_ENOENT`
    /// if the target exited first. The caller owns PTRACE_ATTACH/DETACH.
    pub fn run_to(pid: i32, addr: usize) -> i32 {
        let f = hw_fns()
            .ptrace_run_to
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        unsafe { f(pid as c_int, addr as *const c_void) as i32 }
    }

    /// The executable mapping in `/proc/<pid>/maps` that CONTAINS `addr`, as
    /// `(base, len)`, or `None` if no executable mapping contains it (or the lib
    /// is absent). A pure file read; no ptrace, so it may be called before
    /// attaching.
    pub fn region_by_addr(pid: i32, addr: usize) -> Option<(usize, usize)> {
        let f = hw_fns().proc_region_by_addr?;
        let mut base: *mut c_void = std::ptr::null_mut();
        let mut len: usize = 0;
        let rc = unsafe {
            f(pid as c_int, addr as *const c_void, &mut base, &mut len)
        };
        if rc == ASMTEST_PTRACE_OK {
            Some((base as usize, len))
        } else {
            None
        }
    }

    /// A JIT method by `name` in the perf map at `/tmp/perf-<pid>.map`, as
    /// `(base, len)`, or `None` (no such symbol, no map file, or the lib is
    /// absent). `name` matches the full symbol text after the size field.
    pub fn perfmap_symbol(pid: i32, name: &str) -> Option<(usize, usize)> {
        let f = hw_fns().proc_perfmap_symbol?;
        let cname = CString::new(name).ok()?;
        let mut base: *mut c_void = std::ptr::null_mut();
        let mut len: usize = 0;
        let rc = unsafe {
            f(pid as c_int, cname.as_ptr(), &mut base, &mut len)
        };
        if rc == ASMTEST_PTRACE_OK {
            Some((base as usize, len))
        } else {
            None
        }
    }

    /// A JIT method by `name` from a binary jitdump (`path`, or
    /// `/tmp/jit-<pid>.dump` when `path` is `None`) as a [`JitMethod`], or `None`
    /// (no such method, no file, not a jitdump, or the lib is absent). The latest
    /// re-JIT body (highest timestamp) wins. Up to `want_bytes` of the recorded
    /// code is copied into [`JitMethod::code`]; pass `0` to skip the bytes.
    pub fn jitdump_find(
        path: Option<&str>,
        name: &str,
        pid: i32,
        want_bytes: usize,
    ) -> Option<JitMethod> {
        let f = hw_fns().jitdump_find?;
        let cpath = match path {
            Some(p) => Some(CString::new(p).ok()?),
            None => None,
        };
        let path_ptr = cpath.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        let cname = CString::new(name).ok()?;
        let mut entry = JitEntryRaw { code_addr: 0, code_size: 0, timestamp: 0, code_index: 0 };
        let mut buf: Vec<u8> = vec![0u8; want_bytes];
        let mut bytes_len: usize = 0;
        let (buf_ptr, blen_ptr) = if want_bytes > 0 {
            (buf.as_mut_ptr(), &mut bytes_len as *mut usize)
        } else {
            (std::ptr::null_mut(), std::ptr::null_mut())
        };
        let rc = unsafe {
            f(path_ptr, pid as c_int, cname.as_ptr(), &mut entry, buf_ptr, want_bytes, blen_ptr)
        };
        if rc != ASMTEST_PTRACE_OK {
            return None;
        }
        let code = if want_bytes > 0 {
            buf.truncate(bytes_len);
            buf
        } else {
            Vec::new()
        };
        Some(JitMethod {
            code_addr: entry.code_addr,
            code_size: entry.code_size,
            timestamp: entry.timestamp,
            code_index: entry.code_index,
            code,
        })
    }
}

/// `asmtest_descent_level_t` — call-descent policy deciding what happens at each
/// call-out (all default off). Passed to [`Descent::new`].
#[repr(i32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DescentLevel {
    /// Step over, record nothing — today's behaviour (the default).
    Off = 0,
    /// Record each (call-site -> callee) edge, still step over.
    RecordEdges = 1,
    /// Step INTO calls whose target resolves (allow-set), else edge + step over.
    DescendKnown = 2,
    /// Step INTO every call (denylist + budget + watchdog gated); best-effort.
    DescendAll = 3,
}

/// Call descent (`asmtest_descent_t`): configure how the ptrace stepper handles the
/// call-outs it would otherwise step over, and read back the recorded edges + nested
/// frames. Four levels ([`DescentLevel`]): `Off`, `RecordEdges`, `DescendKnown`,
/// `DescendAll`. Pass to [`Ptrace::trace_call_ex`] and its siblings. Frame 0 is the
/// root region (a superset of the flat trace); descended callees are frames `1..N`.
///
/// This is the **allow-set-only** surface: descent targets are selected with
/// [`allow_region`](Descent::allow_region) / [`deny_region`](Descent::deny_region),
/// not a capturing-closure resolver — a Rust `extern "C"` fn pointer cannot host a
/// GC-safe capturing upcall, so `set_resolver` / `set_denylist` are not offered (they
/// are exempted in `scripts/bindings-parity-allow.txt`).
///
/// Wraps the same `libasmtest_hwtrace` the [`HwTrace`] tier loads, so the same
/// self-skip applies: gate on [`Ptrace::available`] before tracing.
pub struct Descent {
    handle: *mut c_void,
}

impl Descent {
    /// Allocate a descent handle at `level` (conservative defaults for
    /// depth/budget/watchdog; empty allow-set/deny-set). Panics if the lib is
    /// unavailable or the allocation fails — gate on [`Ptrace::available`] first.
    pub fn new(level: DescentLevel) -> Descent {
        let f = hw_fns()
            .descent_new
            .expect("libasmtest_hwtrace not loaded (Ptrace::available() is false)");
        let h = unsafe { f(level as c_int) };
        assert!(!h.is_null(), "asmtest_descent_new failed");
        Descent { handle: h }
    }

    /// Idempotently free the descent handle. NULLs the pointer so a second call (or
    /// [`Drop`]) is a no-op — mirroring the [`Trace`] / [`CodeImage`] handle
    /// discipline (the native `asmtest_descent_free` is itself NULL-safe too).
    pub fn free(&mut self) {
        if !self.handle.is_null() {
            if let Some(f) = hw_fns().descent_free {
                unsafe { f(self.handle) };
            }
            self.handle = std::ptr::null_mut();
        }
    }

    // ---- configuration (in) ---- //

    /// Ceiling on nested descent depth (frame 0 is depth 0). `0` restores the default.
    pub fn set_max_depth(&self, max_depth: u32) {
        if let Some(f) = hw_fns().descent_set_max_depth {
            unsafe { f(self.handle, max_depth) };
        }
    }

    /// Total single-step instruction budget across all descended frames; `0` =
    /// default.
    pub fn set_insn_budget(&self, budget: u64) {
        if let Some(f) = hw_fns().descent_set_insn_budget {
            unsafe { f(self.handle, budget) };
        }
    }

    /// Real-time watchdog in milliseconds for a descended run (the L3 blocked-syscall
    /// escape); `0` = default.
    pub fn set_watchdog_ms(&self, ms: u32) {
        if let Some(f) = hw_fns().descent_set_watchdog_ms {
            unsafe { f(self.handle, ms) };
        }
    }

    /// Arm the built-in L3 default denylist (PLT resolver / vdso / GC-JIT
    /// modules; plus blocking-libc entry points on the fork path).
    pub fn use_default_denylist(&self) {
        if let Some(f) = hw_fns().descent_use_default_denylist {
            unsafe { f(self.handle) };
        }
    }

    /// Add `[base, base+len)` to the level-2 allow-set (descend into calls landing
    /// inside it). Returns `0` on success, negative on OOM (or when the lib is absent).
    pub fn allow_region(&self, base: usize, len: usize) -> i32 {
        match hw_fns().descent_allow_region {
            Some(f) => unsafe { f(self.handle, base as *const c_void, len) },
            None => ASMTEST_PTRACE_ENOENT,
        }
    }

    /// Add `[base, base+len)` to the level-3 deny-set (never descend into it).
    /// Returns `0` on success, negative on OOM (or when the lib is absent).
    pub fn deny_region(&self, base: usize, len: usize) -> i32 {
        match hw_fns().descent_deny_region {
            Some(f) => unsafe { f(self.handle, base as *const c_void, len) },
            None => ASMTEST_PTRACE_ENOENT,
        }
    }

    // ---- results (out) ---- //

    /// Every stepped-over call (level >= 1) as `(site, target, depth)`: the call-site
    /// byte-offset, the ABSOLUTE callee address, and the caller's frame depth. Empty
    /// when the lib is absent.
    pub fn edges(&self) -> Vec<(u64, u64, u32)> {
        let fns = hw_fns();
        match (fns.descent_edges_len, fns.descent_edge_site, fns.descent_edge_target, fns.descent_edge_depth) {
            (Some(len), Some(site), Some(target), Some(depth)) => {
                let n = unsafe { len(self.handle) };
                (0..n)
                    .map(|i| unsafe {
                        (site(self.handle, i), target(self.handle, i), depth(self.handle, i))
                    })
                    .collect()
            }
            _ => Vec::new(),
        }
    }

    /// Number of recorded frames (frame 0 is the root region; descended callees are
    /// `1..N`). `0` when the lib is absent.
    pub fn frames_len(&self) -> usize {
        match hw_fns().descent_frames_len {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// The ABSOLUTE base address of frame `f`.
    pub fn frame_base(&self, f: usize) -> u64 {
        match hw_fns().descent_frame_base {
            Some(func) => unsafe { func(self.handle, f) },
            None => 0,
        }
    }

    /// The byte length of frame `f`.
    pub fn frame_len(&self, f: usize) -> u64 {
        match hw_fns().descent_frame_len {
            Some(func) => unsafe { func(self.handle, f) },
            None => 0,
        }
    }

    /// The descent depth of frame `f` (`0` = frame 0 / root).
    pub fn frame_depth(&self, f: usize) -> u32 {
        match hw_fns().descent_frame_depth {
            Some(func) => unsafe { func(self.handle, f) },
            None => 0,
        }
    }

    /// The parent frame index of frame `f` (`-1` for the root).
    pub fn frame_parent(&self, f: usize) -> i32 {
        match hw_fns().descent_frame_parent {
            Some(func) => unsafe { func(self.handle, f) },
            None => -1,
        }
    }

    /// The ordered instruction-offset stream recorded in frame `f` (each offset
    /// relative to that frame's base). Empty when the lib is absent.
    pub fn frame_insns(&self, f: usize) -> Vec<u64> {
        let fns = hw_fns();
        match (fns.descent_frame_insn_count, fns.descent_frame_insn_at) {
            (Some(count), Some(at)) => {
                let n = unsafe { count(self.handle, f) };
                (0..n).map(|i| unsafe { at(self.handle, f, i) }).collect()
            }
            _ => Vec::new(),
        }
    }

    /// The distinct basic-block start offsets recorded in frame `f`. Empty when the
    /// lib is absent.
    pub fn frame_blocks(&self, f: usize) -> Vec<u64> {
        let fns = hw_fns();
        match (fns.descent_frame_block_count, fns.descent_frame_block_at) {
            (Some(count), Some(at)) => {
                let n = unsafe { count(self.handle, f) };
                (0..n).map(|i| unsafe { at(self.handle, f, i) }).collect()
            }
            _ => Vec::new(),
        }
    }

    /// True if a pool overflowed / a byte failed to decode (the record is incomplete).
    pub fn truncated(&self) -> bool {
        match hw_fns().descent_truncated {
            Some(f) => unsafe { f(self.handle) != 0 },
            None => false,
        }
    }

    /// True if descent stopped at a policy limit (max_depth / budget / recursion cap)
    /// — distinct from a pool overflow ([`truncated`](Descent::truncated)).
    pub fn depth_capped(&self) -> bool {
        match hw_fns().descent_depth_capped {
            Some(f) => unsafe { f(self.handle) != 0 },
            None => false,
        }
    }
}

impl Drop for Descent {
    fn drop(&mut self) {
        self.free();
    }
}

// The handle is an owned C allocation used only behind &self / &mut self / Drop; it
// shares no mutable state across threads, so the wrapper is Send (matching the
// crate's other owned-handle wrappers).
unsafe impl Send for Descent {}

/// `ASMTEST_CI_OK` from `asmtest_codeimage.h`; nonzero is an error/status.
const ASMTEST_CI_OK: c_int = 0;
/// `ASMTEST_CI_ENOENT` from `asmtest_codeimage.h`: address never tracked, or no
/// version at-or-before `when` (the `None`-returning [`CodeImage::bytes_at`] path).
pub const ASMTEST_CI_ENOENT: c_int = -7;

/// How a code-emission event was observed (mirrors the `ASMTEST_CI_KIND_*` macros).
#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CodeKind {
    /// `mprotect(...PROT_EXEC...)` — the common JIT edge.
    Mprotect = 1,
    /// `mmap(...PROT_EXEC...)`; `addr` is the real base.
    Mmap = 2,
    /// `memfd_create` — staging hint; correlate via `fd`.
    Memfd = 3,
}

/// A code-emission event from the optional eBPF detector (the safe wrapper over
/// `asmtest_codeimage_event_t`): where/when new executable code appeared for the
/// tracked pid. Sideband only — never the instruction stream.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct CodeEmission {
    /// Published base address (`0` for a memfd hint).
    pub addr: u64,
    /// Byte length (`0` for a memfd hint).
    pub len: u64,
    /// `bpf_ktime_get_ns()` at emission.
    pub timestamp: u64,
    /// The tgid that published.
    pub pid: u32,
    /// The thread that published.
    pub tid: u32,
    /// How the emission was observed.
    pub kind: CodeKind,
    /// The memfd fd, or `-1`.
    pub fd: i32,
}

/// Map a raw `asmtest_codeimage_event_t.kind` (a C macro value) to [`CodeKind`].
/// Any value outside the known set is a contract break and panics.
fn code_kind_of(kind: u32) -> CodeKind {
    match kind {
        1 => CodeKind::Mprotect,
        2 => CodeKind::Mmap,
        3 => CodeKind::Memfd,
        other => panic!("unexpected codeimage event kind {other}"),
    }
}

/// A time-aware code-image recorder (`asmtest_codeimage.h`): a userspace
/// `PERF_RECORD_TEXT_POKE`. [`track`](CodeImage::track) snapshots a (possibly
/// foreign) region's bytes and arms write-protect-async on its pages;
/// [`refresh`](CodeImage::refresh) re-snapshots only the pages that changed,
/// appending a new version stamped with the next monotonic sequence; and
/// [`bytes_at`](CodeImage::bytes_at) answers "what bytes were live at `addr` as of
/// sequence `when`" — the query a branch-trace decoder or the W2 block-normalizer
/// needs to reconstruct a method whose address was reused. Owns the underlying
/// `asmtest_codeimage_t`; dropping the value frees it (detaching any eBPF watch).
///
/// Wraps the same `libasmtest_hwtrace` the [`HwTrace`] tier loads, so the same
/// self-skip applies: gate on [`CodeImage::available`] (it is `false` when the lib
/// or the host support is absent) before recording.
pub struct CodeImage {
    handle: *mut c_void,
}

impl CodeImage {
    /// True if the userspace recorder can detect page changes on this host
    /// (`PAGEMAP_SCAN` or the soft-dirty fallback) and the lib loaded. Never panics,
    /// so callers (and the test) self-skip cleanly.
    pub fn available() -> bool {
        match hw_fns().ci_available {
            Some(f) => unsafe { f() != 0 },
            None => false,
        }
    }

    /// A human-readable reason [`available`](CodeImage::available) is false. Empty
    /// string when the lib is absent.
    pub fn skip_reason() -> String {
        match hw_fns().ci_skip_reason {
            Some(f) => {
                let mut buf = [0u8; 160];
                unsafe {
                    f(buf.as_mut_ptr() as *mut c_char, buf.len());
                    std::ffi::CStr::from_ptr(buf.as_ptr() as *const c_char)
                        .to_string_lossy()
                        .into_owned()
                }
            }
            None => String::new(),
        }
    }

    /// Create a timeline recording `pid`'s memory (`pid == 0` => this process).
    /// Panics if the lib is unavailable or the allocation fails — gate on
    /// [`available`](CodeImage::available) first.
    pub fn new(pid: i32) -> CodeImage {
        let f = hw_fns()
            .ci_new
            .expect("libasmtest_hwtrace not loaded (CodeImage::available() is false)");
        let h = unsafe { f(pid as c_int) };
        assert!(!h.is_null(), "asmtest_codeimage_new failed");
        CodeImage { handle: h }
    }

    /// Begin tracking `[base, base+len)` in the target: snapshot version 0 now and
    /// arm write-protect-async on its pages so the next [`refresh`](CodeImage::refresh)
    /// sees changes. May be called for several disjoint regions.
    pub fn track(&self, base: usize, len: usize) -> Result<(), String> {
        let f = hw_fns().ci_track.ok_or("libasmtest_hwtrace not loaded")?;
        let rc = unsafe { f(self.handle, base as *const c_void, len) };
        if rc != ASMTEST_CI_OK {
            return Err(format!("asmtest_codeimage_track failed: {rc}"));
        }
        Ok(())
    }

    /// Scan the tracked ranges for pages changed since the last arm, re-snapshot each
    /// as a NEW version, and re-arm. Returns the number of new versions recorded
    /// (`>= 0`), or a negative status. Cheap when nothing changed.
    pub fn refresh(&self) -> i32 {
        match hw_fns().ci_refresh {
            Some(f) => unsafe { f(self.handle) as i32 },
            None => 0,
        }
    }

    /// The current capture sequence — a monotonic logical timestamp the caller stamps
    /// trace positions against. Advances by one for every version recorded (track +
    /// each refresh change). `0` before anything is tracked.
    pub fn now(&self) -> u64 {
        match hw_fns().ci_now {
            Some(f) => unsafe { f(self.handle) },
            None => 0,
        }
    }

    /// The bytes live at `addr` as of capture sequence `when` (`when == 0` => the
    /// latest version), copied out, or `None` when `addr` is in no tracked region /
    /// no version exists at-or-before `when` (`ASMTEST_CI_ENOENT`) or the lib is
    /// absent. The returned bytes run from `addr` to the end of that version's region.
    pub fn bytes_at(&self, addr: usize, when: u64) -> Option<Vec<u8>> {
        let f = hw_fns().ci_bytes_at?;
        let mut out: *const u8 = std::ptr::null();
        let mut out_len: usize = 0;
        let rc = unsafe {
            f(self.handle, addr as *const c_void, when, &mut out, &mut out_len)
        };
        if rc != ASMTEST_CI_OK || out.is_null() {
            return None;
        }
        // *out borrows bytes owned by the timeline (valid until free); copy them out
        // so the returned Vec doesn't outlive the borrow.
        Some(unsafe { std::slice::from_raw_parts(out, out_len) }.to_vec())
    }

    /// True if the optional eBPF emission detector can load and attach on this host
    /// (built with libbpf, kernel BTF present, sufficient privilege) and the lib
    /// loaded. Self-skips cleanly without it.
    pub fn bpf_available() -> bool {
        match hw_fns().ci_bpf_available {
            Some(f) => unsafe { f() != 0 },
            None => false,
        }
    }

    /// A human-readable reason [`bpf_available`](CodeImage::bpf_available) is false.
    /// Empty string when the lib is absent.
    pub fn bpf_skip_reason() -> String {
        match hw_fns().ci_bpf_skip_reason {
            Some(f) => {
                let mut buf = [0u8; 160];
                unsafe {
                    f(buf.as_mut_ptr() as *mut c_char, buf.len());
                    std::ffi::CStr::from_ptr(buf.as_ptr() as *const c_char)
                        .to_string_lossy()
                        .into_owned()
                }
            }
            None => String::new(),
        }
    }

    /// Load the CO-RE program, filter it to this image's pid, and attach it.
    /// Subsequent [`poll_bpf`](CodeImage::poll_bpf) calls drain emission events.
    /// Returns `ASMTEST_CI_OK`, or a negative status (`ASMTEST_CI_ENOSYS` built
    /// without libbpf, `ASMTEST_CI_EUNAVAIL` no privilege / no BTF,
    /// `ASMTEST_CI_ELOAD`); also a negative status when the lib is absent.
    pub fn watch_bpf(&self) -> i32 {
        match hw_fns().ci_watch_bpf {
            Some(f) => unsafe { f(self.handle) as i32 },
            None => ASMTEST_CI_ENOENT,
        }
    }

    /// Drain ready emission events from the BPF ring buffer into this image's queue.
    /// `timeout_ms == 0` is a NON-BLOCKING drain (so it interleaves with a single-step
    /// loop); `> 0` waits up to that long. Returns the number of events queued
    /// (`>= 0`) or a negative status.
    pub fn poll_bpf(&self, timeout_ms: i32) -> i32 {
        match hw_fns().ci_poll_bpf {
            Some(f) => unsafe { f(self.handle, timeout_ms as c_int) as i32 },
            None => ASMTEST_CI_ENOENT,
        }
    }

    /// Pop one queued emission event, or `None` when the queue is empty / on a
    /// negative status / when the lib is absent.
    pub fn next_event(&self) -> Option<CodeEmission> {
        let f = hw_fns().ci_next?;
        let mut raw = CodeEventRaw {
            addr: 0,
            len: 0,
            timestamp: 0,
            pid: 0,
            tid: 0,
            kind: 0,
            fd: 0,
        };
        let rc = unsafe { f(self.handle, &mut raw) };
        if rc != 1 {
            return None;
        }
        Some(CodeEmission {
            addr: raw.addr,
            len: raw.len,
            timestamp: raw.timestamp,
            pid: raw.pid,
            tid: raw.tid,
            kind: code_kind_of(raw.kind),
            fd: raw.fd,
        })
    }
}

impl Drop for CodeImage {
    fn drop(&mut self) {
        if !self.handle.is_null() {
            if let Some(f) = hw_fns().ci_free {
                unsafe { f(self.handle) };
            }
            self.handle = std::ptr::null_mut();
        }
    }
}

// The handle is an owned C allocation used only behind &self / Drop; the
// recorder's reads + pagemap scans don't share mutable state across threads, so
// the wrapper is Send (matches the crate's other owned-handle wrappers).
unsafe impl Send for CodeImage {}
