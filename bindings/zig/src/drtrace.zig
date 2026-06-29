//! In-process native runtime tracing for Zig, backed by DynamoRIO (Track Z).
//!
//! This is the Zig analogue of `bindings/python/asmtest/drtrace.py`: the
//! language-wrapper surface for the optional DynamoRIO native-trace tier (see
//! `include/asmtest_drtrace.h` and `docs/native-tracing.md`). Where the emulator
//! tier (`conformance.zig`'s `emu_*`) traces isolated guest bytes, `NativeTrace`
//! traces host-native code as it runs **inside this process**: bring DynamoRIO
//! up once, materialize host-native machine code, mark a region, call into it,
//! and read back basic-block coverage / the instruction stream.
//!
//! The drapp library needs DynamoRIO and may be absent, so — unlike the rest of
//! the binding, which `linkSystemLibrary`s at build time — this module dlopen()s
//! `libasmtest_drapp` at RUNTIME via `std.DynLib` and resolves each symbol with
//! `lib.lookup`. Nothing here links DynamoRIO: `libdynamorio` is dlopen()ed
//! lazily by the C side after the client is configured. The library path comes
//! from `$ASMTEST_DRAPP_LIB`, else `<repo>/build/libasmtest_drapp.so`.
//!
//! `available()` reports whether the tier can run (the lib opens AND
//! `asmtest_dr_available() != 0`) so callers self-skip cleanly. Only the struct
//! *types* are taken from the C header via `@cImport`; the *functions* are
//! resolved through the DynLib handle.
//!
//! Targets Zig 0.13.x (matching build.zig / build.zig.zon).
const std = @import("std");
const builtin = @import("builtin");

// Struct *types* only — the functions are resolved at runtime via DynLib.
const c = @cImport({
    @cInclude("asmtest_drtrace.h");
});

/// Success status from the lifecycle / registration calls (ASMTEST_DR_OK).
pub const OK: c_int = 0;

/// Errors surfaced by the wrapper. `LibUnavailable` means the tier can't run
/// (lib missing or DynamoRIO unresolvable) — callers should self-skip on it;
/// the rest map nonzero C return codes onto an error.
pub const Error = error{
    LibUnavailable,
    InitFailed,
    StartFailed,
    AllocFailed,
    RegisterFailed,
    TraceNewFailed,
};

// ---- function-pointer prototypes (mirror the exported C symbols) ---- //
const FnAvailable = *const fn () callconv(.C) c_int;
const FnInit = *const fn (*const c.asmtest_drtrace_options_t) callconv(.C) c_int;
const FnStart = *const fn () callconv(.C) c_int;
const FnStop = *const fn () callconv(.C) c_int;
const FnShutdown = *const fn () callconv(.C) void;
const FnRegister = *const fn ([*:0]const u8, ?*anyopaque, usize, ?*anyopaque) callconv(.C) c_int;
const FnUnregister = *const fn ([*:0]const u8) callconv(.C) c_int;
const FnMarker = *const fn ([*:0]const u8) callconv(.C) void;
const FnMarkerError = *const fn () callconv(.C) c_int;
const FnExecAlloc = *const fn ([*]const u8, usize, *c.asmtest_exec_code_t) callconv(.C) c_int;
const FnExecFree = *const fn (*c.asmtest_exec_code_t) callconv(.C) void;
const FnTraceNew = *const fn (usize, usize) callconv(.C) ?*anyopaque;
const FnTraceFree = *const fn (?*anyopaque) callconv(.C) void;
const FnTraceCovered = *const fn (?*anyopaque, u64) callconv(.C) c_int;
const FnBlocksLen = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnInsnsTotal = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnInsnsLen = *const fn (?*anyopaque) callconv(.C) c_ulonglong;
const FnBlockAt = *const fn (?*anyopaque, usize) callconv(.C) c_ulonglong;
const FnInsnAt = *const fn (?*anyopaque, usize) callconv(.C) c_ulonglong;

/// The resolved entry points. Populated once on first `load()`; held in a
/// process-global because the DynLib handle and its symbols outlive any one
/// trace (mirroring how Python keeps a module-level `_lib`).
const Api = struct {
    lib: std.DynLib,
    available: FnAvailable,
    init: FnInit,
    start: FnStart,
    stop: FnStop,
    shutdown: FnShutdown,
    register: FnRegister,
    unregister: FnUnregister,
    begin: FnMarker,
    end: FnMarker,
    marker_error: FnMarkerError,
    exec_alloc: FnExecAlloc,
    exec_free: FnExecFree,
    trace_new: FnTraceNew,
    trace_free: FnTraceFree,
    trace_covered: FnTraceCovered,
    blocks_len: FnBlocksLen,
    insns_total: FnInsnsTotal,
    insns_len: FnInsnsLen,
    block_at: FnBlockAt,
    insn_at: FnInsnAt,
};

var g_api: ?Api = null;
var g_load_failed = false;

fn libName() [*:0]const u8 {
    return if (builtin.os.tag == .macos) "libasmtest_drapp.dylib" else "libasmtest_drapp.so";
}

/// Resolve the drapp library path: `$ASMTEST_DRAPP_LIB` if set, else
/// `<repo>/build/libasmtest_drapp.so`. The repo root is three levels above this
/// file's `bindings/zig/src` location at build time; at runtime we only have an
/// env var or the bare name, so fall back to the plain soname (resolved via the
/// loader search path) when no env var is present.
fn openLib(buf: []u8) ?std.DynLib {
    if (std.posix.getenv("ASMTEST_DRAPP_LIB")) |p| {
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
        .available = lib.lookup(FnAvailable, "asmtest_dr_available") orelse return null,
        .init = lib.lookup(FnInit, "asmtest_dr_init") orelse return null,
        .start = lib.lookup(FnStart, "asmtest_dr_start") orelse return null,
        .stop = lib.lookup(FnStop, "asmtest_dr_stop") orelse return null,
        .shutdown = lib.lookup(FnShutdown, "asmtest_dr_shutdown") orelse return null,
        .register = lib.lookup(FnRegister, "asmtest_dr_register_region") orelse return null,
        .unregister = lib.lookup(FnUnregister, "asmtest_dr_unregister_region") orelse return null,
        .begin = lib.lookup(FnMarker, "asmtest_trace_begin") orelse return null,
        .end = lib.lookup(FnMarker, "asmtest_trace_end") orelse return null,
        .marker_error = lib.lookup(FnMarkerError, "asmtest_dr_marker_error") orelse return null,
        .exec_alloc = lib.lookup(FnExecAlloc, "asmtest_exec_alloc") orelse return null,
        .exec_free = lib.lookup(FnExecFree, "asmtest_exec_free") orelse return null,
        .trace_new = lib.lookup(FnTraceNew, "asmtest_trace_new") orelse return null,
        .trace_free = lib.lookup(FnTraceFree, "asmtest_trace_free") orelse return null,
        .trace_covered = lib.lookup(FnTraceCovered, "asmtest_trace_covered") orelse return null,
        .blocks_len = lib.lookup(FnBlocksLen, "asmtest_emu_trace_blocks_len") orelse return null,
        .insns_total = lib.lookup(FnInsnsTotal, "asmtest_emu_trace_insns_total") orelse return null,
        .insns_len = lib.lookup(FnInsnsLen, "asmtest_emu_trace_insns_len") orelse return null,
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

/// True if the tier can run: the drapp lib opens AND `asmtest_dr_available()`
/// reports DynamoRIO is resolvable. Mirrors `NativeTrace.available()`.
pub fn available() bool {
    const api = load() orelse return false;
    return api.available() != 0;
}

/// Process-init options. Null fields fall back to the C side's defaults: a null
/// `client` makes the C side read `$ASMTEST_DRCLIENT`. `mode` is the default
/// recording mode (per-trace recording is driven by the trace capacities).
pub const Options = struct {
    dynamorio_home: ?[:0]const u8 = null,
    client: ?[:0]const u8 = null,
    client_options: ?[:0]const u8 = null,
    mode: c_int = 0,
};

// Translate-c renders `const char *` as the C pointer type `[*c]const u8`, which
// coerces from a NUL-terminated slice's `.ptr` and from `null`. Return that type
// directly so the assignment to the option struct fields is exact.
fn cStr(s: ?[:0]const u8) [*c]const u8 {
    return if (s) |v| v.ptr else null;
}

/// Bring DynamoRIO up in-process and take over: `asmtest_dr_init` then
/// `asmtest_dr_start`. Returns `Error.LibUnavailable` if the tier isn't loadable,
/// `Error.InitFailed` / `Error.StartFailed` on a nonzero C return.
pub fn initialize(opts: Options) Error!void {
    const api = load() orelse return Error.LibUnavailable;
    var copts: c.asmtest_drtrace_options_t = std.mem.zeroes(c.asmtest_drtrace_options_t);
    copts.dynamorio_home = cStr(opts.dynamorio_home);
    copts.client_path = cStr(opts.client);
    copts.client_options = cStr(opts.client_options);
    copts.mode = @intCast(opts.mode);
    if (api.init(&copts) != OK) return Error.InitFailed;
    if (api.start() != OK) return Error.StartFailed;
}

/// Bring the tier up with all-default options (null client → `$ASMTEST_DRCLIENT`).
pub fn initializeDefault() Error!void {
    return initialize(.{});
}

/// Tear DynamoRIO down (`asmtest_dr_app_stop_and_cleanup` via the C side).
pub fn shutdown() void {
    const api = load() orelse return;
    api.shutdown();
}

/// Count of illegal marker operations since init; 0 means every begin/end was
/// balanced. Mirrors `NativeTrace.marker_error()`.
pub fn markerError() c_int {
    const api = load() orelse return 0;
    return api.marker_error();
}

/// Host-native machine code in real executable (W^X) memory.
pub const NativeCode = struct {
    code: c.asmtest_exec_code_t,
    freed: bool = false,

    /// Map executable memory and copy `bytes` into it. Returns
    /// `Error.AllocFailed` on a nonzero C return.
    pub fn fromBytes(bytes: []const u8) Error!NativeCode {
        const api = load() orelse return Error.LibUnavailable;
        var ec: c.asmtest_exec_code_t = std.mem.zeroes(c.asmtest_exec_code_t);
        if (api.exec_alloc(bytes.ptr, bytes.len, &ec) != OK) return Error.AllocFailed;
        return NativeCode{ .code = ec };
    }

    /// Entry address of the materialized code (offset 0 = entry).
    pub fn base(self: *const NativeCode) ?*anyopaque {
        return self.code.base;
    }

    /// Number of code bytes.
    pub fn len(self: *const NativeCode) usize {
        return self.code.len;
    }

    /// Invoke the code through a function pointer using the SysV integer ABI:
    /// two `c_long` args in, a `c_long` result out (matches the canonical
    /// add/select routines used by the tier).
    pub fn call(self: *const NativeCode, a: c_long, b: c_long) c_long {
        const fp: *const fn (c_long, c_long) callconv(.C) c_long =
            @ptrCast(@alignCast(self.code.base));
        return fp(a, b);
    }

    /// Unmap the executable memory. Idempotent. The caller MUST
    /// `NativeTrace.unregister(name)` first if the range was registered.
    pub fn free(self: *NativeCode) void {
        if (self.freed) return;
        const api = load() orelse return;
        api.exec_free(&self.code);
        self.freed = true;
    }
};

/// An app-owned coverage recorder for a registered native region. Wraps the
/// opaque `asmtest_trace_t*` handle and the per-region begin/end markers.
pub const NativeTrace = struct {
    handle: ?*anyopaque,

    /// Allocate a trace with the given capacities. NOTE the C `asmtest_trace_new`
    /// takes `(insns_cap, blocks_cap)` — instructions FIRST — so this helper
    /// forwards `(instructions, blocks)` in that order while exposing the
    /// Python-style `(blocks, instructions)` argument names. Block recording is
    /// active when `blocks > 0`, instruction recording when `instructions > 0`.
    pub fn create(blocks: usize, instructions: usize) Error!NativeTrace {
        const api = load() orelse return Error.LibUnavailable;
        const h = api.trace_new(instructions, blocks) orelse return Error.TraceNewFailed;
        return NativeTrace{ .handle = h };
    }

    /// Register a non-overlapping native code range under `name`, recording into
    /// this trace. `name` must be NUL-terminated.
    pub fn register(self: *NativeTrace, name: [:0]const u8, code: *const NativeCode) Error!void {
        const api = load() orelse return Error.LibUnavailable;
        if (api.register(name.ptr, code.code.base, code.code.len, self.handle) != OK)
            return Error.RegisterFailed;
    }

    /// Drop the named region (and the client's cached translation).
    pub fn unregister(self: *NativeTrace, name: [:0]const u8) void {
        _ = self;
        const api = load() orelse return;
        _ = api.unregister(name.ptr);
    }

    /// Open recording for the named region on the calling thread.
    pub fn begin(self: *NativeTrace, name: [:0]const u8) void {
        _ = self;
        const api = load() orelse return;
        api.begin(name.ptr);
    }

    /// Close recording for the named region.
    pub fn end(self: *NativeTrace, name: [:0]const u8) void {
        _ = self;
        const api = load() orelse return;
        api.end(name.ptr);
    }

    /// Scoped begin/end around `body` — the Zig analogue of Python's
    /// `with trace.region(name):`. Guarantees the matching `end` even if `body`
    /// errors. `body` is a function taking the args tuple `args`.
    pub fn region(
        self: *NativeTrace,
        name: [:0]const u8,
        args: anytype,
        comptime body: anytype,
    ) @typeInfo(@TypeOf(body)).Fn.return_type.? {
        self.begin(name);
        defer self.end(name);
        return @call(.auto, body, args);
    }

    /// True if the basic block at byte offset `off` was entered.
    pub fn covered(self: *const NativeTrace, off: u64) bool {
        const api = load() orelse return false;
        return api.trace_covered(self.handle, off) != 0;
    }

    /// Number of distinct basic blocks recorded so far.
    pub fn blocksLen(self: *const NativeTrace) u64 {
        const api = load() orelse return 0;
        return api.blocks_len(self.handle);
    }

    /// Total instructions recorded in the ordered stream (instruction mode).
    pub fn insnsTotal(self: *const NativeTrace) u64 {
        const api = load() orelse return 0;
        return api.insns_total(self.handle);
    }

    /// The distinct basic-block start offsets recorded, in first-seen order.
    /// Returns an allocator-owned slice the caller must `free`; an empty slice if
    /// the tier is unavailable.
    pub fn blockOffsets(self: *const NativeTrace, allocator: std.mem.Allocator) ![]u64 {
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
    pub fn insnOffsets(self: *const NativeTrace, allocator: std.mem.Allocator) ![]u64 {
        const api = load() orelse return allocator.alloc(u64, 0);
        const n: usize = @intCast(api.insns_len(self.handle));
        const out = try allocator.alloc(u64, n);
        for (out, 0..) |*slot, i| slot.* = api.insn_at(self.handle, i);
        return out;
    }

    /// Release the trace handle. Idempotent.
    pub fn free(self: *NativeTrace) void {
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
