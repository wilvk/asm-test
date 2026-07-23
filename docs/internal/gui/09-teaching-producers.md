# Teaching producers: per-step register ring, scrubber, ABI x-ray — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md):
> Phasing **Phase 4** (per-step register-capture ring → "the real scrubber and
> the Loom's complete now-column"; ABI x-ray; blame intake), the engine-work-
> items row "Per-step register-capture ring (opt-in)", and the view-catalog
> rows *Register time-travel scrubber* and *ABI x-ray*. Written 2026-07-24.
> If this doc and a source disagree, this doc wins (sources may be stale); if
> the CODE and this doc disagree, re-verify before implementing.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; shared
> decisions D1–D11 live in this directory's README. Siblings:
> [01-asmtrace-format.md](01-asmtrace-format.md) (`regstate` kind + state
> descriptors; `tools/asmtrace_record.c`),
> [04-replay-views.md](04-replay-views.md) (deep-link router),
> [05-loom-day-one.md](05-loom-day-one.md) (the fabric's now-column),
> [06-doors-and-learning.md](06-doors-and-learning.md) (walkthrough player the
> x-ray builds on). Symbols from siblings are marked **(new — 0N)**;
> everything else cited exists at HEAD `a460d40`.

## Why this work exists

The plan's constraint 3 (verified again at HEAD) still holds: **no per-step
register producer exists** — `emu_result_t.regs` is the file *after* the run,
and the scrubber/ABI x-ray were correctly scheduled *behind* the producer, not
as UI work. This doc funds the producer (a small, opt-in emulator ring), turns
it into `regstate` events, and only then builds the two teaching views on top.
The classroom payoff is the plan's: scrub any corpus routine step by step with
the full register file, and watch a call's arguments marshal — SysV vs Win64 —
on the emulator, deterministically, on any host.

## What already exists (verified 2026-07-24)

- **The hook seam** — `src/emu.c` installs exactly one `UC_HOOK_CODE` (ordered
  trace) and one `UC_HOOK_BLOCK` (coverage) for tracing
  ([src/emu.c:105–136](../../../src/emu.c#L105)); Track F added further hooks
  (memory watch [:139](../../../src/emu.c#L139), block-entry register guard
  [:166](../../../src/emu.c#L166)) — the ring is one more hook of the same
  shape, x86-64 guest first exactly as Track F scoped its guards.
- **The end-of-run register file** — `emu_x86_regs_t` (GP + rip + rflags +
  XMM; no MXCSR — the Wave 1 gap the plan records) in
  [include/asmtest_emu.h:62](../../../include/asmtest_emu.h#L62); read-back
  already implemented for `emu_result_t`.
- **Call surfaces for both ABIs** — `emu_call_traced`
  ([include/asmtest_emu.h:178](../../../include/asmtest_emu.h#L178)) and
  `emu_call_win64` / `emu_call_win64_traced`
  ([:200–202](../../../include/asmtest_emu.h#L200)) — the SysV-vs-Win64
  contrast the x-ray needs is emulator-backed today.
- **Snapshot/restore** — `emu_snapshot`/`emu_restore`
  ([:595–606](../../../include/asmtest_emu.h#L595)) for deterministic
  repeat runs.
- **Test style to mirror** — Track F's host-independent emulator tests, e.g.
  `TEST(emu, watchpoint_only_flags_escaping_write)`
  ([examples/test_emu.c:523](../../../examples/test_emu.c#L523)),
  snapshot tests ([:778–808](../../../examples/test_emu.c#L778)).
- **The `regstate` event kind + descriptor references** are defined by 01
  (`{"desc":"<id>","values":{...}}` — never a bare register list); the
  recorder `tools/asmtrace_record.c` is 01's (**new — 01**).
- **The walkthrough kind** is 01's `note` (ordered stops); the player is
  06-T4 (**new — 06**).
- **Blame** is a *reserved* kind (`blame`) with no producer — Wave 2; only
  deep-link plumbing belongs here.

## Tasks

### T1 — Per-step register ring in the emulator  (M, depends on: none)

**Goal.** An opt-in, bounded, drop-accounted per-step register capture on the
x86-64 guest — the producer everything else here consumes.

**Steps.**
1. Declare in [include/asmtest_emu.h](../../../include/asmtest_emu.h) (new
   section after the Track F guards):

```c
/* Opt-in per-step register capture (x86-64 guest). Arms a UC_HOOK_CODE that
 * snapshots the full emu_x86_regs_t BEFORE each executed instruction into a
 * caller-sized ring. Persists across emu_call_* until cleared (Track F
 * arming discipline). Memory cost = cap * sizeof(emu_x86_regs_t). When more
 * than `cap` steps execute, the EARLIEST entries are dropped and
 * dropped_steps counts them — truncation is data, never silent. */
bool emu_step_capture(emu_t *e, size_t cap);        /* arm (realloc ok)     */
void emu_step_capture_clear(emu_t *e);              /* disarm + free        */
size_t emu_step_count(const emu_t *e);              /* entries held         */
uint64_t emu_step_dropped(const emu_t *e);          /* steps evicted        */
/* Entry i (0 = oldest held): the register file BEFORE step_index's insn.   */
bool emu_step_at(const emu_t *e, size_t i,
                 uint64_t *step_index, emu_x86_regs_t *out);
```

2. Implement in [src/emu.c](../../../src/emu.c): a `step_ring` context on
   `struct emu` beside the Track F `watch`/`reg` contexts; one additional
   `UC_HOOK_CODE` added in the x86-64 run path next to the guard hook
   ([:166](../../../src/emu.c#L166) neighborhood); per step, `uc_reg_read`
   the same register set the existing result read-back uses. Non-x86-64
   guests: `emu_step_capture` returns false (documented self-skip, mirroring
   Track F's guest scoping).
3. Ring discipline: fixed `cap`, head/tail indices, `dropped_steps` counter —
   the shared-trace-sink truncation culture applied to registers.
4. Tests in [examples/test_emu.c](../../../examples/test_emu.c)
   (host-independent, Track F style):
   `TEST(emu, step_capture_records_prestates)` — run a 4-insn routine with
   cap 8; assert count == steps, entry 0's rip == entry offset, a register
   written by insn 2 differs between entries 2 and 3;
   `TEST(emu, step_capture_drops_oldest_and_counts)` — cap 2 over 4 insns;
   assert count == 2, dropped == 2, survivors are the last two.
5. `make emu-test` + `make docker-emu DOCKER_PLATFORM=linux/arm64` (guest is
   x86-64 under Unicorn — host-independent).

**Docs.** Emulator guide section + CHANGELOG `Added`.

**Done when.** both tests pass on x86-64 and arm64 lanes; disarm frees; a
second `emu_step_capture` re-arms with a new cap.

### T2 — `regstate` events from the ring  (S, depends on: T1; 01)

**Goal.** Recordings gain per-step register states: `tools/asmtrace_record.c`
(01's tool) grows `--steps=<cap>`, emitting one `regstate` event per held
entry with the descriptor reference (`"emu_x86_regs_t@x86_64/sysv"`) and a
drop-count note in the `end` footer when the ring evicted.

**Steps.** Arm via T1 before the run; after the run, walk `emu_step_at` and
emit; extend one golden corpus routine's recording (regen via
`make asmtrace-golden`, 01's target) plus a **cap-2 dishonesty fixture** whose
eviction must render as truncation (D7).

**Done when.** the regenerated golden is byte-stable across two runs; 03's
loader accepts `regstate` (it already must — 01 defined the kind); the
dishonesty fixture carries the drop count.

### T3 — Register time-travel scrubber  (M, depends on: T2; 03, 04)

**Goal.** `desktop/src/views/scrubber.cpp` (**new**): a playhead over a
recording's steps showing the **full register file at that step**, O(1) per
seek when `regstate` events are present; per-step change highlighting.

**Steps.** Index `regstate` by step; playhead slider + `[`/`]` step keys
(04's binding table); diff-highlight registers that changed vs the previous
held step; when the ring dropped steps, the timeline renders the torn-edge
region (no data ≠ zero data); when a recording has **no** `regstate` events
the view states the producer is absent and deep-links the docs — the
re-run-with-increasing-`max_insns` fallback is documented as NOT day-one
(plan's honest-limits stance). Golden-render tests: corpus fixture scrubs;
dishonesty fixture shows the tear.

**Done when.** `desktop-test` covers seek, diff-highlight, tear, and the
no-producer message; the Loom's now-column (05) can read the same index
(export the tiny lookup as `desktop/src/analysis/stepindex.h`, shared).

### T4 — ABI x-ray  (M, depends on: T2, T3; 06)

**Goal.** The classroom flagship: an animated, walkthrough-driven view of one
call's argument marshalling — SysV vs Win64 side by side — corpus-seeded and
deterministic.

**Steps.**
1. Record the same corpus routine twice via 01's recorder: once through
   `emu_call_traced` (SysV) and once through `emu_call_win64_traced`
   ([include/asmtest_emu.h:200–202](../../../include/asmtest_emu.h#L200)),
   both with `--steps`, into paired goldens.
2. Author the x-ray walkthrough as `note` stops (06's format): each stop
   anchors a step and names the marshalling fact ("a0 → rdi (SysV) vs rcx
   (Win64)", stack-slot spill points, callee-saved contrast); eightbyte
   classification callouts are authored stops on a struct-passing corpus
   routine.
3. The view: `desktop/src/views/abixray.cpp` (**new**) — two scrubber panes
   locked to one playhead, the walkthrough rail driving the animation
   (06's player), register-delta highlights doing the "animation" (no bespoke
   graphics beyond the scrubber's).
4. Golden-render test over the paired fixtures + stop navigation.

**Done when.** the paired recording opens as a locked two-pane x-ray; every
stop lands on its step; the walkthrough narrates SysV-vs-Win64 differences
that the register deltas visibly show.

### T5 — Blame intake socket  (S, depends on: 04)

**Goal.** Zero UI work left when Wave 2's backward-slice blame producer lands:
the deep-link plumbing exists and is tested against a hand-authored fixture.

**Steps.** Extend 04's router with the `blame` target (failure → recording →
step → pre-selected backward cone / Loom thread); commit one hand-authored
fixture recording carrying a `blame`-kind attachment (fields per 01's reserved
row — mark the fixture as schema-unfrozen); a `desktop-test` case asserts the
link opens the slice explorer with the cone pre-selected. No producer work.

**Done when.** the fixture-driven deep link works end to end; the test is
marked to be re-pointed at the real producer when Wave 2 lands.

## Task order & parallelism

T1 → T2 → {T3, then T4}; T5 is independent after 04. T1 is pure engine work
and can start immediately, in parallel with any sibling doc. Critical path:
T1 → T2 → T3 → T4.

## Constraints & gates

- x86-64 guest first (Track F's scoping); other guests self-skip by returning
  false — a documented gap, not a silent one. MXCSR is **not** in the captured
  file (the Wave 1 FP-env item is a different, future producer — do not
  smuggle it in here; the descriptor mechanism absorbs it later).
- The ring is opt-in and bounded — never armed by default; `emu_snapshot`
  /`emu_restore` do **not** clear it (Track F's arming-survives-restore
  discipline; state this in the header comment and test it).
- Recording size: `regstate` events are the largest kind; the recorder's
  `--steps` default stays 0 (off), goldens use small caps.
- All views render from recordings only — no live producer path here.

## Out of scope

- The Wave 2 blame **producer** (slice-to-assert wiring in the library);
  FP-environment capture (Wave 1); per-guest rings (arm64/riscv32/arm32
  guests); the Loom's Reweave rung (mid-run edits — its own plan-table row);
  any native (non-emulator) per-step register capture.
