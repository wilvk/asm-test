# L5: ARM32 + RISC-V run/trace in Author mode — another `df_guest` instance

> **Source.** Gap **L5** of [38-live-feed-completion-roadmap.md](38-live-feed-completion-roadmap.md)
> ("ARM32 + RISC-V run/trace in Author mode (another `df_guest` instance)" —
> Effort: Medium, "REAL, narrow audience"). Cut as its own per-gap brief per
> 38's own instruction ("cut a per-gap brief here (38+n) when picking one
> up"), mirroring how [39](../archive/gui/39-auto-capture-reliability.md)/
> [40](../archive/gui/40-segment-dataflow-by-invocation.md)/
> [41](../archive/gui/41-live-blame-statediff-serve-leg.md) were cut one at a
> time. Read [_conventions.md](../implementations/_conventions.md) first;
> D1–D11 live in [README.md](README.md).
>
> **Prerequisites.** [32-per-guest-value-producer.md](../archive/gui/32-per-guest-value-producer.md)
> (R5), which built the `df_guest` vtable and landed the arm64 instance —
> this brief is explicitly "another `df_guest` instance", not new
> infrastructure. Doc 32's own closing line: *"RISC-V is now another
> `df_guest` instance (and, following the same mirrored pattern, another
> `emu_<arch>_t` ring instance), exactly as designed."*
>
> Authored 2026-08-02. If a cited file:line disagrees with the code when you
> implement, the code wins — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/3 (ARM32) + spike, not started.**

## Why this work exists

Author mode (assemble typed-in text, run it on the emulator, weave a value
fabric) supports x86-64 and — since doc 32 — arm64. `author_arch_table()`
([author_vm.cpp](../../../desktop/src/author_vm.cpp)) still refuses ARM32 and
RISC-V with the shared label `kAuthorArchLimit`, currently "x86-64/AArch64-only
in v1". Doc 32 explicitly scoped RISC-V and ARM32 out as follow-on `df_guest`
instances rather than declaring them unreachable in principle.

## What already exists / what gates each target (verified 2026-08-02)

The two targets are **not symmetric** — this is the headline finding of the
scoping pass for this brief, and it changes the shape of the work:

- **ARM32 is fully unblocked today.** Keystone already builds `KS_ARCH_ARM`
  (`scripts/build-keystone.sh`'s `LLVM_TARGETS_TO_BUILD="AArch64;ARM;X86"`
  covers it; `src/assemble.c`'s `ASM_ARM32` row already dispatches
  `KS_ARCH_ARM`), Unicorn ships `UC_ARCH_ARM`, and Capstone decode already
  covers `ASMTEST_ARCH_ARM32` for disassembly. The one real gap is the
  **operand read/write-set enumerator**: `src/dataflow_operands.c`'s
  `cs_target()` returns `false` for `ASMTEST_ARCH_ARM32`
  (memory-operand branch `add_mem_ops` has no `CS_ARCH_ARM` case) — value-fabric
  capture needs it, exactly the "missing enumerator arm is producer work, not a
  blocker" case doc 32 T2 flagged for itself. ARM32 is doc-32-shaped end to end:
  a `df_guest_arm32` instance, an `emu_arm32_t` ring, an enumerator arm, an
  Author dispatch flip, a golden.
- **RISC-V has a real, previously-unsurfaced dependency gap.** The Keystone
  pin in `scripts/build-keystone.sh` is `0.9.2` (Feb 2019) — the script's own
  comment already states this is "upstream's newest **release**". `KS_ARCH_RISCV`
  does **not** exist in that release's header (confirmed against the locally
  built `/usr/local/include/keystone/keystone.h`). It **does** exist on
  upstream's unreleased `master` branch (confirmed 2026-08-02 by fetching
  `include/keystone/keystone.h` from `github.com/keystone-engine/keystone`
  at `master`: the `ks_arch` enum lists `KS_ARCH_RISCV` between `KS_ARCH_EVM`
  and `KS_ARCH_MAX`). So RISC-V assembly is blocked not by a hardware/credential
  gate (CLAUDE.md's only legitimate self-skip reasons) but by an **unreleased**
  upstream feature — the "built from pinned source" pattern applies (pin a
  commit, not a tag), but the commit has never been through a tagged release,
  so its stability is unverified by upstream itself. This is the one item in
  this brief that is a genuine spike, not mechanical mirroring.

## Tasks

### T1 — the ARM32 `df_guest` instance (M)

**Goal.** `df_guest_arm32`: `UC_ARCH_ARM` / `UC_MODE_ARM` (not Thumb — the
existing `ASM_ARM32` assembly path already commits to ARM mode, not Thumb
interworking, so this task inherits that choice rather than deciding it), a
`cap_arm32_to_uc` reg map (`r0`–`r12`, `sp`/`r13`, `lr`/`r14`, `pc`/`r15`,
`cpsr`), an AAPCS32 arg table (`r0`–`r3`), arm32 GP-zero set + layout
constants — mirroring `df_guest_arm64` in
[dataflow_emu.c](../../../src/dataflow_emu.c) exactly (doc 32 T1's vtable is
unchanged; this is a new instance selected by guest arch, not a new seam).

**Steps.**
1. `src/dataflow_operands.c`: extend `cs_target()` to accept
   `ASMTEST_ARCH_ARM32` → `CS_ARCH_ARM`/`CS_MODE_ARM`, and add the `CS_ARCH_ARM`
   branch to `add_mem_ops` (base + optional index register, offset — the same
   shape as the existing `CS_ARCH_ARM64` branch, adjusted for AArch32's
   `arm_op_mem` field names). `cs_regs_access` is already arch-generic
   (verified 2026-08-02) — no change needed there.
2. `src/dataflow_emu.c`: `df_guest_arm32` — open mode, `cap_arm32_to_uc`,
   `ASMTEST_ARCH_ARM32` enumerator tag, `{r0,r1,r2,r3}` arg table, GP-zero set,
   layout constants (reuse `DF_CODE_BASE`/`DF_STACK_BASE` unless ARM32's 32-bit
   address space needs its own — state which and why).
3. `src/emu.c` / `include/asmtest_emu.h`: `emu_arm32_t` gains its own
   drop-oldest ring mirroring `emu_arm64_t`'s (`emu_arm32_step_capture`/
   `_clear`/`_count`/`_dropped`/`_at` over `emu_arm32_regs_t`). Zero changes to
   `emu_t`/`emu_x86_regs_t`/`emu_arm64_t` or their snapshot/restore paths — a
   third mirrored ring, not a shared one.
4. A `regstate` descriptor `emu_arm32_regs_t@arm/aapcs32` in
   [asmtrace-schema.md](asmtrace-schema.md) (append-only) naming `r0`–`r12`,
   `sp`, `lr`, `pc`, `cpsr` — integer-only, no VFP/NEON deck, mirroring arm64's
   integer-only scope (D7: do not claim a register file this task does not
   capture).
5. `tools/asmtrace_record.c`: a `record_arm32` worked example (an ARM32
   `add`/`add`/`mov`/`bx lr` chain, or whatever the assembler round-trips
   cleanly) producing golden `arm32-df-chain.asmtrace` — `"arch":"arm"` via
   the existing `asmtrace_writer_set_arch` override. A D7 low-fidelity
   companion (`arm32-regstate-truncated.asmtrace`, small `steps_cap`) mirrors
   arm64's.

**Tests.** `examples/test_dataflow_emu.c` (`df_guest_arm32` produces the
expected value trace + def-use edges for the worked chain — mirror the arm64
case). `examples/test_emu.c` (`emu_arm32.step_capture_records_prestates` /
`_drops_oldest_and_counts` / `_arming_survives_across_calls_on_same_handle`,
mirroring `emu_arm64.*`). `make asmtrace-golden-check` clean on the new
goldens; the existing x86-64/arm64 goldens byte-identical (pure addition).

**Done when.** An ARM32 leaf routine produces a value fabric + `regstate`
through the descriptor mechanism with **no desktop reader change** (the Loom /
Slice / Timeline / Scrubber are already arch-generic over the descriptor, per
doc 32's own "no desktop change" result) — the same bar doc 32 T2 set for
arm64.

### T2 — Author-mode ARM32 + gate flip (M, depends on T1)

**Goal.** Author mode runs/traces ARM32 code; the shared refusal label
narrows again.

**Steps.**
1. `desktop/src/author_vm.cpp`: `author_arch_table()`'s `ASM_ARM32` row flips
   `can_run = true`, dispatching through
   `asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM32, …)` exactly as the
   arm64 row does (`author_run_vf`, the same re-declared-producer precedent
   `loom/forks.cpp` set). `kAuthorArchLimit` and RISC-V's own note narrow to
   name only RISC-V as still unsupported (verbatim wherever quoted elsewhere,
   per doc 32's own instruction for its label edits).
2. The capability panel ([06](../archive/gui/06-doors-and-learning.md) T6)
   reports ARM32 as a positive capability via `author_arch_table()` — one
   source of truth, no second hardcoded arch list.
3. `author_recording`'s `vf` path materialises the ARM32 fabric as
   `trace`/`df_step`/`df_edge` through the same writer the arm64 path uses —
   no `recording_to_asmtrace` change expected; state it if one is needed.

**Tests.** `test_author_vm.cpp`: the arch-gating table flip for ARM32, the
four `author_apply_run_vf` fidelity branches exercised for ARM32 (clean run,
non-return-sentinel, truncated buffer, producer setup failure), and an
end-to-end `author_recording` → `recording_to_asmtrace` → `load_recording`
round-trip carrying `trace`/`df_step`/`df_edge` for a synthetic ARM32 fabric.

**Done when.** An ARM32 routine authored in the box runs on the emulator and
weaves a fabric; RISC-V (until T3) keeps a faithful, now-accurate
single-arch label; `test_author_vm` / `desktop-ui-test` cover the ARM32 path.

### T3 — RISC-V: the Keystone spike, then the guest (L, gated on the spike)

**Goal.** Either RISC-V lands the same T1+T2 shape ARM32 does, or the
Keystone gap is recorded as a **stated, verified** blocker rather than
silently deferred — CLAUDE.md's dependency rule ("add it, do not self-skip")
applies here, but so does its own bar: a spike proves whether "add it" is
actually safe before this brief commits to it.

**Steps.**
1. **Spike (bounded).** Identify the oldest upstream commit on
   `keystone-engine/keystone`'s default branch after `0.9.2` at which
   `KS_ARCH_RISCV` exists and the tree still configures/builds cleanly under
   this repo's pinned CMake + the two existing patches
   (`scripts/build-keystone.sh`'s `CMP0051` sed and the `-include cstdint`
   force-include). Add `"RISCV"` to `LLVM_TARGETS_TO_BUILD`. Pin the exact
   commit via `scripts/refresh-thirdparty-digests.sh` (the same `git-commit
   keystone <version>` mechanism the existing tag pin uses — a branch name is
   not a valid pin; the version string becomes the commit's short form or a
   new synthetic version label, whichever `lib-thirdparty.sh`'s digest
   manifest already expects — check `tp_digest`'s contract before inventing a
   new format).
2. **Verify, do not assume.** Build the new pin in the `docker-cli`/
   `docker-desktop` image (wherever Keystone is built today), assemble a
   trivial RISC-V routine (`addi`/`add`/`ret` or equivalent), and confirm the
   emitted bytes actually execute correctly under Unicorn's `UC_ARCH_RISCV`
   (not merely that `ks_asm` returns success — an unreleased backend can
   assemble a wrong encoding and still return 0 errors). If this step fails
   or the pinned commit does not build cleanly within a reasonable number of
   attempts, **stop here**: record the exact failure (commit tried, build or
   semantic error) in this doc's status and leave RISC-V's `author_arch_table`
   row and `kAuthorArchLimit` unchanged (still refusing, still naming
   RISC-V) — a real, verified blocker, not a guess.
3. **If the spike succeeds**, T3 becomes T1+T2's mirror for RISC-V:
   `df_guest_riscv` (`UC_ARCH_RISCV`, `UC_MODE_RISCV64` — confirm which XLEN
   the assembled default targets and match Unicorn's mode to it), a
   `cap_riscv_to_uc` reg map, the RV64 integer calling convention (`a0`–`a7`),
   `dataflow_operands.c`'s `CS_ARCH_RISCV` branch, `emu_riscv_t`'s ring, a
   `regstate` descriptor, golden `riscv-df-chain.asmtrace`, and the
   `author_arch_table` flip — each step cross-referencing the matching ARM32
   step above rather than re-deriving it.

**Tests.** If the spike succeeds: the T1/T2 test list above, RISC-V flavoured.
If it does not: no new tests — the existing "RISC-V refuses with a stated
label" coverage (already exercised by `test_author_vm.cpp`'s arch-gating
table) is the correct, and sufficient, assertion of the honest blocked state.

**Done when.** Either a RISC-V routine authored in the box runs and weaves a
fabric with the same fidelity bar as ARM32, or this doc states, with the
exact commit and error tried, why not — never a silent "RISC-V: TODO".

## Non-goals / acknowledged limits

- **Live (ptrace) ARM32/RISC-V capture** is out of scope — this brief is the
  **emulator** value producer for Author mode, mirroring doc 32's own
  boundary. A live path for either architecture is a further brief, and for
  ARM32 specifically is a much smaller lift than [L1](38-live-feed-completion-roadmap.md)'s
  arm64 leg (no known detach-fatal single-step hazard on ARM32 has been
  recorded in this tree — unverified, not claimed here).
- **ARM32/Thumb interworking** is out of scope. The existing Keystone
  `ASM_ARM32` path already assembles ARM-mode only; this brief inherits that,
  it does not decide it.
- **RISC-V compressed instructions (RVC) / vector extension** are out of
  scope even if T3's spike succeeds — the base integer ISA only, mirroring
  arm64's "integer-only, no vector deck" scope from doc 32.
- **N-way lockstep / cross-ISA semantic alignment** stays out of scope,
  exactly as doc 32 stated for arm64.

## Cross-references

Producer [dataflow_emu.c](../../../src/dataflow_emu.c); operand enumerator
`asmtest_operands` ([dataflow_operands.c](../../../src/dataflow_operands.c));
descriptor mechanism [asmtrace-schema.md](asmtrace-schema.md); Author door
[author_vm.cpp](../../../desktop/src/author_vm.cpp); consumers
[05](../archive/gui/05-loom-day-one.md),
[06](../archive/gui/06-doors-and-learning.md),
[09](../archive/gui/09-teaching-producers.md). Parent root
[32](../archive/gui/32-per-guest-value-producer.md) (R5) — this brief is its
own closing line made concrete. Missing-enumerator-arm / missing-dependency
policy: repo `CLAUDE.md`. Golden/fidelity D6/D7.
