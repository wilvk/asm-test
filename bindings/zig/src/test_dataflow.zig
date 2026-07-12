// Zig data-flow binding smoke (Phase 6): GC-move canonicalizer + method resolver,
// mirroring the Python/C++/Node/Ruby/Lua suites. dlopen's libasmtest_dataflow via
// std.DynLib (like hwtrace.zig) and calls the two pure helpers. Run:
//   zig run bindings/zig/src/test_dataflow.zig   (ASMTEST_DATAFLOW_LIB set)
const std = @import("std");

const GcMove = extern struct { old_base: u64, new_base: u64, len: u64, step: u32 };
const Method = extern struct { addr: u64, size: u64, name: [*:0]const u8, version: u64 };
const GcmoveCanonFn = *const fn (?[*]const GcMove, usize, u32, u64) callconv(.C) u64;
const MethodResolveFn = *const fn (?[*]const Method, usize, u64) callconv(.C) c_int;

var n: u32 = 0;
var failed: bool = false;

fn check(cond: bool, desc: []const u8) void {
    n += 1;
    std.debug.print("{s} {d} - {s}\n", .{ if (cond) "ok" else "not ok", n, desc });
    if (!cond) failed = true;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    const alloc = gpa.allocator();
    const path = std.process.getEnvVarOwned(alloc, "ASMTEST_DATAFLOW_LIB") catch {
        std.debug.print("# SKIP zig dataflow: ASMTEST_DATAFLOW_LIB unset\n1..0 # skipped\n", .{});
        return;
    };
    defer alloc.free(path);
    var lib = std.DynLib.open(path) catch |err| {
        std.debug.print("# SKIP zig dataflow: cannot dlopen '{s}': {s}\n1..0 # skipped\n", .{ path, @errorName(err) });
        return;
    };
    defer lib.close();

    const gcmove_canon = lib.lookup(GcmoveCanonFn, "asmtest_gcmove_canon") orelse return error.SymNotFound;
    const method_resolve_pc = lib.lookup(MethodResolveFn, "asmtest_method_resolve_pc") orelse return error.SymNotFound;

    // --- GC-move canonicalizer --- //
    check(gcmove_canon(null, 0, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    const mv = [_]GcMove{.{ .old_base = 0x1000, .new_base = 0x2000, .len = 0x100, .step = 5 }};
    check(gcmove_canon(&mv, mv.len, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove_canon(&mv, mv.len, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove_canon(&mv, mv.len, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove_canon(&mv, mv.len, 3, 0x1100) == 0x1100, "gcmove: one past the window not forwarded");
    check(gcmove_canon(&mv, mv.len, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove_canon(&mv, mv.len, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    const mv2 = [_]GcMove{
        .{ .old_base = 0x1000, .new_base = 0x2000, .len = 0x100, .step = 3 },
        .{ .old_base = 0x2000, .new_base = 0x3000, .len = 0x100, .step = 6 },
    };
    check(gcmove_canon(&mv2, mv2.len, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // --- method resolver --- //
    const ms = [_]Method{
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 3 },
        .{ .addr = 0x2000, .size = 0x20, .name = "Bar", .version = 1 },
        .{ .addr = 0x3000, .size = 0, .name = "Baz", .version = 2 },
    };
    check(method_resolve_pc(&ms, ms.len, 0x1000) == 0, "method: Foo range start");
    check(method_resolve_pc(&ms, ms.len, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method_resolve_pc(&ms, ms.len, 0x1040) == -1, "method: one past Foo -> none");
    check(method_resolve_pc(&ms, ms.len, 0x2010) == 1, "method: Bar range");
    check(method_resolve_pc(&ms, ms.len, 0x3000) == 2, "method: Baz point match");
    check(method_resolve_pc(&ms, ms.len, 0x3001) == -1, "method: Baz is point-only");
    const rj = [_]Method{
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 1 },
        .{ .addr = 0x1000, .size = 0x40, .name = "Foo", .version = 5 },
    };
    check(method_resolve_pc(&rj, rj.len, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method_resolve_pc(null, 0, 0x1000) == -1, "method: empty map -> -1");

    std.debug.print("1..{d}\n", .{n});
    if (failed) std.process.exit(1);
}
