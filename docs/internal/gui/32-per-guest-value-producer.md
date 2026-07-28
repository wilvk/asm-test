# R5: the per-guest value producer (arm64 first) — implementation

> **Root R5 of the [extension roadmap](27-extension-roadmap.md).** The emulator L0
> value producer is hard-wired to the x86-64 guest; every value fabric, and
> Author-mode run/trace, is x86-64-only until a per-guest producer exists. This
> root arch-parameterizes the producer, arm64 first. Independent of R1–R4 (a
> different axis); **demand-gated** on a non-x86-64 persona.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.

## Status (2026-07-29) — T1 + T2 (value fabric + regstate/Scrubber) + T3 all landed

The **value-producer core, its per-step register ring, and the Author door's
arm64 run path are all landed and docker-verified**:

- **T1 — DONE (byte-identical).** The six inline x86-64 seams in
  [`dataflow_emu.c`](../../../src/dataflow_emu.c) are now a `df_guest` descriptor
  (open mode, Capstone→Unicorn reg map, operand-enumerator arch tag, ABI arg
  table, register init, call/return setup). The run is arch-neutral and selects a
  guest by arch; `asmtest_dataflow_emu_run` is exactly the x86-64 guest. A
  clean-baseline-vs-refactor regen diff proved the value trace is **byte-for-byte
  identical** — the ONLY golden churn is `make_pair`'s 64-byte-window `code.sha256`
  (the documented R1-T1 fragility: adding the arm64 functions shifted the binary
  `.text` the window reads past the real routine), regenerated with the corpus.
- **T2 value fabric — DONE.** A `df_guest_arm64` (`UC_ARCH_ARM64`, a
  `cap_arm64_to_uc` map folding W→X and X29/X30=FP/LR, AAPCS64 args x0–x7, the
  register init, and a link-register return). The operand enumerator already
  covered `ASMTEST_ARCH_ARM64`, so no enumerator change was needed. Proven end to
  end in `examples/test_dataflow_emu.c` (an AArch64 `add/add/mov/ret` chain: the
  value fabric captures x2=12, x3=24, x0=24 and the def-use edges 0→1, 1→2), and a
  **golden `arm64-df-chain.asmtrace`** is committed (`"arch":"aarch64"` via a new
  `asmtrace_writer_set_arch` guest-arch override, correct disasm, byte-stable,
  `asmtrace-golden-check` green). The Loom / Slice / Timeline render it with **no
  desktop change** (the reader is arch-generic over locations; `loom_reg_name`
  degrades an unmapped arm64 id to `reg#N`, a cosmetic reg-name follow-on).
- **T2 `regstate`/Scrubber ring — DONE.** The per-step register RING
  ([`emu.c`](../../../src/emu.c)) is arch-parameterized the same way the value
  producer was: `emu_arm64_t` gains its OWN drop-oldest ring
  (`emu_arm64_step_capture`/`_clear`/`_count`/`_dropped`/`_at`, mirroring the
  x86-64 `emu_step_*` shape 1:1 over `emu_arm64_regs_t` instead of a union
  grafted onto `emu_t`/`emu_x86_regs_t` — `emu_t` and `emu_arm64_t` are already
  separate per-guest handle types, so a second guest's ring is one more
  mirrored seam, exactly as `df_guest_arm64` was for the value producer). Zero
  changes to `emu_t`, `emu_x86_regs_t`, or `emu_snapshot`/`emu_restore` — the
  x86-64 ring, `src/dataflow_resume.c`'s hosted/Reweave path, and
  `desktop/src/views/regsynth.cpp`'s synthesizer stay exactly as they were
  (`cli/test_reweave.c` and `cli/test_regstate_parity.c` both still pass
  unchanged). A new `emu_arm64_regs_t@aarch64/aapcs64` `regstate` descriptor
  (`docs/internal/gui/asmtrace-schema.md`) names the raw AArch64 register file
  (`x0`..`x30`, `sp`, `pc`, `nzcv`; no vector/NEON deck, mirroring the value
  fabric's own integer-only scope); `tools/asmtrace_record.c` bakes a
  `steps_cap = 8` ring into the **same** `arm64-df-chain.asmtrace` golden (the
  arm64 analogue of `add_signed`'s worked example — no separate `--steps`-style
  flag exists yet), so its `regstate` events now show x2=12 / x3=24 / x0=24 at
  the SAME steps the value fabric already proved, plus a new D7 dishonesty
  fixture `arm64-regstate-truncated.asmtrace` (`steps_cap = 2`, the ring evicts
  2 of 4 steps, `truncated`/`drops.lost` honest). **No desktop/reader change**:
  `desktop/src/analysis/stepindex.cpp`'s `read_values` already renders any
  integer `values` key it does not specifically name via its generic
  "extra keys, sorted" fallback, so the Scrubber time-travels an arm64 capture
  today — the field ORDER is merely lexicographic rather than hand-curated
  (`nzcv`, `pc`, `sp`, `x0`, `x1`, `x10`...), a cosmetic follow-on exactly like
  `loom_reg_name`'s arm64 degradation above, not a correctness gap. New
  coverage in `examples/test_emu.c`
  (`emu_arm64.step_capture_records_prestates` /
  `_drops_oldest_and_counts` / `_arming_survives_across_calls_on_same_handle`).
  Deferred, honestly out of scope for this root: arm64 snapshot/restore (an
  x86-64 `emu_t` / Reweave concern) and a vector/NEON regstate deck (a further
  descriptor row, like R4 was for x86-64).
- **T3 Author-mode arm64 run — DONE.** The Author door's Run button dispatches
  arm64 through the per-guest value-fabric producer
  (`asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM64, …)`, re-declared in
  `author_door.cpp`'s new `author_run_vf` per the `loom/forks.cpp`
  tier-producer-has-no-public-header precedent) instead of the x86-64-only
  `emu_call_traced`/`emu_result_t` path — T3's OWN changes touch no `emu.c`
  code (that is the T2-regstate item above, landed separately so the two
  did not collide). The result is a genuinely different SHAPE,
  `author_result_t::ran_value_fabric` (step + def-use-edge counts, an honest
  `vf_note`), which deliberately never populates the x86-64 `ran`/register/
  fault fields: this producer captures no register file and no fault
  kind/address, and the door says so in every branch (a clean run, a run that
  did not reach the return sentinel, a truncated buffer, and a producer setup
  failure all get distinct, honest notes — `author_vm.h`'s
  `author_valuefabric_t` / `author_apply_run_vf`) rather than rendering
  zeros. Saving the run materialises the fabric as `trace` + `df_step` +
  `df_edge` events through the SAME writer the `codeimage` event already used
  (`author_recording`'s new optional `vf` parameter, unchanged
  `recording_to_asmtrace`) — the exact schema shape
  `tools/asmtrace_record.c`'s `record_arm64` writes for the `arm64-df-chain`
  golden — so an arm64 Author run opens in the Loom / Slice / Timeline with
  **no reader change**. `author_arch_table()`'s AArch64 row is now
  `can_run = true`; the shared refusal label (`kAuthorArchLimit`) and RISC-V's
  own note now read "x86-64/AArch64-only in v1" (not "x86-64-only"), so ARM32
  and RISC-V's limits stay accurate rather than going stale. The capability
  panel (06 T6) now states which arches Author mode runs straight from
  `author_arch_table()` — one source of truth, not a second hardcoded arch
  list that could drift from the door's own gate. Tested in
  `test_author_vm.cpp`: the arch-gating table flip, all four
  `author_apply_run_vf` honesty branches, and an end-to-end
  `author_recording` → `recording_to_asmtrace` → `load_recording` round-trip
  carrying `trace`/`df_step`/`df_edge` for a synthetic arm64 fabric (the same
  `arm64_df_chain` listing as the T2 golden). `desktop-ui-test`'s Author
  coverage is unchanged — it only pins that the door is reachable, and no
  per-arch interaction test existed for x86-64 to mirror, so none was invented
  uniquely for arm64 either; noted here rather than silently left out.

**Honest limits that remain (not scoped as tasks — see Non-goals below):**
arm64 snapshot/restore + Reweave, and a vector/NEON regstate deck, both noted
in their landing bullets above as further, separate seams — not this root.

The remainder of this brief is the original plan; T1's seam, T2's value-fabric
and regstate/Scrubber halves, and T3's Author-mode routing are all now shipped.
RISC-V is now another `df_guest` instance (and, following the same mirrored
pattern, another `emu_<arch>_t` ring instance), exactly as designed.

## Why this work exists

The plan states the limit plainly and the code enforces it: "value fabrics are
x86-64-guest-only until a per-guest valtrace producer exists" (plan Honest
limits); Author mode showed non-x86-64 code as bytes + a labelled "run/trace is
x86-64-only in v1" ([06](06-doors-and-learning.md) door;
`desktop/src/author_vm.cpp`) — **since T3 above, this is no longer true for
arm64 specifically**: the label and the gate now name only ARM32/RISC-V,
verbatim wherever this paragraph is quoted elsewhere. The Loom says the same about
the value fabric itself: "arm64-guest fabrics — no arm64-code value producer
exists; the fabric is x86-64-guest-only and says so" ([05](05-loom-day-one.md)
out-of-scope) — also superseded by T2's value fabric above. The plan's expansion
table lists an "*(opportunistic)* arm64-guest emulator L0 value producer —
`src/dataflow_emu.c` guest seam — medium, demand-gated."

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
