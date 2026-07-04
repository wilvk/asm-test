//! Hardware-tier native runtime tracing for Zig (single-step / Intel PT / AMD).
//!
//! This is the Zig analogue of `bindings/python/asmtest/hwtrace.py`: the
//! language-wrapper surface for the optional hardware-trace tier (see
//! `include/asmtest_hwtrace.h` and `docs/native-tracing.md`). It records the same
//! `asmtest_trace_t` offsets as the emulator and DynamoRIO tiers, but by observing
//! the **real CPU** — and unlike the DynamoRIO wrapper it needs no DynamoRIO
//! install. Four backends share one API, selected by enum:
//!
//! * `SINGLESTEP` — EFLAGS.TF single-step (#DB -> SIGTRAP). Exact and complete on
//!   ANY x86-64 Linux (Intel, any-Zen AMD, VM, CI, container): no PMU, no
//!   perf_event, no privilege. This is the portable default and the one this
//!   binding's self-test exercises live.
//! * `INTEL_PT` / `CORESIGHT` / `AMD_LBR` — hardware branch-trace backends that
//!   self-skip off the specific bare-metal hardware they need.
//!
//! Like the DynamoRIO wrapper, the hwtrace library is optional and may be absent,
//! so — unlike the rest of the binding, which `linkSystemLibrary`s at build time —
//! this module dlopen()s `libasmtest_hwtrace` at RUNTIME via `std.DynLib` and
//! resolves each symbol with `lib.lookup`. The library path comes from
//! `$ASMTEST_HWTRACE_LIB`, else `<repo>/build/libasmtest_hwtrace.so`.
//!
//! `available(backend)` reports whether the chosen backend can run (the lib opens
//! AND `asmtest_hwtrace_available(backend) != 0`) so callers self-skip cleanly.
//! Only the struct *types* are taken from the C header via `@cImport`; the
//! *functions* are resolved through the DynLib handle.
//!
//! Targets Zig 0.13.x (matching build.zig / build.zig.zon).
const std = @import("std");
const builtin = @import("builtin");

// Struct *types* only — the functions are resolved at runtime via DynLib.
const c = @cImport({
    @cInclude("asmtest_hwtrace.h");
});

/// Success status from the lifecycle / registration calls (ASMTEST_HW_OK).
pub const OK: c_int = 0;

/// asmtest_trace_backend_t — the backend selected at init. SINGLESTEP is the
/// portable default that runs on any x86-64 Linux.
pub const Backend = enum(c_int) {
    intel_pt = 0,
    coresight = 1,
    amd_lbr = 2,
    singlestep = 3,
};

/// The default backend: EFLAGS.TF single-step, portable across any x86-64 Linux.
pub const SINGLESTEP: Backend = .singlestep;

/// asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
/// (hardware + DynamoRIO + emulator), not just the hardware backends above.
/// asmtest_trace_tier_t — the trace tiers, most-faithful to least.
pub const Tier = enum(c_int) {
    hwtrace = 0, // HW branch trace / single-step (real CPU)
    dynamorio = 1, // in-process software DBI (real CPU)
    emulator = 2, // Unicorn virtual CPU (isolated guest)
};

/// asmtest_trace_fidelity_t — NATIVE runs the real bytes on the real CPU
/// in-process; VIRTUAL runs an isolated guest on an emulated CPU.
pub const Fidelity = enum(c_int) {
    native = 0,
    virtual = 1,
};

/// Cross-tier policy bitmask (composable), passed across the FFI as an int.
/// `TRACE_BEST` resolves the most-faithful available choice (emulator floor
/// allowed); `TRACE_CEILING_FREE` drops the one fixed-window backend (AMD LBR);
/// `TRACE_NATIVE_ONLY` forbids the native->emulator fidelity crossing.
pub const TracePolicy = enum(c_int) {
    best = 0x0,
    ceiling_free = 0x1,
    native_only = 0x2,
};

/// Most-faithful available; the emulator floor is allowed.
pub const TRACE_BEST: TracePolicy = .best;

/// Like `TRACE_BEST`, but dropping the one fixed-window backend (AMD LBR).
pub const TRACE_CEILING_FREE: TracePolicy = .ceiling_free;

/// Forbid the native->emulator fidelity crossing (resolve only the real-CPU tiers).
pub const TRACE_NATIVE_ONLY: TracePolicy = .native_only;

/// asmtest_trace_choice_t — a resolved cross-tier trace option: which `tier` to
/// use, which hardware `backend` within it (meaningful only when
/// `tier == .hwtrace`), and the `fidelity` class. Exactly three int-sized fields,
/// no padding, so it marshals as three consecutive C ints.
pub const Choice = extern struct {
    tier: c_int,
    backend: c_int,
    fidelity: c_int,
};

/// asmtest_hwtrace_policy_t — backend auto-selection policy for `resolve`/`auto`.
/// `BEST` is the most faithful available backend; `CEILING_FREE` is the same but
/// skips the one fixed-window backend (AMD LBR) — re-resolve under it after a trace
/// comes back truncated.
pub const Policy = enum(c_int) {
    best = 0,
    ceiling_free = 1,
};

/// Pick the most faithful available backend for the host.
pub const BEST: Policy = .best;

/// Like `BEST`, but skipping the one fixed-window backend (AMD LBR).
pub const CEILING_FREE: Policy = .ceiling_free;

/// No hardware-trace backend is available on this host (ASMTEST_HW_EUNAVAIL); the
/// value `auto` returns in that case.
pub const ASMTEST_HW_EUNAVAIL: c_int = -3;

/// asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
/// `OK` is success; `ENOENT` means a region / symbol / method was not found
/// (the not-found wrappers map it onto `null`).
pub const ASMTEST_PTRACE_OK: c_int = 0;
pub const ASMTEST_PTRACE_ENOENT: c_int = -7;

/// asmtest_descent_level_t — call-descent policy: what the ptrace stepper does at
/// each call-out it would otherwise step over (all default off). `OFF` records
/// nothing; `RECORD_EDGES` records each (call-site -> callee) edge and steps over;
/// `DESCEND_KNOWN` single-steps INTO calls whose target resolves (allow-set /
/// resolver); `DESCEND_ALL` steps into everything (denylist + budget + watchdog
/// gated). Pass to `Descent.init`.
pub const DescentLevel = enum(c_int) {
    off = 0,
    record_edges = 1,
    descend_known = 2,
    descend_all = 3,
};

/// Step over, record nothing — today's behaviour (the default).
pub const DESCENT_OFF: DescentLevel = .off;

/// Record each (call-site -> callee) edge, still step over.
pub const DESCENT_RECORD_EDGES: DescentLevel = .record_edges;

/// Single-step INTO calls whose target resolves (allow-set / resolver).
pub const DESCENT_DESCEND_KNOWN: DescentLevel = .descend_known;

/// Single-step INTO every call (denylist + budget + watchdog gated).
pub const DESCENT_DESCEND_ALL: DescentLevel = .descend_all;

/// asmtest_codeimage.h — time-aware code-image recorder status codes (its own
/// namespace, mirroring the ptrace ones). `OK` is success; `ENOENT` means the
/// address was never tracked, or no version exists at-or-before `when` (the
/// `bytesAt` wrapper maps it onto `null`).
pub const ASMTEST_CI_OK: c_int = 0;
pub const ASMTEST_CI_ENOENT: c_int = -7;

/// asmtest_codeimage.h — how a code-emission event was observed (event `kind`).
pub const ASMTEST_CI_KIND_MPROTECT: u32 = 1;
pub const ASMTEST_CI_KIND_MMAP: u32 = 2;
pub const ASMTEST_CI_KIND_MEMFD: u32 = 3;

/// asmtest_codeimage_event_t — a code-emission event from the optional eBPF
/// detector. A byte-compatible mirror of the C struct (40 bytes: three `u64`,
/// three `u32`, one `i32`, no padding), so it marshals as the raw C struct
/// across the FFI.
pub const Event = extern struct {
    addr: u64, // published base address (0 for a memfd hint)
    len: u64, // byte length (0 for a memfd hint)
    timestamp: u64, // bpf_ktime_get_ns() at emission
    pid: u32, // tgid that published
    tid: u32, // thread that published
    kind: u32, // ASMTEST_CI_KIND_*
    fd: i32, // memfd fd, or -1
};

/// asmtest_jitdump_entry_t — a JIT method as recorded in a jitdump
/// `JIT_CODE_LOAD` record: exactly four consecutive `u64` fields (no padding),
/// so it marshals as the raw C struct across the FFI.
pub const JitEntry = extern struct {
    code_addr: u64,
    code_size: u64,
    timestamp: u64,
    code_index: u64,
};

/// A JIT method resolved from a jitdump (the safe wrapper over `JitEntry`): its
/// load address, byte size, the JIT's timestamp/index, and — unlike the text
/// perf-map — optionally the recorded native code bytes. `code` is a slice into
/// the caller-provided buffer passed to `jitdumpFind` (empty when no bytes were
/// requested or copied), mirroring how `skipReason` writes into a caller buffer
/// rather than allocating.
pub const JitMethod = struct {
    code_addr: u64,
    code_size: u64,
    timestamp: u64,
    code_index: u64,
    code: []const u8 = &.{},
};

/// Errors surfaced by the wrapper. `LibUnavailable` means the tier can't run
/// (lib missing or the backend self-skips) — callers should self-skip on it; the
/// rest map nonzero C return codes onto an error.
pub const Error = error{
    LibUnavailable,
    InitFailed,
    AllocFailed,
    RegisterFailed,
    TraceNewFailed,
    TraceFailed,
};

// ---- function-pointer prototypes (mirror the exported C symbols) ---- //
const FnAvailable = *const fn (c_int) callconv(.C) c_int;
const FnSkipReason = *const fn (c_int, [*c]u8, usize) callconv(.C) void;
const FnResolve = *const fn (c_int, [*]c_int, usize) callconv(.C) usize;
const FnAuto = *const fn (c_int) callconv(.C) c_int;
// Cross-tier orchestrator (asmtest_trace_auto.h). `policy` is an `unsigned`
// bitmask; the choices marshal as the 3-int `Choice` struct.
const FnResolveTiers = *const fn (c_uint, [*]Choice, usize) callconv(.C) usize;
const FnAutoTier = *const fn (c_uint, *Choice) callconv(.C) c_int;
const FnInit = *const fn (*const c.asmtest_hwtrace_options_t) callconv(.C) c_int;
const FnRegister = *const fn ([*:0]const u8, ?*anyopaque, usize, ?*anyopaque) callconv(.C) c_int;
const FnMarker = *const fn ([*:0]const u8) callconv(.C) void;
// Scoped-tracing shared core (§0/§1): error-returning begin + render-on-close.
const FnTryBegin = *const fn ([*:0]const u8) callconv(.C) c_int;
const FnRender = *const fn ([*:0]const u8, ?[*]u8, usize) callconv(.C) c_int;
const FnShutdown = *const fn () callconv(.C) void;
const FnExecAlloc = *const fn (?*const anyopaque, usize, *?*anyopaque, *usize) callconv(.C) c_int;
const FnExecFree = *const fn (?*anyopaque, usize) callconv(.C) void;
const FnTraceNew = *const fn (usize, usize) callconv(.C) ?*anyopaque;
const FnTraceFree = *const fn (?*anyopaque) callconv(.C) void;
const FnTraceCovered = *const fn (?*anyopaque, u64) callconv(.C) c_int;
const FnBlocksLen = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnInsnsTotal = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnInsnsLen = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnTruncated = *const fn (?*anyopaque) callconv(.C) c_int;
const FnBlockAt = *const fn (?*anyopaque, usize) callconv(.C) c_ulonglong;
const FnInsnAt = *const fn (?*anyopaque, usize) callconv(.C) c_ulonglong;
// asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit. Same lib,
// same resolve-at-runtime idiom; `long` marshals as `c_long`, `pid_t` as `c_int`.
const FnPtraceAvailable = *const fn () callconv(.C) c_int;
const FnPtraceSkipReason = *const fn ([*c]u8, usize) callconv(.C) void;
const FnPtraceTraceCall = *const fn (?*const anyopaque, usize, [*c]const c_long, c_int, ?*c_long, ?*anyopaque) callconv(.C) c_int;
const FnPtraceTraceAttached = *const fn (c_int, ?*const anyopaque, usize, ?*c_long, ?*anyopaque) callconv(.C) c_int;
// The versioned variant: it consults a code-image timeline (`*anyopaque`, the
// opaque `struct asmtest_codeimage`) at logical timestamp `when` for the bytes
// to step, instead of one late `process_vm_readv` snapshot.
const FnPtraceTraceAttachedVersioned = *const fn (c_int, ?*const anyopaque, usize, ?*anyopaque, u64, ?*c_long, ?*anyopaque) callconv(.C) c_int;
const FnPtraceRunTo = *const fn (c_int, ?*const anyopaque) callconv(.C) c_int;
const FnProcRegionByAddr = *const fn (c_int, ?*const anyopaque, *?*anyopaque, *usize) callconv(.C) c_int;
const FnProcPerfmapSymbol = *const fn (c_int, [*:0]const u8, *?*anyopaque, *usize) callconv(.C) c_int;
const FnJitdumpFind = *const fn ([*c]const u8, c_int, [*:0]const u8, *JitEntry, [*c]u8, usize, ?*usize) callconv(.C) c_int;
// asmtest_ptrace.h — call descent (asmtest_descent_t): edges + nested frames. The
// opaque `asmtest_descent_t*` marshals as `?*anyopaque`; `size_t` indices/lengths
// as `usize`; addresses as `u64`; the frame-parent as `i32`. Zig is allow-set-only
// (std.DynLib hosts no GC-safe capturing upcall), so the two callback installers
// (set_resolver / set_denylist) are intentionally NOT wrapped — see the exemption
// lines in scripts/bindings-parity-allow.txt.
const FnDescentNew = *const fn (c_int) callconv(.C) ?*anyopaque;
const FnDescentFree = *const fn (?*anyopaque) callconv(.C) void;
const FnDescentSetMaxDepth = *const fn (?*anyopaque, u32) callconv(.C) void;
const FnDescentSetInsnBudget = *const fn (?*anyopaque, u64) callconv(.C) void;
const FnDescentSetWatchdogMs = *const fn (?*anyopaque, u32) callconv(.C) void;
const FnDescentAllowRegion = *const fn (?*anyopaque, ?*const anyopaque, usize) callconv(.C) c_int;
const FnDescentDenyRegion = *const fn (?*anyopaque, ?*const anyopaque, usize) callconv(.C) c_int;
const FnDescentEdgesLen = *const fn (?*anyopaque) callconv(.C) usize;
const FnDescentEdgeSite = *const fn (?*anyopaque, usize) callconv(.C) u64;
const FnDescentEdgeTarget = *const fn (?*anyopaque, usize) callconv(.C) u64;
const FnDescentEdgeDepth = *const fn (?*anyopaque, usize) callconv(.C) u32;
const FnDescentFramesLen = *const fn (?*anyopaque) callconv(.C) usize;
const FnDescentFrameBase = *const fn (?*anyopaque, usize) callconv(.C) u64;
const FnDescentFrameLen = *const fn (?*anyopaque, usize) callconv(.C) u64;
const FnDescentFrameDepth = *const fn (?*anyopaque, usize) callconv(.C) u32;
const FnDescentFrameParent = *const fn (?*anyopaque, usize) callconv(.C) i32;
const FnDescentFrameInsnCount = *const fn (?*anyopaque, usize) callconv(.C) usize;
const FnDescentFrameInsnAt = *const fn (?*anyopaque, usize, usize) callconv(.C) u64;
const FnDescentFrameBlockCount = *const fn (?*anyopaque, usize) callconv(.C) usize;
const FnDescentFrameBlockAt = *const fn (?*anyopaque, usize, usize) callconv(.C) u64;
const FnDescentTruncated = *const fn (?*anyopaque) callconv(.C) c_int;
const FnDescentDepthCapped = *const fn (?*anyopaque) callconv(.C) c_int;
// Descending variants of the three trace entry points; each threads the descent
// handle through the loop. `trace`/`descent` may be null; `trace_call_ex` takes an
// explicit region length (the traced region), distinct from the whole allocation.
const FnPtraceTraceCallEx = *const fn (?*const anyopaque, usize, [*c]const c_long, c_int, ?*c_long, ?*anyopaque, ?*anyopaque) callconv(.C) c_int;
const FnPtraceTraceAttachedEx = *const fn (c_int, ?*const anyopaque, usize, ?*c_long, ?*anyopaque, ?*anyopaque) callconv(.C) c_int;
const FnPtraceTraceAttachedVersionedEx = *const fn (c_int, ?*const anyopaque, usize, ?*anyopaque, u64, ?*c_long, ?*anyopaque, ?*anyopaque) callconv(.C) c_int;
// asmtest_codeimage.h — time-aware code-image recorder. Same lib, same
// resolve-at-runtime idiom; the opaque `asmtest_codeimage_t*` marshals as
// `?*anyopaque`, `pid_t` as `c_int`, `size_t` as `usize`, the logical timestamp
// as `u64`, and the borrowed byte pointer out-param as `*[*c]const u8`.
const FnCiAvailable = *const fn () callconv(.C) c_int;
const FnCiSkipReason = *const fn ([*c]u8, usize) callconv(.C) void;
const FnCiNew = *const fn (c_int) callconv(.C) ?*anyopaque;
const FnCiFree = *const fn (?*anyopaque) callconv(.C) void;
const FnCiTrack = *const fn (?*anyopaque, ?*const anyopaque, usize) callconv(.C) c_int;
const FnCiRefresh = *const fn (?*anyopaque) callconv(.C) c_int;
const FnCiNow = *const fn (?*const anyopaque) callconv(.C) u64;
const FnCiBytesAt = *const fn (?*const anyopaque, ?*const anyopaque, u64, *[*c]const u8, *usize) callconv(.C) c_int;
const FnCiBpfAvailable = *const fn () callconv(.C) c_int;
const FnCiBpfSkipReason = *const fn ([*c]u8, usize) callconv(.C) void;
const FnCiWatchBpf = *const fn (?*anyopaque) callconv(.C) c_int;
const FnCiPollBpf = *const fn (?*anyopaque, c_int) callconv(.C) c_int;
const FnCiNext = *const fn (?*anyopaque, *Event) callconv(.C) c_int;

/// The resolved entry points. Populated once on first `load()`; held in a
/// process-global because the DynLib handle and its symbols outlive any one
/// trace (mirroring how Python keeps a module-level `_lib`).
const Api = struct {
    lib: std.DynLib,
    available: FnAvailable,
    skip_reason: FnSkipReason,
    resolve: FnResolve,
    auto: FnAuto,
    resolve_tiers: FnResolveTiers,
    auto_tier: FnAutoTier,
    init: FnInit,
    register: FnRegister,
    begin: FnMarker,
    end: FnMarker,
    // Optional (an older lib without them still loads; the scope falls back to begin).
    try_begin: ?FnTryBegin,
    render: ?FnRender,
    shutdown: FnShutdown,
    exec_alloc: FnExecAlloc,
    exec_free: FnExecFree,
    trace_new: FnTraceNew,
    trace_free: FnTraceFree,
    trace_covered: FnTraceCovered,
    blocks_len: FnBlocksLen,
    insns_total: FnInsnsTotal,
    insns_len: FnInsnsLen,
    truncated: FnTruncated,
    block_at: FnBlockAt,
    insn_at: FnInsnAt,
    // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
    ptrace_available: FnPtraceAvailable,
    ptrace_skip_reason: FnPtraceSkipReason,
    ptrace_trace_call: FnPtraceTraceCall,
    ptrace_trace_attached: FnPtraceTraceAttached,
    ptrace_trace_attached_versioned: FnPtraceTraceAttachedVersioned,
    ptrace_run_to: FnPtraceRunTo,
    proc_region_by_addr: FnProcRegionByAddr,
    proc_perfmap_symbol: FnProcPerfmapSymbol,
    jitdump_find: FnJitdumpFind,
    // asmtest_ptrace.h — call descent (asmtest_descent_t).
    descent_new: FnDescentNew,
    descent_free: FnDescentFree,
    descent_set_max_depth: FnDescentSetMaxDepth,
    descent_set_insn_budget: FnDescentSetInsnBudget,
    descent_set_watchdog_ms: FnDescentSetWatchdogMs,
    descent_allow_region: FnDescentAllowRegion,
    descent_deny_region: FnDescentDenyRegion,
    descent_edges_len: FnDescentEdgesLen,
    descent_edge_site: FnDescentEdgeSite,
    descent_edge_target: FnDescentEdgeTarget,
    descent_edge_depth: FnDescentEdgeDepth,
    descent_frames_len: FnDescentFramesLen,
    descent_frame_base: FnDescentFrameBase,
    descent_frame_len: FnDescentFrameLen,
    descent_frame_depth: FnDescentFrameDepth,
    descent_frame_parent: FnDescentFrameParent,
    descent_frame_insn_count: FnDescentFrameInsnCount,
    descent_frame_insn_at: FnDescentFrameInsnAt,
    descent_frame_block_count: FnDescentFrameBlockCount,
    descent_frame_block_at: FnDescentFrameBlockAt,
    descent_truncated: FnDescentTruncated,
    descent_depth_capped: FnDescentDepthCapped,
    ptrace_trace_call_ex: FnPtraceTraceCallEx,
    ptrace_trace_attached_ex: FnPtraceTraceAttachedEx,
    ptrace_trace_attached_versioned_ex: FnPtraceTraceAttachedVersionedEx,
    // asmtest_codeimage.h — time-aware code-image recorder.
    ci_available: FnCiAvailable,
    ci_skip_reason: FnCiSkipReason,
    ci_new: FnCiNew,
    ci_free: FnCiFree,
    ci_track: FnCiTrack,
    ci_refresh: FnCiRefresh,
    ci_now: FnCiNow,
    ci_bytes_at: FnCiBytesAt,
    ci_bpf_available: FnCiBpfAvailable,
    ci_bpf_skip_reason: FnCiBpfSkipReason,
    ci_watch_bpf: FnCiWatchBpf,
    ci_poll_bpf: FnCiPollBpf,
    ci_next: FnCiNext,
};

var g_api: ?Api = null;
var g_load_failed = false;

// The library string `openLib` actually opened, captured so `libraryPath()` can
// report which candidate satisfied the load (env override first, else the build/
// default, else the bare soname). Length 0 until a successful open.
var g_path_buf: [std.fs.max_path_bytes]u8 = undefined;
var g_path_len: usize = 0;

fn libName() [*:0]const u8 {
    return if (builtin.os.tag == .macos) "libasmtest_hwtrace.dylib" else "libasmtest_hwtrace.so";
}

/// Record `s` as the resolved library path for `libraryPath()` to report. Called
/// by `openLib` just before returning the opened handle, so only the winning
/// candidate is committed.
fn rememberPath(s: []const u8) void {
    const n = @min(s.len, g_path_buf.len);
    @memcpy(g_path_buf[0..n], s[0..n]);
    g_path_len = n;
}

/// Resolve the hwtrace library path: `$ASMTEST_HWTRACE_LIB` if set, else
/// `<repo>/build/libasmtest_hwtrace.so`. At runtime we only have an env var or
/// the bare name, so fall back to the conventional repo build dir relative to
/// cwd, then the plain soname (resolved via the loader search path), when no env
/// var is present. Records the opened candidate in `g_path_buf` (via
/// `rememberPath`) so the self-report reflects the exact search order without
/// changing it.
fn openLib(buf: []u8) ?std.DynLib {
    if (std.posix.getenv("ASMTEST_HWTRACE_LIB")) |p| {
        // env-provided absolute/relative path; pass it through verbatim.
        const z = std.fmt.bufPrintZ(buf, "{s}", .{p}) catch return null;
        const lib = std.DynLib.open(z) catch return null;
        rememberPath(std.mem.sliceTo(z, 0));
        return lib;
    }
    // No env var: try the conventional repo build dir relative to cwd, then the
    // bare soname (loader search path). `make zig-test`/callers normally set the
    // env var, so this is a best-effort convenience.
    const z = std.fmt.bufPrintZ(buf, "build/{s}", .{libName()}) catch return null;
    if (std.DynLib.open(z)) |lib| {
        rememberPath(std.mem.sliceTo(z, 0));
        return lib;
    } else |_| {}
    const z2 = std.fmt.bufPrintZ(buf, "{s}", .{libName()}) catch return null;
    const lib2 = std.DynLib.open(z2) catch return null;
    rememberPath(std.mem.sliceTo(z2, 0));
    return lib2;
}

fn lookupAll(lib: *std.DynLib) ?Api {
    // Every symbol must resolve; a partial lib is treated as unavailable.
    return Api{
        .lib = lib.*,
        .available = lib.lookup(FnAvailable, "asmtest_hwtrace_available") orelse return null,
        .skip_reason = lib.lookup(FnSkipReason, "asmtest_hwtrace_skip_reason") orelse return null,
        .resolve = lib.lookup(FnResolve, "asmtest_hwtrace_resolve") orelse return null,
        .auto = lib.lookup(FnAuto, "asmtest_hwtrace_auto") orelse return null,
        .resolve_tiers = lib.lookup(FnResolveTiers, "asmtest_trace_resolve") orelse return null,
        .auto_tier = lib.lookup(FnAutoTier, "asmtest_trace_auto") orelse return null,
        .init = lib.lookup(FnInit, "asmtest_hwtrace_init") orelse return null,
        .register = lib.lookup(FnRegister, "asmtest_hwtrace_register_region") orelse return null,
        .begin = lib.lookup(FnMarker, "asmtest_hwtrace_begin") orelse return null,
        .end = lib.lookup(FnMarker, "asmtest_hwtrace_end") orelse return null,
        .try_begin = lib.lookup(FnTryBegin, "asmtest_hwtrace_try_begin"),
        .render = lib.lookup(FnRender, "asmtest_hwtrace_render"),
        .shutdown = lib.lookup(FnShutdown, "asmtest_hwtrace_shutdown") orelse return null,
        .exec_alloc = lib.lookup(FnExecAlloc, "asmtest_hwtrace_exec_alloc") orelse return null,
        .exec_free = lib.lookup(FnExecFree, "asmtest_hwtrace_exec_free") orelse return null,
        .trace_new = lib.lookup(FnTraceNew, "asmtest_trace_new") orelse return null,
        .trace_free = lib.lookup(FnTraceFree, "asmtest_trace_free") orelse return null,
        .trace_covered = lib.lookup(FnTraceCovered, "asmtest_trace_covered") orelse return null,
        .blocks_len = lib.lookup(FnBlocksLen, "asmtest_emu_trace_blocks_len") orelse return null,
        .insns_total = lib.lookup(FnInsnsTotal, "asmtest_emu_trace_insns_total") orelse return null,
        .insns_len = lib.lookup(FnInsnsLen, "asmtest_emu_trace_insns_len") orelse return null,
        .truncated = lib.lookup(FnTruncated, "asmtest_emu_trace_truncated") orelse return null,
        .block_at = lib.lookup(FnBlockAt, "asmtest_emu_trace_block_at") orelse return null,
        .insn_at = lib.lookup(FnInsnAt, "asmtest_emu_trace_insn_at") orelse return null,
        // asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit.
        .ptrace_available = lib.lookup(FnPtraceAvailable, "asmtest_ptrace_available") orelse return null,
        .ptrace_skip_reason = lib.lookup(FnPtraceSkipReason, "asmtest_ptrace_skip_reason") orelse return null,
        .ptrace_trace_call = lib.lookup(FnPtraceTraceCall, "asmtest_ptrace_trace_call") orelse return null,
        .ptrace_trace_attached = lib.lookup(FnPtraceTraceAttached, "asmtest_ptrace_trace_attached") orelse return null,
        .ptrace_trace_attached_versioned = lib.lookup(FnPtraceTraceAttachedVersioned, "asmtest_ptrace_trace_attached_versioned") orelse return null,
        .ptrace_run_to = lib.lookup(FnPtraceRunTo, "asmtest_ptrace_run_to") orelse return null,
        .proc_region_by_addr = lib.lookup(FnProcRegionByAddr, "asmtest_proc_region_by_addr") orelse return null,
        .proc_perfmap_symbol = lib.lookup(FnProcPerfmapSymbol, "asmtest_proc_perfmap_symbol") orelse return null,
        .jitdump_find = lib.lookup(FnJitdumpFind, "asmtest_jitdump_find") orelse return null,
        // asmtest_ptrace.h — call descent (asmtest_descent_t).
        .descent_new = lib.lookup(FnDescentNew, "asmtest_descent_new") orelse return null,
        .descent_free = lib.lookup(FnDescentFree, "asmtest_descent_free") orelse return null,
        .descent_set_max_depth = lib.lookup(FnDescentSetMaxDepth, "asmtest_descent_set_max_depth") orelse return null,
        .descent_set_insn_budget = lib.lookup(FnDescentSetInsnBudget, "asmtest_descent_set_insn_budget") orelse return null,
        .descent_set_watchdog_ms = lib.lookup(FnDescentSetWatchdogMs, "asmtest_descent_set_watchdog_ms") orelse return null,
        .descent_allow_region = lib.lookup(FnDescentAllowRegion, "asmtest_descent_allow_region") orelse return null,
        .descent_deny_region = lib.lookup(FnDescentDenyRegion, "asmtest_descent_deny_region") orelse return null,
        .descent_edges_len = lib.lookup(FnDescentEdgesLen, "asmtest_descent_edges_len") orelse return null,
        .descent_edge_site = lib.lookup(FnDescentEdgeSite, "asmtest_descent_edge_site") orelse return null,
        .descent_edge_target = lib.lookup(FnDescentEdgeTarget, "asmtest_descent_edge_target") orelse return null,
        .descent_edge_depth = lib.lookup(FnDescentEdgeDepth, "asmtest_descent_edge_depth") orelse return null,
        .descent_frames_len = lib.lookup(FnDescentFramesLen, "asmtest_descent_frames_len") orelse return null,
        .descent_frame_base = lib.lookup(FnDescentFrameBase, "asmtest_descent_frame_base") orelse return null,
        .descent_frame_len = lib.lookup(FnDescentFrameLen, "asmtest_descent_frame_len") orelse return null,
        .descent_frame_depth = lib.lookup(FnDescentFrameDepth, "asmtest_descent_frame_depth") orelse return null,
        .descent_frame_parent = lib.lookup(FnDescentFrameParent, "asmtest_descent_frame_parent") orelse return null,
        .descent_frame_insn_count = lib.lookup(FnDescentFrameInsnCount, "asmtest_descent_frame_insn_count") orelse return null,
        .descent_frame_insn_at = lib.lookup(FnDescentFrameInsnAt, "asmtest_descent_frame_insn_at") orelse return null,
        .descent_frame_block_count = lib.lookup(FnDescentFrameBlockCount, "asmtest_descent_frame_block_count") orelse return null,
        .descent_frame_block_at = lib.lookup(FnDescentFrameBlockAt, "asmtest_descent_frame_block_at") orelse return null,
        .descent_truncated = lib.lookup(FnDescentTruncated, "asmtest_descent_truncated") orelse return null,
        .descent_depth_capped = lib.lookup(FnDescentDepthCapped, "asmtest_descent_depth_capped") orelse return null,
        .ptrace_trace_call_ex = lib.lookup(FnPtraceTraceCallEx, "asmtest_ptrace_trace_call_ex") orelse return null,
        .ptrace_trace_attached_ex = lib.lookup(FnPtraceTraceAttachedEx, "asmtest_ptrace_trace_attached_ex") orelse return null,
        .ptrace_trace_attached_versioned_ex = lib.lookup(FnPtraceTraceAttachedVersionedEx, "asmtest_ptrace_trace_attached_versioned_ex") orelse return null,
        // asmtest_codeimage.h — time-aware code-image recorder.
        .ci_available = lib.lookup(FnCiAvailable, "asmtest_codeimage_available") orelse return null,
        .ci_skip_reason = lib.lookup(FnCiSkipReason, "asmtest_codeimage_skip_reason") orelse return null,
        .ci_new = lib.lookup(FnCiNew, "asmtest_codeimage_new") orelse return null,
        .ci_free = lib.lookup(FnCiFree, "asmtest_codeimage_free") orelse return null,
        .ci_track = lib.lookup(FnCiTrack, "asmtest_codeimage_track") orelse return null,
        .ci_refresh = lib.lookup(FnCiRefresh, "asmtest_codeimage_refresh") orelse return null,
        .ci_now = lib.lookup(FnCiNow, "asmtest_codeimage_now") orelse return null,
        .ci_bytes_at = lib.lookup(FnCiBytesAt, "asmtest_codeimage_bytes_at") orelse return null,
        .ci_bpf_available = lib.lookup(FnCiBpfAvailable, "asmtest_codeimage_bpf_available") orelse return null,
        .ci_bpf_skip_reason = lib.lookup(FnCiBpfSkipReason, "asmtest_codeimage_bpf_skip_reason") orelse return null,
        .ci_watch_bpf = lib.lookup(FnCiWatchBpf, "asmtest_codeimage_watch_bpf") orelse return null,
        .ci_poll_bpf = lib.lookup(FnCiPollBpf, "asmtest_codeimage_poll_bpf") orelse return null,
        .ci_next = lib.lookup(FnCiNext, "asmtest_codeimage_next") orelse return null,
    };
}

/// Open + resolve the lib once. Returns null (and latches the failure) if the
/// lib can't open or a symbol is missing — `available()` turns that into `false`
/// rather than an error so the test self-skips.
fn load() ?*Api {
    if (g_api) |*api| return api;
    if (g_load_failed) return null;
    var path_buf: [std.fs.max_path_bytes]u8 = undefined;
    var lib = openLib(&path_buf) orelse {
        g_load_failed = true;
        return null;
    };
    if (lookupAll(&lib)) |api| {
        g_api = api;
        return &g_api.?;
    }
    lib.close();
    g_load_failed = true;
    return null;
}

/// A coverage recorder for a registered native region, via the hardware tier.
/// Process-wide lifecycle (available/init/shutdown) lives here as namespaced
/// functions; per-trace state lives in the `HwTrace` struct below.

/// True if the chosen backend can run on this host: the hwtrace lib opens AND
/// `asmtest_hwtrace_available(backend)` reports the full detect chain passes.
/// Self-skip otherwise. Mirrors `HwTrace.available()`.
pub fn available(backend: Backend) bool {
    const api = load() orelse return false;
    return api.available(@intFromEnum(backend)) != 0;
}

/// The `libasmtest_hwtrace` string this process actually opened — the
/// `$ASMTEST_HWTRACE_LIB` override if set, else the `build/` default, else the
/// bare soname (the resolver's search order, env-override first). Returns an empty
/// slice until the tier resolves. Counterpart of the Python wrapper's
/// `hwtrace.library_path()`, letting a clean-room test assert which candidate
/// satisfied the load.
///
/// NOTE: this binding is a SOURCE distribution — the consumer builds/links
/// libasmtest themselves, so there is no bundled `native/` directory to point at.
/// The value is the exact string handed to `std.DynLib.open`, resolved at RUN TIME
/// (this tier is DynLib-based, not one of the binding's `linkSystemLibrary` libs).
pub fn libraryPath() []const u8 {
    _ = load(); // ensure the loader has run so g_path_buf is populated
    return g_path_buf[0..g_path_len];
}

/// Human-readable reason `available()` is false (or "available"), written into
/// `buf` (always NUL-terminated). Returns the slice up to the NUL. If the lib
/// can't load, reports that so the self-skip message is still useful.
pub fn skipReason(backend: Backend, buf: []u8) []const u8 {
    const api = load() orelse {
        const msg = "libasmtest_hwtrace not loadable";
        const n = @min(msg.len, buf.len);
        @memcpy(buf[0..n], msg[0..n]);
        return buf[0..n];
    };
    api.skip_reason(@intFromEnum(backend), buf.ptr, buf.len);
    const z = std.mem.sliceTo(buf, 0);
    return z;
}

/// This host's hardware-trace fallback cascade under `policy`: the available
/// backend enums, most-faithful first (INTEL_PT > AMD_LBR > SINGLESTEP >
/// CORESIGHT). Self-owns a fixed 4-element buffer (there are only four backends)
/// and returns the live `[0..n]` slice into it; empty only off x86-64 Linux
/// (single-step is the floor there) or when the lib can't load. `CEILING_FREE`
/// drops the ceiling-bounded backend (AMD LBR). Mirrors `HwTrace.resolve()`.
pub const Resolved = struct {
    buf: [4]c_int = undefined,
    len: usize = 0,

    /// The resolved backend enums, in descending-fidelity (ascending-enum) order.
    pub fn slice(self: *const Resolved) []const c_int {
        return self.buf[0..self.len];
    }
};

/// Resolve `policy` into the available backend cascade (see `Resolved`).
pub fn resolve(policy: Policy) Resolved {
    var r = Resolved{};
    const api = load() orelse return r;
    r.len = api.resolve(@intFromEnum(policy), &r.buf, r.buf.len);
    return r;
}

/// The single most-preferred available backend under `policy` (a backend enum
/// >= 0, ready to `init`), or `ASMTEST_HW_EUNAVAIL` (< 0) when no hardware-trace
/// backend is available on this host. `auto` is not a Zig keyword, so this keeps
/// the C name. Mirrors `HwTrace.auto()`.
pub fn auto(policy: Policy) c_int {
    const api = load() orelse return ASMTEST_HW_EUNAVAIL;
    return api.auto(@intFromEnum(policy));
}

/// This host's full CROSS-TIER fallback cascade under `policy` (the cross-tier
/// orchestrator over hwtrace + DynamoRIO + emulator, `asmtest_trace_resolve`),
/// most-faithful first: Intel PT -> AMD LBR -> DynamoRIO -> single-step ->
/// CoreSight -> emulator, each included only if its tier is available. Self-owns
/// a fixed buffer (one slot per cascade stage) and returns the live `[0..n]`
/// slice into it. `TRACE_NATIVE_ONLY` drops the emulator floor (no
/// native->emulator fidelity crossing); `TRACE_CEILING_FREE` drops AMD LBR.
/// Mirrors `HwTrace.resolve_tiers()`.
pub const ResolvedTiers = struct {
    buf: [8]Choice = undefined,
    len: usize = 0,

    /// The resolved choices, in descending-fidelity order.
    pub fn slice(self: *const ResolvedTiers) []const Choice {
        return self.buf[0..self.len];
    }
};

/// Resolve `policy` into the available cross-tier cascade (see `ResolvedTiers`).
pub fn resolveTiers(policy: TracePolicy) ResolvedTiers {
    var r = ResolvedTiers{};
    const api = load() orelse return r;
    r.len = api.resolve_tiers(@intCast(@intFromEnum(policy)), &r.buf, r.buf.len);
    return r;
}

/// The single most-preferred available cross-tier choice under `policy`
/// (`asmtest_trace_auto`), or `null` when the cascade is empty (only off a native
/// host under `TRACE_NATIVE_ONLY`) or the lib can't load. Mirrors
/// `HwTrace.auto_tier()`.
pub fn autoTier(policy: TracePolicy) ?Choice {
    const api = load() orelse return null;
    var out: Choice = undefined;
    if (api.auto_tier(@intCast(@intFromEnum(policy)), &out) != OK) return null;
    return out;
}

/// Select a backend and initialize the tier. SINGLESTEP is the portable default
/// that runs on any x86-64 Linux. Returns `Error.LibUnavailable` if the tier
/// isn't loadable, `Error.InitFailed` on a nonzero C return.
pub fn init(backend: Backend) Error!void {
    const api = load() orelse return Error.LibUnavailable;
    var opts: c.asmtest_hwtrace_options_t = std.mem.zeroes(c.asmtest_hwtrace_options_t);
    opts.backend = @intCast(@intFromEnum(backend));
    if (api.init(&opts) != OK) return Error.InitFailed;
}

/// Bring the tier up with the default backend (SINGLESTEP).
pub fn initDefault() Error!void {
    return init(SINGLESTEP);
}

/// Tear the tier down (`asmtest_hwtrace_shutdown`).
pub fn shutdown() void {
    const api = load() orelse return;
    api.shutdown();
}

/// Host-native machine code in real executable (W^X) memory. Unlike the
/// DynamoRIO tier's `asmtest_exec_alloc` (which fills an `exec_code_t` struct),
/// the hwtrace tier returns the base pointer and length through two out-params,
/// so we hold them directly.
pub const NativeCode = struct {
    base: ?*anyopaque = null,
    len: usize = 0,
    freed: bool = false,

    /// Map executable memory and copy `bytes` into it via
    /// `asmtest_hwtrace_exec_alloc`. Returns `Error.AllocFailed` on a nonzero C
    /// return.
    pub fn fromBytes(bytes: []const u8) Error!NativeCode {
        const api = load() orelse return Error.LibUnavailable;
        var base_out: ?*anyopaque = null;
        var len_out: usize = 0;
        if (api.exec_alloc(bytes.ptr, bytes.len, &base_out, &len_out) != OK)
            return Error.AllocFailed;
        return NativeCode{ .base = base_out, .len = len_out };
    }

    /// Invoke the code through a function pointer using the SysV integer ABI:
    /// two `c_long` args in, a `c_long` result out (matches the canonical
    /// add/select routines used by the tier).
    pub fn call(self: *const NativeCode, a: c_long, b: c_long) c_long {
        const fp: *const fn (c_long, c_long) callconv(.C) c_long =
            @ptrCast(@alignCast(self.base));
        return fp(a, b);
    }

    /// Unmap the executable memory. Idempotent.
    pub fn free(self: *NativeCode) void {
        if (self.freed) return;
        const api = load() orelse return;
        api.exec_free(self.base, self.len);
        self.freed = true;
    }
};

/// The `basename` of a path (the tail after the last '/'), so an auto-name dodges
/// the 64-char C name ceiling and full-path aliasing under Core §0.4's by-name
/// registry.
fn basename(path: []const u8) []const u8 {
    var i: usize = path.len;
    while (i > 0) : (i -= 1) {
        if (path[i - 1] == '/') return path[i..];
    }
    return path;
}

/// A `deinit`-able scope value (NOT RAII — Zig has no destructors). Obtained from
/// `HwTrace.scope`; close it with `defer scope.deinit()`. It owns its own trace
/// handle and renders the executed assembly into `path()` on close.
pub const Scope = struct {
    name_buf: [64]u8 = undefined,
    name_len: usize = 0,
    handle: ?*anyopaque = null,
    emit: bool = true,
    armed: bool = false,
    truncated_flag: bool = false,
    path_buf: [4096]u8 = undefined,
    path_len: usize = 0,
    closed: bool = false,

    /// The auto-generated (or explicit) region name.
    pub fn name(self: *const Scope) []const u8 {
        return self.name_buf[0..self.name_len];
    }

    /// The rendered assembly listing (populated by `deinit`).
    pub fn path(self: *const Scope) []const u8 {
        return self.path_buf[0..self.path_len];
    }

    /// True if the close hopped OS threads / the capture overflowed (Core §0.2/§1).
    /// Meaningful after `deinit`.
    pub fn truncated(self: *const Scope) bool {
        return self.truncated_flag;
    }

    /// End the region, render the executed assembly into `path()`, and emit it (when
    /// `emit`). Reads — but does NOT free — the caller's HwTrace handle (the HwTrace
    /// owns it). Idempotent.
    pub fn deinit(self: *Scope) void {
        if (self.closed) return;
        self.closed = true;
        const api = load() orelse return;
        const nptr: [*:0]const u8 = @ptrCast(&self.name_buf);
        api.end(nptr);
        if (api.render) |r| {
            const need = r(nptr, null, 0);
            if (need > 0) {
                const want: usize = @intCast(need);
                const cap = self.path_buf.len;
                const buflen = if (want + 1 <= cap) want + 1 else cap;
                _ = r(nptr, @ptrCast(&self.path_buf), buflen);
                self.path_len = if (want < cap) want else cap - 1;
            }
        }
        if (self.handle) |h|
            self.truncated_flag = api.truncated(h) != 0;
        if (self.emit and self.path_len > 0)
            std.io.getStdOut().writeAll(self.path_buf[0..self.path_len]) catch {};
    }
};

/// A coverage recorder for a registered native region, via the hardware tier.
/// Wraps the opaque `asmtest_trace_t*` handle and the per-region begin/end
/// markers.
pub const HwTrace = struct {
    handle: ?*anyopaque,

    /// Allocate a trace with the given capacities. NOTE the C `asmtest_trace_new`
    /// takes `(insns_cap, blocks_cap)` — instructions FIRST — so this helper
    /// forwards `(instructions, blocks)` in that order while exposing the
    /// Python-style `(blocks, instructions)` argument names. Block recording is
    /// active when `blocks > 0`, instruction recording when `instructions > 0`.
    pub fn create(blocks: usize, instructions: usize) Error!HwTrace {
        const api = load() orelse return Error.LibUnavailable;
        const h = api.trace_new(instructions, blocks) orelse return Error.TraceNewFailed;
        return HwTrace{ .handle = h };
    }

    /// Register a native code range under `name`, recording into this trace.
    /// `name` must be NUL-terminated.
    pub fn register(self: *HwTrace, name: [:0]const u8, code: *const NativeCode) Error!void {
        const api = load() orelse return Error.LibUnavailable;
        if (api.register(name.ptr, code.base, code.len, self.handle) != OK)
            return Error.RegisterFailed;
    }

    /// Open hardware AUX capture for the named region on the calling thread.
    pub fn begin(self: *HwTrace, name: [:0]const u8) void {
        _ = self;
        const api = load() orelse return;
        api.begin(name.ptr);
    }

    /// Close capture for the named region and decode it into the trace.
    pub fn end(self: *HwTrace, name: [:0]const u8) void {
        _ = self;
        const api = load() orelse return;
        api.end(name.ptr);
    }

    /// Scoped begin/end around `body` — the Zig analogue of Python's
    /// `with trace.region(name):`. Guarantees the matching `end` even if `body`
    /// errors. `body` is a function taking the args tuple `args`.
    pub fn region(
        self: *HwTrace,
        name: [:0]const u8,
        args: anytype,
        comptime body: anytype,
    ) @typeInfo(@TypeOf(body)).Fn.return_type.? {
        self.begin(name);
        defer self.end(name);
        return @call(.auto, body, args);
    }

    /// A scope over the register-then-begin/end pair with the shared-core
    /// render-on-close. Zig has no destructors, so the returned `Scope` is NOT RAII —
    /// close it with `defer scope.deinit()`. Pass `@src()` at the call site (Zig has
    /// no default-arg / caller-location propagation) to auto-name the region
    /// `basename:line`. The C core flags the trace truncated on a cross-thread close
    /// (§0.2/§1). NOTE: `defer` is skipped on `panic`, so on the abnormal path the
    /// trace is simply not emitted, never emitted-partial-as-complete.
    pub fn scope(self: *HwTrace, code: *const NativeCode, src: std.builtin.SourceLocation, emit: bool) Scope {
        var s = Scope{ .handle = self.handle, .emit = emit };
        const base = basename(src.file);
        const written = std.fmt.bufPrintZ(&s.name_buf, "{s}:{d}", .{ base, src.line }) catch blk: {
            const fallback = "asmscope";
            @memcpy(s.name_buf[0..fallback.len], fallback);
            s.name_buf[fallback.len] = 0;
            break :blk s.name_buf[0..fallback.len :0];
        };
        s.name_len = written.len;
        const api = load() orelse return s;
        const nptr: [*:0]const u8 = @ptrCast(&s.name_buf);
        // Register-then-begin under the same generated name (Core §0.4 idempotent).
        _ = api.register(nptr, code.base, code.len, self.handle);
        if (api.try_begin) |tb| {
            s.armed = tb(nptr) == OK; // nonzero -> clean self-skip
        } else {
            api.begin(nptr);
            s.armed = true;
        }
        return s;
    }

    /// True if the basic block at byte offset `off` was entered.
    pub fn covered(self: *const HwTrace, off: u64) bool {
        const api = load() orelse return false;
        return api.trace_covered(self.handle, off) != 0;
    }

    /// Number of distinct basic blocks recorded so far.
    pub fn blocksLen(self: *const HwTrace) u64 {
        const api = load() orelse return 0;
        return api.blocks_len(self.handle);
    }

    /// Total instructions recorded in the ordered stream (instruction mode),
    /// including any beyond the stored window.
    pub fn insnsTotal(self: *const HwTrace) u64 {
        const api = load() orelse return 0;
        return api.insns_total(self.handle);
    }

    /// Number of instruction offsets actually stored (insns_len <= insns_total).
    pub fn insnsLen(self: *const HwTrace) u64 {
        const api = load() orelse return 0;
        return api.insns_len(self.handle);
    }

    /// True if the instruction stream overflowed its capacity (entries dropped).
    pub fn truncated(self: *const HwTrace) bool {
        const api = load() orelse return false;
        return api.truncated(self.handle) != 0;
    }

    /// The distinct basic-block start offsets recorded, in first-seen order.
    /// Returns an allocator-owned slice the caller must `free`; an empty slice if
    /// the tier is unavailable.
    pub fn blockOffsets(self: *const HwTrace, allocator: std.mem.Allocator) ![]u64 {
        const api = load() orelse return allocator.alloc(u64, 0);
        const n: usize = @intCast(api.blocks_len(self.handle));
        const out = try allocator.alloc(u64, n);
        for (out, 0..) |*slot, i| slot.* = api.block_at(self.handle, i);
        return out;
    }

    /// The ordered instruction-offset stream actually stored (insns_len entries,
    /// in execution order — not the possibly-larger insns_total). Returns an
    /// allocator-owned slice the caller must `free`; an empty slice if the tier is
    /// unavailable.
    pub fn insnOffsets(self: *const HwTrace, allocator: std.mem.Allocator) ![]u64 {
        const api = load() orelse return allocator.alloc(u64, 0);
        const n: usize = @intCast(api.insns_len(self.handle));
        const out = try allocator.alloc(u64, n);
        for (out, 0..) |*slot, i| slot.* = api.insn_at(self.handle, i);
        return out;
    }

    /// Release the trace handle. Idempotent.
    pub fn free(self: *HwTrace) void {
        if (self.handle) |h| {
            const api = load() orelse {
                self.handle = null;
                return;
            };
            api.trace_free(h);
            self.handle = null;
        }
    }
};

/// An executable code region's extent, as returned by `procRegionByAddr` /
/// `procPerfmapSymbol`: the mapping/method base address and its byte length —
/// the `(base, len)` to hand `ptraceTraceAttached`. Mirrors the Python wrappers'
/// `(base, length)` tuple.
pub const Region = struct {
    base: usize,
    len: usize,
};

/// Out-of-process / foreign-process tracing (`asmtest_ptrace.h`): single-step a
/// forked or externally-attached target out of band, and resolve the code region
/// to trace from the OS — `/proc/<pid>/maps`, a JIT perf-map, or a binary
/// jitdump. The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is
/// unavailable and in-process DynamoRIO cannot seize the runtime's threads).
/// Linux x86-64.
///
/// Wraps the same `libasmtest_hwtrace` the `HwTrace` tier loads, so the same
/// self-skip applies: gate on `Ptrace.available()` (it is `false` when the lib or
/// the backend is absent) before tracing. The Zig analogue of Python's `Ptrace`.
pub const Ptrace = struct {
    /// True if the out-of-process single-step tracer can run on this host: the
    /// hwtrace lib opens AND `asmtest_ptrace_available()` reports a Linux x86-64
    /// host. Never errors, so callers (and the test) self-skip cleanly. Mirrors
    /// `Ptrace.available()`.
    pub fn available() bool {
        const api = load() orelse return false;
        return api.ptrace_available() != 0;
    }

    /// Human-readable reason `available()` is false (or "available"), written into
    /// `buf` (always NUL-terminated). Returns the slice up to the NUL. If the lib
    /// can't load, reports that so the self-skip message is still useful. Mirrors
    /// `Ptrace.skip_reason()`.
    pub fn skipReason(buf: []u8) []const u8 {
        const api = load() orelse {
            const msg = "libasmtest_hwtrace not loadable";
            const n = @min(msg.len, buf.len);
            @memcpy(buf[0..n], msg[0..n]);
            return buf[0..n];
        };
        api.ptrace_skip_reason(buf.ptr, buf.len);
        return std.mem.sliceTo(buf, 0);
    }

    /// Fork a tracee that calls the code range at `code_ptr` (`code_len` bytes,
    /// already executable in this process — e.g. a `NativeCode`) with up to six
    /// integer `args`, single-step it OUT OF PROCESS, and fill `trace`; returns
    /// the routine's return value (the child's RAX at the `ret`). Returns
    /// `Error.LibUnavailable` if the lib isn't loadable, `Error.TraceFailed` on a
    /// nonzero C return — gate on `available()` first. Mirrors `Ptrace.trace_call()`.
    pub fn traceCall(
        code_ptr: ?*anyopaque,
        code_len: usize,
        args: []const i64,
        trace: *const HwTrace,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        // `i64` and `c_long` are the same width on x86-64 Linux, but reinterpret
        // explicitly so the FFI sees the C `long` the contract declares.
        const cargs: [*c]const c_long = @ptrCast(args.ptr);
        var result: c_long = 0;
        const rc = api.ptrace_trace_call(
            code_ptr,
            code_len,
            cargs,
            @intCast(args.len),
            &result,
            trace.handle,
        );
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Trace a region `[base, base+len)` in a SEPARATE, already-ptrace-stopped
    /// process `pid` (the caller owns PTRACE_ATTACH/DETACH); reads the target's
    /// bytes via `process_vm_readv` and returns the region's return value (the
    /// target's RAX at the `ret`). Returns `Error.LibUnavailable` if the lib isn't
    /// loadable, `Error.TraceFailed` on a nonzero C return. Mirrors
    /// `Ptrace.trace_attached()`.
    pub fn traceAttached(
        pid: c_int,
        base: ?*const anyopaque,
        len: usize,
        trace: *const HwTrace,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        var result: c_long = 0;
        const rc = api.ptrace_trace_attached(pid, base, len, &result, trace.handle);
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Like `traceAttached`, but VERSION-AWARE: instead of one late
    /// `process_vm_readv` snapshot (wrong the moment the JIT patches or frees the
    /// region), it consults a code-image timeline (`img`, a `CodeImage`) for the
    /// bytes that were live at logical timestamp `when` (`when == 0` => the latest
    /// version) — the fix for tracing a region whose address was reused mid-run.
    /// Returns the target's RAX at the `ret`. Returns `Error.LibUnavailable` if the
    /// lib isn't loadable, `Error.TraceFailed` on a nonzero C return. Mirrors
    /// `Ptrace.trace_attached_versioned()`.
    pub fn traceAttachedVersioned(
        pid: c_int,
        base: ?*const anyopaque,
        len: usize,
        img: *const CodeImage,
        when: u64,
        trace: *const HwTrace,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        var result: c_long = 0;
        const rc = api.ptrace_trace_attached_versioned(pid, base, len, img.handle, when, &result, trace.handle);
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Descending variant of `traceCall`: thread a `Descent` handle through the
    /// single-step loop so the call-outs are recorded as edges and (at level >= 2)
    /// descended as nested frames. `trace` (the flat frame-0 view) may be `null` to
    /// record only into `descent`, and `descent` may be `null` to reproduce
    /// `traceCall` exactly. CRITICAL: `region` is the traced region's byte length —
    /// pass it (not the whole allocation) when the call target is an in-blob sibling
    /// that must stay OUTSIDE the traced region, else the sibling falls inside the
    /// region and mis-records as recursion. `region == null` defaults to `code_len`
    /// (the whole allocation). Mirrors `Ptrace.trace_call_ex()`.
    pub fn traceCallEx(
        code_ptr: ?*anyopaque,
        code_len: usize,
        args: []const i64,
        trace: ?*const HwTrace,
        descent: ?*const Descent,
        region: ?usize,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        const cargs: [*c]const c_long = @ptrCast(args.ptr);
        var result: c_long = 0;
        const th: ?*anyopaque = if (trace) |t| t.handle else null;
        const dh: ?*anyopaque = if (descent) |d| d.handle else null;
        const rc = api.ptrace_trace_call_ex(
            code_ptr,
            region orelse code_len,
            cargs,
            @intCast(args.len),
            &result,
            th,
            dh,
        );
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Descending variant of `traceAttached` for an externally-attached process:
    /// thread a `Descent` handle (`null` reproduces `traceAttached`) through the
    /// loop over `[base, base+len)`. `trace` may be `null` to record only into
    /// `descent`. Mirrors `Ptrace.trace_attached_ex()`.
    pub fn traceAttachedEx(
        pid: c_int,
        base: ?*const anyopaque,
        len: usize,
        trace: ?*const HwTrace,
        descent: ?*const Descent,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        var result: c_long = 0;
        const th: ?*anyopaque = if (trace) |t| t.handle else null;
        const dh: ?*anyopaque = if (descent) |d| d.handle else null;
        const rc = api.ptrace_trace_attached_ex(pid, base, len, &result, th, dh);
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Descending, version-aware variant of `traceAttached`: consult the code-image
    /// timeline `img` for the bytes live at logical timestamp `when` (`when == 0` =>
    /// latest) and thread a `Descent` handle (`null` reproduces
    /// `traceAttachedVersioned`) through the loop. `trace` may be `null` to record
    /// only into `descent`. Mirrors `Ptrace.trace_attached_versioned_ex()`.
    pub fn traceAttachedVersionedEx(
        pid: c_int,
        base: ?*const anyopaque,
        len: usize,
        img: *const CodeImage,
        when: u64,
        trace: ?*const HwTrace,
        descent: ?*const Descent,
    ) Error!i64 {
        const api = load() orelse return Error.LibUnavailable;
        var result: c_long = 0;
        const th: ?*anyopaque = if (trace) |t| t.handle else null;
        const dh: ?*anyopaque = if (descent) |d| d.handle else null;
        const rc = api.ptrace_trace_attached_versioned_ex(pid, base, len, img.handle, when, &result, th, dh);
        if (rc != ASMTEST_PTRACE_OK) return Error.TraceFailed;
        return @intCast(result);
    }

    /// Run an already-attached, ptrace-stopped target `pid` forward until it reaches
    /// `addr` (a software breakpoint that fires when the program itself next calls in),
    /// leaving it stopped there ready for `traceAttached` — the step that makes a
    /// resolved JIT method traceable when you don't control call timing. Returns the
    /// status code (`ASMTEST_PTRACE_OK`, or `ASMTEST_PTRACE_ENOENT` if the target
    /// exited first), or `Error.LibUnavailable` if the lib isn't loadable. The caller
    /// owns PTRACE_ATTACH/DETACH. Mirrors `Ptrace.run_to()`.
    pub fn runTo(pid: c_int, addr: usize) Error!c_int {
        const api = load() orelse return Error.LibUnavailable;
        return api.ptrace_run_to(pid, @ptrFromInt(addr));
    }

    /// The executable mapping in `/proc/<pid>/maps` that CONTAINS `addr`, as a
    /// `Region` (`base`, `len`), or `null` if no executable mapping contains it
    /// (or the lib is absent). A pure file read; no ptrace, so it may be called
    /// before attaching. Mirrors `Ptrace.region_by_addr()`.
    pub fn regionByAddr(pid: c_int, addr: usize) ?Region {
        const api = load() orelse return null;
        var base: ?*anyopaque = null;
        var len: usize = 0;
        const rc = api.proc_region_by_addr(pid, @ptrFromInt(addr), &base, &len);
        if (rc != ASMTEST_PTRACE_OK) return null;
        return Region{ .base = @intFromPtr(base), .len = len };
    }

    /// A JIT method by `name` in the perf map at `/tmp/perf-<pid>.map`, as a
    /// `Region` (`base`, `len`), or `null` (no such symbol, no map file, or the
    /// lib is absent). `name` (NUL-terminated) matches the full symbol text after
    /// the size field. Mirrors `Ptrace.perfmap_symbol()`.
    pub fn perfmapSymbol(pid: c_int, name: [:0]const u8) ?Region {
        const api = load() orelse return null;
        var base: ?*anyopaque = null;
        var len: usize = 0;
        const rc = api.proc_perfmap_symbol(pid, name.ptr, &base, &len);
        if (rc != ASMTEST_PTRACE_OK) return null;
        return Region{ .base = @intFromPtr(base), .len = len };
    }

    /// A JIT method by `name` from a binary jitdump (`path`, or
    /// `/tmp/jit-<pid>.dump` when `path` is `null`) as a `JitMethod`, or `null`
    /// (no such method, no file, not a jitdump, or the lib is absent). The latest
    /// re-JIT body (highest timestamp) wins. Up to `out_bytes` of the recorded
    /// code is copied into the caller-provided buffer, and the returned
    /// `JitMethod.code` is the `out_bytes[0..bytes_len]` sub-slice of it (empty
    /// when `out_bytes` is empty) — the same caller-buffer idiom as `skipReason`.
    /// Mirrors `Ptrace.jitdump_find()`.
    pub fn jitdumpFind(
        path: ?[:0]const u8,
        name: [:0]const u8,
        pid: c_int,
        out_bytes: []u8,
    ) ?JitMethod {
        const api = load() orelse return null;
        const path_ptr: [*c]const u8 = if (path) |p| p.ptr else null;
        var entry: JitEntry = .{ .code_addr = 0, .code_size = 0, .timestamp = 0, .code_index = 0 };
        const want = out_bytes.len;
        var bytes_len: usize = 0;
        const buf_ptr: [*c]u8 = if (want != 0) out_bytes.ptr else null;
        const blen_ptr: ?*usize = if (want != 0) &bytes_len else null;
        const rc = api.jitdump_find(path_ptr, pid, name.ptr, &entry, buf_ptr, want, blen_ptr);
        if (rc != ASMTEST_PTRACE_OK) return null;
        return JitMethod{
            .code_addr = entry.code_addr,
            .code_size = entry.code_size,
            .timestamp = entry.timestamp,
            .code_index = entry.code_index,
            .code = if (want != 0) out_bytes[0..bytes_len] else &.{},
        };
    }
};

/// A recorded (call-site -> callee) edge for a stepped-over call (level >= 1): the
/// call-site byte `site` within its frame, the ABSOLUTE `target` address of the
/// callee, and the caller `depth` (0 = frame 0). Mirrors the Python `edges()`
/// tuples.
pub const Edge = struct {
    site: u64,
    target: u64,
    depth: u32,
};

/// Call descent (`asmtest_descent_t`): configure how the ptrace stepper handles the
/// call-outs it would otherwise step over, and read back the recorded edges + nested
/// frames. Four levels (see the `DESCENT_*` constants / `DescentLevel`): OFF,
/// RECORD_EDGES, DESCEND_KNOWN, DESCEND_ALL. Pass to `Ptrace.traceCallEx` and
/// friends. Frame 0 is the root region (a superset of the flat trace); descended
/// callees are frames 1..N.
///
/// Wraps the opaque `asmtest_descent_t*` and resolves the same `libasmtest_hwtrace`
/// the rest of this module loads. `deinit` NULLs the handle so a double free is a
/// no-op (the native side is also idempotent), mirroring the trace-handle
/// discipline in this binding. The Zig analogue of Python's `Descent`.
///
/// NOTE: Zig is allow-set-only. `std.DynLib` gives raw C function pointers with no
/// place to anchor a GC-safe capturing trampoline, so the level-2/3 upcall
/// installers (`set_resolver` / `set_denylist`) are intentionally not offered —
/// configure descent through `allowRegion` / `denyRegion` instead (the allow-set
/// path the C engine also drives). The two FFI symbols carry exemption lines in
/// scripts/bindings-parity-allow.txt.
pub const Descent = struct {
    handle: ?*anyopaque,

    /// Allocate a descent handle at `level` (conservative defaults for
    /// depth/budget/watchdog; empty allow-set/deny-set). Returns
    /// `Error.LibUnavailable` if the tier isn't loadable, `Error.AllocFailed` on a
    /// NULL C return. Mirrors `asmtest_descent_new()`.
    pub fn init(level: DescentLevel) Error!Descent {
        const api = load() orelse return Error.LibUnavailable;
        const h = api.descent_new(@intFromEnum(level)) orelse return Error.AllocFailed;
        return Descent{ .handle = h };
    }

    /// Free the descent handle. Idempotent: NULLs the handle so a double free (or a
    /// free after the lib unloaded) is a no-op. Mirrors `asmtest_descent_free()`.
    pub fn deinit(self: *Descent) void {
        if (self.handle) |h| {
            const api = load() orelse {
                self.handle = null;
                return;
            };
            api.descent_free(h);
            self.handle = null;
        }
    }

    // ---- configuration (in) ---- //

    /// Ceiling on nested descent depth (frame 0 is depth 0). 0 restores the default.
    pub fn setMaxDepth(self: *Descent, max_depth: u32) void {
        const api = load() orelse return;
        api.descent_set_max_depth(self.handle, max_depth);
    }

    /// Total single-step instruction budget across all descended frames; 0 = default.
    pub fn setInsnBudget(self: *Descent, budget: u64) void {
        const api = load() orelse return;
        api.descent_set_insn_budget(self.handle, budget);
    }

    /// Real-time watchdog in milliseconds for a descended run; 0 = default.
    pub fn setWatchdogMs(self: *Descent, ms: u32) void {
        const api = load() orelse return;
        api.descent_set_watchdog_ms(self.handle, ms);
    }

    /// Add `[base, base+len)` to the level-2 allow-set (descend into calls landing
    /// inside). Returns 0 on success, negative on OOM (or if the lib is absent).
    pub fn allowRegion(self: *Descent, base: ?*const anyopaque, len: usize) c_int {
        const api = load() orelse return -1;
        return api.descent_allow_region(self.handle, base, len);
    }

    /// Add `[base, base+len)` to the level-3 deny-set (never descend into it).
    /// Returns 0 on success, negative on OOM (or if the lib is absent).
    pub fn denyRegion(self: *Descent, base: ?*const anyopaque, len: usize) c_int {
        const api = load() orelse return -1;
        return api.descent_deny_region(self.handle, base, len);
    }

    // ---- results (out) ---- //

    /// Number of recorded (call-site -> callee) edges (level >= 1).
    pub fn edgesLen(self: *const Descent) usize {
        const api = load() orelse return 0;
        return api.descent_edges_len(self.handle);
    }

    /// The call-site byte offset of edge `i`.
    pub fn edgeSite(self: *const Descent, i: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_edge_site(self.handle, i);
    }

    /// The ABSOLUTE target address of edge `i`.
    pub fn edgeTarget(self: *const Descent, i: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_edge_target(self.handle, i);
    }

    /// The caller depth of edge `i` (0 = frame 0).
    pub fn edgeDepth(self: *const Descent, i: usize) u32 {
        const api = load() orelse return 0;
        return api.descent_edge_depth(self.handle, i);
    }

    /// Every recorded edge as an allocator-owned `[]Edge` the caller must `free`;
    /// an empty slice if the tier is unavailable. Mirrors Python's `edges()`.
    pub fn edges(self: *const Descent, allocator: std.mem.Allocator) ![]Edge {
        const api = load() orelse return allocator.alloc(Edge, 0);
        const n = api.descent_edges_len(self.handle);
        const out = try allocator.alloc(Edge, n);
        for (out, 0..) |*slot, i| slot.* = .{
            .site = api.descent_edge_site(self.handle, i),
            .target = api.descent_edge_target(self.handle, i),
            .depth = api.descent_edge_depth(self.handle, i),
        };
        return out;
    }

    /// Number of recorded frames (frame 0 plus every descended callee).
    pub fn framesLen(self: *const Descent) usize {
        const api = load() orelse return 0;
        return api.descent_frames_len(self.handle);
    }

    /// The ABSOLUTE base address of frame `f`.
    pub fn frameBase(self: *const Descent, f: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_frame_base(self.handle, f);
    }

    /// The byte length of frame `f`.
    pub fn frameLen(self: *const Descent, f: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_frame_len(self.handle, f);
    }

    /// The nesting depth of frame `f` (0 = frame 0).
    pub fn frameDepth(self: *const Descent, f: usize) u32 {
        const api = load() orelse return 0;
        return api.descent_frame_depth(self.handle, f);
    }

    /// The parent frame index of frame `f`, or -1 for the root (frame 0).
    pub fn frameParent(self: *const Descent, f: usize) i32 {
        const api = load() orelse return -1;
        return api.descent_frame_parent(self.handle, f);
    }

    /// Number of instruction offsets recorded in frame `f`.
    pub fn frameInsnCount(self: *const Descent, f: usize) usize {
        const api = load() orelse return 0;
        return api.descent_frame_insn_count(self.handle, f);
    }

    /// The `i`th instruction byte offset recorded in frame `f`.
    pub fn frameInsnAt(self: *const Descent, f: usize, i: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_frame_insn_at(self.handle, f, i);
    }

    /// Number of basic-block offsets recorded in frame `f`.
    pub fn frameBlockCount(self: *const Descent, f: usize) usize {
        const api = load() orelse return 0;
        return api.descent_frame_block_count(self.handle, f);
    }

    /// The `i`th basic-block byte offset recorded in frame `f`.
    pub fn frameBlockAt(self: *const Descent, f: usize, i: usize) u64 {
        const api = load() orelse return 0;
        return api.descent_frame_block_at(self.handle, f, i);
    }

    /// Frame `f`'s instruction-offset stream as an allocator-owned `[]u64` the
    /// caller must `free`; an empty slice if the tier is unavailable. Mirrors
    /// Python's `frame_insns(f)`.
    pub fn frameInsns(self: *const Descent, allocator: std.mem.Allocator, f: usize) ![]u64 {
        const api = load() orelse return allocator.alloc(u64, 0);
        const n = api.descent_frame_insn_count(self.handle, f);
        const out = try allocator.alloc(u64, n);
        for (out, 0..) |*slot, i| slot.* = api.descent_frame_insn_at(self.handle, f, i);
        return out;
    }

    /// Frame `f`'s basic-block-offset stream as an allocator-owned `[]u64` the caller
    /// must `free`; an empty slice if the tier is unavailable. Mirrors Python's
    /// `frame_blocks(f)`.
    pub fn frameBlocks(self: *const Descent, allocator: std.mem.Allocator, f: usize) ![]u64 {
        const api = load() orelse return allocator.alloc(u64, 0);
        const n = api.descent_frame_block_count(self.handle, f);
        const out = try allocator.alloc(u64, n);
        for (out, 0..) |*slot, i| slot.* = api.descent_frame_block_at(self.handle, f, i);
        return out;
    }

    /// True if a pool overflowed / a byte failed to decode (the record is incomplete).
    pub fn truncated(self: *const Descent) bool {
        const api = load() orelse return false;
        return api.descent_truncated(self.handle) != 0;
    }

    /// True if descent stopped at a policy limit (max_depth / budget / recursion
    /// cap) — distinct from a pool overflow (`truncated`).
    pub fn depthCapped(self: *const Descent) bool {
        const api = load() orelse return false;
        return api.descent_depth_capped(self.handle) != 0;
    }
};

/// Time-aware code-image recorder (`asmtest_codeimage.h`): a userspace
/// PERF_RECORD_TEXT_POKE. A single `process_vm_readv` snapshot of a live JIT
/// region is wrong the moment the code is patched, freed, or its address reused
/// mid-trace; this keeps a TIMESTAMPED CODE-IMAGE TIMELINE so a branch-trace
/// decoder (or `Ptrace.traceAttachedVersioned`) can ask "what bytes were live at
/// `addr` as of logical timestamp `when`". Works on a FOREIGN process (the JIT
/// case) or this one (`init(0)`); change detection is pure userspace and
/// arch-independent (soft-dirty / PAGEMAP_SCAN).
///
/// Wraps the opaque `asmtest_codeimage_t*` and resolves the same
/// `libasmtest_hwtrace` the rest of this module loads, so the same self-skip
/// applies: gate on `CodeImage.available()` before recording. The Zig analogue of
/// Python's `CodeImage`.
pub const CodeImage = struct {
    handle: ?*anyopaque,

    /// True if the userspace recorder can detect page changes on this host
    /// (PAGEMAP_SCAN, or the soft-dirty fallback): the hwtrace lib opens AND
    /// `asmtest_codeimage_available()` reports the detect chain passes. Never
    /// errors, so callers (and the test) self-skip cleanly. Mirrors
    /// `asmtest_codeimage_available()`.
    pub fn available() bool {
        const api = load() orelse return false;
        return api.ci_available() != 0;
    }

    /// Human-readable reason `available()` is false (or "available"), written into
    /// `buf` (always NUL-terminated). Returns the slice up to the NUL. If the lib
    /// can't load, reports that so the self-skip message is still useful. Mirrors
    /// `asmtest_codeimage_skip_reason()`.
    pub fn skipReason(buf: []u8) []const u8 {
        const api = load() orelse {
            const msg = "libasmtest_hwtrace not loadable";
            const n = @min(msg.len, buf.len);
            @memcpy(buf[0..n], msg[0..n]);
            return buf[0..n];
        };
        api.ci_skip_reason(buf.ptr, buf.len);
        return std.mem.sliceTo(buf, 0);
    }

    /// Create a timeline recording `pid`'s memory (`pid == 0` => this process).
    /// Returns `Error.LibUnavailable` if the lib isn't loadable,
    /// `Error.AllocFailed` on a NULL C return. Mirrors `asmtest_codeimage_new()`.
    pub fn init(pid: c_int) Error!CodeImage {
        const api = load() orelse return Error.LibUnavailable;
        const h = api.ci_new(pid) orelse return Error.AllocFailed;
        return CodeImage{ .handle = h };
    }

    /// Free the timeline and all recorded versions (detaches any eBPF watch).
    /// Idempotent. Mirrors `asmtest_codeimage_free()`.
    pub fn deinit(self: *CodeImage) void {
        if (self.handle) |h| {
            const api = load() orelse {
                self.handle = null;
                return;
            };
            api.ci_free(h);
            self.handle = null;
        }
    }

    /// Begin tracking `[base, base+len)`: snapshot version 0 now and arm
    /// write-protect on its pages. May be called for several disjoint regions.
    /// Returns the C status (`ASMTEST_CI_OK`, or a negative status). Mirrors
    /// `asmtest_codeimage_track()`.
    pub fn track(self: *CodeImage, base: ?*const anyopaque, len: usize) c_int {
        const api = load() orelse return ASMTEST_CI_ENOENT;
        return api.ci_track(self.handle, base, len);
    }

    /// Scan the tracked ranges for changed pages, re-snapshot each as a NEW
    /// version, and re-arm. Returns the number of new versions recorded (>= 0), or
    /// a negative status. Mirrors `asmtest_codeimage_refresh()`.
    pub fn refresh(self: *CodeImage) c_int {
        const api = load() orelse return ASMTEST_CI_ENOENT;
        return api.ci_refresh(self.handle);
    }

    /// The current capture sequence — a monotonic logical timestamp. Advances by
    /// one for every version recorded (track + each refresh change); 0 before
    /// anything is tracked. Mirrors `asmtest_codeimage_now()`.
    pub fn now(self: *const CodeImage) u64 {
        const api = load() orelse return 0;
        return api.ci_now(self.handle);
    }

    /// The bytes live at `addr` as of capture sequence `when` (`when == 0` => the
    /// latest version), as a borrowed slice owned by the timeline (valid until
    /// `deinit`), or `null` if `addr` is not in any tracked region / no version
    /// exists at-or-before `when` (`ASMTEST_CI_ENOENT`) or the lib is absent. The
    /// slice runs from `addr` to the end of that version's region. Mirrors
    /// `asmtest_codeimage_bytes_at()`.
    pub fn bytesAt(self: *const CodeImage, addr: ?*const anyopaque, when: u64) ?[]const u8 {
        const api = load() orelse return null;
        var out: [*c]const u8 = null;
        var out_len: usize = 0;
        const rc = api.ci_bytes_at(self.handle, addr, when, &out, &out_len);
        if (rc != ASMTEST_CI_OK) return null;
        if (out == null) return null;
        return out[0..out_len];
    }

    /// True if the optional eBPF emission detector can load and attach on this host
    /// (built with libbpf, kernel BTF present, sufficient privilege), else false.
    /// Self-skips cleanly. Mirrors `asmtest_codeimage_bpf_available()`.
    pub fn bpfAvailable() bool {
        const api = load() orelse return false;
        return api.ci_bpf_available() != 0;
    }

    /// Human-readable reason `bpfAvailable()` is false (or "available"), written
    /// into `buf` (always NUL-terminated). Returns the slice up to the NUL.
    /// Mirrors `asmtest_codeimage_bpf_skip_reason()`.
    pub fn bpfSkipReason(buf: []u8) []const u8 {
        const api = load() orelse {
            const msg = "libasmtest_hwtrace not loadable";
            const n = @min(msg.len, buf.len);
            @memcpy(buf[0..n], msg[0..n]);
            return buf[0..n];
        };
        api.ci_bpf_skip_reason(buf.ptr, buf.len);
        return std.mem.sliceTo(buf, 0);
    }

    /// Load the CO-RE program, filter it to this timeline's pid, and attach it so
    /// `pollBpf` can drain emission events. Returns the C status (`ASMTEST_CI_OK`,
    /// or a negative status — e.g. `ENOSYS` without libbpf). Mirrors
    /// `asmtest_codeimage_watch_bpf()`.
    pub fn watchBpf(self: *CodeImage) c_int {
        const api = load() orelse return ASMTEST_CI_ENOENT;
        return api.ci_watch_bpf(self.handle);
    }

    /// Drain ready emission events from the BPF ring buffer into the internal
    /// queue. `timeout_ms == 0` is a NON-BLOCKING drain; `> 0` waits up to that
    /// long. Returns the number of events queued (>= 0) or a negative status.
    /// Mirrors `asmtest_codeimage_poll_bpf()`.
    pub fn pollBpf(self: *CodeImage, timeout_ms: c_int) c_int {
        const api = load() orelse return ASMTEST_CI_ENOENT;
        return api.ci_poll_bpf(self.handle, timeout_ms);
    }

    /// Pop one queued emission event, or `null` if the queue is empty / on a
    /// negative status / the lib is absent. Mirrors `asmtest_codeimage_next()`.
    pub fn nextEvent(self: *CodeImage) ?Event {
        const api = load() orelse return null;
        var out: Event = undefined;
        if (api.ci_next(self.handle, &out) != 1) return null;
        return out;
    }
};
