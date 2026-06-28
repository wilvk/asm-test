# asm-test — Zig binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from Zig.

The lowest-ceremony binding: Zig consumes the C headers **directly** via
`@cImport` — no separate binding layer, no generated code. `@cImport` translates
`asmtest.h` / `asmtest_emu.h` into Zig declarations (the structs, the
binding-ABI functions, and the integer-constant flag masks), and Zig's target
selects the right architecture branch. No GC, so arg arrays are plain stack
slices passed by pointer.

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
**assembly string**. The assembler lives in `libasmtest_emu`, compiled in by
default, so `zig build test` (and `make zig-test`) links it and compiles the asm
test in.

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

## Build the shared libraries

```sh
make shared-emu     # from the repo root: libasmtest_emu.{so,dylib}
```

## Tests

`make zig-test` (from the repo root) builds the shared libs + a routine fixture
lib, then runs `zig build test`, which compiles
[`src/conformance.zig`](src/conformance.zig) — the conformance corpus replayed
in Zig — and links the libraries. [`build.zig`](build.zig) targets Zig 0.13.x and
takes `-Dincdir=` / `-Dlibdir=` to locate the headers and shared libs.

## DynamoRIO native-trace tier (optional)

[`src/drtrace.zig`](src/drtrace.zig) mirrors the Python `asmtest.drtrace`
wrapper: trace **host-native** code as it runs in-process via DynamoRIO. Unlike
the rest of the binding, it does **not** link `libasmtest_drapp` (that lib needs
DynamoRIO and may be absent) — it `dlopen()`s the lib at runtime with
`std.DynLib`, resolving the path from `$ASMTEST_DRAPP_LIB` (else
`<repo>/build/libasmtest_drapp.so`). `drtrace.available()` self-skips cleanly
when the tier can't run.

```zig
const drtrace = @import("drtrace.zig");
if (!drtrace.available()) return;          // self-skip
try drtrace.initializeDefault();           // null client -> $ASMTEST_DRCLIENT
defer drtrace.shutdown();
var code = try drtrace.NativeCode.fromBytes(&bytes);
var tr = try drtrace.NativeTrace.create(64, 0); // (blocks, instructions)
try tr.register("add", &code);
tr.begin("add"); const r = code.call(20, 22); tr.end("add"); // r == 42
_ = tr.covered(0); _ = tr.blocksLen();
```

The smoke test [`src/drtrace_test.zig`](src/drtrace_test.zig) (the Zig analogue
of `tests/test_drtrace.py`) is a **separate** build step — it never links the
drapp lib, so it can't break `zig build test`:

```sh
cd bindings/zig
ASMTEST_DRAPP_LIB=$PWD/../../build/libasmtest_drapp.so \
ASMTEST_DRCLIENT=$PWD/../../build/libasmtest_drclient.so \
  zig build drtrace-test -Dincdir=$PWD/../../include
# or, equivalently, as a standalone:
#   zig run src/drtrace_test.zig -lc -I$PWD/../../include
```

It prints `SKIP: ...` (exit 0) when the tier is unavailable and `PASS` on
success. The `make docker-drtrace` lane builds the lib + client in a container.

## Deferred

A published Zig package (`build.zig.zon` + module export for `@import("asmtest")`)
and a Tier-2 idiomatic assertion layer are future work; this is the Tier-1
binding that proves the `@cImport` path.
