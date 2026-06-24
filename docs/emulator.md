# Emulator tier (optional)

ABI-boundary capture sees the registers *after* a routine returns. The emulator
tier runs a routine inside a virtual CPU ([Unicorn Engine]) so you can do what a
real call cannot: preload arbitrary registers and memory, single-step to a point
*mid-routine*, read back the **full** register file, catch invalid memory
accesses as **precise faults**, and measure **basic-block coverage**.

Because the guest CPU is independent of the host, the AArch64, RISC-V, and ARM32
guests emulate even on an x86-64 host.

## Building and running

The tier needs **libunicorn** and is compiled with `-lunicorn`. Install it and
run the emulator suites:

```sh
make deps DEPS_ARGS=--emu     # install libunicorn (and only that)
make emu-test                 # build and run the emulator suites
```

In a test, include the extra header alongside the main one:

```c
#include "asmtest.h"
#include "asmtest_emu.h"
```

## The x86-64 guest

The x86-64 path copies bytes from a built routine into the guest and runs them:

```c
extern long add_signed(long a, long b);

#define CODE_WINDOW 64   /* >= the routine's byte length; emulation stops at ret */

static emu_t *E;
SETUP(emu)    { E = emu_open(); }
TEARDOWN(emu) { emu_close(E); E = NULL; }

TEST(emu, runs_routine_in_isolation) {
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_EMU_REG_EQ(&r, rax, 42);
}
```

`emu_call(e, fn, code_len, args, nargs, max_insns, &out)` runs `fn` with System V
integer arguments and fills an `emu_result_t`. `code_len` only needs to be **at
least** the routine's length — emulation stops at the routine's own `ret`. Pass
`max_insns > 0` to stop after that many instructions instead, for mid-routine
inspection.

### Reading the result

```c
typedef struct {
    bool ok;                  // ran to ret (or hit the step limit) cleanly
    int uc_err;               // raw Unicorn error code (0 == OK)
    bool faulted;             // an invalid memory access occurred
    uint64_t fault_addr;      // faulting guest address
    emu_fault_kind_t fault_kind;  // READ / WRITE / FETCH / NONE
    emu_x86_regs_t regs;      // full GP file + rip/rflags + xmm[16]
} emu_result_t;
```

The full register file means you can see what a routine left in a register
*mid-flight* — including a callee-saved register it clobbered and restored, which
an ABI-boundary check can never observe:

```c
TEST(emu, reveals_clobbered_callee_saved) {
    emu_result_t r;
    long args[] = {3, 4};
    emu_call(E, (void *)clobbers_rbx, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_EQ(r.regs.rax, 7);
    ASSERT_EQ(r.regs.rbx, 7);   // the value left in rbx mid-routine
}
```

### Preloading memory

Map a guest region, write to it, run a routine that reads it, and read the result
back:

```c
emu_map(E, 0x100000, 0x1000);
emu_write(E, 0x100000, &value, sizeof value);
/* call a routine that loads from 0x100000 … */
emu_read(E, 0x100000, &out, sizeof out);
```

### Faults

An invalid access sets `faulted`, `fault_addr`, and `fault_kind` instead of
crashing the harness:

```c
ASSERT_NO_FAULT(&r);                                  // expect a clean run
ASSERT_FAULT(&r);                                     // expect *some* fault
ASSERT_FAULT_AT(&r, EMU_FAULT_WRITE, 0xdead0000);     // a specific fault
```

### Floating-point and vectors

`emu_call_fp` marshals `double` args into `xmm0`–`xmm7`; `emu_call_vec` marshals
128-bit vectors. Both capture the whole XMM file (`r.regs.xmm[]`):

```c
ASSERT_EMU_FP_EQ(&r, 3.75);          // r.regs.xmm[0].f64[0]
ASSERT_EMU_VEC_EQ(&r, 0, &want);     // a full 128-bit lane
```

## Other guests

All guests share the same shape (`*_open`, `*_map`, `*_write`, `*_read`,
`*_call`, `*_call_traced`) and the same fault, trace, and coverage hooks. The
cross-architecture guests take **raw machine code** (you can't copy bytes from a
host-native routine of a different ISA).

| Guest | Open | Args in | Return | Notes |
|---|---|---|---|---|
| **x86-64** (System V) | `emu_open` | `rdi, rsi, rdx, rcx, r8, r9` | `rax` | copies bytes from a built routine |
| **AArch64** | `emu_arm64_open` | `x0`–`x7` | `x0` | NEON `v[]`; `emu_arm64_call_fp/_vec` |
| **RISC-V (RV64)** | `emu_riscv_open` | `a0`–`a7` | `a0` | no flags register; scalar FP (`f[]`) |
| **ARM32 (A32)** | `emu_arm_open` | `r0`–`r3` (AAPCS) | `r0` | full `r0`–`r15` + `cpsr`; scalar FP |
| **Windows x64** | (x86-64 engine) | `rcx, rdx, r8, r9` | `rax` | `emu_call_win64`; 32-byte shadow space, `rsi`/`rdi` nonvolatile |

### Windows x64 on a System V host

Win64 rides the existing x86-64 engine rather than needing a Windows host. The
contrast is instructive — identical bytes (`mov rax, rcx`) yield different results
under the two conventions, because Win64 puts the first integer argument in `rcx`
while System V puts it in `rdi`:

```c
long args[] = {111, 222};
emu_call_win64(E, (void *)mov_rax_rcx, CODE_WINDOW, args, 2, 0, &r);
ASSERT_EMU_REG_EQ(&r, rax, 111);   // Win64: first arg in rcx
```

## Coverage & tracing

The `_traced` variants take an opt-in `emu_trace_t` and record, into
caller-owned buffers, an **instruction trace** (each executed instruction's byte
offset from the routine entry, in order) and **basic-block coverage** (the
*distinct* block-start offsets). Either buffer may be `NULL` to skip that
dimension.

```c
uint64_t blocks[64];
emu_trace_t tr = {0};
tr.blocks = blocks;
tr.blocks_cap = 64;

long inputs[] = {-5, 0, +7};         // three branch paths in `classify`
for (int i = 0; i < 3; i++) {
    emu_result_t r;
    long a[] = { inputs[i] };
    emu_call_traced(E, (void *)classify, CODE_WINDOW, a, 1, 0, &r, &tr);
}
ASSERT_BLOCKS_AT_LEAST(&tr, 3);      // the union covered all three paths
```

Tracing **appends**, so re-running with the same struct unions coverage across
inputs — that's how you answer *"did the tests exercise every branch?"*. Key
fields:

| Field | Meaning |
|---|---|
| `insns` / `insns_len` / `insns_total` | ordered trace buffer; entries written; total executed (counts past the cap) |
| `blocks` / `blocks_len` / `blocks_total` | distinct block buffer; distinct count; total entries (a loop re-counts each pass) |
| `truncated` | a buffer filled and at least one entry was dropped |

### Reporting

| Function | Purpose |
|---|---|
| `emu_trace_report(&tr, out)` | human-readable trace/coverage summary |
| `emu_coverage_uncovered(covered, universe, …)` | blocks a run missed vs. a universe trace |
| `emu_trace_lcov(&tr, name, out)` | offset-level lcov export |
| `emu_trace_covered(&tr, off)` | predicate: was an offset's block entered? |

Coverage assertions:

```c
ASSERT_BLOCK_COVERED(&tr, 0x12);     // a specific block offset was entered
ASSERT_BLOCKS_AT_LEAST(&tr, 3);      // at least N distinct blocks
```

## When to reach for the emulator

| Use the emulator when you need… | Otherwise use… |
|---|---|
| Mid-routine register state | [ABI capture](abi-capture.md) (post-return only) |
| Precise memory faults at an address | [guard-page buffers](runner.md#guard-page-buffers) |
| A non-host architecture (ARM/RISC-V) on this host | native build for the host arch |
| The Windows x64 ABI on a Unix host | — |
| Branch / basic-block coverage | — |

The emulator pairs naturally with [property testing](property-testing.md): a
looping or faulting fuzz input is contained by the instruction cap and fault
hooks instead of taking down the run.

[Unicorn Engine]: https://www.unicorn-engine.org/
