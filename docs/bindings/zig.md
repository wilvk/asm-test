# Zig binding

The [Zig binding](https://github.com/wilvk/asm-test/tree/main/bindings/zig) is the
lowest-ceremony binding: Zig consumes the C headers **directly** via `@cImport` —
no separate binding layer, no generated code. `@cImport` translates `asmtest.h` /
`asmtest_emu.h` into Zig declarations (the structs, the binding-ABI functions, and
the integer-constant flag masks), and Zig's target selects the right architecture
branch. No GC, so arg arrays are plain stack slices passed by pointer. See
[Language bindings](../bindings.md) for the shared architecture.

Like C++, Zig gates the optional assembler at **build time** (`-Dasm=true`), so
the default build stays Keystone-free.

## Setup

From the repository root, build the native library:

```sh
make shared-emu     # libasmtest_emu.{so,dylib} — capture trampoline + emulator
```

## Usage

```zig
const c = @cImport({
    @cInclude("asmtest.h");
    @cInclude("asmtest_emu.h");
});
extern fn add_signed(c_long, c_long) c_long;

test "add" {
    var r: c.regs_t = std.mem.zeroes(c.regs_t);
    var args = [_]c_long{ 40, 2, 0, 0, 0, 0 };
    c.asm_call_capture(&r, @ptrFromInt(@intFromPtr(&add_signed)), &args);
    try std.testing.expectEqual(@as(c_ulong, 42), r.ret);
    try std.testing.expect(c.asmtest_check_abi(&r, null, 0) == 0);
}
```

The emulator (`c.emu_open` / `c.emu_call` / `c.emu_close`) returns a
`c.emu_result_t` whose `faulted` / `fault_addr` / `fault_kind` (`c.EMU_FAULT_READ`
…) surface invalid accesses as data rather than crashing — where and why a fault
hit, not just that one did. Prefer it for untrusted routines.

## In-line assembler (optional)

Add `@cInclude("asmtest_assemble.h")` to the `@cImport` and pass a routine as an
**assembly string**. The assembler lives in `libasmtest_emu_asm`; `zig build test
-Dasm=true` (`make zig-asm-test`) links it and compiles the asm test in, so the
default build stays Keystone-free.

```zig
var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
// Intel, up to six args + an instruction cap; returns 0 on a bad string, with
// the Keystone diagnostic from c.asmtest_asm_last_error().
_ = c.asmtest_emu_call_asm6(e, "mov rax, rdi; add rax, rsi; ret",
    c.ASM_SYNTAX_INTEL, 40, 2, 0, 0, 0, 0, 2, 0, &res);   // res.regs.rax == 42

// Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
var buf: [16]u8 = undefined;
const n = c.asmtest_asm_bytes(c.ASM_ARM64, c.ASM_SYNTAX_INTEL, "ret", 0x00100000, &buf, buf.len);
```

## Function reference

Zig calls the binding-ABI entry points **directly** through `@cImport` (the `c.`
prefix), so the "functions available" are the C contract from the
[API reference](../api-reference.md), used Zig-idiomatically: `std.mem.zeroes` for
result structs, fixed-size stack arrays passed by pointer, and
`@ptrFromInt(@intFromPtr(&fn))` to hand a routine address as `?*anyopaque`. Every
emulator call returns its status as a value and writes the result into an
out-struct; `max_insns = 0` runs to `ret`.

### Capture tier

```zig
var r: c.regs_t = std.mem.zeroes(c.regs_t);
const fn = @as(?*anyopaque, @ptrFromInt(@intFromPtr(&add_signed)));

var args = [_]c_long{ 40, 2, 0, 0, 0, 0 };          // 6 integer slots
c.asm_call_capture(&r, fn, &args);

var iargs = [_]c_long{0} ** 6;
var fargs = [_]f64{ 1.5, 2.25 } ++ [_]f64{0} ** 6;  // up to 8 doubles
c.asm_call_capture_fp(&r, fn, &iargs, &fargs);

var vargs: [8]c.vec128_t = std.mem.zeroes([8]c.vec128_t);
vargs[0].f32 = .{ 1, 2, 3, 4 };                     // up to 8 128-bit vectors
c.asm_call_capture_vec(&r, fn, &iargs, &vargs);

_ = r.ret;                          // integer return (rax)
_ = r.fret;                         // scalar double return (xmm0)
_ = (r.flags & c.ASMTEST_CF) != 0;  // condition flag (ASMTEST_CF/PF/ZF/SF/OF)
_ = r.vec[0].f32[0];                // a lane of vector register 0
_ = c.asmtest_check_abi(&r, null, 0) == 0;  // every callee-saved register restored
```

### Emulator tier

```zig
const e = c.emu_open();
defer c.emu_close(e);
var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);

var args = [_]c_long{ 40, 2 };
_ = c.emu_call(e, fn, 64, &args, 2, 0, &res);            // addr: copies 64 bytes, ints
_ = c.emu_call(e, &code, code.len, &args, 2, 0, &res);   // or raw bytes (run whole)
_ = c.emu_call_fp(e, &code, code.len, &iargs, 0, &fargs, 2, 0, &res);  // doubles -> xmm0..7
_ = c.emu_call_vec(e, &code, code.len, &iargs, 0, &vargs, 2, 0, &res); // 128-bit vecs
_ = c.emu_call_win64(e, &code, code.len, &args, 2, 0, &res);          // Microsoft x64

_ = res.faulted;                 // invalid access? (data, not a crash)
_ = res.fault_addr;              // where (valid when faulted)
_ = res.fault_kind;              // c.EMU_FAULT_NONE / READ / WRITE / FETCH
_ = res.regs.rax;                // any GP register; also res.regs.rip / rflags
_ = res.regs.xmm[0].f64[0];      // scalar FP return; .f32[lane] for a vector return
```

Each `emu_call*` signature is `(e, code, code_len, args…, max_insns, &res)` and
returns a `bool` you can ignore (the verdict is in `res`).

### Execution trace / coverage

The trace struct owns caller-provided buffers (no allocator needed):

```zig
var ib: [64]u64 = undefined;
var bb: [64]u64 = undefined;
var tr: c.emu_trace_t = std.mem.zeroes(c.emu_trace_t);
tr.insns = &ib;  tr.insns_cap = ib.len;
tr.blocks = &bb; tr.blocks_cap = bb.len;

_ = c.emu_call_traced(e, &code, code.len, &args, 1, 0, &res, &tr);
_ = c.emu_trace_covered(&tr, 0);   // basic block at byte-offset entered?
_ = tr.insns_total;                // instructions executed (counts past the cap)
_ = tr.blocks_len;                 // distinct basic blocks recorded
```

### Cross-arch guests (raw bytes, any host)

```zig
const g = c.emu_arm64_open();            // also emu_riscv_open / emu_arm_open
defer c.emu_arm64_close(g);
var ar: c.emu_arm64_result_t = std.mem.zeroes(c.emu_arm64_result_t);
_ = c.emu_arm64_call(g, &code, code.len, &args, 2, 0, &ar);
_ = ar.regs.x[0];        // arm64 x0..x30; ar.regs.sp / pc / nzcv; ar.regs.v[0].f64[0]
// riscv: res.regs.x[10] (a0) / res.regs.pc / res.regs.f[0].f64[0]
// arm:   res.regs.r[0] / res.regs.r[13] (sp) / res.regs.q[0].f64[0]
```

`emu_arm64_call_traced` records into an `emu_trace_t` as above (arm64).

### In-line assembler (build with `-Dasm=true`)

```zig
var res: c.emu_result_t = std.mem.zeroes(c.emu_result_t);
// assemble x86-64 + run: (e, src, syntax, a0..a5, nargs, max_insns, &res); 0 on a bad string
_ = c.asmtest_emu_call_asm6(e, "mov rax, rdi; ret", c.ASM_SYNTAX_INTEL,
                            42, 0, 0, 0, 0, 0, 1, 0, &res);
_ = c.asmtest_asm_last_error();   // Keystone diagnostic ("" on success)

var buf: [16]u8 = undefined;       // assemble-only, any arch
const n = c.asmtest_asm_bytes(c.ASM_ARM64, c.ASM_SYNTAX_INTEL, "ret", 0x00100000, &buf, buf.len);
```

* `asmtest_emu_call_asm6` — `syntax` is `c.ASM_SYNTAX_INTEL`/`ATT`/`NASM`/`MASM`/
  `GAS`; six scalar arg slots + `nargs`; returns `0` on failure.
* `asmtest_asm_bytes(arch, syntax, src, addr, buf, cap)` — `arch` is
  `c.ASM_X86_64`/`ASM_ARM64`/`ASM_RISCV64`/`ASM_ARM32`; returns the byte count (and
  the needed size if `> cap`).

## Run the tests

```sh
make zig-test       # from the repo root
make zig-asm-test   # adds libasmtest_emu_asm and compiles the asm test in
```

`make zig-test` builds the shared libs + a routine fixture lib, then runs `zig
build test`, which compiles
[`src/conformance.zig`](https://github.com/wilvk/asm-test/blob/main/bindings/zig/src/conformance.zig)
— the conformance corpus replayed in Zig — and links the libraries.
[`build.zig`](https://github.com/wilvk/asm-test/blob/main/bindings/zig/build.zig)
targets Zig 0.13.x and takes `-Dincdir=` / `-Dlibdir=` to locate the headers and
shared libs.

## Maturity

A published Zig package (`build.zig.zon` + module export for `@import("asmtest")`)
and a Tier-2 idiomatic assertion layer are future work; this is the Tier-1 binding
that proves the `@cImport` path. See [Packaging the bindings](../packaging.md).
