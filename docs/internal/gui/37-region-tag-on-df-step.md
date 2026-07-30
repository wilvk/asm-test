# Tag `df_step` with its region on the wire — resolve the span instead of refusing it — implementation

> **The producer half of [36](36-anchor-the-3d-plane.md)**, and a tail follow-on to
> the [extension roadmap](27-extension-roadmap.md). 36 places a routine-relative
> offset by *deriving* the base from the recording's single `codeimage` code span,
> and **refuses** — correctly — whenever a recording carries zero or ≥2 spans,
> because `df_step` states no region
> ([asmtrace_ndjson.c:269-290](../../../cli/asmtrace_ndjson.c#L269) writes `step`,
> `off`, optional `disasm`, `ops` — and nothing else). This brief removes the
> ambiguity at its source: the producer already **knows** the base at the moment it
> writes the offset — it is a parameter of `dataflow_record`
> ([asmspy.c:2306](../../../cli/asmspy.c#L2306)), used today only for disassembly —
> and simply does not say so. Emitting it turns 36's *derivation* into a **stated
> fact**, makes the multi-span case resolvable rather than refusable, and redeems
> the promise 36 T3 leaves in the code: the churn walk becomes the sound "region
> as-of this step" resolver that 36 explicitly declined to attempt.
>
> **This does not retire 36's anchor.** `resolve_anchor` stays the permanent
> documented fallback for every recording produced before this lands, and for rel
> `trace` recordings, which this brief deliberately does not tag. Land **36 in
> full, then 37** — do not interleave.
>
> Authored 2026-07-29, verified against HEAD `81e6ade`. If a cited file:line
> disagrees with the code when you implement, the code wins — re-verify, then fix
> this doc in the same change.
>
> **Status (2026-07-29) — ☑ 5/6. LANDED.** T1 the `rbase` writer + all 4 producers +
> schema + corpus regen (`6612ef8`); T2 the desktop reader resolves the span from
> the wire (`ad32299`); T3 the churn walk region-as-of-step resolver (`06f2991`, + a
> review-surfaced fidelity fix `07fa0d1`); T5 the `anchor_source` HUD grade + 36
> reconciliation (`7f5b048`); T6 the `scene-df-two-span` end-to-end golden
> (`2516d06`). **T4 (`when`) DEFERRED as the severable extension this brief marks it**
> — a serve-path disasm-version refinement, no view breaks without it (reader-rule 2
> is sound). Corpus byte-stable under docker-cli; a live `cli_smoke` asserts
> `df_step.rbase` == the region base. This is a producer + schema change (D5).

## Why this work exists

A live `auto` session may walk several candidate regions before one runs
([asmspy.c:3733-3761](../../../cli/asmspy.c#L3733)), emitting a `codeimage` per
candidate. The resulting recording carries several code spans and a stream of
offsets that belong to different ones — with nothing on the wire saying which. 36
must refuse that recording: it draws no path and states why. That refusal is genuine
and it is also unnecessary, because the information was in the producer's hand and
was dropped on the way out.

The same silence has a second cost inside the terrain. The churn walk
([terrain.cpp:160-207](../../../desktop/src/space/terrain.cpp#L160)) associates a
step with a `codeimage` version by counting **`trace` events that carry an `off`**.
A dataflow recording has none, so the step counter never advances, every detected
churn lands at step 0, and the region reads as churned from the first slice. That
half is a plain pre-existing bug, independent of the schema, and this brief is where
it gets fixed because it is the same walk.

## The decision — what the field is

**Emit `rbase`: an optional u64 giving the absolute base address that `off` is
relative to.** A base, not a version and not an opaque id:

- **A base is the key the whole consumer stack already joins on.**
  `regions_from_codeimage` folds by base
  ([terrain.cpp:83](../../../desktop/src/space/terrain.cpp#L83)), `Projection`
  resolves by base, the deep-link picker computes `addr - r->base`, and the churn map
  is already keyed by base. A base tag joins to all of it with no new registry, no id
  allocation, and no ordering assumption.
- **A version is not a key.** `codeimage.version` is per-session and 0-based, and is
  **reset to 0 mid-recording** by the auto candidate walk
  ([asmspy.c:3738](../../../cli/asmspy.c#L3738)); `when` restarts with it, because
  the timeline is freed and re-created per span. In a three-candidate auto recording
  three different spans each emit `version:0, when:1`. `base` is the only globally
  distinguishing field `codeimage` carries today.
- **An opaque id costs a registry and degrades worse.** It needs a second definition
  to map id→span, and a reader that lost the registry line — a torn recording — has
  nothing, whereas `rbase + off` is a complete address on its own. Under
  *ignore-unknown-fields*, an opaque id is exactly the value a partial reader drops
  silently.
- **Zero plumbing at the writer.** The base is already in scope at all four
  producers (below).

**Why `rbase` and not `base`:** an operand object inside `df_step.ops[]` already has
a `base` field (the memory operand's base register). JSON-path-wise there is no
ambiguity, but two readers in this tree are **substring** scanners that cannot
disambiguate — the conformance reader's `field(line, "base")` and `cli_smoke.sh`.
`rbase` ("region base") keeps the schema's short-name style (`off`, `ea`, `rw`,
`pass`, `when`, `tid`) with no collision.

**Field order** (normative): `step`, `off`, `rbase?`, `disasm?`, `ops` — the anchor
beside the thing it anchors, matching the house practice of appending at the end of
the *logical* group rather than the physical end. `"step":N,"off":N` stays the line
prefix, so `cli_smoke.sh`'s `grep -q '"step":0,'` is unaffected and `ops` stays last.

**Omission rule (D7).** `rbase` is **omitted entirely — never `null`, never 0-as-
unknown** — when the producer does not know a base. Address 0 is never a mapped code
span, so `base == 0` means "not known" at the writer.

**Normative producer rule to record.** `df_step.off` is region-relative *by
definition*. A producer whose offsets are already absolute — the window-survey
opener at [dataflow_ptrace.c:2671-2672](../../../src/dataflow_ptrace.c#L2671), which
opens steps with an absolute `pc` and `base == 0` — is **out of contract** and must
not be wired to `df_step` without its own schema change. Nothing in the tree
violates this today; asmspy's dataflow engine uses the `attach_jit_stop` path.

## How a reader resolves it (normative)

1. **With `rbase`:** the step's absolute PC is `rbase + off`, and its span is the
   `codeimage` whose `base == rbase`. No wire-order inference, no single-span
   requirement, no ambiguity — this holds for a three-candidate auto recording and
   for any future multi-span producer.
2. **Without `rbase`:** 36's single-span anchor is the documented fallback,
   **permanently**. Exactly one code span ⇒ `base + off`; zero or ≥2 ⇒ refuse with
   the stated reason.
3. A reader **never** guesses `rbase` from wire order or from the nearest preceding
   `codeimage`. Seq order is "steps then image" — the refresh path emits `codeimage`
   *after* the invocation it belongs to — the opposite of what a naive as-of rule
   assumes.
4. **`rbase` present but matching no `codeimage` span:** the placement is sound (the
   producer stated the base) but there are no bytes. Place the vertex; report "no
   code image for this span". Placement and disassembly are different questions and
   degrade separately.
5. **Precedence:** a per-event `rbase` always wins over the recording-wide anchor. A
   recording mixing tagged and untagged events resolves each by its own rule and
   states that it did both.

## What already exists (verified 2026-07-29)

- **One field-order owner.** [`asmtrace_df_step_body`](../../../cli/asmtrace_ndjson.c#L269)
  is the sole C writer, declared with its normative doc comment at
  [asmtrace_ndjson.h:144-154](../../../cli/asmtrace_ndjson.h#L144).
- **Four producers, three of them through that owner.** Live ptrace
  ([asmspy.c:2333](../../../cli/asmspy.c#L2333), base already the `dataflow_record`
  parameter at [:2306](../../../cli/asmspy.c#L2306), forwarded unchanged by both
  sinks); the two corpus recorders
  ([asmtrace_record.c:540](../../../tools/asmtrace_record.c#L540) and
  [:867](../../../tools/asmtrace_record.c#L867), base is the compile-time
  `REC_CODE_BASE 0x00100000UL` at [:68](../../../tools/asmtrace_record.c#L68)); and
  — **bypassing the C builder entirely** — the desktop Author-mode VM, which
  hand-builds the JSON in C++ at
  [author_vm.cpp:310-330](../../../desktop/src/author_vm.cpp#L310). Miss the fourth
  and Author-mode saves diverge from the CLI recorders.
- **The compatibility rule, already written.** *"Ignore unknown fields. Additive
  fields on a known kind are not a break"*
  ([asmtrace-schema.md:558](asmtrace-schema.md)), with the worked precedent in
  [28](28-schema-freeze-completion.md) T3. **No reader in this tree rejects unknown
  fields on an event body** — the desktop reader stores the body verbatim and
  re-dumps it on round trip, the stream decoder is find-or-skip, and the exporter has
  no else-error branch. The tolerance is already pinned by a test against an
  unknown-kind fixture.
- **The freeze status.** [asmtrace-schema.md](asmtrace-schema.md) still reads
  *"Status: draft, not frozen"* — the v1 freeze is a named Phase-3 checkpoint that
  has not been passed, so this lands under the append-only rule (D5).
- **The corpus blast radius, counted.** 20 generated flat goldens carry `df_step`
  and will each gain `,"rbase":1048576`; 3 hand-authored goldens
  (`low-fidelity/continuous-df`, `views/trunc-dataflow`, `views/no-disasm`) and 1
  desktop fixture (`blame-attribution`) carry it and are **deliberately left
  untagged** — they become the standing regression coverage for reader rule 2.

## Tasks

### T1 — the `rbase` tag: writer, all four producers, schema, corpus regen  (M)

**Steps.**
1. Add `uint64_t rbase` to `asmtrace_df_step_body` after `off`; emit
   `,"rbase":%llu` **only when nonzero**, immediately after `off`. Extend the doc
   comment with the shape, the omit-when-unknown rule, and the "`off` is
   region-relative by definition" contract.
2. Update the three C production callers and the four positional test callers.
3. Update the Author-mode C++ writer
   ([author_vm.cpp:310-330](../../../desktop/src/author_vm.cpp#L310)) from its `base`
   parameter.
4. **Add the exact-body test `df_step` has never had.** Its existing unit coverage is
   substring-only (`strstr`), so field order for this kind has **no unit gate** — the
   golden corpus is the only one. Add an exact-body case plus an explicit order
   assertion, mirroring the `mem` and `df_invocation` body tests, including a
   `rbase == 0` case asserting the body is **byte-identical to today's**.
5. Schema: extend the `df_step` definition (field + normative order), update the
   parsed `## Example` block, append the signed-off section per the append-only
   template, and close the freeze-checklist item this raises — *"`df_step` states no
   region"* — with the `~~strikethrough~~ CLOSED` form already used there.
6. Regenerate: `make docker-cli` → `make asmtrace-golden` → commit the 20 flat files
   **with** the writer change (the determinism rule requires it in the same change).
   Regenerate **only** inside `docker-cli` — a host's Capstone 4.x would churn the
   whole corpus.

**Done when.** `make asmtrace-golden-check` byte-clean inside `docker-cli`;
`make cli-test` green including the new exact-body and field-order checks; every
generated `df_step` carries `"rbase":1048576`; a `rbase == 0` call produces today's
exact bytes; `cli_smoke.sh` gains an assertion that a live `--dataflow` recording's
`df_step.rbase` equals the region base the session reports, and its existing
`'"step":0,'` check still passes unmodified.

### T2 — the desktop reader: resolve the span from the wire  (M, depends on: T1, 36 T1+T2)

**Goal.** A tagged `df_step` is placed from a *stated* fact; an untagged one still
goes through 36's derivation; the two are distinguishable at every surface.

**Steps.**
1. `desktop/src/doc/streams.h`: `DataflowStream` gains `insn_rbase` (parallel to
   `insn_off`) and an `rbase_present` flag.
2. `desktop/src/doc/streams.cpp`: decode `rbase` beside `off` in the same indexed
   write. **Known aliasing, pre-existing, do not fix here:** the decoder indexes by
   `step`, while a continuous capture restarts `step` at 0 per pass
   ([35](35-continuous-live-dataflow.md) T1), so passes already alias. Record it —
   with `rbase` the aliasing can now produce a *wrong base*, not merely a wrong
   offset, if two passes ever carry different bases.
3. `desktop/src/space/trajectory.cpp`: when a `df_step` carries `rbase`, set
   `p.addr = rbase + off` and `p.placed = true` at build time and mark the set
   anchored; otherwise leave the offset for 36 T2's anchoring pass. Per-event
   precedence over the recording-wide anchor.
4. `desktop/src/space/trajectory.h`: add `anchor_source` with values `"wire"`,
   `"single-span"`, `"mixed"`, `""`. `TRAJ_ANCHORED` and `TRAJ_RELATIVE_BASIS` both
   stay set — the wire basis is still rel. **This is a real fidelity grade, not
   cosmetics:** a wire-stated placement and a derived one are different claims and
   must not share a label.
5. `desktop/src/live/ptslice.cpp` builds a `DataflowStream` **directly from an
   in-memory value trace, bypassing JSON** — populate `insn_rbase` there too, or the
   live pane silently disagrees with replay of the same capture.

**Tests.** `desktop/test/test_trajectory.cpp`, inline NDJSON. **The headline:** two
`codeimage` spans plus `df_step`s tagged with each ⇒ every vertex placed, both spans,
**no refusal** — precisely the recording 36 T1 must refuse. Then: `rbase` absent with
two spans ⇒ still refused with 36's reason (the fallback is intact); `rbase` matching
no `codeimage` ⇒ placed, no bytes, stated; a mixed recording ⇒ `anchor_source ==
"mixed"` and the note names both mechanisms; `rbase + off` equals the abs path's
address for the same instruction.

**Done when.** `make desktop-test` green; the two-span fixture that 36 refuses now
places every vertex; reverting the wire read makes it refuse again; reverting the
`anchor_source` distinction fails a named check.

### T3 — terrain: the churn walk becomes a sound "region as-of this step" resolver  (M, depends on: T2, 36 T3)

The sentence 36 T3 leaves in the code as a promise, redeemed.

**Steps.**
1. Add `df_step` events to the seq-merge walk
   ([terrain.cpp:160-207](../../../desktop/src/space/terrain.cpp#L160)) and count a
   `df_step` carrying `off` as a step. Today the counter advances **only** on `trace`
   events with an `off`, so for any dataflow recording every detected churn lands at
   step 0 and the region reads as churned from the first slice. This half is
   independent of the schema and is the pre-existing bug.
2. Key the churn join on the step's **own** `rbase` instead of the geometric round
   trip (`unproject` → region → base), which is the only link today and fails for any
   offset projecting into no region. The base-keyed churn map is unchanged — it
   already takes a base.
3. Record the limit that remains: `version` resets to 0 mid-recording on a candidate
   walk, so a re-armed span's baseline never registers as churn under the
   greater-than rule. Benign while candidates have distinct bases; state it rather
   than paper over it.

**Tests.** `desktop/test/test_terrain.cpp`: two tagged spans ⇒ two distinct, correct
churn steps; a dataflow-only recording's churn no longer pins at step 0; an untagged
recording keeps today's behaviour with a stated reason.

**Done when.** The fixture whose churn lands at 0 today lands at its true step;
reverting the `rbase` key fails a named check.

### T4 — `when`: which bytes were live at this step  (M, depends on: T1) — **severable**

`rbase` answers *which span*; it does not answer *which version's bytes*, and the
schema's resolution rule is emphatic that guessing the latest version is silently
wrong. Nothing in the tree derives a `when` from a step today — the observer's
`disasm_when` is a **manual input box**.

**Steps.** Add an optional `when` after `rbase`; plumb it from the serve sink, which
holds the session, sampled **once before the pass** and stamped on every `df_step` of
that invocation — the bytes in force when the invocation started. This is the sound
rule precisely because the refresh runs *after* the invocation. Emitted **only** by
the serve path: the headless sink and both corpus recorders have no codeimage
timeline, so they omit it — **T4 churns no golden.** Reader: the disasm resolver
already handles `(addr, when)` with a `(when, version)` tiebreak; wire the per-step
`when` in as the default for the manual slider. Normative caveat: `when` restarts
across a candidate walk, so a reader keys `(rbase, when)` and **never `when` alone**.

**Done when.** A serve dataflow recording disassembles each step against the bytes in
force at that step; a step whose bytes are genuinely unknown says so instead of
showing the newest version; no golden churns.

### T5 — the fidelity chrome and the doc-36 reconciliation  (S, depends on: T2, T3, 36 T4)

[`hud.cpp`](../../../desktop/src/scene3d/hud.cpp) gains the grade distinction 36 T4
could not make: `anchor_source == "wire"` ⇒ *"rel: span stated on the wire (rbase)"*;
`"single-span"` ⇒ 36's existing *"rel: anchored to the codeimage span (derived
placement)"*; `"mixed"` ⇒ both, named. Amend [36](36-anchor-the-3d-plane.md): T1's
`resolve_anchor` is retitled **the fallback**, and its non-goals point here — the
refusal branch now fires only for (a) recordings produced before this brief and
(b) rel `trace` recordings. Reconcile `CHANGELOG.md`, this README's follow-on row, and
[10](10-spacetime-3d-overview.md).

**Done when.** Deleting any one chip branch fails a named check; every doc asserting
the multi-span refusal is reconciled in the same change.

### T6 — the end-to-end bar: a two-span golden  (M, depends on: T1, T2, T3)

The multi-span shape is this brief's entire justification and **no committed artifact
carries it.** Add a `record_scene_df_multi()` beside `record_scene_abs`
([asmtrace_record.c:989](../../../tools/asmtrace_record.c#L989)), reusing its
`codeimage` emission twice at two bases with `df_step`s tagged to each, producing
`tests/golden-asmtrace/scene-df-two-span.asmtrace`. Regenerate only in `docker-cli`;
commit only the new file. `desktop/test/test_shell.cpp` opens it and asserts both
spans place, `pc_placed == pc_points > 0`, and `anchor_source == "wire"`. The recorder
controls its own map, so this is a real test — **not** a lane that can only self-skip.

**Done when.** `make asmtrace-golden-check` passes in `docker-cli` with the new file;
`make desktop-test` and `make docker-desktop` green; the two-span golden renders a
placed path where 36 alone renders a labelled-empty plane.

## Task order & parallelism

`T1` → `T2` → (`T3` ∥ `T4` ∥ `T5`) → `T6`. **T4 is severable** — T1–T3 + T6 are
complete and useful without it.

## Constraints & gates

- **Regenerate only in `docker-cli`.** `make asmtrace-golden` is gated on x86-64 +
  Unicorn, and the image's pinned Capstone 5.0.1 is what makes the disasm text
  byte-stable; a host's apt Capstone 4.x would churn the entire corpus. CI's check
  **fails on a self-skip**, so this is a correctness gate, not a convenience.
- **Golden regen lands in the same commit as the writer change** (the schema's
  determinism rule).
- **Sequence against 36.** 36 T4 commits a new generated golden; landing 37 after it
  regenerates that file once more. Land 36 in full, then 37 — do not interleave.
- **`desktop/test/expected/*.txt` must be unchanged** (no view renders `rbase`).
  Verify by running `make desktop-test` **without** `UPDATE_GOLDEN`; any diff there is
  a T2 bug, not a regen.
- **D4/D9 unchanged.** The desktop stays engine-free; the capture host is still
  `asmspy`.

## Non-goals / acknowledged limits

- **Not a tag on `trace`.** A `basis:"rel"` `trace` recording has the identical
  ambiguity, but `trace` already carries `basis` (so a reader knows it is rel), its
  events are an order of magnitude more numerous, and tagging it churns a far larger
  share of the corpus. Same shape, its own brief. **Consequence: 36's
  `resolve_anchor` refusal branch survives for rel `trace` and must not be deleted.**
- **Not `len` on `df_step`.** Recoverable from the `codeimage` span, and per-step
  emission would go inconsistent across versions with different clamps.
- **Not a `basis` field on `df_step`.** `off` stays region-relative by definition; an
  absolute-offset producer is out of contract.
- **Not an envelope-major bump and not a new kind.** Additive optional field only.
- **Not a fix for the step-index aliasing** across continuous passes (T2 step 2). It
  is recorded, and `rbase` makes its consequence sharper, but the fix belongs with
  the segmentation work in [35](35-continuous-live-dataflow.md).
- **Does not give the Auto/Dataflow pane convergence marks.** `df_step` still carries
  no `tid`, so such a capture stays a single trajectory — see
  [36](36-anchor-the-3d-plane.md) T5's acknowledged limit. Tagging `tid` is neither
  proposed nor blocked here.

## Cross-references

The producer half of [36](36-anchor-the-3d-plane.md) (which stays the permanent
fallback). Schema/D5 [01](01-asmtrace-format.md) + [asmtrace-schema.md](asmtrace-schema.md),
under the append-only rule established by [28](28-schema-freeze-completion.md) T3.
Consumers: [10](10-spacetime-3d-overview.md) (terrain/trajectory),
[04](04-replay-views.md) (dataflow views), [08](08-observer-views.md) (the
`codeimage` kind and the disasm-at-`when` resolver). The live capture path is
[07](07-serve-live-host.md); the continuous session whose step indices alias is
[35](35-continuous-live-dataflow.md). Fidelity chrome D7 /
[23](23-graded-truth-layer.md); wording D7 / [24](24-one-visual-language.md).
