# R3: resume-from-state seam + Reweave — implementation

> **Root R3 of the [extension roadmap](27-extension-roadmap.md)** — the headline.
> This is the engine prerequisite behind the "not a day-one feature" refusal the
> Scrubber shows, behind the Loom's forks-re-run-from-entry limit, and behind the
> plan's "mid-run state editing (no resume-from-state API)" and "Reweave
> (Phase 4+, demand-gated)" items — **one blocker surfacing in four places.**
>
> Largest root, lowest urgency (the honesty wins R1/R2 come first). But cheaper
> than "large" suggests: the keystone already exists on `emu_t` and the value
> producer merely bypasses it.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.
>
> **ALL FOUR TASKS LANDED 2026-07-28** (● 4/4). T1 + T2: the value producer is
> re-hosted on `emu_t` (`asmtest_dataflow_emu_run_hosted`, `src/dataflow_resume.c`)
> sharing the exact seed/run engine with the standalone (`asmtest_dataflow_internal.h`),
> byte-identical by construction — proved by `cli/test_reweave.c`. The
> checkpoint/resume pair (`_checkpoint` snapshots at a chosen step; `_resume`
> restores + re-runs) reproduces the tail byte-for-byte and a one-register edit at K
> (`_edit_gp`, plus `_edit_mem` for the memory half added in T3) diverges only
> downstream. Only `dataflow_resume.o` references `emu.o`, so every other consumer
> of `dataflow_emu.o` stays emu-free.
>
> **T3** (Reweave): the Loom fork model gains a fork-from-step-K take
> (`loom_take_run_from_step`, `desktop/src/loom/forks.cpp`): checkpoint at K, apply
> one register/memory edit, resume, and weave the result as a STITCHED full
> worldline (`[0,K)` = the unchanged parent prefix, `[K,end)` = the edited tail) so
> it drops straight into the existing take-view divergence card — "patient zero at
> at/after K" on a control-flow edit, values-only divergence on straight-line code.
> It carries the mandatory crossing-the-line disclosure (`kLoomForkDisclosure` /
> the pure `kLoomReweaveBanner`) — a reweave is emulator replay, never silicon (D7).
> `test_loom_forks` extended (straight-line + control-flow + determinism + loud
> refusals); the pure `draw_loom_reweave_form` gesture is driven in `desktop-ui-test`
> (`loom/reweave_form_emits_request`). **Landed `cc4ad3b`.**
>
> **T4** (retire the Scrubber refusal): `dt_scrubber_replayable` (pure) decides —
> when a producer-absent recording is emulator-replayable (Author/emulator, x86-64,
> `codeimage` bytes) the Scrubber OFFERS "synthesize register history"; where not
> (no code bytes / non-x86-64 / a live capture) it keeps the honest refusal naming
> WHY. The synthesiser (`desktop/src/views/regsynth.cpp`, full-build-only, links
> `emu.o`) re-runs the recorded code under the per-step ring and builds the SAME
> `StepIndex` a `--steps` capture would, flagged `synthesized` so the deck carries a
> RE-DERIVATION banner, never presented as original capture (D6). `test_scrubber`
> asserts BOTH branches; `test_regsynth` proves the re-run; the "not a day-one
> feature" disclaimer is gone. **Landed `8f38b81`.**

## Why this work exists

The Scrubber, asked to show a register file for a recording that has none,
refuses to synthesize one — and says so
([scrubber.cpp:45-49](../../../desktop/src/views/scrubber.cpp#L45)): "Re-running
with a larger max_insns to synthesise one is not a day-one feature." The Loom's
fork mechanic re-runs **from entry**, never from a chosen step
([05](05-loom-day-one.md) T5). Both refusals trace to one missing capability:
**there is no way to seed the value producer from a saved mid-run state and
re-run forward.** The plan records it four times — "Reweave / mid-run edits (no
resume-from-state API)" ([05:515](05-loom-day-one.md)), "`emu_snapshot` cannot
reach the dataflow guest" (plan growth-rung), "mid-run state editing as day-one
work (no resume-from-state API exists)" (plan Killed-in-grounding), and "*(Phase
4+, demand-gated)* Reweave: edit-at-step-K + deterministic re-run + per-step
alignment" (plan expansion table).

The keystone is **already built** — just not wired to the value producer:

- `emu_t` has a full checkpoint/restore pair: `emu_snapshot` captures "the full
  register context plus the extents, permissions, and contents of every mapped
  region"; `emu_restore` puts it back exactly
  ([asmtest_emu.h:616-636](../../../include/asmtest_emu.h#L616)).
- `emu_t` also has the opt-in per-step register ring, and the two are **designed
  to coexist**: "emu_snapshot / emu_restore deliberately do NOT clear it (Track F
  arming-survives-restore discipline)"
  ([asmtest_emu.h:592-610](../../../include/asmtest_emu.h#L592)).
- But the value producer `asmtest_dataflow_emu_run` opens its **own** `uc_engine`
  ([dataflow_emu.c:257](../../../src/dataflow_emu.c#L257)), zeroes GP registers
  deterministically (`df_zero_gp`, `:270`), seeds SysV args (`:272-283`) and runs
  one `uc_emu_start` entry→sentinel (`:299-300`). It never constructs an `emu_t`,
  so snapshot/restore and the ring cannot reach it. Its fork today just
  re-invokes the whole run from scratch.

So R3 is chiefly a **re-hosting job**: give the value producer access to a
checkpoint at step K, and a resume entry that starts from a checkpoint instead of
`df_zero_gp` + entry.

## What already exists (verified 2026-07-28)

- **Checkpoint/restore on `emu_t`** — `emu_snapshot_t`, `emu_snapshot`,
  `emu_restore`, `emu_snapshot_free`
  ([asmtest_emu.h:623-636](../../../include/asmtest_emu.h#L623); impl
  `src/emu.c:875` snapshot / `:913` restore per the engine survey).
- **Per-step ring on `emu_t`**, survives restore
  ([asmtest_emu.h:601-610](../../../include/asmtest_emu.h#L601)).
- **The value producer's self-contained engine** and its deterministic seed
  ([dataflow_emu.c:249-309](../../../src/dataflow_emu.c#L249)); the pre/post
  code-hook value-capture timing (`df_on_code`, `df_finalize`) that a mid-run
  resume must reconstruct (banner "SELF-CONTAINED Unicorn client",
  [dataflow_emu.c:7](../../../src/dataflow_emu.c#L7)).
- **The Loom fork model** — `loom_take_run` applies one edit and re-runs
  ([05](05-loom-day-one.md) T5); the snapshot/restore bracket is today applied
  only to the `emu_call_traced` fault-card leg, **not** the value/fabric leg
  (05 T6).
- **The Scrubber's refusal + its test** —
  [scrubber.cpp:40-51](../../../desktop/src/views/scrubber.cpp#L40);
  `test_scrubber` pins the "not a day-one feature" wording
  ([test_scrubber.cpp:157-158](../../../desktop/test/test_scrubber.cpp#L157)).

## Tasks

### T1 — route the value producer through `emu_t`  (L)

**Goal.** Give the L0 value producer an `emu_t`-backed execution path so it
inherits snapshot/restore and the per-step ring, without changing its output for
the existing entry→sentinel run.

**Steps.**
1. Add an `emu_t`-backed variant of `asmtest_dataflow_emu_run` (or lift its guts
   onto an `emu_t` the caller owns): map code/stack via the `emu_t` region API,
   seed with the same `df_zero_gp` + SysV-arg convention, and attach the existing
   `df_on_code` / `df_on_mem` hooks to that engine's Unicorn handle.
2. **Prove byte-identity first.** Before any resume feature, a golden test must
   show the `emu_t`-hosted run produces the **same** valtrace as the standalone
   one for every corpus routine — this re-hosting is a refactor with zero
   observable change until T2/T3 use it. (The `emu_t` path must reproduce the
   deferred-write finalize timing exactly.)

**Done when.** `make asmtrace-golden` is byte-identical with the value producer
re-hosted on `emu_t`; `docker-desktop` + `docker-cli` green.

### T2 — checkpoint-at-step-K + resume  (M, depends on T1)

**Goal.** A `snapshot at step K → resume forward` API on the re-hosted producer.

**Steps.**
1. During a run, take an `emu_snapshot` at a requested step K (the per-step ring
   already fires a hook per step — snapshot inside it). Return the checkpoint plus
   the ring index so K is addressable.
2. Add a resume entry that `emu_restore`s a checkpoint and runs forward under a
   fresh valtrace, re-establishing the value-capture hook state (the deferred
   `df_finalize` bookkeeping) from the checkpoint rather than from `df_zero_gp`.
3. Per-step alignment: because both the original and the resumed run share the
   ring's absolute step numbering (`dropped_steps + i`,
   [asmtest_emu.h:605-610](../../../include/asmtest_emu.h#L605)), a resumed
   trajectory aligns to the original by absolute step — the plan's "per-step
   alignment" requirement, sound and exact (not shared-prefix guessing).

**Done when.** A resume from step K reproduces the tail of the original run
byte-for-byte (K→end identical); a resume after a register edit at K diverges
only where the edit reaches — pinned by a new `test_reweave`.

### T3 — Reweave: edit-at-step-K + deterministic re-run  (M, depends on T2)

**Goal.** The user-facing counterfactual: edit a register (or a memory cell) at
step K and re-run forward, exact and labelled.

**Steps.**
1. Loom fork model gains a **fork-from-step-K** take (extends `loom_take_run`,
   [05](05-loom-day-one.md) T5): apply one edit to the T2 checkpoint's register
   file / memory, resume, weave the resumed fabric as a new take.
2. Alignment UX reuses the existing take-view divergence card ("patient zero",
   `desktop/src/views/diff_view.h`) — a fork-from-K diverges from its parent at
   the edit, shown on the shared spacetime fabric.
3. **The honesty label is mandatory (D7).** A reweave is emulator-replay, an
   explicit crossing of the native→virtual line — carry the existing forks
   banner ("forks re-run the emulator replay", 05 T6) and never present a
   counterfactual as observed silicon.

**Done when.** A one-register edit at step K produces an aligned counterfactual
take under the crossing-the-line banner; `test_loom_forks` extended;
`desktop-ui-test` drives the fork-from-K gesture.

### T4 — retire the Scrubber's "not a day-one feature" refusal  (S, depends on T2)

**Goal.** With T2 in hand, a recording that lacks `regstate` **can** offer to
synthesize one by re-running under the ring — the exact fallback the message
disclaims today.

**Steps.**
1. When `!idx.present()` **and** the recording is emulator-replayable (Author
   mode, x86-64 guest, code bytes available via [R1](28-schema-freeze-completion.md)
   T1's `code` header), the Scrubber offers a "synthesize register history"
   action that runs T2's ring path and populates the `StepIndex` — labelled as a
   re-derivation, not original capture.
2. Where the recording is **not** replayable (a live non-Author capture, a
   non-x86-64 guest), keep the current honest refusal and its deep link.
3. Update the pinned message + `test_scrubber` to reflect the new capability while
   preserving the honest refusal on the non-replayable path.

**Done when.** An emulator recording with no `regstate` gains a synthesized,
clearly-labelled register history on demand; a non-replayable one still refuses
honestly; `test_scrubber` updated to assert **both** branches.

## Unblocks / downstream

Retires the "not a day-one feature" refusal (T4); lands the plan's Reweave
expansion item (T3); makes the Loom's forks fork-from-anywhere (T3); and provides
the general checkpoint/resume the growth-rung mid-run-edit views need.

## Non-goals / honest limits

- **Emulator-only, replay-only, forever.** A reweave never touches a live process
  and is never evidence about silicon timing (plan Honest limits). This root does
  not add live-process resume — that stays killed.
- **x86-64 guest only** until [R5](32-per-guest-value-producer.md) lands a
  per-guest value producer; a resumed run inherits the producer's guest scope.
- A synthesized register history (T4) is a **re-derivation from the code + seed**,
  sound and exact but explicitly not the bytes the original run observed — the
  label says so.

## Cross-references

Engine [asmtest_emu.h](../../../include/asmtest_emu.h) /
[src/emu.c](../../../src/emu.c) (snapshot/restore/ring),
[dataflow_emu.c](../../../src/dataflow_emu.c) (value producer); consumers
[05](05-loom-day-one.md) (forks), Scrubber
([scrubber.cpp](../../../desktop/src/views/scrubber.cpp)). Depends on
[R1](28-schema-freeze-completion.md) T1 (`code` header) for T4's replayability
check. Honesty D7.
