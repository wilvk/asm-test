# R5: the per-guest value producer (arm64 first) — implementation

> **Root R5 of the [extension roadmap](27-extension-roadmap.md).** The emulator L0
> value producer is hard-wired to the x86-64 guest; every value fabric, and
> Author-mode run/trace, is x86-64-only until a per-guest producer exists. This
> root arch-parameterizes the producer, arm64 first. Independent of R1–R4 (a
> different axis); **demand-gated** on a non-x86-64 persona.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.

## Why this work exists

The plan states the limit plainly and the code enforces it: "value fabrics are
x86-64-guest-only until a per-guest valtrace producer exists" (plan Honest
limits); Author mode shows non-x86-64 code as bytes + a labelled "run/trace is
x86-64-only in v1" ([06](06-doors-and-learning.md) door;
`desktop/src/author_vm.cpp:18`). The Loom says the same: "arm64-guest fabrics —
no arm64-code value producer exists; the fabric is x86-64-guest-only and says so"
([05](05-loom-day-one.md) out-of-scope). The plan's expansion table lists an
"*(opportunistic)* arm64-guest emulator L0 value producer — `src/dataflow_emu.c`
guest seam — medium, demand-gated."

The producer is a self-contained Unicorn client with **every arch decision inline
at one call site**, so arch-parameterizing it is mechanical but broad — nothing is
generic today.

## What already exists (verified 2026-07-28)

`asmtest_dataflow_emu_run` ([dataflow_emu.c:249-309](../../../src/dataflow_emu.c#L249))
hard-codes the x86-64 guest at each seam:

- **Engine open** — `uc_open(UC_ARCH_X86, UC_MODE_64, …)`
  ([dataflow_emu.c:257](../../../src/dataflow_emu.c#L257)).
- **Register map** — `cap_x86_to_uc` maps Capstone `X86_REG_*` → `UC_X86_REG_*`
  ([dataflow_emu.c:64-137](../../../src/dataflow_emu.c#L64)); reader `df_reg_read`
  (`:139`).
- **Operand enumerator arch tag** — `asmtest_operands(ASMTEST_ARCH_X86_64, …)`
  ([dataflow_emu.c:186](../../../src/dataflow_emu.c#L186)).
- **ABI arg table + GP zero** — SysV integer `arg_regs {rdi,rsi,rdx,rcx,r8,r9}`
  ([dataflow_emu.c:273-275](../../../src/dataflow_emu.c#L273)); `df_zero_gp`
  (`:270`); fixed guest layout constants (`DF_CODE_BASE` / `DF_STACK_BASE`,
  `:33-37`).
- **The per-step ring + regstate deck** are likewise x86-64 (`emu_x86_regs_t`,
  [R4](31-wide-register-deck.md); ring [asmtest_emu.h:592-610](../../../include/asmtest_emu.h#L592)).
- **The descriptor mechanism** already parameterizes the *reader* on
  `@<arch>/<abi>` ([asmtrace-schema.md:342-359](asmtrace-schema.md#L342)) — a new
  guest lands as a new descriptor id, no reader change.

## Tasks

### T1 — extract the guest seam behind a vtable  (M)

**Goal.** Factor the six inline arch decisions into a `df_guest` descriptor
(open-mode, reg map, operand-enumerator arch, arg table, GP-zero set, layout
constants) so `asmtest_dataflow_emu_run` selects one by guest arch and keeps its
current x86-64 behaviour byte-identical.

**Steps.**
1. Define `df_guest` with the six seams; move the x86-64 constants into a
   `df_guest_x86_64` instance; route the run through it.
2. **Byte-identity gate** — `make asmtrace-golden` unchanged for the x86-64 corpus
   after extraction (pure refactor).

**Done when.** x86-64 golden byte-identical; the guest seam is data, not inline.

### T2 — the arm64 guest  (L, depends on T1)

**Goal.** A `df_guest_arm64` instance: `UC_ARCH_ARM64`, a `cap_arm64_to_uc` reg
map, AAPCS64 arg table (x0–x7), `ASMTEST_ARCH_ARM64` operand-enumerator coverage,
and an arm64 GP-zero set + layout.

**Steps.**
1. Implement each seam for arm64; confirm the operand enumerator
   (`asmtest_operands`) covers `ASMTEST_ARCH_ARM64` to the depth the value capture
   needs (extend it if not — a missing enumerator arm is producer work, not a
   blocker per the repo's missing-dependency rule).
2. An arm64 regfile descriptor + `regstate` body (`user_regs@aarch64/aapcs64` or
   the emulator analogue), so the Scrubber time-travels an arm64 capture through
   the same descriptor path — no reader change.
3. Run the corpus routines that are arch-neutral (or add arm64 worked examples)
   under the arm64 guest.

**Done when.** An arm64 leaf routine produces a value fabric + `regstate`; the
Loom, Slice, Timeline, and Scrubber render it via the descriptor mechanism with
**no desktop change**; a golden arm64 recording is committed and byte-stable.

### T3 — Author-mode arm64 + honest gate flip  (M, depends on T2)

**Goal.** Author mode runs/traces arm64 code, and the "x86-64-only in v1" label
becomes conditional.

**Steps.**
1. `author_vm` runs arm64 through the new guest when the assembled arch is arm64;
   the labelled refusal (`author_vm.cpp:18`) narrows to arches still unsupported
   rather than "x86-64 only".
2. The capability panel ([06](06-doors-and-learning.md)) reports arm64 as a
   positive capability where the guest is available.

**Done when.** An arm64 routine authored in the box runs on the emulator and
weaves a fabric; unsupported arches keep an honest, now-accurate label;
`test_author` / `desktop-ui-test` cover the arm64 path.

## Unblocks / downstream

- arm64 value fabrics (05), arm64 Author-mode run/trace (06), arm64 Scrubber
  time-travel (09/26 via descriptor).
- **Partial progress on N-ISA braids** — a second guest is the precondition; the
  N-way *semantic alignment* remains unfunded (plan Killed-in-grounding) and is
  not in scope here.
- The seam generalizes: a third guest (RISC-V) is then another `df_guest`
  instance, not another rewrite.

## Non-goals / honest limits

- **N-way lockstep braids stay out of scope** — this root lands the per-guest
  producer, not the cross-ISA semantic alignment the plan killed as unfunded.
- Live (ptrace) arm64 capture is a separate host concern (the `--dataflow` engine
  is x86-64-fixed under its own guards); this root is the **emulator** value
  producer. A live arm64 path is a further brief.
- Each guest ships its own descriptor + honesty fixtures (D6/D7) — no guest is
  "mostly x86-64 with tweaks"; the seam is explicit per arch.

## Cross-references

Producer [dataflow_emu.c](../../../src/dataflow_emu.c); operand enumerator
`asmtest_operands`; descriptor mechanism [asmtrace-schema.md](asmtrace-schema.md)
(reader is already arch-parameterized); consumers [05](05-loom-day-one.md),
[06](06-doors-and-learning.md), [09](09-teaching-producers.md). Relates to
[R4](31-wide-register-deck.md) (each guest's vector deck is that guest's
descriptor). Missing-enumerator-arm policy: repo `CLAUDE.md` (add it, do not
self-skip). Golden/honesty D6/D7.
