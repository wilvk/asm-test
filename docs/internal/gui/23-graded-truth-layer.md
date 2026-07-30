# Wave 3: restructure the truth layer — graded fidelity tiers, session-end placard, split paused, progress everywhere — implementation

> **LANDED 2026-07-27 (T1–T4, all four tasks).** T1: the ONE graded fidelity
> vocabulary (`ui/fidelity.h` grader + `draw_fidelity_banner`/`draw_fidelity_chip` +
> a Codicon glyph per tier) over a **derivable `severity` schema field** landed
> under doc 01's append-only rule with **01-owner sign-off recorded in
> `asmtrace-schema.md`** (the Phase-3-freeze checkpoint, D5) — the field is
> derivable, additive, and gates no truth off (D7); `test_fidelity` pins the three
> tiers against the committed low-fidelity fixtures. T2: the persistent,
> cause-distinguished session-end placard (`live/end_state.h` `end_cause`) with the
> verbatim protocol-mismatch fix; toasts supplement, never replace. T3: "paused"
> split into "PAUSED (you) — Resume" vs "BLOCKED — jack held by …" with explicit
> Swap/Queue/Cancel and no auto-swap. T4: `progress.h` generalised to a `LongOp`
> (elapsed + Cancel + faithful mode) with a degrade-to-coarse 3D scrub. Tests:
> `test_fidelity`, `test_progress`, `test_terrain` (coarse_slice), `test_budget`
> (queue + BLOCKED label), `test_inspect` (patch-bay split), `test_live_session`
> (end_cause). All green on the host `make desktop desktop-render desktop-test`.

> **Sources.** Actioned from the UX restructure plan
> (../plans/desktop-gui-ux-restructure-plan.md) rows **T4.1, T4.3, T4.4, T4.6**
> and the review findings **F5, F20, F23, F21**
> (../plans/desktop-gui-ux-review.md). Written 2026-07-27 against HEAD `243f092`.
> This doc wins over the review/plan on disagreement; the CODE wins over this doc
> — re-verify file:line before editing. Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites:
> [01-asmtrace-format.md](01-asmtrace-format.md)** (schema-owner sign-off for the
> T1 `severity` field — the Phase-3 freeze checkpoint, D5),
> **[24-one-visual-language.md](24-one-visual-language.md) T5.1** (the semantic
> palette the T1 tier colours name), **[16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md) T1**
> (ImGuiNotify toasts — the supplement layer T2 leans on), and
> **[14-quick-wins.md](14-quick-wins.md) T3** (`progress.h`, generalised by T4).

## Why this work exists

- **F5 — fidelity chrome is ungraded and proliferating.** A single recording
  stacks a redaction placard, a statistical chip, a coarse-provenance chip, a
  bounded-window note, an `identity_note` row *and* sometimes a torn banner — all
  equally loud and non-collapsible — even though the schema grades them very
  differently. The one signal that means "do not trust the tail of this data"
  drowns among four that mean "this is normal": fidelity degrades into banner
  blindness (T1/F5).
- **F20 — the live session-end state is collapsed and silent.** When streaming
  stops the user cannot tell *why*: clean detach, a torn drop, an EOF, a
  mid-capture crash, or the overwhelmingly common cause — a stale `build/asmspy`
  that prints a usage banner and exits `0`. All fold into one "ended" and the
  differentiating diagnosis is offloaded to an external README essay (T2/F20).
- **F23 — "paused" is overloaded.** The bare word names two states with disjoint
  recoveries: an operator pause (Resume) and a budget preemption (another view
  holds the ptrace jack — swap/queue/stop the blocker). Neither recovery is
  offered at the state, and the swap-vs-queue fork's consequences are unspecified
  (T3/F23).
- **F21 — no uniform busy signal.** A PT decode or terrain rebuild that exceeds a
  frame budget freezes the UI thread with no "working" indicator; an expert
  cannot tell the tool from a hang and may kill it. `progress.h` shipped for file
  loads + live sessions but is not generalised (T4/F21).

**Standing constraint for the whole brief: this restructures the fidelity layer;
it removes no truth (D7).** Every field the schema already carries
(`truncated`, `torn`/no-`end`, `redacted`, `trust`, `skip`, `basis`,
`identity_note`, `paused_dropped`, `drops`) still renders. F5's fix is to *grade*
loudness, not to hide any tier — the graded system must keep passing against the
committed deliberate low-fidelity fixtures, and the T3 (integrity) tier stays
non-collapsible exactly as `draw_banner`'s refusal path is today.

## What already exists (verified 2026-07-27)

- **The one banner primitive.** `draw_banner(const char *text, bool refusal)` is
  defined in [`views/canvas_draw.cpp`](../../../desktop/src/views/canvas_draw.cpp):14-21
  (declared [`views/views_draw.h`](../../../desktop/src/views/views_draw.h):27):
  it pushes `dt_refuse_col()` for `refusal`, else `dt_warn_col()`, draws one
  `TextWrapped`, and a `Separator`. It is **already the shared component** — ten
  call sites route through it (`abixray_draw.cpp:81,87,117`,
  `scrubber_draw.cpp:21,27`, `diff_view_draw.cpp:12`, `timeline_draw.cpp:9`,
  `completeness.cpp:73`, `canvas_draw.cpp:26`, `slice_view_draw.cpp`). But it has
  **exactly two loudness levels** (`refusal` bool → red or amber), no chip form,
  no glyph, no placement contract — that boolean is the ungraded axis F5 names.
- **The colours are consolidated but binary.** [`ui/theme.h`](../../../desktop/src/ui/theme.h)
  holds `dt_warn_col()` (amber `0.95,0.75,0.25`) and `dt_refuse_col()` (red
  `0.95,0.45,0.40`) plus their `_u32` forms — the substrate T1's three tiers
  extend, via doc 24 T5.1's named semantic accessors. Today every warn/truncation/
  statistical/skip/redacted placard shares the *same* amber (theme.h:14-18) — the
  proliferation is that one amber wears five meanings.
- **The fidelity schema fields.** [`asmtrace-schema.md`](asmtrace-schema.md): the
  header carries `trust` (`exact|statistical|weak|strong`, :78) and `redacted`
  (:81); `coverage`/`end` carry `truncated` (:128,:300); a missing `end` **is
  torn** (:393); `basis` is mandatory and never defaulted (:115,:390); the `end`
  footer's `skip` and `state:"skip"` are a **successful session** with nothing to
  report, *"never an error, and a client that renders it as one is wrong"*
  (:544-546). **No `severity` field exists** — the schema grades severity in
  *data* (F5's own observation) but nothing names a tier for the *rendered*
  chrome.
- **The committed low-fidelity fixtures (D6/D7).**
  [`tests/golden-asmtrace/low-fidelity/`](../../../tests/golden-asmtrace/low-fidelity/)
  holds `torn.asmtrace` (no `end`), `truncated.asmtrace`, `redacted.asmtrace`,
  `dropped.asmtrace` (statistical, `lost:12345`, `throttled:true`); each opens
  with a `note` stating the deliberate breach. These are what T1's grading must
  stay testable against. `test_completeness_view.cpp` already asserts a truncated
  capture renders "loud" and verbatim (:134-135).
- **The live session state machine.** [`live/session.h`](../../../desktop/src/live/session.h):
  `LiveState { Idle, Running, Ended, Failed }` (:33-38) — **`Ended` is the single
  collapsed bucket** F20 names. The status struct carries the raw distinguishers
  already: `host_exited`/`host_status` (:68-69), `last_stop_reason` (:60),
  `skip_code`/`skip_reason` (:61-62), `paused_dropped` (:63); `malformed_lines()`
  (:133) counts the unparseable/usage-banner lines; `mark_eof()` (:132) marks the
  open recording torn. The *data to distinguish the causes is present*; nothing
  fans `Ended` into them.
- **The end-state is rendered flat.** [`ui/inspect_door.cpp`](../../../desktop/src/ui/inspect_door.cpp):
  `draw_status` maps state to a bare word (`"ended"`, :112), then shows malformed
  lines as one amber line (:132-134) and torn recordings as one red line
  (:166-169) — *co-located, but undifferentiated and offering no fix*. The
  `no capture yet` empty (:236) is the other half of the "stale binary" symptom.
- **The termination essay is external.** [`desktop/README.md`](../../../desktop/README.md):705-738
  is the authoritative diagnosis — the state table (:710-715), the "overwhelmingly
  common cause" (stale `build/asmspy` prints its usage banner, exits `0`,
  :723-728), and the exact one-line fix (`make cli`; Disconnect+reconnect,
  :730-738). T2 pulls this into the pane; the README stays the long-form.
- **The patch-bay and the swap it already gates.** [`live/budget.h`](../../../desktop/src/live/budget.h):
  `budget_can_start` returns a `BudgetDecision{allowed, blocker, reason}`
  (:57-74); `budget_blocked_label` produces *"paused — another live view holds
  the tracer"* naming the holder (:76-78). `draw_patch_bay`
  ([`inspect_door.cpp`](../../../desktop/src/ui/inspect_door.cpp):173-223) renders
  that blocked label in amber (:186), a **`Pause`/`Resume` pair** (:206-210) for
  the operator pause, and a **swap confirm** — `inspect_request_start` arms
  `swap_pending`/`swap_blocker` when blocked (:59-67), `inspect_confirm_swap`
  performs the stop-then-start (:76-84), never silently (`doors.h:105,121-127`).
  **The word "paused" is doing double duty**: the operator `Pause` button and the
  budget `budget_blocked_label` both say "paused" — F23 exactly. There is **no
  Queue path yet.**
- **The schema paused/preempt facts.** [`asmtrace-schema.md`](asmtrace-schema.md):
  `pause` suspends emission not tracing; paused events are counted and reported,
  never dropped (:476); a paused gap marks the `end` `truncated` and carries
  `paused_dropped` (:638-640). The budget rule — one ptrace jack per target tree
  — is the second `start`'s refusal (:517).
- **`progress.h` — the landed helper.** [`ui/progress.h`](../../../desktop/src/ui/progress.h)
  is a pure, header-only, ImGui-free decision: `progress_mode(active, has_total,
  total) → {Hidden, Determinate, Indeterminate}` (:23-29) and
  `progress_fraction(done, total)` (:33-38). A determinate bar renders **only**
  when a genuine total exists (an `end` footer / a bounded budget); an unbounded
  op faithfully gets the indeterminate bar — no fabricated percentage. **One caller
  today**: the growing-recording bar in `inspect_door.cpp:156-161`. It carries no
  elapsed-time, no Cancel, no spinner, and every other long op ignores it.
- **The long-op sites with no busy signal.** The PT decode/replay slice
  ([`live/ptslice.*`](../../../desktop/src/live/ptslice.h), doc 08), the
  codeimage load (`doc/recording.cpp`, consumed by `views/region.cpp`,
  `space/terrain.cpp`, `views/observer_draw.cpp`), and the 3D terrain re-slice /
  trajectory rebuild — [`scene3d/hud.h`](../../../desktop/src/scene3d/hud.h):27-28
  sets `playhead_moved` and the caller re-slices `TerrainModel::slice(t)`
  ([`space/terrain.h`](../../../desktop/src/space/terrain.h):9-11) + rebuilds the
  trajectory **synchronously on the UI thread** on every scrub. `terrain.h`
  already documents a **coarse rung** (a flat plane labelled coarse when the rich
  `mem[]` stream is absent, :18-23) — the degrade target T4 reuses.
- **The test lanes.** `make desktop-test` (null backend, D1, `mk/desktop.mk`) is
  the model-level lane every task here targets; `make desktop-ui-test` (doc 17
  T1, `desktop/test/test_ui.cpp` on imgui_test_engine) is available for any
  genuine interaction step. Existing fidelity tests to extend:
  `test_completeness_view.cpp`, `test_null_render.cpp`, `test_loom_chrome.cpp`,
  `test_inspect.cpp`, `test_live_session.cpp`, `test_budget.cpp`.

## Tasks

### T1 — one graded fidelity-chrome vocabulary + a schema `severity` field  (L, depends on: 01 Phase-3 freeze, 24 T5.1)

**Goal.** Collapse the proliferating fidelity forms into a small **fixed
vocabulary** — one banner form, one inline chip, one glyph set — with **mandated
placement**, built as shared components, and grade every fidelity signal into
**three tiers** driven by a new `severity` field carried *beside* the existing
fidelity schema fields (so grading stays testable against the low-fidelity
fixtures). Restructure, never remove: every existing field still renders; F5's
fix is loudness, not suppression (D7).

**The three tiers** (loudness graded to the schema's own severity gradient):

| Tier | Signals | Chrome | Colour | Collapsible |
|---|---|---|---|---|
| **T1 neutral** | `skip`=success · coarse-provenance rung · bounded window · redacted-by-policy · statistical (`trust:"statistical"`) | quiet **chip** on the stream header | no warn colour (neutral/`dt_maybe` per 24 T5.1) | n/a (already minimal) |
| **T2 caution** | truncated-but-usable (`truncated:true` with usable prefix; `paused_dropped`) | **banner**, collapsible to a chip **after first read** | amber (`dt_warn`) | yes (collapses to chip; never disappears) |
| **T3 integrity** | `torn` (no `end`) · mixed-basis / basis-mismatch refusal · dropped-prefix→UNKNOWN (`dropped`) | **banner**, loud | red (`dt_refuse`) | **no** — exactly as the refusal path is today |

**Steps.**
1. **Schema `severity` field (coordinate with doc 01's Phase-3 freeze — D5).**
   This is a **schema change**: add a `severity` enum
   (`"neutral"|"caution"|"integrity"`) to `asmtrace-schema.md`, defined *beside*
   the existing fidelity fields, **derivable** from them so no producer is forced
   to emit it and old recordings still grade (a reader computes the tier from
   `torn`/`truncated`/`trust`/`redacted`/`skip`/`basis` when the field is
   absent). Because it touches the frozen `.asmtrace` schema, land it under 01's
   append-only rule with **01 owner sign-off recorded in the schema doc** (as the
   `codeimage`/`stitch` additions were, schema:657) — do not merge the render
   half until that sign-off exists. State plainly in the schema note: the field
   *grades* fidelity, it does not gate any truth off.
2. **A pure severity mapper.** Write `fidelity_severity()` in a new header-only
   `ui/fidelity.h` (ImGui-free, like `progress.h`): given the fidelity facts of a
   recording/stream (or the parsed `severity` when present), return the tier
   enum. This is the part that must be unit-testable without a context (D4) and
   the part the fixtures pin.
3. **The fixed chrome components.** In `views_draw.h`/`canvas_draw.cpp` add,
   beside `draw_banner`, a `draw_fidelity_chip(text, tier)` (the quiet inline chip
   for T1) and generalise the banner to take a **tier** rather than the `refusal`
   bool — keep the current signature as a thin shim (`refusal ? integrity :
   caution`) so the ten call sites migrate incrementally. Add **one glyph set**
   (a Codicon per tier — reuse doc 13 F3's Codicons; neutral/caution/integrity)
   so the tier reads without colour alone (feeds 24 T5.2's second-channel rule).
   Colours come from doc 24 T5.1's named accessors — do **not** add new literals
   to `theme.h` here.
4. **Mandated placement.** Banner → pane-top (the full-pane placard contract
   `draw_canvas` already follows, canvas_draw.cpp:24-28). Chip → on the stream
   header row. Encode the contract in the component API (a `draw_banner` that a
   pane calls at top; a `draw_fidelity_chip` a header calls inline) so a caller
   cannot place a T3 integrity banner as a chip.
5. **T2 collapse-after-read.** The caution banner collapses to its chip after
   first read — hold the collapsed/expanded bit in the *view-model* (not ImGui id
   state) so it is model-testable; the collapsed form is the same text as a chip,
   never gone. **T3 never collapses** (assert this).
6. **Migrate the call sites.** Route the ten `draw_banner` sites and the loose
   `TextColored(kMaybe, …)` fidelity rows (`inspect_door.cpp:124,128,133`) through
   the graded components so no view hand-rolls a placard. A `skip` becomes a T1
   chip (schema:544 — a successful session), not an amber banner.

**Tests.** `desktop/test/test_fidelity.cpp` (new): load each
`tests/golden-asmtrace/low-fidelity/*.asmtrace` fixture and assert `fidelity_severity()`
grades it into the right tier — `torn`→integrity, `truncated`→caution,
`redacted`→neutral, `dropped`(statistical)→neutral chip **plus** its `lost`/
`throttled` drop record still surfaced (D7: the drop is not hidden by being
neutral); assert a `skip`=success grades **neutral**, never caution/integrity
(schema:544). Assert the model exposes: T3 integrity is non-collapsible, T2
caution collapses to a chip whose text equals the banner's, T1 is a chip. Extend
`test_completeness_view.cpp`/`test_null_render.cpp` to assert the migrated sites
still render every field loud-enough for its tier (the truncated-is-loud
assertion at :134-135 must still pass). Model state, not pixels (D4/D7). Null
backend, `make desktop-test`.

**Docs.** CHANGELOG `Changed`: fidelity chrome is now a graded 3-tier system
(neutral chip / caution banner / integrity banner) over a derivable `severity`
field; `Added`: schema `severity`. `asmtrace-schema.md`: the `severity` field +
the 01-owner sign-off line. `desktop/README.md`: the tier table and the
placement contract. Cross-note doc 24 T5.1 (tier colours) and doc 16 (toasts map
to tiers).

**Done when.** the `severity` field is in the schema with 01 owner sign-off and
is derivable for old recordings; `fidelity_severity()` grades all four low-fidelity
fixtures into the right tier and a `skip` grades neutral; the fixed vocabulary
(one banner, one chip, one glyph set) is the only fidelity chrome and its
placement is enforced by the API; T3 is non-collapsible and no truth was removed
(every field still renders); `make desktop-test` green.

### T2 — persistent in-pane end-of-session placard, cause-distinguished  (M, depends on: 16 T1)

**Goal.** Replace the single collapsed `Ended` state with a **persistent in-pane
placard co-located with the last events**, distinguishing the four termination
causes and rendering the **one-line fix** in the pane. Toasts (doc 16 T1)
supplement, never replace it (F20; schema/D7 — the partial data's trust differs
per cause and the user must be told which).

**The four causes** (all derivable from `LiveStatus` today — session.h):

| Cause | Derived from | Placard says | Trust of on-screen data |
|---|---|---|---|
| **stopped-clean** | `last_stop_reason ∈ {stop,quit,max,exit}`, no torn recording | "Session ended cleanly." | complete for what it captured |
| **torn: host exited/crashed** | `host_exited` with a non-clean `host_status` + an open recording torn by `mark_eof()` | "Host crashed mid-capture — this recording is TORN (integrity)." | tail untrusted (T1 integrity tier) |
| **torn: EOF** | EOF with an open recording, clean host status | "Stream ended (EOF) mid-capture — recording is torn." | tail untrusted |
| **PROTOCOL-MISMATCH** | `Ended` + `malformed_lines() > 0` + never a header (no recordings, no session) | "The host is not a serve-capable `asmspy` — it printed a usage banner and exited. **Fix: rebuild `build/asmspy` (`make cli`); Disconnect + reconnect.**" | none — nothing was captured |

**Steps.**
1. **Fan `Ended` into a cause.** Add a pure `end_cause()` (a free function over
   `const LiveStatus &` + `malformed_lines()` + the torn-recording facts, in
   `live/session.h` or a new `live/end_state.h`) returning an
   `EndCause { StoppedClean, TornHostGone, TornEof, ProtocolMismatch }`. Pure and
   header-testable (no subprocess) — the state machine `feed_line`/`mark_eof`
   already exposes everything it needs (session.h:127-133). Do **not** collapse
   `Ended`; classify it.
2. **The placard.** In `draw_live_views` / `draw_status`
   (`inspect_door.cpp:106-170,229-`) render a persistent placard **at the pane,
   co-located with the last events** (not a transient line) whenever state is
   `Ended`: the cause sentence, the trust statement, and — for
   `ProtocolMismatch` — the verbatim one-line fix pulled from the README essay
   (README:730-738). Grade it through T1's components: `ProtocolMismatch` and the
   torn causes are **integrity (T3)**; `StoppedClean` is **neutral (T1)**. It
   persists until the next Connect/Start (survives frames), unlike a toast.
3. **Toasts supplement.** Emit a doc 16 T1 toast on the transition into `Ended`
   naming the cause, but the toast is the ephemeral echo — the placard is the
   durable record. Never let the toast be the *only* surface (16 T1's own rule:
   toasts supplement, never replace).
4. **Reuse the remedy voice.** The `ProtocolMismatch` fix text is the README's,
   verbatim — do not paraphrase; it is a machine-checkable instruction
   (`make cli` rebuilds the serve loop). This mirrors T4.2's remedy-map ethos
   (verbatim reason as floor, remedy on top) without depending on that task.

**Tests.** Extend `test_live_session.cpp`: drive `feed_line`/`mark_eof` through
each termination shape via `fixtures/fake_serve.sh`-style canned inputs (the
harness that already tests refusal/skip/torn/never-ran, README:697-703) and
assert `end_cause()` returns the right `EndCause` for each — a usage-banner run
(malformed>0, no header) → `ProtocolMismatch`; a mid-stream drop → `TornEof`; a
non-zero host status → `TornHostGone`; a `quit` → `StoppedClean`. Add a draw-model
assertion (`test_inspect.cpp` or `test_null_render.cpp`) that each cause yields a
distinct persistent placard string **and** that the `ProtocolMismatch` placard
contains the `make cli` / Disconnect+reconnect fix. Model state, not pixels
(D4/D7). Null backend, `make desktop-test`.

**Docs.** CHANGELOG `Changed`: the live session-end state is now a persistent,
cause-distinguished in-pane placard with an inline fix; toasts supplement it.
`desktop/README.md`: point the troubleshooting table (:710-715) at the in-app
placard (the essay stays as the long-form reference).

**Done when.** `end_cause()` classifies all four causes from `LiveStatus` and is
unit-tested per cause; each renders a distinct **persistent** placard co-located
with the last events; the `ProtocolMismatch` placard carries the verbatim one-line
fix; toasts fire but never replace the placard; `make desktop-test` green.

### T3 — split the overloaded "paused": operator pause vs budget block  (M, depends on: —)

**Goal.** Separate the two states the bare word "paused" names, each with its own
recovery. Operator pause → **"PAUSED (you) — Resume"**. Budget preemption →
**"BLOCKED — jack held by \<session\> on \<target\>"** with explicit **[Swap]**
(confirm, naming what detaches), **[Queue]** (a visible cancellable chip + a
one-line note of what Queue does when the jack frees), **[Cancel]**. **Never
auto-swap without confirmation** (F23; the budget rule is D6).

**Steps.**
1. **Name the operator-pause state.** In `draw_patch_bay`
   (`inspect_door.cpp:206-210`) the `Pause`/`Resume` pair drives operator pause;
   surface an explicit label **"PAUSED (you)"** (with the paused_dropped
   truncation consequence already shown, :127-131) so it never reads as the
   budget block. This state's only recovery is **Resume** — say so.
2. **Rename the budget-block label.** `budget_blocked_label` (budget.h:76-78)
   currently returns *"paused — another live view holds the tracer"* — the exact
   overload. Change it to **"BLOCKED — jack held by \<blocker\> on \<target\>"**
   (a *fact* string; the `BudgetDecision` already carries `blocker` and the
   verbatim `reason`, budget.h:57-64). Update `test_budget.cpp` — this is a pure
   string change, headlessly asserted. It reads BLOCKED, not paused.
3. **The three explicit actions.** Replace the single swap-confirm block
   (`inspect_door.cpp:212-222`) with, when `!d.allowed`:
   - **[Swap]** — arms the existing `swap_pending`/`swap_blocker` two-step
     (`inspect_request_start`/`inspect_confirm_swap`, :59-84); the confirm text
     **names what detaches** ("Stop the \<blocker\> capture on \<target\> and
     start \<want\>"). Keep it a two-step confirm — never auto-swap.
   - **[Queue]** — a new field on `InspectState` (`queued_want`, beside `want`,
     `doors.h:121-127`): stash the desired mode as a **visible cancellable chip**
     rendered near the patch bay, with a one-line note — *"Queued: \<want\> will
     start automatically when the \<blocker\> jack frees."* When
     `budget_can_start(queued_want, active)` turns `allowed` (the blocker
     stopped/ended), fire the start. This is a model transition, driveable in
     `test_budget.cpp`/`test_inspect.cpp` without a live target.
   - **[Cancel]** — clears both `swap_pending` and `queued_want`; the chip
     disappears.
4. **No silent auto-swap.** Assert in code and test that a Queue firing only ever
   happens when the jack is genuinely free (Swap requires the confirm; Queue
   requires the blocker gone) — the budget's one-jack invariant (budget.h:1-19) is
   never bypassed.

**Tests.** Extend `test_budget.cpp`: assert `budget_blocked_label` now reads
"BLOCKED — jack held by …" naming the blocker (not "paused"); assert a queued
want does not start while the blocker is `active` and *does* start on the frame
`budget_can_start` flips to allowed; assert Swap needs the explicit confirm
(`swap_pending` must be armed then confirmed — no path from block to start
without it). Extend `test_inspect.cpp`: operator-pause and budget-block render
**distinct** model states (a `PausedByOperator` vs `BlockedByBudget` predicate),
each with the right action set (Resume vs Swap/Queue/Cancel), and no auto-swap.
Model state, not pixels (D4). Null backend, `make desktop-test`.

**Docs.** CHANGELOG `Changed`: "paused" is split into operator-pause ("PAUSED
(you) — Resume") and budget-block ("BLOCKED — jack held by …") with explicit
Swap/Queue/Cancel; `Added`: a Queue path that starts when the jack frees.
`desktop/README.md`: the patch-bay section gains the two-states distinction.

**Done when.** the two states render distinct labels and action sets; Swap is a
named two-step confirm and Queue is a visible cancellable chip that starts only
when the jack frees; no path auto-swaps without confirmation; `test_budget.cpp`
and `test_inspect.cpp` assert both states and the no-auto-swap invariant;
`make desktop-test` green.

### T4 — generalise `progress.h` to every long op + degrade the 3D scrub  (M, depends on: 14 T3)

**Goal.** Give **every** operation that can exceed a frame budget the faithful busy
signal `progress.h` already decides — elapsed-time + Cancel + spinner — and give
the 3D scrub a **degrade-to-coarse-terrain** path instead of a silent UI-thread
stall (F21).

**Steps.**
1. **Extend the helper, keep it faithful.** Add to `ui/progress.h` (still pure,
   header-only, ImGui-free): an elapsed-time carrier and a `cancel_requested`
   flag on a small `LongOp` struct — the *decision* half only; the draw half
   (spinner/bar/Cancel button) stays in a thin `draw_progress(LongOp&)` in a
   `.cpp`. The determinate-vs-indeterminate fidelity rule is unchanged: no
   fabricated total (progress.h:1-9) — an op with no genuine total gets the
   indeterminate spinner, never a fake percentage.
2. **Wire the long-op sites.** Route each through the helper:
   - **PT decode / replay slice** (`live/ptslice.*`) — indeterminate + elapsed +
     Cancel.
   - **symbol / codeimage load** (`doc/recording.cpp`, consumed by
     `views/region.cpp`, `space/terrain.cpp`) — determinate when a byte count is
     known, else indeterminate.
   - **terrain re-slice / trajectory rebuild** (`scene3d/hud.h:27-28` →
     `TerrainModel::slice(t)` + trajectory rebuild) — the scrub site.
3. **Degrade the 3D scrub to coarse terrain.** When a re-slice on `playhead_moved`
   (hud.h:27) would exceed the frame budget, render the **coarse rung**
   `terrain.h` already defines (the flat/coarse plane, labelled coarse,
   terrain.h:18-23) for the in-flight frames and show the progress + Cancel, then
   swap to the full slice when it lands — instead of blocking the UI thread. The
   coarse plane is *already a faithful, labelled* representation (terrain.h:22),
   so degrading to it hides nothing (D7): it is the same provenance chip the
   coarse rung shows normally. Decide the "exceeds budget → degrade" purely (a
   `should_degrade(cell_count, budget)` predicate) so it is testable without GL.
4. **Cancel semantics.** A Cancel abandons the in-flight op and leaves the last
   good state (the coarse terrain, the prior slice) — never a half-built model.
   Cancel is a model flag the caller polls; assert it in the pure layer.

**Tests.** Extend `progress.h`'s unit test (or a new `test_progress.cpp`): assert
`progress_mode` still refuses a fabricated total (the fidelity rule), that elapsed
advances, and that `cancel_requested` is observable. New `test_scene_degrade.cpp`
(or extend `test_camera.cpp`/`test_drillin.cpp`): assert `should_degrade` returns
coarse for an over-budget cell count and full otherwise, and that the degraded
frame carries the **coarse provenance label** (not presented as measured
emptiness — terrain.h:22). Model state, not pixels (D4). Null backend / no GL,
`make desktop-test`.

**Docs.** CHANGELOG `Added`: a uniform elapsed-time + Cancel busy signal on every
long op (PT decode, symbol/codeimage load, terrain/trajectory rebuild); the 3D
scrub degrades to coarse terrain instead of stalling. `desktop/README.md`: note
the busy signal + the coarse-degrade behaviour.

**Done when.** every long-op site (PT decode, symbol/codeimage load, terrain/
trajectory rebuild) shows progress + elapsed + Cancel; the 3D scrub renders the
labelled coarse terrain while a re-slice is in flight instead of freezing; the
no-fabricated-total fidelity rule still holds; `make desktop-test` green.

## Task order & parallelism

- **T1 is the keystone of this brief** and gates nothing else here, but its
  schema `severity` change must clear **doc 01's Phase-3 freeze sign-off** before
  the render half merges — start the sign-off conversation first (it is the long
  pole), and land T1's tier *colours* on doc 24 T5.1's palette (do the two in
  coordination; if 24 T5.1 has not landed, T1 may use `theme.h`'s existing
  accessors as a temporary floor and migrate to the named semantic set when 24
  lands — but do not add new colour literals).
- **T2, T3, T4 are mutually independent** and independent of T1's schema pole —
  each is a self-contained restructuring of one live/rendering surface and can go
  to three different developers in parallel. T2 and T3 both touch
  `inspect_door.cpp` / the live layer, so coordinate merges (they edit adjacent
  regions of `draw_status`/`draw_patch_bay`).
- **T2 depends on doc 16 T1** (toasts) only for the supplement layer — if 16 T1
  has not landed, build the placard first and add the toast echo when it does
  (the placard is the load-bearing half; the toast is the supplement).
- **T4 depends on doc 14 T3** (`progress.h`, already landed).

## Constraints & gates

- **D7 — restructure, never remove; keep EVERY truth.** This is the load-bearing
  constraint of the whole brief. No task hides a field: T1 grades loudness but
  every fidelity field still renders and the T3 integrity tier stays
  non-collapsible; T1's neutral tier for a statistical/dropped survey still
  surfaces its `lost`/`throttled` drop record; T2 makes the torn/mismatch causes
  *louder and more specific*, never quieter; T4's coarse-degrade shows the same
  labelled coarse provenance the rung shows normally. The graded system must keep
  passing against the committed deliberate low-fidelity fixtures
  (`tests/golden-asmtrace/low-fidelity/`, D6/D7).
- **T1 is a schema change (D5, Phase-3 freeze).** The `severity` field lands
  under 01's append-only rule with **01 owner sign-off recorded in
  `asmtrace-schema.md`** (as `codeimage`/`stitch` did, schema:657); it must be
  **derivable** from the existing fields so no producer is forced to emit it and
  every old recording still grades. Do not merge the render half before sign-off.
- **The colours are doc 24 T5.1's, not new literals.** T1's tier colours come
  from doc 24's semantic palette accessors; do not add colour literals to
  `theme.h` in this brief (F14/T5.1 owns that axis).
- **Toasts supplement, never replace (16 T1).** T2's placard is the durable
  surface; the toast is the ephemeral echo.
- **No auto-swap without confirmation (D6).** T3's Swap is a named two-step
  confirm and Queue starts only when the jack is genuinely free; the one-ptrace-
  jack invariant (budget.h) is never bypassed.
- **No fabricated totals (14 T3).** T4 preserves `progress.h`'s fidelity rule:
  an op with no genuine total gets the indeterminate spinner, never a fake
  percentage.
- **Headlessly testable (D4).** Every task asserts **model state, not pixels**,
  on the null backend under `make desktop-test`; the pure decision halves
  (`fidelity_severity`, `end_cause`, the budget/queue predicates,
  `should_degrade`) are header-testable without an ImGui context. Any genuine
  interaction step (a Swap/Queue click flow) may use the doc 17 T1
  `make desktop-ui-test` engine lane, but the states themselves are model-checked.
- **Permissive-license floor (D2/D4).** No new third-party dependency is
  introduced by this brief; the Codicon glyphs (T1 step 3) reuse doc 13 F3's
  already-vendored Codicons.

## Out of scope

- **The full semantic palette + CVD second channel** (T5.1/T5.2, doc 24) — T1
  *consumes* T5.1's colours and feeds T5.2's second-channel rule with its glyph
  set, but does not build the palette. Keep the axis in doc 24.
- **The capability-panel positives + remedy map** (T4.2, Wave 0, doc 18) — T2
  reuses the remedy *voice* (verbatim reason + one-line fix) but does not touch
  `capability_panel`.
- **The perturbing-mode pre-commit confirm** (T4.5, Wave 0, doc 18) — a different
  live-arming hazard; not this brief.
- **The glossary / term registry** (T5.3, doc 24) — the coined-lexicon problem
  (Loom, patch-bay, jack) is doc 24's; T3 uses "jack"/"target" as they already
  read in `budget.h`.
- **Author-output save + dirty-close** (T2.6, doc 18) and **workspace
  persistence** (T2.5, doc 20) — unrelated data-loss surfaces.
