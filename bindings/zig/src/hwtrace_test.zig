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
const BEST = hwtrace.BEST;
const CEILING_FREE = hwtrace.CEILING_FREE;
const AMD_LBR = @intFromEnum(hwtrace.Backend.amd_lbr);
const SINGLESTEP_ENUM = @intFromEnum(hwtrace.Backend.singlestep);
const ASMTEST_HW_EUNAVAIL = hwtrace.ASMTEST_HW_EUNAVAIL;

// Cross-tier orchestrator constants.
const TRACE_BEST = hwtrace.TRACE_BEST;
const TRACE_CEILING_FREE = hwtrace.TRACE_CEILING_FREE;
const TRACE_NATIVE_ONLY = hwtrace.TRACE_NATIVE_ONLY;
const TIER_HWTRACE = @intFromEnum(hwtrace.Tier.hwtrace);
const TIER_EMULATOR = @intFromEnum(hwtrace.Tier.emulator);
const FIDELITY_NATIVE = @intFromEnum(hwtrace.Fidelity.native);
const FIDELITY_VIRTUAL = @intFromEnum(hwtrace.Fidelity.virtual);

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

// The orchestrator's selection invariants hold on EVERY host (even where all
// backends self-skip and the cascade is empty), so this runs before the skip
// guard. Mirrors Python's `test_auto_resolve_selection_invariants`.
fn checkAutoResolveInvariants() Failure!void {
    const best = hwtrace.resolve(BEST).slice();
    const cf = hwtrace.resolve(CEILING_FREE).slice();

    // Every resolved backend is actually available, ordered by descending fidelity
    // (ascending enum), with no duplicates.
    for (best, 0..) |b, i| {
        try check(hwtrace.available(@enumFromInt(b)), "resolve(BEST) entry is available");
        if (i != 0) try check(b > best[i - 1], "resolve(BEST) strictly ascending, no dups");
    }

    // CEILING_FREE drops the one fixed-window backend (AMD LBR) and is otherwise a
    // subset of BEST.
    for (cf) |c| {
        try check(c != AMD_LBR, "resolve(CEILING_FREE) never selects AMD LBR");
        try check(std.mem.indexOfScalar(c_int, best, c) != null,
            "resolve(CEILING_FREE) is a subset of resolve(BEST)");
    }

    // auto(policy) is the head of resolve(policy), or EUNAVAIL when empty.
    const ab = hwtrace.auto(BEST);
    const want = if (best.len == 0) ASMTEST_HW_EUNAVAIL else best[0];
    try check(ab == want, "auto(BEST) is the head of the resolved cascade");
}

// The CROSS-TIER orchestrator (resolve over hwtrace + DynamoRIO + emulator) holds
// its structural invariants on EVERY host, so this runs before the skip guard.
// Mirrors Python's `test_cross_tier_resolve_invariants`.
fn checkCrossTierResolveInvariants() Failure!void {
    const best_r = hwtrace.resolveTiers(TRACE_BEST);
    const nat_r = hwtrace.resolveTiers(TRACE_NATIVE_ONLY);
    const cf_r = hwtrace.resolveTiers(TRACE_CEILING_FREE);
    const best = best_r.slice();
    const nat = nat_r.slice();
    const cf = cf_r.slice();

    // Every HW choice satisfies the hardware-tier probe; NATIVE choices precede the
    // single VIRTUAL emulator floor, which is the last entry under BEST.
    for (best) |c| {
        if (c.tier == TIER_HWTRACE)
            try check(hwtrace.available(@enumFromInt(c.backend)), "cross-tier HW choice is available");
        const want_fid: c_int = if (c.tier == TIER_EMULATOR) FIDELITY_VIRTUAL else FIDELITY_NATIVE;
        try check(c.fidelity == want_fid, "cross-tier choice has the expected fidelity class");
    }
    try check(best.len != 0 and best[best.len - 1].tier == TIER_EMULATOR,
        "resolve(TRACE_BEST) ends at the emulator floor");
    var emu_count: usize = 0;
    for (best) |c| {
        if (c.tier == TIER_EMULATOR) emu_count += 1;
    }
    try check(emu_count == 1, "resolve(TRACE_BEST) has exactly one emulator entry");

    // NATIVE_ONLY forbids the native->emulator crossing: it is BEST minus the floor.
    for (nat) |c| try check(c.tier != TIER_EMULATOR, "TRACE_NATIVE_ONLY drops the emulator floor");
    try check(nat.len == best.len - 1, "TRACE_NATIVE_ONLY is BEST minus the floor");

    // CEILING_FREE drops AMD LBR.
    for (cf) |c|
        try check(!(c.tier == TIER_HWTRACE and c.backend == AMD_LBR),
            "TRACE_CEILING_FREE never selects AMD LBR");

    // auto(policy) is the head of resolve(policy).
    const one = hwtrace.autoTier(TRACE_BEST);
    try check(one != null, "auto_tier(TRACE_BEST) resolves a choice");
    try check(one.?.tier == best[0].tier and one.?.backend == best[0].backend,
        "auto_tier(TRACE_BEST) is the head of the resolved cascade");
}

// On any x86-64 Linux host the single-step backend is a native floor, so even
// NATIVE_ONLY resolves (the cascade never collapses to nothing here). Runs only
// after the SINGLESTEP skip guard. Mirrors Python's
// `test_cross_tier_native_only_resolves_on_linux_x86_64`.
fn checkCrossTierNativeOnly() Failure!void {
    const nat_r = hwtrace.resolveTiers(TRACE_NATIVE_ONLY);
    const nat = nat_r.slice();
    const pick = hwtrace.autoTier(TRACE_NATIVE_ONLY);
    try check(nat.len != 0 and pick != null and pick.?.fidelity == FIDELITY_NATIVE,
        "TRACE_NATIVE_ONLY resolves a native choice on x86-64 Linux");
    var found = false;
    for (nat) |c| {
        if (c.tier == TIER_HWTRACE and c.backend == SINGLESTEP_ENUM) found = true;
    }
    try check(found, "single-step is in the native-only cascade on x86-64 Linux");
}

// ---- Out-of-process / foreign-process toolkit (hwtrace.Ptrace) ---- //
// The Zig analogue of test_hwtrace.py's tests after the "foreign-process toolkit"
// banner. Each self-skips (returns without failing) when the ptrace backend is
// unavailable, exactly like Python's `_skip_if_no_ptrace`.

const Ptrace = hwtrace.Ptrace;

// Little-endian integer writers (std.mem.writeInt(.little) into the image stream),
// matching the Python struct.pack("<...") jitdump layout.
fn writeU32(writer: anytype, v: u32) !void {
    var b: [4]u8 = undefined;
    std.mem.writeInt(u32, &b, v, .little);
    try writer.writeAll(&b);
}
fn writeU64(writer: anytype, v: u64) !void {
    var b: [8]u8 = undefined;
    std.mem.writeInt(u64, &b, v, .little);
    try writer.writeAll(&b);
}

// Fork a tracee, single-step it out of process, get the same offsets as the
// in-process stepper. Mirrors Python's `test_ptrace_trace_call`.
fn checkPtraceTraceCall(alloc: std.mem.Allocator) !void {
    var code = try hwtrace.NativeCode.fromBytes(&ROUTINE);
    defer code.free();
    var tr = try hwtrace.HwTrace.create(64, 64); // blocks=64, instructions=64
    defer tr.free();

    const args = [_]i64{ 20, 22 };
    const result = try Ptrace.traceCall(code.base, code.len, &args, &tr);
    try check(result == 42, "ptrace trace_call returns 42 (forked + single-stepped)");

    const insns = try tr.insnOffsets(alloc);
    defer alloc.free(insns);
    try check(std.mem.eql(u64, insns, &[_]u64{ 0x0, 0x3, 0x6, 0xC, 0x11 }), "ptrace trace_call yields the exact shared offset stream");
    try check(!tr.truncated(), "ptrace trace_call not truncated");
}

// run_to drives an attached target to a resolved method (software breakpoint). A live
// foreign attach is covered by the C suite (forking + ptrace of a foreign process is
// impractical here, same as traceAttached); exercise the FFI round-trip safely — a NULL
// target address is rejected (EINVAL, non-zero) before any ptrace call.
fn checkPtraceRunTo() !void {
    const rc = try Ptrace.runTo(std.os.linux.getpid(), 0);
    try check(rc != 0, "ptrace run_to(NULL addr) rejected (EINVAL) via the FFI round-trip");
}

// Discover an executable region's extent from /proc/<pid>/maps by an interior
// address (this process). Mirrors Python's `test_proc_region_by_addr`.
fn checkProcRegionByAddr() !void {
    var code = try hwtrace.NativeCode.fromBytes(&ROUTINE);
    defer code.free();
    const base = @intFromPtr(code.base);

    const region = Ptrace.regionByAddr(std.os.linux.getpid(), base + 4);
    try check(region != null, "region_by_addr finds the mapping containing an interior addr");
    try check(region.?.base == base, "region_by_addr base == the code mapping base");
    try check(region.?.len >= ROUTINE.len, "region_by_addr len covers the whole region");
    try check(Ptrace.regionByAddr(std.os.linux.getpid(), 0x1) == null, "nothing maps addr 1");
}

// Parse a JIT perf-map (/tmp/perf-<pid>.map) and resolve a method by name.
// Mirrors Python's `test_proc_perfmap_symbol`.
fn checkProcPerfmapSymbol() !void {
    const pid = std.os.linux.getpid();
    var path_buf: [64]u8 = undefined;
    const path = try std.fmt.bufPrintZ(&path_buf, "/tmp/perf-{d}.map", .{pid});

    {
        const f = try std.fs.cwd().createFile(path, .{ .truncate = true });
        defer f.close();
        try f.writeAll("400000 1a void demo(long, long)\n500000 8 other\n");
    }
    defer std.fs.cwd().deleteFile(path) catch {};

    const hit = Ptrace.perfmapSymbol(pid, "void demo(long, long)");
    try check(hit != null, "perfmap_symbol resolves the named method");
    try check(hit.?.base == 0x400000 and hit.?.len == 0x1A, "perfmap_symbol returns the method's (base, len)");
    try check(Ptrace.perfmapSymbol(pid, "missing") == null, "perfmap_symbol misses an absent name");
}

// Read a binary jitdump and resolve a method to (addr,size,index,ts) + bytes,
// the same little-endian byte layout as Python's `test_jitdump_find`, written
// here with std.mem.writeInt(.little).
fn checkJitdumpFind(alloc: std.mem.Allocator) !void {
    const name = "void demo(long, long)";
    var path_buf: [std.fs.max_path_bytes]u8 = undefined;
    const path = try std.fmt.bufPrintZ(&path_buf, "/tmp/asmtest-zig-jit-{d}.dump", .{std.os.linux.getpid()});
    defer std.fs.cwd().deleteFile(path) catch {};

    // Build the jitdump image byte-for-byte like the Python struct.pack layout.
    var img = std.ArrayList(u8).init(alloc);
    defer img.deinit();
    const w = img.writer();
    // header: magic, version, total_size=40, elf_mach, pad1, pid, timestamp, flags
    //   struct.pack("<IIIIIIQQ", 0x4A695444, 1, 40, 62, 0, 0, 0, 0)
    try writeU32(w, 0x4A695444); // magic "DTit" (little-endian)
    try writeU32(w, 1); // version
    try writeU32(w, 40); // total_size (header size)
    try writeU32(w, 62); // elf_mach (EM_X86_64)
    try writeU32(w, 0); // pad1
    try writeU32(w, 0); // pid
    try writeU64(w, 0); // timestamp
    try writeU64(w, 0); // flags
    // JIT_CODE_LOAD record: id=0, total_size, timestamp=5
    //   struct.pack("<IIQ", 0, total, 5)
    const total: u64 = 16 + 40 + (name.len + 1) + ROUTINE.len;
    try writeU32(w, 0); // id = JIT_CODE_LOAD
    try writeU32(w, @intCast(total)); // record total_size
    try writeU64(w, 5); // record timestamp
    // body: pid, tid, vma, code_addr, code_size, code_index
    //   struct.pack("<IIQQQQ", 0, 0, 0x2000, 0x2000, len(ROUTINE), 9)
    try writeU32(w, 0); // pid
    try writeU32(w, 0); // tid
    try writeU64(w, 0x2000); // vma
    try writeU64(w, 0x2000); // code_addr
    try writeU64(w, ROUTINE.len); // code_size
    try writeU64(w, 9); // code_index
    try w.writeAll(name);
    try w.writeByte(0); // NUL-terminate the symbol name
    try w.writeAll(&ROUTINE);

    {
        const f = try std.fs.cwd().createFile(path, .{ .truncate = true });
        defer f.close();
        try f.writeAll(img.items);
    }

    var bytes_buf: [64]u8 = undefined;
    const m = Ptrace.jitdumpFind(path, name, 0, &bytes_buf);
    try check(m != null, "jitdump_find resolves the named method");
    try check(m.?.code_addr == 0x2000, "jitdump_find code_addr == 0x2000");
    try check(m.?.code_size == ROUTINE.len, "jitdump_find code_size == len(ROUTINE)");
    try check(m.?.code_index == 9, "jitdump_find code_index == 9");
    try check(m.?.timestamp == 5, "jitdump_find timestamp == 5");
    try check(std.mem.eql(u8, m.?.code, &ROUTINE), "jitdump_find returns the recorded code bytes");

    try check(Ptrace.jitdumpFind(path, "missing", 0, &bytes_buf) == null, "jitdump_find misses an absent name");
}

pub fn main() !void {
    try checkAutoResolveInvariants();
    try checkCrossTierResolveInvariants();

    if (!hwtrace.available(SINGLESTEP)) {
        var buf: [192]u8 = undefined;
        const reason = hwtrace.skipReason(SINGLESTEP, &buf);
        std.debug.print("# SKIP: single-step backend unavailable: {s}\n", .{reason});
        return; // exit 0
    }

    // On any x86-64 Linux host single-step is a native floor, so even NATIVE_ONLY
    // resolves the cross-tier cascade (it never collapses to nothing here).
    try checkCrossTierNativeOnly();

    const alloc = std.heap.page_allocator;

    // ---- live trace via auto-select: trace the shared fixture through whatever
    // auto picked (its own init/shutdown — single global lifecycle). Mirrors
    // Python's `test_auto_resolve_traces_live`. On any x86-64 Linux host the cascade
    // is non-empty (single-step floor), so auto() resolves a usable backend. ---- //
    {
        const best = hwtrace.resolve(BEST).slice();
        const ab = hwtrace.auto(BEST);
        try check(best.len != 0 and ab >= 0, "auto resolves a backend (single-step floor)");

        try hwtrace.init(@enumFromInt(ab));
        defer hwtrace.shutdown();

        var code = try hwtrace.NativeCode.fromBytes(&ROUTINE);
        defer code.free();
        var tr = try hwtrace.HwTrace.create(64, 64); // blocks=64, instructions=64
        defer tr.free();
        try tr.register("auto", &code);

        tr.region("auto", .{ &code, @as(c_long, 20), @as(c_long, 22) }, callBody);
        try check(g_result == 42, "auto-selected backend traces a live call (returns 42)");
        try check(tr.covered(0), "auto-selected backend covers block offset 0");

        if (ab == SINGLESTEP_ENUM) { // the pick off PT/AMD hosts: byte-exact parity
            const insns = try tr.insnOffsets(alloc);
            defer alloc.free(insns);
            try check(std.mem.eql(u64, insns, &[_]u64{ 0x0, 0x3, 0x6, 0xC, 0x11 }),
                "auto pick (single-step) yields the exact shared offset stream");
        }
    }

    hwtrace.init(SINGLESTEP) catch |e| {
        // init can fail even when "available" if the host is misconfigured;
        // treat as a skip like the Python fixture does.
        std.debug.print("# SKIP: hwtrace init failed: {s}\n", .{@errorName(e)});
        return;
    };
    defer hwtrace.shutdown();

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

    // ---- Out-of-process / foreign-process toolkit (hwtrace.Ptrace) ---- //
    // Self-skip cleanly when the ptrace backend is unavailable, like Python's
    // `_skip_if_no_ptrace`; otherwise run the four foreign-process tests.
    if (!Ptrace.available()) {
        var buf: [192]u8 = undefined;
        const reason = Ptrace.skipReason(&buf);
        std.debug.print("# SKIP: ptrace backend unavailable: {s}\n", .{reason});
    } else {
        try checkPtraceTraceCall(alloc);
        try checkPtraceRunTo();
        try checkProcRegionByAddr();
        try checkProcPerfmapSymbol();
        try checkJitdumpFind(alloc);
    }

    std.debug.print("PASS\n", .{});
}
