//! Standalone live test for the single-step hardware-trace wrapper (hwtrace.zig).
//!
//! The Zig analogue of `bindings/python/tests/test_hwtrace.py`. Built as a
//! separate `hwtrace-test` step (NOT part of `zig build test`) because it must
//! NOT link `libasmtest_hwtrace` at build time — the lib is optional and may be
//! absent. It dlopen()s the lib at runtime and **self-skips** cleanly: if
//! `hwtrace.available(SINGLESTEP)` is false it prints `# SKIP: ...` and exits 0.
//!
//! Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the PT/AMD
//! backends (which need specific bare-metal hardware), the SINGLESTEP backend runs
//! on ANY x86-64 Linux — so this asserts a real, live trace here and in
//! CI/containers, self-skipping only off x86-64 Linux or without Capstone.
//!
//! A `main` (not a `test` block) so it doubles as `zig run src/hwtrace_test.zig`
//! and as an `addExecutable` run artifact. Uses explicit `if (!cond) return
//! error.Failed` checks rather than `std.debug.assert` (assert is compiled out in
//! ReleaseFast) so the assertions keep their teeth in any optimize mode.
const std = @import("std");
const hwtrace = @import("hwtrace.zig");

const SINGLESTEP = hwtrace.SINGLESTEP;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
const ROUTINE = [_]u8{
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
};

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
const LOOP = [_]u8{
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3,
};

const Failure = error{Failed};

fn check(cond: bool, comptime msg: []const u8) Failure!void {
    if (!cond) {
        std.debug.print("FAIL: {s}\n", .{msg});
        return Failure.Failed;
    }
}

// `region` body helper: invoke the code and stash the result so the caller can
// read it after the scoped begin/end closes.
var g_result: c_long = 0;
fn callBody(code: *const hwtrace.NativeCode, a: c_long, b: c_long) void {
    g_result = code.call(a, b);
}

pub fn main() !void {
    if (!hwtrace.available(SINGLESTEP)) {
        var buf: [192]u8 = undefined;
        const reason = hwtrace.skipReason(SINGLESTEP, &buf);
        std.debug.print("# SKIP: single-step backend unavailable: {s}\n", .{reason});
        return; // exit 0
    }

    hwtrace.init(SINGLESTEP) catch |e| {
        // init can fail even when "available" if the host is misconfigured;
        // treat as a skip like the Python fixture does.
        std.debug.print("# SKIP: hwtrace init failed: {s}\n", .{@errorName(e)});
        return;
    };
    defer hwtrace.shutdown();

    const alloc = std.heap.page_allocator;

    // ---- routine: two blocks, full instruction stream ---- //
    {
        var code = try hwtrace.NativeCode.fromBytes(&ROUTINE);
        defer code.free();
        var tr = try hwtrace.HwTrace.create(64, 64); // blocks=64, instructions=64
        defer tr.free();
        try tr.register("add2", &code);

        tr.region("add2", .{ &code, @as(c_long, 20), @as(c_long, 22) }, callBody);
        try check(g_result == 42, "add2(20,22) == 42 (42 <= 100 -> jle taken, dec skipped)");

        // Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
        const insns = try tr.insnOffsets(alloc);
        defer alloc.free(insns);
        try check(std.mem.eql(u64, insns, &[_]u64{ 0x0, 0x3, 0x6, 0xC, 0x11 }),
            "insn_offsets == [0x0,0x3,0x6,0xC,0x11]");
        try check(tr.insnsTotal() == 5, "insns_total == 5");
        try check(tr.covered(0) and tr.covered(0x11), "covered(0) and covered(0x11)");
        try check(tr.blocksLen() == 2, "blocks_len == 2");
        try check(!tr.truncated(), "not truncated");
    }

    // ---- loop: 19 back-edges (no depth ceiling) ---- //
    {
        var code = try hwtrace.NativeCode.fromBytes(&LOOP);
        defer code.free();
        var tr = try hwtrace.HwTrace.create(64, 256); // blocks=64, instructions=256
        defer tr.free();
        try tr.register("loop", &code);

        tr.region("loop", .{ &code, @as(c_long, 1), @as(c_long, 20) }, callBody);
        try check(g_result == 20, "loop(1,20) == 20");
        try check(tr.insnsTotal() == 62, "insns_total == 62 (1 + 20*3 + 1, all captured)");
        try check(tr.covered(0) and tr.covered(0x7), "covered(0) and covered(0x7)");
        try check(tr.blocksLen() == 2, "blocks_len == 2");
        try check(!tr.truncated(), "not truncated");
    }

    std.debug.print("PASS\n", .{});
}
