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
const builtin = @import("builtin");
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
extern fn vec_add4d() void; // AVX2 256-bit (Track D); x86-64 only

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

// In-line assembler (Keystone): libasmtest_emu carries it, and the asm build
// option defaults on, so `zig build test` (and `make zig-test`) compiles this
// in without `-Dasm=true`.
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

// Disassembler (Capstone): decode known x86-64 bytes back to instruction text.
// libasmtest_emu links Capstone, and the asm build option defaults on, so
// `zig build test` (and `make zig-test`) compiles this in without `-Dasm=true`;
// it stays gated under build_options.with_asm as a defensive fallback for a
// build with asm disabled.
test "disas.x86" {
    if (@import("builtin").cpu.arch != .x86_64) return error.SkipZigTest;
    if (build_options.with_asm) {
        if (!c.emu_disas_available()) return error.SkipZigTest;
        const code = [_]u8{ 0x48, 0x31, 0xC0, 0xC3 }; // xor rax, rax ; ret
        var buf: [160]u8 = undefined;
        const n0 = c.emu_disas(c.EMU_ARCH_X86_64, &code, code.len, 0x00100000, 0, &buf, buf.len);
        try std.testing.expect(n0 != 0);
        try std.testing.expectEqualStrings("xor rax, rax", std.mem.sliceTo(&buf, 0));
        const n3 = c.emu_disas(c.EMU_ARCH_X86_64, &code, code.len, 0x00100000, 3, &buf, buf.len);
        try std.testing.expect(n3 != 0);
        try std.testing.expectEqualStrings("ret", std.mem.sliceTo(&buf, 0));
    } else {
        return error.SkipZigTest;
    }
}

// Cross-arch guests run raw machine-code bytes through their ISA's Unicorn guest,
// emulated regardless of the host arch — checked-in `add` routines per ISA.
test "cross-arch emu guests" {
    {
        const code = [_]u8{ 0x00, 0x00, 0x01, 0x8B, 0xC0, 0x03, 0x5F, 0xD6 };
        const e = c.emu_arm64_open();
        defer c.emu_arm64_close(e);
        var res: c.emu_arm64_result_t = std.mem.zeroes(c.emu_arm64_result_t);
        var args = [_]c_long{ 40, 2 };
        _ = c.emu_arm64_call(e, &code, code.len, &args, 2, 0, &res);
        try std.testing.expect(!res.faulted);
        try std.testing.expectEqual(@as(c_ulong, 42), res.regs.x[0]);
    }
    {
        const code = [_]u8{ 0x33, 0x05, 0xB5, 0x00, 0x67, 0x80, 0x00, 0x00 };
        const e = c.emu_riscv_open();
        defer c.emu_riscv_close(e);
        var res: c.emu_riscv_result_t = std.mem.zeroes(c.emu_riscv_result_t);
        var args = [_]c_long{ 40, 2 };
        _ = c.emu_riscv_call(e, &code, code.len, &args, 2, 0, &res);
        try std.testing.expect(!res.faulted);
        try std.testing.expectEqual(@as(c_ulong, 42), res.regs.x[10]); // a0 == x10
    }
    {
        const code = [_]u8{ 0x01, 0x00, 0x80, 0xE0, 0x1E, 0xFF, 0x2F, 0xE1 };
        const e = c.emu_arm_open();
        defer c.emu_arm_close(e);
        var res: c.emu_arm_result_t = std.mem.zeroes(c.emu_arm_result_t);
        var args = [_]c_long{ 40, 2 };
        _ = c.emu_arm_call(e, &code, code.len, &args, 2, 0, &res);
        try std.testing.expect(!res.faulted);
        try std.testing.expectEqual(@as(u32, 42), res.regs.r[0]);
    }
}

// Extended x86-64 emulator calls over raw bytes: wide integer args, FP args,
// vector args, and the Win64 convention (host-portable via Unicorn).
test "extended x86 emu calls" {
    const e = c.emu_open();
    defer c.emu_close(e);
    {
        const code = [_]u8{ 0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x01, 0xD0, 0xC3 };
        var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        var args = [_]c_long{ 10, 20, 12 };
        _ = c.emu_call(e, &code, code.len, &args, 3, 0, &res);
        try std.testing.expectEqual(@as(c_ulong, 42), res.regs.rax);
    }
    {
        const code = [_]u8{ 0xF2, 0x0F, 0x58, 0xC1, 0xC3 }; // addsd xmm0,xmm1; ret
        var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        var ia = [_]c_long{0};
        var fa = [_]f64{ 1.5, 2.25 };
        _ = c.emu_call_fp(e, &code, code.len, &ia, 0, &fa, 2, 0, &res);
        try std.testing.expectEqual(@as(f64, 3.75), res.regs.xmm[0].f64[0]);
    }
    {
        const code = [_]u8{ 0x0F, 0x58, 0xC1, 0xC3 }; // addps xmm0,xmm1; ret
        var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        var ia = [_]c_long{0};
        var va: [2]c.emu_vec128_t = std.mem.zeroes([2]c.emu_vec128_t);
        va[0].f32 = .{ 1, 2, 3, 4 };
        va[1].f32 = .{ 10, 20, 30, 40 };
        _ = c.emu_call_vec(e, &code, code.len, &ia, 0, &va, 2, 0, &res);
        try std.testing.expectEqual(@as(f32, 11), res.regs.xmm[0].f32[0]);
        try std.testing.expectEqual(@as(f32, 44), res.regs.xmm[0].f32[3]);
    }
    {
        const code = [_]u8{ 0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0xC3 }; // mov rax,rcx; add rax,rdx; ret
        var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
        var args = [_]c_long{ 40, 2 };
        _ = c.emu_call_win64(e, &code, code.len, &args, 2, 0, &res);
        try std.testing.expectEqual(@as(c_ulong, 42), res.regs.rax);
    }
}

// Execution trace / coverage: a two-block arm64 select; with x0=0 the entry block
// (offset 0) and the .zero block (offset 12) are entered, not offset 4.
test "execution trace coverage" {
    const sel = [_]u8{
        0x60, 0x00, 0x00, 0xB4, 0x60, 0x0C, 0x80, 0xD2, 0xC0, 0x03,
        0x5F, 0xD6, 0x40, 0x05, 0x80, 0xD2, 0xC0, 0x03, 0x5F, 0xD6,
    };
    const e = c.emu_arm64_open();
    defer c.emu_arm64_close(e);
    var ib: [64]u64 = undefined;
    var bb: [64]u64 = undefined;
    var tr: c.emu_trace_t = std.mem.zeroes(c.emu_trace_t);
    tr.insns = &ib;
    tr.insns_cap = ib.len;
    tr.blocks = &bb;
    tr.blocks_cap = bb.len;
    var res: c.emu_arm64_result_t = std.mem.zeroes(c.emu_arm64_result_t);
    var args = [_]c_long{0};
    _ = c.emu_arm64_call_traced(e, &sel, sel.len, &args, 1, 0, &res, &tr);
    try std.testing.expect(!res.faulted);
    try std.testing.expectEqual(@as(c_ulong, 42), res.regs.x[0]);
    try std.testing.expect(c.emu_trace_covered(&tr, 0));
    try std.testing.expect(c.emu_trace_covered(&tr, 12));
    try std.testing.expect(!c.emu_trace_covered(&tr, 4));
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

// Track F: mid-execution guards (byte-literal routines).
test "guards.watchpoint and reg invariant" {
    const e = c.emu_open();
    defer c.emu_close(e);
    var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
    const two_writes = [_]u8{ 0x48, 0x89, 0x07, 0x48, 0x89, 0x87, 0x00, 0x08, 0x00, 0x00, 0xC3 };
    try std.testing.expect(c.emu_map(e, 0x400000, 0x1000));
    var w: c.emu_watch_t = std.mem.zeroes(c.emu_watch_t);
    c.emu_watch_writes(e, 0x400000, 8, 1, &w); // EMU_WATCH_ONLY
    var args = [_]c_long{0x400000};
    _ = c.emu_call(e, &two_writes, two_writes.len, &args, 1, 0, &res);
    c.emu_watch_clear(e);
    try std.testing.expect(w.violated and w.addr == 0x400800 and w.rip_off == 3);

    const clobber = [_]u8{ 0x48, 0xC7, 0xC3, 0x99, 0x00, 0x00, 0x00, 0xEB, 0x00, 0xC3 };
    var g: c.emu_reg_guard_t = std.mem.zeroes(c.emu_reg_guard_t);
    try std.testing.expect(c.emu_guard_reg(e, "rbx", 0, &g));
    var noargs = [_]c_long{0};
    _ = c.emu_call(e, &clobber, clobber.len, &noargs, 0, 0, &res);
    c.emu_guard_reg_clear(e);
    try std.testing.expect(g.violated and g.got == 0x99);
}

// Track E: coverage-guided fuzzing + mutation testing over classify3.
test "fuzz and mutation" {
    const e = c.emu_open();
    defer c.emu_close(e);
    const classify3 = [_]u8{
        0x31, 0xC0, 0x48, 0x85, 0xFF, 0x78, 0x0B, 0x48, 0x85, 0xFF, 0x74, 0x05,
        0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3,
    };
    const uni = c.asmtest_emu_trace_new(0, 256);
    defer c.asmtest_emu_trace_free(uni);
    var fixed: c.emu_fuzz_stat_t = std.mem.zeroes(c.emu_fuzz_stat_t);
    _ = c.emu_fuzz_cover1(e, &classify3, classify3.len, 5, 5, 1, 0xC0FFEE, uni, &fixed);
    const uni2 = c.asmtest_emu_trace_new(0, 256);
    defer c.asmtest_emu_trace_free(uni2);
    var guided: c.emu_fuzz_stat_t = std.mem.zeroes(c.emu_fuzz_stat_t);
    _ = c.emu_fuzz_cover1(e, &classify3, classify3.len, -50, 50, 2000, 0xC0FFEE, uni2, &guided);
    try std.testing.expect(guided.blocks_reached > fixed.blocks_reached);

    var w_in = [_]c_long{5};
    var s_in = [_]c_long{ -7, 0, 9 };
    var weak: c.emu_mutation_stat_t = std.mem.zeroes(c.emu_mutation_stat_t);
    var strong: c.emu_mutation_stat_t = std.mem.zeroes(c.emu_mutation_stat_t);
    _ = c.emu_mutation_test1(e, &classify3, classify3.len, &w_in, 1, 0, 0xABCD, &weak);
    _ = c.emu_mutation_test1(e, &classify3, classify3.len, &s_in, 3, 0, 0xABCD, &strong);
    try std.testing.expect(weak.survived > 0 and strong.survived < weak.survived);
}

// Track D: AVX2 256-bit capture (x86-64 + AVX2; comptime-gated, runtime self-skip).
test "vec256.add4d (AVX2)" {
    if (comptime builtin.cpu.arch == .x86_64) {
        if (c.asmtest_cpu_has_avx2() == 0) return error.SkipZigTest;
        var a: c.vec256_t = std.mem.zeroes(c.vec256_t);
        var b: c.vec256_t = std.mem.zeroes(c.vec256_t);
        a.f64[0] = 1; a.f64[1] = 2; a.f64[2] = 3; a.f64[3] = 4;
        b.f64[0] = 10; b.f64[1] = 20; b.f64[2] = 30; b.f64[3] = 40;
        var varr = [_]c.vec256_t{ a, b };
        var out: [16]c.vec256_t = std.mem.zeroes([16]c.vec256_t);
        var ia = [_]c_long{ 0, 0, 0, 0, 0, 0 };
        c.asm_call_capture_vec256(&out, fnPtr(&vec_add4d), &ia, &varr);
        try std.testing.expect(out[0].f64[0] == 11 and out[0].f64[3] == 44);
    } else {
        return error.SkipZigTest;
    }
}
