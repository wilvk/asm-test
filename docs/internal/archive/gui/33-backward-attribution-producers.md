# R6: backward-attribution producers — `blame` + `statediff` — implementation

> **Root R6 of the [extension roadmap](27-extension-roadmap.md).** Two reserved
> schema kinds whose GUI **sockets are already built** but which have no producer:
> `blame` (attribute a value to its source location) and `statediff` (step-to-step
> architectural state delta). Both reason *backward* over a recording. The
> `statediff` leg needs routine identity from [R1](28-schema-freeze-completion.md)
> T1 to pair two recordings.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.

## Why this work exists

Two deferred views are "Wave 2" backward analysis with the consumer plumbing
already landed:

1. **Blame cones.** `blame` is a reserved kind claimed by
   [09](09-teaching-producers.md) — "attribution of a value to a source location"
   ([asmtrace-schema.md:323](../../gui/asmtrace-schema.md#L323)). Doc 09 shipped the
   **deep-link socket** (blame is a reserved kind with no producer — Wave 2; "only
   deep-link plumbing belongs here", [09](09-teaching-producers.md) blame socket).
   The producer — a backward slice from a value to the instruction(s) that
   produced it — is the missing half.
2. **Slice-diff / two-recording state-diff.** The slice explorer "explicitly
   refuses a merged two-recording graph until [the Wave 2 state-diff producer]"
   ([04](04-replay-views.md) T-slice); `statediff` is reserved as "step-to-step
   architectural state delta" ([asmtrace-schema.md:322](../../gui/asmtrace-schema.md#L322)).

The raw material for both already exists in the L1 dataflow pass: last-writer
def-use edges. `blame` is a backward walk of those edges; `statediff` is a
per-step diff of the register file the ring already captures.

## What already exists (verified 2026-07-28)

- **Reserved kinds** — `blame` and `statediff`
  ([asmtrace-schema.md:322-323](../../gui/asmtrace-schema.md#L322)); adding either is a new
  registry-row definition under the ignore-unknown-kinds rule, never a new major.
- **The blame deep-link socket** — landed in [09](09-teaching-producers.md)
  (deep-link plumbing only; the "reserved kind with no producer" note).
- **Def-use edges (L1)** — the pure last-writer pass yields def-use edges over the
  L0 valtrace ([05](05-loom-day-one.md) T1 feeder; the Loom's lineage is built on
  exactly these), the substrate a blame slice walks.
- **The per-step register file** — the `regstate` ring
  ([asmtest_valtrace.h:117-124](../../../../include/asmtest_valtrace.h#L117)) gives
  `statediff` its per-step architectural state to delta.
- **Routine identity** — [R1](28-schema-freeze-completion.md) T1's `code` header is
  the precondition for `statediff` to assert two recordings are the same routine
  before merging their graphs (today the diff states identity is the reader's
  assertion).

## Tasks

### T1 — the `blame` producer (backward slice)  (M)

**Goal.** Emit `blame` events attributing a selected value at a step to the
producing location(s), by walking L1 def-use edges backward.

**Steps.**
1. Define `blame` fields in [asmtrace-schema.md](../../gui/asmtrace-schema.md): the queried
   `{step, loc}` and the attributed source `{off/step, kind}` chain (mirror the
   `srcmap` row shape). Fix field order (D6); coordinate the definition with the
   freeze (D5).
2. Implement the backward slice over the existing def-use edges (a pure pass — no
   engine re-run): from a value's def, follow last-writer edges to the
   instruction(s) that produced its inputs, to a bounded depth, faithfully flagging
   where provenance ends at instrumentation (a value born of untraced state has no
   ancestry — reuse the lineage "born of untraced state" fidelity,
   `desktop/src/loom/lineage.cpp`).
3. Consumer: the blame **cone** view lights the existing 09 deep-link socket — a
   click routes through 04's `dt_nav_go` router to the producing location(s).

**Done when.** A blame query on a golden fabric returns the correct backward cone;
a value with no traced ancestry returns a faithful "provenance starts at
instrumentation" result, not an empty one; `desktop-test` covers both.

### T2 — the `statediff` producer (step-to-step delta)  (M)

**Goal.** Emit `statediff` events — the architectural state delta between
consecutive steps — so a two-recording merged view has an exact per-step basis.

**Steps.**
1. Define `statediff` fields: per-step changed-register set + values, derived from
   the `regstate` ring (the Scrubber already computes change-highlighting between
   held steps — lift that pure logic into the producer so the wire carries it).
2. Emit under an opt-in alongside `regstate` (it is a function of the ring; arm it
   with the same flag or a sibling).
3. Consumer: the slice explorer's refused two-recording merge
   ([04](04-replay-views.md) T-slice) becomes buildable — **gated on
   [R1](28-schema-freeze-completion.md) T1**, so the merge refuses a
   wrong-routine pair by identity before diffing state.

**Done when.** Two `code`-matched recordings of the same routine produce a merged
state-diff graph; a `code`-mismatched pair is refused (R1 T1); a `code`-less pair
keeps the faithful "identity is the reader's assertion" caveat; `test_diff` /
`test_slice` extended.

## Unblocks / downstream

- Blame cones ([04](04-replay-views.md)/[09](09-teaching-producers.md)) — the
  socket goes live with no new plumbing.
- Slice-diff / two-recording state-diff rendering ([04](04-replay-views.md)) — the
  slice explorer's standing refusal lifts.

## Non-goals / acknowledged limits

- **Exact-only, provenance-starts-at-instrumentation.** A blame cone never invents
  ancestry for pre-existing state, and never crosses threads (no cross-thread
  value hops — plan Acknowledged limits). Both are hard refusals, surfaced as such.
- `statediff` is architectural state (the register file), not memory-content diff
  (that rides the `mem` / `wide[]` channels, [R2](29-mem-address-stream.md) /
  [R1](28-schema-freeze-completion.md)); a full two-recording memory diff is a
  later composition, not this root.
- Both are **derived** from an existing recording (a pure pass), not a new
  execution — no perturbation, no re-run.

## Cross-references

Depends on [R1](28-schema-freeze-completion.md) T1 (`code` header, for
`statediff` pairing) and the L1 def-use substrate
([05](05-loom-day-one.md) T1). Schema/D5 [01](01-asmtrace-format.md); consumers
[04](04-replay-views.md) (slice/diff), [09](09-teaching-producers.md) (blame
socket). Golden/fidelity D6/D7.
