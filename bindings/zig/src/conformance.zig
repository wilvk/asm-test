//! asm-test Zig binding (Track Z) — conformance corpus, via `@cImport`.
//!
//! Zig consumes the C headers directly (no separate binding layer): `@cImport`
//! translates `asmtest.h` / `asmtest_emu.h`, giving the structs, function
//! declarations, and integer-constant macros (flag masks). The test drives the
//! canonical routines through the binding-ABI entry points and reproduces the
//! conformance corpus (mirrors bindings/conformance/corpus.json). No GC; the
//! arg arrays are plain stack slices passed by pointer.
//!
//! Run via `make zig-test` (or `zig build test` from this dir).
const std = @import("std");
const build_options = @import("build_options");

const c = @cImport({
    @cInclude("asmtest.h");
    @cInclude("asmtest_emu.h");
    @cInclude("asmtest_assemble.h");
});

// Canonical routines under test (examples/{add,flags,fp,simd}.s), linked from
// the fixture lib (make's CORPUS_LIB). Signatures are nominal — only addresses
// are used.
extern fn add_signed(c_long, c_long) c_long;
extern fn sum_via_rbx(c_long, c_long) c_long;
extern fn clobbers_rbx(c_long, c_long) c_long;
extern fn set_carry() c_long;
extern fn clear_carry() c_long;
extern fn fp_add(f64, f64) f64;
extern fn vec_add4f() void;
extern fn read_fault(?*const c_long) c_long; // loads *p; faults if p is unmapped
extern fn int_to_double(c_long) f64; // (double)n into xmm0 from an integer arg

/// Address of a routine as the opaque pointer the trampoline expects.
fn fnPtr(p: anytype) ?*anyopaque {
    return @ptrFromInt(@intFromPtr(p));
}

fn captureInt(f: ?*anyopaque, a0: c_long, a1: c_long) c.regs_t {
    var r: c.regs_t = std.mem.zeroes(c.regs_t);
    var args = [_]c_long{ a0, a1, 0, 0, 0, 0 };
    c.asm_call_capture(&r, f, &args);
    return r;
}

test "add_signed.basic" {
    const r = captureInt(fnPtr(&add_signed), 40, 2);
    try std.testing.expectEqual(@as(c_ulong, 42), r.ret);
    try std.testing.expect(c.asmtest_check_abi(&r, null, 0) == 0);
}

test "sum_via_rbx.abi_preserved" {
    const r = captureInt(fnPtr(&sum_via_rbx), 20, 22);
    try std.testing.expectEqual(@as(c_ulong, 42), r.ret);
    try std.testing.expect(c.asmtest_check_abi(&r, null, 0) == 0);
}

test "clobbers_rbx.abi_violation_detected" {
    const r = captureInt(fnPtr(&clobbers_rbx), 1, 2);
    try std.testing.expect(c.asmtest_check_abi(&r, null, 0) != 0);
}

test "set_carry.cf_set" {
    const r = captureInt(fnPtr(&set_carry), 0, 0);
    try std.testing.expect((r.flags & c.ASMTEST_CF) != 0);
}

test "clear_carry.cf_clear" {
    const r = captureInt(fnPtr(&clear_carry), 0, 0);
    try std.testing.expect((r.flags & c.ASMTEST_CF) == 0);
}

test "fp_add.basic" {
    var r: c.regs_t = std.mem.zeroes(c.regs_t);
    var iargs = [_]c_long{ 0, 0, 0, 0, 0, 0 };
    var fargs = [_]f64{ 1.5, 2.25, 0, 0, 0, 0, 0, 0 };
    c.asm_call_capture_fp(&r, fnPtr(&fp_add), &iargs, &fargs);
    try std.testing.expectEqual(@as(f64, 3.75), r.fret);
}

test "vec_add4f.basic" {
    var r: c.regs_t = std.mem.zeroes(c.regs_t);
    var iargs = [_]c_long{ 0, 0, 0, 0, 0, 0 };
    var v: [8]c.vec128_t = std.mem.zeroes([8]c.vec128_t);
    v[0].f32 = .{ 1, 2, 3, 4 };
    v[1].f32 = .{ 10, 20, 30, 40 };
    c.asm_call_capture_vec(&r, fnPtr(&vec_add4f), &iargs, &v);
    try std.testing.expectEqual(@as(f32, 11), r.vec[0].f32[0]);
    try std.testing.expectEqual(@as(f32, 44), r.vec[0].f32[3]);
}

// Emulator x86-64 guest runs host-compiled bytes — valid only on an x86-64 host.
test "emu.add_signed" {
    if (@import("builtin").cpu.arch != .x86_64) return error.SkipZigTest;
    const e = c.emu_open();
    defer c.emu_close(e);
    var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
    var args = [_]c_long{ 40, 2 };
    _ = c.emu_call(e, fnPtr(&add_signed), 64, &args, 2, 0, &res);
    try std.testing.expect(!res.faulted);
    try std.testing.expectEqual(@as(c_ulong, 42), res.regs.rax);
}

// read_fault dereferences an unmapped address: the fault is data — where
// (fault_addr) and why (fault_kind) — not a crash.
test "emu.read_fault" {
    if (@import("builtin").cpu.arch != .x86_64) return error.SkipZigTest;
    const fault_addr: c_long = 0x00DEAD00;
    const e = c.emu_open();
    defer c.emu_close(e);
    var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
    var args = [_]c_long{ fault_addr, 0 };
    _ = c.emu_call(e, fnPtr(&read_fault), 64, &args, 1, 0, &res);
    try std.testing.expect(res.faulted);
    try std.testing.expectEqual(@as(u64, @intCast(fault_addr)), res.fault_addr);
    try std.testing.expectEqual(@as(c_int, c.EMU_FAULT_READ), @as(c_int, @intCast(res.fault_kind)));
}

// int_to_double lands (double)42 in xmm0, so the XMM file is readable beyond the
// GP registers; a clean run also keeps rflags live (x86 holds bit 1 set).
test "emu.int_to_double" {
    if (@import("builtin").cpu.arch != .x86_64) return error.SkipZigTest;
    const e = c.emu_open();
    defer c.emu_close(e);
    var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
    var args = [_]c_long{ 42, 0 };
    _ = c.emu_call(e, fnPtr(&int_to_double), 64, &args, 1, 0, &res);
    try std.testing.expect(!res.faulted);
    try std.testing.expectEqual(@as(f64, 42.0), res.regs.xmm[0].f64[0]);
    try std.testing.expect((res.regs.rflags & 0x2) != 0);
}

// In-line assembler (Keystone): compiled in only when linked against the
// assembler-carrying lib (build with -Dasm=true, i.e. `make zig-asm-test`).
test "asm.inline_assembler" {
    if (@import("builtin").cpu.arch != .x86_64) return error.SkipZigTest;
    if (build_options.with_asm) {
        const e = c.emu_open();
        defer c.emu_close(e);

        // Intel, two args.
        var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        try std.testing.expect(c.asmtest_emu_call_asm6(
            e, "mov rax, rdi; add rax, rsi; ret", c.ASM_SYNTAX_INTEL,
            40, 2, 0, 0, 0, 0, 2, 0, &res) != 0);
        try std.testing.expectEqual(@as(c_ulong, 42), res.regs.rax);

        // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
        var att: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        try std.testing.expect(c.asmtest_emu_call_asm6(
            e, "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret", c.ASM_SYNTAX_ATT,
            10, 20, 12, 0, 0, 0, 3, 0, &att) != 0);
        try std.testing.expectEqual(@as(c_ulong, 42), att.regs.rax);

        // Failure path: ok == 0 and a non-empty Keystone diagnostic.
        var bad: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        try std.testing.expect(c.asmtest_emu_call_asm6(
            e, "mov rax, nonsense_token", c.ASM_SYNTAX_INTEL,
            0, 0, 0, 0, 0, 0, 0, 0, &bad) == 0);
        try std.testing.expect(c.asmtest_asm_last_error()[0] != 0);

        // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
        var buf: [16]u8 = undefined;
        const n = c.asmtest_asm_bytes(c.ASM_ARM64, c.ASM_SYNTAX_INTEL, "ret", 0x00100000, &buf, buf.len);
        try std.testing.expectEqual(@as(c_int, 4), n);
        try std.testing.expectEqual(@as(u8, 0xC0), buf[0]);
        try std.testing.expectEqual(@as(u8, 0xD6), buf[3]);
    } else {
        return error.SkipZigTest;
    }
}

// --- Tier-2 idiomatic assertions (error-union helpers over std.testing) --- //
fn assertRet(r: *const c.regs_t, expected: u64) !void {
    try std.testing.expectEqual(@as(c_ulong, expected), r.ret);
}
fn assertAbiPreserved(r: *c.regs_t) !void {
    try std.testing.expect(c.asmtest_check_abi(r, null, 0) == 0);
}
fn assertFp(r: *const c.regs_t, expected: f64) !void {
    try std.testing.expectEqual(expected, r.fret);
}

test "tier2.assertions pass" {
    var r = captureInt(fnPtr(&add_signed), 40, 2);
    try assertRet(&r, 42);
    try assertAbiPreserved(&r);
    var rf: c.regs_t = std.mem.zeroes(c.regs_t);
    var iargs = [_]c_long{ 0, 0, 0, 0, 0, 0 };
    var fargs = [_]f64{ 1.5, 2.25, 0, 0, 0, 0, 0, 0 };
    c.asm_call_capture_fp(&rf, fnPtr(&fp_add), &iargs, &fargs);
    try assertFp(&rf, 3.75);
}

test "tier2.assertions have teeth" {
    var r = captureInt(fnPtr(&add_signed), 40, 2);
    try std.testing.expectError(error.TestExpectedEqual, assertRet(&r, 99));
}
