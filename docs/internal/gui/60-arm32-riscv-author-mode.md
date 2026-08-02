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
> **Status — ✅ 3/3 (T1 + T2 + T3 all landed). Doc complete.**

## Status (2026-08-02) — T3 (RISC-V) spike run, landed in full

The T3 Keystone spike (this doc's own gate on RISC-V) **succeeded on the
first candidate tried**, and T3 landed in full, mirroring T1/T2's ARM32
shape end to end — **docker-cli/docker-desktop verified**
(`make dataflow-test`, `make emu-test`, `make cli-smoke` including
`asmtrace-golden-check`; `make desktop-test` + `desktop-ui-test` +
`desktop-test-xvfb`):

- **The spike.** Upstream's pinned release (0.9.2, Feb 2019) has no
  `KS_ARCH_RISCV`. Searched `keystone-engine/keystone`'s default branch via
  the GitHub commits API (`?q=repo:keystone-engine/keystone+riscv`, sorted
  oldest-to-newest after the release tag) and found the RISC-V merge
  (`ce229be18379`, 2023-05-19) plus a much more recent commit —
  `0d9567f08c0c23e8f604b2cad3d49450c93cfb40` ("Fix build compatibility with
  CMake 4, GCC 15, and MSVC", 2026-07-18, only 4 commits ahead of the
  RISC-V merge on the direct line) — that ALSO independently fixes the
  exact CMake4/CMP0051 and GCC13+/`<cstdint>` issues this repo's own
  `build-keystone.sh` already patches around, plus an MSVC register-info
  bug in the RISC-V backend itself. That recency plus "fixes our own
  workarounds for free" made it candidate 1, tried first over the older
  merge commit. **Candidate 1 built clean** inside `docker-cli`'s base
  image (the CMP0051 sed and `-include cstdint` patches both correctly
  no-op against it — confirmed by grep before applying) with `"RISCV"`
  added to `LLVM_TARGETS_TO_BUILD`. No second candidate was needed.
- **Verification (beyond `ks_asm` returning 0).** Assembled
  `.option norvc; addi a0, a0, 5; add a0, a0, a1; ret` with the new
  Keystone, hand-verified the emitted bytes against the known RV64I
  encoding (`0x00550513`/`0x00b50533`/`0x00008067`, byte-exact match),
  decoded them back with Capstone (`CS_ARCH_RISCV`/`CS_MODE_RISCV64` —
  already supported by this repo's pinned Capstone 5.0.1, no bump needed)
  to confirm the round-trip, and executed them under Unicorn
  (`UC_ARCH_RISCV`/`UC_MODE_RISCV64` — already in the apt-packaged Unicorn
  2.0.1 this repo uses) with `a0=10, a1=7`, confirming `a0 == 22` after
  execution — the produced bytes are the CORRECT encoding AND execute
  correctly, not merely non-erroring.
- **The pin.** `KS_ARCH_RISCV` never shipped in a tagged Keystone release,
  so `scripts/build-keystone.sh`'s `VERSION` is now a git **commit**
  (`0d9567f08c0c`, the short-sha manifest key), not a tag — the same
  mechanism `scripts/fetch-libdft.sh`/`scripts/build-dynamorio-macos.sh`
  already use for their own untagged pins. The clone step switched from
  `git clone --branch` (tags/branches only) to fetch-by-SHA with a
  full-clone fallback (works for a tag too, so this generalizes the
  mechanism rather than forking it).
  `scripts/refresh-thirdparty-digests.sh` resolves the commit via the
  GitHub commits API (`ks_commit_resolve`, mirroring the pre-existing
  `drfork_commit`) instead of the tag-only `tag_commit`, since keystone
  now has no tag to resolve. `scripts/check-thirdparty-versions.sh`'s
  keystone regex widened to hex to match. `scripts/fetch-corresponding-
  source.sh`'s GitHub archive fetch drops the `refs/tags/` path segment
  (verified to resolve identically for a tag or a commit), so the GPL
  corresponding-source step still works.
- **T3 code — DONE.** `DF_GUEST_RISCV64` (`src/dataflow_emu.c`): the same
  `df_guest` shape as `DF_GUEST_ARM32`/`DF_GUEST_ARM64`
  (`cap_riscv_to_uc`, RV64I integer ABI `{a0..a7}`, no wide-register read /
  FP-arg marshalling), guarded behind `#if CS_API_MAJOR >= 5` so a build
  against an older system Capstone degrades to the existing "no guest
  armed" refusal rather than a compile error. Also maps a small read/exec
  landing page at `DF_RET_MAGIC` for this guest only — Unicorn's RISC-V
  core fetches the instruction AT the `until` address before honoring
  `uc_emu_start`'s stop (unlike x86-64/AArch64/ARM32), the same quirk
  `src/emu.c`'s `emu_riscv_open` already works around, so leaving that
  address unmapped would fault the sentinel `ret` itself.
- **T3 code-vs-doc correction, and a real gap the doc's own scoping pass
  didn't anticipate.** The pre-existing `emu_riscv_t` guest handle
  (predates this brief — `emu_riscv_call`/`_call_fp`/`_call_traced`
  already existed in `src/emu.c`/`include/asmtest_emu.h`) determined
  naming, exactly as T1 found for `emu_arm_t`; the new ring mirrors it as
  `emu_riscv_step_capture`/`_clear`/`_count`/`_dropped`/`_at` over
  `emu_riscv_regs_t`, and `emu_riscv_run` now takes the whole
  `emu_riscv_t *` handle (mirroring `emu_arm64_run`'s/`emu_arm_run`'s
  shape) so it can wire in the ring. Separately, **the operand enumerator
  arm turned out to be a real second gap, not "add a `CS_ARCH_RISCV`
  case"**: Capstone's RISCV backend implements NEITHER `cs_regs_access`
  (`RISCVModule.c` never sets `handle->reg_access` — confirmed against the
  pinned Capstone 5.0.1 source, it returns `CS_ERR_ARCH` unconditionally
  for `CS_ARCH_RISCV`) NOR a per-operand `.access` field (`cs_riscv_op` is
  bare `type`/`reg`/`imm`/`mem`, no access bits, unlike
  `cs_x86_op`/`cs_arm64_op`/`cs_arm_op`). `src/dataflow_operands.c`'s
  `add_riscv_ops` infers read/write direction instead from the RV32I/RV64I
  base ISA's own regular instruction-format rules (closed GP store/branch
  mnemonic sets; Capstone's own operand order already mirrors the assembly
  syntax's dest-first form) — and dedupes a register read twice by one
  instruction (`add a3, a2, a2`) the way `cs_regs_access`'s SET semantics
  give every other arch here for free (caught live by the golden itself:
  3 def-use edges instead of arm64/arm32's 2 for the identical instruction
  shape, before the dedup fix).
- **T3 golden.** A `emu_riscv_regs_t@riscv64/lp64` `regstate` descriptor
  ([asmtrace-schema.md](asmtrace-schema.md), the schema's fifth concrete
  descriptor) naming `x0`..`x31`, `pc` — integer-only, no F/D-extension
  deck, mirroring arm64/arm32's own integer-only scope.
  `tools/asmtrace_record.c` gained `record_riscv` (mirroring
  `record_arm64`/`record_arm32`), producing `riscv-df-chain.asmtrace`
  (`"arch":"riscv64"`) with a baked `steps_cap=8` regstate ring, plus the
  D7 low-fidelity companion `riscv-regstate-truncated.asmtrace`
  (`steps_cap=2`). `examples/test_operands.c`'s RISCV64 stub test replaced
  with real decode assertions (add/load/store — the store case
  specifically proving operand[0] is correctly READ not WRITTEN for
  S-type, the one case a naive "operand[0] is always the destination" rule
  gets wrong). **The only other golden churn** is the same well-known R1-T1
  `code.sha256` fragility T1 already recorded
  (`abixray-make_pair-sysv`/`-win64`'s 64-byte code window shifted because
  the new RISC-V producer functions moved `.text`) — a one-line sha diff,
  not a content change. Every other existing golden (x86-64, arm64, arm32,
  everything else) is byte-identical.
- **T2's RISC-V half — DONE.** `desktop/src/author_vm.cpp`'s `ASM_RISCV64`
  row now has `can_run = true`, dispatching through the same per-guest
  value-fabric producer arm64/arm32 use. `kAuthorArchLimit` is now
  UNCONDITIONAL — every architecture in `author_arch_table()` has
  `can_run=true`, so the label is reachable only for an arch value outside
  the table altogether; its text no longer names any one architecture,
  updated everywhere quoted (`author_vm.h`'s RULE 3 docstring,
  `desktop/test/test_author_vm.cpp`'s assertions, `author_door.cpp`'s
  comments). `ui/author_door.cpp`'s dispatch gate widened from
  `s.arch == ASM_ARM64 || s.arch == ASM_ARM32` to also include
  `ASM_RISCV64`. The capability panel and `author_recording`'s `vf`
  materialisation path needed no change (both already arch-generic,
  confirming T2's own prediction for the third guest too).
- **T3 tests.** `examples/test_dataflow_emu.c` (a RISC-V `add`/`mv`/`ret`
  chain mirroring the arm64/arm32 `df_add` cases). `examples/test_emu.c`
  (`emu_riscv.step_capture_records_prestates`/
  `_drops_oldest_and_counts`/`_arming_survives_across_calls_on_same_handle`,
  mirroring `emu_arm64.*`/`emu_arm.*`). `desktop/test/test_author_vm.cpp`:
  the arch-gating table flip, the four `author_apply_run_vf` fidelity
  branches re-exercised over riscv64-shaped data, and an end-to-end
  `author_recording` -> `recording_to_asmtrace` -> `load_recording`
  round-trip for a synthetic RISC-V fabric.

**Not done:** nothing — all three tasks (T1 ARM32, T2 Author dispatch, T3
RISC-V) are landed. `kAuthorArchLimit` and `author_arch_table()` now run
every architecture Author mode assembles.

## Status (2026-08-02) — T1 + T2 landed

**The ARM32 `df_guest` instance and the Author-mode dispatch flip are both
landed and docker-verified** (`docker-cli` cli-smoke, including
`asmtrace-golden-check`; `docker-desktop` desktop-test):

- **T1 — DONE.** `src/dataflow_operands.c`'s `cs_target()` now maps
  `ASMTEST_ARCH_ARM32` -> `CS_ARCH_ARM`/`CS_MODE_ARM`, and `add_mem_ops`
  gained a `CS_ARCH_ARM` branch (`cs_arm_op`'s `arm_op_mem`: `base`/`index`/
  `disp`, mirroring the arm64 branch's shape — `cs_arm_op` carries no `size`
  field on the operand itself, so, exactly like the arm64 branch, `size` is
  passed 0). `src/dataflow_emu.c` gained `DF_GUEST_ARM32`: `UC_ARCH_ARM`/
  `UC_MODE_ARM` (A32, never Thumb), `cap_arm32_to_uc` (`ARM_REG_R0..R12`
  contiguous + `SP`/`LR`/`PC`/`CPSR`), a 4-register AAPCS32 `{r0,r1,r2,r3}`
  arg table, a GP-zero init that preserves CPSR's mode/T/IT bits (unlike
  AArch64's NZCV, ARM32's CPSR packs processor mode alongside the flags), and
  the shared `DF_CODE_BASE`/`DF_STACK_BASE` layout (already well inside the
  32-bit address space, so no ARM32-specific bases were needed). Proven end
  to end in `examples/test_dataflow_emu.c` (an ARM32 `add/mov/bx lr` chain,
  `arm32_df_add`: the value fabric captures r2 = 7 + 5 = 12 at step0, r0 = 12
  at step1, and the def-use edge 0->1).
- **T1 code-vs-doc correction.** This doc's steps named `emu_arm32_t` /
  `emu_arm32_regs_t` for the ring; the code already has a full ARM32 guest
  handle predating this brief, spelled `emu_arm_t` / `emu_arm_regs_t` (not
  `emu_arm32_*`) — `emu_arm_open`/`_call`/`_call_fp`/`_call_vec` and their
  `emu_arm_regs_t` (r0..r15, cpsr, q0..q15) already existed in `src/emu.c` /
  `include/asmtest_emu.h` before this brief touched them. Per this doc's own
  "the code wins" rule, the new ring mirrors that existing naming:
  `emu_arm_step_capture`/`_clear`/`_count`/`_dropped`/`_at`, a new
  `arm_step_ring_t` scoped to `emu_arm_t` (mirroring `arm64_step_ring_t`
  exactly), snapshotting the full `emu_arm_regs_t` BEFORE each executed
  instruction — zero changes to `emu_t`/`emu_x86_regs_t`/`emu_arm64_t` or
  their snapshot/restore paths. `emu_arm_run` (the shared run-and-capture
  helper) now takes the whole `emu_arm_t *` handle rather than a bare
  `uc_engine *`, mirroring `emu_arm64_run`'s own shape, so it can wire in the
  ring. The **descriptor id** stays `emu_arm32_regs_t@arm/aapcs32` as this
  doc specified (naming the architecture, not literally the C struct tag —
  the schema doc says so explicitly to avoid the same confusion twice). A
  `regstate` descriptor entry
  ([asmtrace-schema.md](asmtrace-schema.md)) names `r0`..`r12`, `sp`, `lr`,
  `pc`, `cpsr` — integer-only, no VFP/NEON deck, mirroring arm64's own scope.
  New coverage in `examples/test_emu.c`
  (`emu_arm.step_capture_records_prestates` / `_drops_oldest_and_counts` /
  `_arming_survives_across_calls_on_same_handle`, mirroring `emu_arm64.*`).
- **T1 golden.** `tools/asmtrace_record.c` gained `record_arm32` (mirroring
  `record_arm64`), an `ARM32_DF_CHAIN` worked example (`add r2,r0,r1` /
  `add r3,r2,r2` / `mov r0,r3` / `bx lr` — the same shape as
  `ARM64_DF_CHAIN`), producing `arm32-df-chain.asmtrace` (`"arch":"arm"`) with
  a baked `steps_cap=8` regstate ring, plus the D7 low-fidelity companion
  `arm32-regstate-truncated.asmtrace` (`steps_cap=2`, two of four steps
  evicted, `truncated`/`drops.lost` faithful) — both committed,
  `asmtrace-golden-check` clean. `examples/test_operands.c`'s ARM32 branch
  (previously asserting the stub "enumerates nothing") now asserts real
  decode, mirroring its arm64 block; RISC-V's stub assertion is untouched.
  **The only other golden churn** is the well-known R1-T1 `code.sha256`
  fragility doc 32 already recorded (`abixray-make_pair-sysv`/`-win64`'s
  64-byte code window shifted because new ARM32 functions moved `.text`) —
  a one-line sha diff, not a content change, regenerated with the corpus.
  Every other existing golden (x86-64, arm64, everything else) is
  byte-identical.
- **T2 — DONE.** `desktop/src/author_vm.cpp`'s `ASM_ARM32` row now has
  `can_run = true`, with a note describing the same value-fabric shape
  arm64's row states. `kAuthorArchLimit` and RISC-V's own row note narrowed
  from "x86-64/AArch64-only in v1" to name only RISC-V as still unsupported
  ("RISC-V is the only architecture without run/trace in v1") — updated
  everywhere it was quoted verbatim in this tree: `author_vm.h`'s RULE 3
  docstring, `desktop/test/test_author_vm.cpp`'s assertion on the row note,
  and the comments in `desktop/src/ui/author_door.cpp` that named arm64
  exclusively. `ui/author_door.cpp`'s `author_run_vf` now takes an
  `asmtest_arch_t arch` parameter instead of hardcoding
  `ASMTEST_ARCH_ARM64`/`EMU_ARCH_ARM64`, and `author_run`'s dispatch gate
  widened from `s.arch == ASM_ARM64` to `s.arch == ASM_ARM64 || s.arch ==
  ASM_ARM32` — `asm_arch_t` and `asmtest_arch_t`/`emu_arch_t` share the same
  enum ordering (`X86_64=0, ARM64=1, RISCV64=2, ARM32=3`) so `s.arch` casts
  straight through to the right `df_guest`. The capability panel
  (`desktop/src/ui/capability_panel.cpp`) already read `author_arch_table()`
  generically with no second hardcoded arch list, exactly as this doc
  predicted — it now reports ARM32 automatically, no code change needed.
  `author_recording`'s `vf` materialisation path was already arch-generic
  (`arch_wire_name` already mapped `ASM_ARM32` -> `"arm"` before this brief),
  so **no `recording_to_asmtrace` change was needed**, confirming this doc's
  own prediction.
- **T2 tests.** `desktop/test/test_author_vm.cpp`: the arch-gating table flip
  (arm32 `can_run`, RISC-V now the sole refusal, the shared label's new
  text), the four `author_apply_run_vf` fidelity branches exercised again
  over arm32-shaped data (clean run, non-return-sentinel, truncated buffer,
  producer setup failure — the same arch-agnostic fold function arm64 uses,
  since there is no separate arm32 code path to diverge from it), and an
  end-to-end `author_recording` -> `recording_to_asmtrace` -> `load_recording`
  round-trip for a synthetic ARM32 fabric (`"arch":"arm"`, trace/df_step/
  df_edge survive the round-trip, no fabricated `regstate`).

**T3 (RISC-V) — now DONE too, see the "T3 (RISC-V) spike run, landed in
full" status section above** (dated the same day this T1+T2 status was
written but appended later): the Keystone spike succeeded and T3 landed in
full. `kAuthorArchLimit` and the RISC-V row no longer refuse it.

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
