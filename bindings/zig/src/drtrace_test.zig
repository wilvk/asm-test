//! Standalone smoke test for the DynamoRIO native-trace wrapper (drtrace.zig).
//!
//! The Zig analogue of `bindings/python/tests/test_drtrace.py`. Built as a
//! separate `drtrace-test` step (NOT part of `zig build test`) because it must
//! NOT link `libasmtest_drapp` at build time — the lib needs DynamoRIO and may
//! be absent. It dlopen()s the lib at runtime and **self-skips** cleanly: if
//! `drtrace.available()` is false it prints `SKIP: ...` and exits 0.
//!
//! A `main` (not a `test` block) so it doubles as `zig run src/drtrace_test.zig`
//! and as an `addExecutable` run artifact. Uses explicit `if (!cond) return
//! error.Failed` checks rather than `std.debug.assert` (assert is compiled out
//! in ReleaseFast) so the assertions keep their teeth in any optimize mode.
const std = @import("std");
const drtrace = @import("drtrace.zig");

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE = [_]u8{
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
};

const Failure = error{Failed};

fn check(cond: bool, comptime msg: []const u8) Failure!void {
    if (!cond) {
        std.debug.print("FAIL: {s}\n", .{msg});
        return Failure.Failed;
    }
}

// `region` body helpers: invoke the code and stash the result so the caller can
// read it after the scoped begin/end closes.
var g_result: c_long = 0;
fn callBody(code: *const drtrace.NativeCode, a: c_long, b: c_long) void {
    g_result = code.call(a, b);
}

pub fn main() !void {
    if (!drtrace.available()) {
        std.debug.print("SKIP: DynamoRIO native-trace tier unavailable (self-skip)\n", .{});
        return; // exit 0
    }

    drtrace.initializeDefault() catch |e| {
        // dr_init/start can fail even when "available" if the client/env is
        // misconfigured; treat as a skip like the Python fixture does.
        std.debug.print("SKIP: dr_init/start failed: {s}\n", .{@errorName(e)});
        return;
    };
    defer drtrace.shutdown();

    // ---- block-coverage + accumulation ---- //
    var code = try drtrace.NativeCode.fromBytes(&ROUTINE);
    var tr = try drtrace.NativeTrace.create(64, 0); // blocks=64, instructions=0
    try tr.register("add2", &code);

    tr.region("add2", .{ &code, @as(c_long, 20), @as(c_long, 22) }, callBody);
    try check(g_result == 42, "add2(20,22) == 42");
    try check(tr.covered(0), "entry block (offset 0) covered");

    const before = tr.blocksLen();
    tr.region("add2", .{ &code, @as(c_long, 60), @as(c_long, 60) }, callBody);
    try check(g_result == 119, "add2(60,60) == 119 (other block: 120>100 -> dec)");
    try check(tr.blocksLen() >= before, "blocks_len accumulates");
    try check(drtrace.markerError() == 0, "marker_error == 0 (markers balanced)");

    tr.unregister("add2");
    code.free();
    tr.free();

    // ---- instruction mode ---- //
    var code2 = try drtrace.NativeCode.fromBytes(&ROUTINE);
    var tr2 = try drtrace.NativeTrace.create(64, 64); // blocks=64, instructions=64
    try tr2.register("add2i", &code2);

    tr2.region("add2i", .{ &code2, @as(c_long, 1), @as(c_long, 2) }, callBody);
    try check(g_result == 3, "add2i(1,2) == 3");
    try check(tr2.insnsTotal() >= 4, "insns_total >= 4 (ordered instruction stream)");

    tr2.unregister("add2i");
    code2.free();
    tr2.free();

    std.debug.print("PASS\n", .{});
}
