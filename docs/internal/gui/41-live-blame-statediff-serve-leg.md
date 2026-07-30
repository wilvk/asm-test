# Emit `blame` + `statediff` from the live serve leg — the recorder-only kinds go live — implementation

> **A per-gap brief cut from [38](38-live-feed-completion-roadmap.md)** (gap **L3**).
> [R6/doc 33](33-backward-attribution-producers.md) landed the `blame` (backward
> def-use attribution) and `statediff` (step-to-step register delta) producers
> **recorder-only** — `tools/asmtrace_record.c`'s `emit_blame` / `emit_regstates`
> over the emulator value trace. The live `asmspy --serve` / `--dataflow` leg
> (`src/dataflow_ptrace.c` fills the trace, `cli/asmspy.c`'s `dataflow_record`
> serializes it) emits `df_step` / `df_edge` / `regstate` / `mem` / `fpenv` but
> **not** those two kinds, so a live attach cannot produce a reproducible,
> deep-linkable `blame` / `statediff` artifact.
>
> **This is genuinely low-value and candid about it** — [38](38-live-feed-completion-roadmap.md)
> corrected the audit here: the backward-cone *view* already works live (it derives
> client-side over the live `df_edge` graph, the `b`/`f` keys), and the statediff
> *diff* is `dt_statediff_build` over two recordings' `regstate`, of which a live
> recording is already a valid leg. Only the **precomputed kinds** — a convenience
> that makes the artifact reproducible/deep-linkable without the client recomputing —
> are recorder-only. This brief closes that convenience gap; it does not unlock any
> view.
>
> **Why it is nonetheless cheap and clean.** Both are pure projections over data the
> serve leg *already has*: `blame` is `asmtest_slice_backward_seed` over the def-use
> graph `g` the sink already receives (and already uses for the interactive `b`/`f`
> slice, [asmspy.c:6328](../../../cli/asmspy.c#L6328)); `statediff` is a delta of the
> `regstate` ring the leg already captures ([26 T1](26-live-regstate-producer.md)).
> The wire body builders (`asmtrace_blame_body` / `asmtrace_statediff_body`,
> [asmtrace_ndjson.c:431](../../../cli/asmtrace_ndjson.c#L431)) are **shared** with the
> recorder, so a live `blame` and a golden one spell identically. Both are
> **serialization-only** flags: they ride the sink ctx like `emit_mem`
> ([asmspy.c:2295](../../../cli/asmspy.c#L2295)), never the capture engine
> (`asmspy_engine_dataflow`'s signature is untouched — the engine single-steps the
> same either way). No engine change, no schema change (both kinds are already
> defined, R6), so no envelope bump.
>
> Authored 2026-07-29 against HEAD `2a624c9`. If a cited file:line disagrees with the
> code when you implement, the code wins — re-verify, then fix this doc in the same
> change.
>
> **Status (2026-07-29) — ✅ 3/3.** T1 `blame` + T2 `statediff` from the live serve
> leg (both ride `dataflow_ctx` like `emit_mem`, `dataflow_emit_blame` ports the
> recorder's cone, `--statediff` self-arms the ring); T3 this reconcile. Proven live
> against `attach_victim hotfn`: one `blame` (penultimate seed, ascending cone incl.
> sink, `born_untraced:false`) + one `statediff` per step (step 0 `computed:false`,
> later `computed:true`). `cli_smoke` asserts both; both `cli-smoke` lanes green.

## Why this work exists

`dataflow_record` ([asmspy.c:2304](../../../cli/asmspy.c#L2304)) is the live serve
leg's per-invocation serializer — the exact live parallel of
`tools/asmtrace_record.c`'s recorder loop. The recorder, when `--blame` /
`--statediff` are armed, emits one `blame` (seeded at the penultimate step, over the
def-use graph) and one `statediff` per held step (a delta of the register ring). The
serve leg omits both. Bringing them over is the whole task:

- **`blame`** — after the `df_edge` loop ([asmspy.c:2375](../../../cli/asmspy.c#L2375)),
  when armed and `g != NULL` and `nsteps >= 1`, seed at `nsteps >= 2 ? nsteps-2 :
  nsteps-1` and emit the backward cone. This is `emit_blame`
  ([asmtrace_record.c:148](../../../tools/asmtrace_record.c#L148)) with `rec_emit(r,
  "blame", …)` instead of `asmtrace_emit`; the cone build, the `loc` = first register
  write of the seed, and the `born_untraced = cone <= 1` verdict are identical.
- **`statediff`** — inside the `regstate` emission ([asmspy.c:2345](../../../cli/asmspy.c#L2345)),
  when armed, emit one `statediff` right after each `regstate`, pairing `prev = s == 0
  ? NULL : &vt->regfile[s-1]` with `cur = &vt->regfile[s]`. The live ring is
  **tail-drop** (`first_step = 0`, [live-vs-replay note]), so step 0 is genuinely the
  first held step (`computed:false`, never a "born at step 0" lie) and every later
  step has its true predecessor — simpler than the recorder's drop-oldest eviction
  bookkeeping.

## Tasks

### T1 — `blame` from the live serve leg

**Steps.**
1. Add `emit_blame` to `dataflow_ctx` ([asmspy.c:2295](../../../cli/asmspy.c#L2295))
   and a parameter to `dataflow_record`; thread it from both call sites (the recording
   sink [2417](../../../cli/asmspy.c#L2417) via `dc->emit_blame`, the serve path
   [3359](../../../cli/asmspy.c#L3359) via `s->p.blame`).
2. Add a static `dataflow_emit_blame(rec_t *r, vt, g, seed)` in `asmspy.c` — a
   faithful port of `emit_blame` (fail-closed on OOM: no blame beats a truncated one),
   emitting via `rec_emit(r, "blame", body)`. Call it after the `df_edge` loop when
   `emit_blame && g && nsteps >= 1`, seeded at the penultimate step.
3. Serve grammar: parse `"blame":true|false` into `p->blame`
   ([asmspy.c:4084](../../../cli/asmspy.c#L4084) is the `mem` template) and echo it in
   the started-params announce ([3651](../../../cli/asmspy.c#L3651)/[3674](../../../cli/asmspy.c#L3674))
   — fidelity on the wire, not just the pane.
4. CLI: `--blame` for `--dataflow --record` / the serve invocation (mirror `--mem`,
   [asmspy.c:7605](../../../cli/asmspy.c#L7605)); set the ctx flag.

**Definition of done.** `cli_smoke` records a live `--dataflow hotfn` capture with
`--blame` (and `--record`) and asserts the recording carries a `blame` event with a
non-empty ascending `cone` and a `born_untraced` field; a value with no traced
producer would read `born_untraced:true` (faithful), never an empty cone. No new golden
(a live capture's addresses vary); the shared body builder is the format contract.
Both `cli-smoke` lanes green.

### T2 — `statediff` from the live serve leg

**Steps.**
1. Add `emit_statediff` to `dataflow_ctx` and `dataflow_record`; thread it from both
   call sites. **`statediff` requires the register ring**, so `--statediff` /
   `"statediff":true` arms `steps` too (the same coupling `fpregs` uses,
   [asmspy.c:4092](../../../cli/asmspy.c#L4092)).
2. In the `regstate` block ([asmspy.c:2345](../../../cli/asmspy.c#L2345)), when
   `emit_statediff`, emit one `statediff` right after each `regstate`:
   `asmtrace_statediff_body(body, cap, s, prev, &vt->regfile[s])` with `prev = s == 0 ?
   NULL : &vt->regfile[s-1]`. `prev == NULL` ⇒ `changed:{},computed:false` (the body
   builder already does this) — the first held step is not a full-delta lie.
3. Serve grammar `"statediff":true|false` + started-params announce; CLI `--statediff`
   (arms `steps`).

**Definition of done.** `cli_smoke` records `--dataflow hotfn --steps --statediff` and
asserts a `statediff` per step, step 0 carrying `computed:false` and a later step
carrying `computed:true` with a non-empty `changed`. `statediff:true` with no `steps`
still produces the stream (it self-arms the ring). Both `cli-smoke` lanes green.

### T3 — reconcile the roadmap and changelog

Flip [38](38-live-feed-completion-roadmap.md)'s L3 row to **CLOSED (doc 41)** with the
faithful one-liner (the *views* already worked live; this lands the precomputed
convenience kinds), add the doc-41 row to [README.md](README.md), and a CHANGELOG line.
Update this brief to ✅ 3/3.

## What this brief is NOT

- **Not a new view or unlock.** The blame cone and the state-diff both already render
  live client-side ([38](38-live-feed-completion-roadmap.md)'s correction); this is the
  reproducible/deep-linkable *artifact* only.
- **Not an engine or schema change.** `blame`/`statediff` are defined (R6); the capture
  engine is untouched (serialization-only, in the sink ctx).
- **Not exact live↔recorder byte-parity.** The live leg captures a real process, the
  recorder emulates a corpus routine — different inputs. The shared body builders
  guarantee *format* identity; the cli_smoke asserts *shape*, not a golden.
- **Acknowledged limits carry over from R6** — exact-only, provenance-starts-at-instrumentation
  (`born_untraced`), no cross-thread hops. A live dataflow capture is single-threaded by
  scope, so this is automatic.

## Claim

| Task | Who · when | Status |
|---|---|---|
| T1 `blame` serve leg | will · 2026-07-29 | ✅ landed |
| T2 `statediff` serve leg | will · 2026-07-29 | ✅ landed |
| T3 roadmap/changelog | will · 2026-07-29 | ✅ landed |
