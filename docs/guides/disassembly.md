# Disassembly

asm-test can annotate emulator faults, traces, and coverage reports with
human-readable instructions. The disassembler is optional, backed by Capstone,
and lives in the emulator tier rather than the core native runner.

Use it when byte offsets are not enough:

- a fault report should name the instruction that touched bad memory
- a trace should read like an instruction listing
- an uncovered block report should show which instruction was missed
- a binding wants a simple `disas(bytes, off)` helper for diagnostics

## Build

The disassembly helpers are compiled from `src/disasm.c`. They are
auto-detected with `pkg-config --exists capstone`.

```sh
make deps DEPS_ARGS=--emu
make emu-test
```

For language bindings and packaged native payloads, build the emulator library:

```sh
make shared-emu
```

`libasmtest_emu` is the full superset — it carries the emulator, the Keystone
in-line assembler, and the Capstone disassembler.

The API is self-skipping. If Capstone is absent, `emu_disas_available()` returns
false, `emu_disas(...)` returns `0` and writes an empty string, and report helpers
fall back to offset-only output.

## One Instruction

`emu_disas` decodes the instruction at a byte offset within a routine:

```c
#include "asmtest.h"
#include "asmtest_emu.h"

TEST(disasm, decodes_one_instruction) {
    uint8_t code[] = {0x48, 0x89, 0xc0}; /* mov rax, rax */
    char text[128];

    if (!emu_disas_available())
        SKIP("Capstone not in this build");

    size_t n = emu_disas(EMU_ARCH_X86_64, code, sizeof code,
                         EMU_CODE_BASE, 0, text, sizeof text);

    ASSERT_UEQ(n, 3);
    ASSERT_TRUE(strstr(text, "mov") != NULL);
    ASSERT_TRUE(strstr(text, "rax") != NULL);
}
```

Arguments:

| Argument | Meaning |
|---|---|
| `arch` | Guest ISA: `EMU_ARCH_X86_64`, `EMU_ARCH_ARM64`, `EMU_ARCH_RISCV64`, or `EMU_ARCH_ARM32` |
| `code`, `code_len` | Routine bytes, starting at offset zero |
| `base_addr` | Address those bytes run at, usually `EMU_CODE_BASE` |
| `off` | Byte offset of the instruction to decode |
| `buf`, `buflen` | Caller-owned output buffer |

The return value is the decoded instruction length in bytes. A return value of
`0` means "not decoded": Capstone is absent, the guest is not supported by this
Capstone build, the offset is outside the buffer, or the bytes are invalid.

`base_addr` matters for PC-relative operands. Passing the same address used by
the emulator lets Capstone print resolved branch and memory targets.

## Faults

`emu_fault_describe` turns an `emu_result_t` into a one-line diagnostic:

```c
emu_result_t r;
long args[] = {0};
emu_call(E, (void *)load_from_arg, 64, args, 1, 0, &r);

char msg[160];
emu_fault_describe(&r, EMU_ARCH_X86_64, (const uint8_t *)load_from_arg, 64,
                   EMU_CODE_BASE, msg, sizeof msg);
```

With Capstone, a fault includes the offending instruction:

```text
read fault accessing 0x0: mov rax, qword ptr [rdi]  (@0x0)
```

Without Capstone, the same call still produces useful offset-only output:

```text
read fault accessing 0x0  (@0x0)
```

`emu_watch_describe` does the same for emulator write-watchpoint violations.

## Trace And Coverage Reports

The traced emulator calls record instruction and basic-block offsets in an
`emu_trace_t`. The disassembly report helpers annotate those offsets:

```c
uint64_t insns[64], blocks[32];
emu_trace_t tr = {0};
tr.insns = insns;
tr.insns_cap = 64;
tr.blocks = blocks;
tr.blocks_cap = 32;

emu_call_traced(E, (void *)classify, 64, args, 1, 0, &r, &tr);

emu_trace_disasm(&tr, EMU_ARCH_X86_64,
                 (const uint8_t *)classify, 64, stdout);
emu_trace_report_disasm(&tr, EMU_ARCH_X86_64,
                        (const uint8_t *)classify, 64, stdout);
```

Typical annotated coverage output:

```text
coverage: 3 distinct blocks, 3 block entries, 5 instructions
  blocks:
    0x0  xor rax, rax
    0x7  jl 0x10000e
    0x9  ret
```

For comparing a run against a "universe" trace, use
`emu_coverage_uncovered_disasm(covered, universe, arch, code, len, out)`.

## API Reference

| Function | Purpose |
|---|---|
| `emu_disas_available()` | Returns true when this build links Capstone |
| `emu_disas(arch, code, len, base, off, buf, n)` | Decodes one instruction at `off` |
| `emu_fault_describe(&r, arch, code, len, base, buf, n)` | Formats a fault with the faulting instruction when available |
| `emu_watch_describe(&w, arch, code, len, base, buf, n)` | Formats a write-watch violation with the offending store when available |
| `emu_trace_disasm(&tr, arch, code, len, out)` | Prints the ordered instruction trace with disassembly |
| `emu_trace_report_disasm(&tr, arch, code, len, out)` | Prints coverage blocks with each block's first instruction |
| `emu_coverage_uncovered_disasm(cov, uni, arch, code, len, out)` | Prints missed blocks with disassembly |

## Notes

RISC-V disassembly requires Capstone 5 or newer. Older Capstone builds can still
decode the other supported guests; `emu_disas` self-skips RISC-V by returning
`0`.

The disassembler is diagnostic-only. It does not change emulator execution and
does not require source-level debug information. Source-line coverage is handled
separately through caller-supplied line maps in the [emulator guide](emulator.md).
