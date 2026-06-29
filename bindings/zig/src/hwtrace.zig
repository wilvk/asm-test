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

/// Errors surfaced by the wrapper. `LibUnavailable` means the tier can't run
/// (lib missing or the backend self-skips) — callers should self-skip on it; the
/// rest map nonzero C return codes onto an error.
pub const Error = error{
    LibUnavailable,
    InitFailed,
    AllocFailed,
    RegisterFailed,
    TraceNewFailed,
};

// ---- function-pointer prototypes (mirror the exported C symbols) ---- //
const FnAvailable = *const fn (c_int) callconv(.C) c_int;
const FnSkipReason = *const fn (c_int, [*c]u8, usize) callconv(.C) void;
const FnResolve = *const fn (c_int, [*]c_int, usize) callconv(.C) usize;
const FnAuto = *const fn (c_int) callconv(.C) c_int;
const FnInit = *const fn (*const c.asmtest_hwtrace_options_t) callconv(.C) c_int;
const FnRegister = *const fn ([*:0]const u8, ?*anyopaque, usize, ?*anyopaque) callconv(.C) c_int;
const FnMarker = *const fn ([*:0]const u8) callconv(.C) void;
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

/// The resolved entry points. Populated once on first `load()`; held in a
/// process-global because the DynLib handle and its symbols outlive any one
/// trace (mirroring how Python keeps a module-level `_lib`).
const Api = struct {
    lib: std.DynLib,
    available: FnAvailable,
    skip_reason: FnSkipReason,
    resolve: FnResolve,
    auto: FnAuto,
    init: FnInit,
    register: FnRegister,
    begin: FnMarker,
    end: FnMarker,
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
};

var g_api: ?Api = null;
var g_load_failed = false;

fn libName() [*:0]const u8 {
    return if (builtin.os.tag == .macos) "libasmtest_hwtrace.dylib" else "libasmtest_hwtrace.so";
}

/// Resolve the hwtrace library path: `$ASMTEST_HWTRACE_LIB` if set, else
/// `<repo>/build/libasmtest_hwtrace.so`. At runtime we only have an env var or
/// the bare name, so fall back to the conventional repo build dir relative to
/// cwd, then the plain soname (resolved via the loader search path), when no env
/// var is present.
fn openLib(buf: []u8) ?std.DynLib {
    if (std.posix.getenv("ASMTEST_HWTRACE_LIB")) |p| {
        // env-provided absolute/relative path; pass it through verbatim.
        const z = std.fmt.bufPrintZ(buf, "{s}", .{p}) catch return null;
        return std.DynLib.open(z) catch null;
    }
    // No env var: try the conventional repo build dir relative to cwd, then the
    // bare soname (loader search path). `make zig-test`/callers normally set the
    // env var, so this is a best-effort convenience.
    const z = std.fmt.bufPrintZ(buf, "build/{s}", .{libName()}) catch return null;
    if (std.DynLib.open(z)) |lib| return lib else |_| {}
    const z2 = std.fmt.bufPrintZ(buf, "{s}", .{libName()}) catch return null;
    return std.DynLib.open(z2) catch null;
}

fn lookupAll(lib: *std.DynLib) ?Api {
    // Every symbol must resolve; a partial lib is treated as unavailable.
    return Api{
        .lib = lib.*,
        .available = lib.lookup(FnAvailable, "asmtest_hwtrace_available") orelse return null,
        .skip_reason = lib.lookup(FnSkipReason, "asmtest_hwtrace_skip_reason") orelse return null,
        .resolve = lib.lookup(FnResolve, "asmtest_hwtrace_resolve") orelse return null,
        .auto = lib.lookup(FnAuto, "asmtest_hwtrace_auto") orelse return null,
        .init = lib.lookup(FnInit, "asmtest_hwtrace_init") orelse return null,
        .register = lib.lookup(FnRegister, "asmtest_hwtrace_register_region") orelse return null,
        .begin = lib.lookup(FnMarker, "asmtest_hwtrace_begin") orelse return null,
        .end = lib.lookup(FnMarker, "asmtest_hwtrace_end") orelse return null,
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
/// drops the depth-bounded backend (AMD LBR). Mirrors `HwTrace.resolve()`.
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
