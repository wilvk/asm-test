# Consume the Reweave request — wire the fork/take verdict overlay into the app — implementation

> **A per-finding brief for [the 2026-07-29 UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #20** ("Wire the fork/take verdict overlay and Reweave gesture into the app"),
> cut after a code-grounded design pass found the review under-scoped the engine
> leg. The review framed #20 as a mechanical wire-up ("consume
> `L.loom.reweave.requested` each frame, gate the engine leg behind
> `ASMTEST_DESKTOP_CAN_AUTHOR`"). It is not — a correct wire-up needs a data
> source the review did not name and a correctness latch it did not mention.
>
> **The dead interaction (unchanged from the review).** The Reweave form
> ([`fabric_imgui.cpp:533`](../../../desktop/src/loom/fabric_imgui.cpp#L533)) sets
> `L.reweave.requested = true` with a captured `req_step`/`req_reg`/`req_value`, but
> **nothing consumes it** — `shell.cpp` never reads `requested`. The engine leg
> `loom_take_run_from_step` ([`forks.h:130`](../../../desktop/src/loom/forks.h#L130)),
> the take-view builder `loom_take_view` and the overlay painter `loom_take_plan`
> ([`take_view.h`](../../../desktop/src/loom/take_view.h)) are referenced **only by
> tests**, and `draw_loom_plan` carries `take_dim`/`take_hot`/`patient_zero` painter
> cases no in-app plan ever emits. The accumulator half is already live: `L.takes`
> (`std::vector<loom_take_node_t>`), the takes gutter with per-take remove + clear,
> and the reversible `UndoCommand::Kind::TakeSet`
> ([`shell.cpp:3048`](../../../desktop/src/ui/shell.cpp#L3048)) all exist and work.
>
> **The blocker the review missed — entry args are never recorded.**
> `loom_take_run_from_step(code, code_len, args, nargs, edit, out)` **re-executes**
> the emulator: it resumes the run to the checkpoint at step K with the ORIGINAL
> entry arguments, applies the edit, and runs forward. It needs the routine's raw
> bytes AND its entry args. A recording carries neither in a form the loom draw path
> holds: `Streams` exposes only the code *identity* (`code_sha`/`code_name`/
> `code_len`, [streams.h:194](../../../desktop/src/doc/streams.h#L194)), never the
> bytes, and **no schema event records the entry args at all** (grep: zero hits).
> Code bytes are recoverable from a `codeimage` event (`asmtest_codeimage_bytes_at`,
> used at [disasm.cpp:121](../../../desktop/src/views/disasm.cpp#L121)) — but the
> **args are simply gone** once a run is over. So a reweave on a reopened or attached
> recording is **impossible**, not merely unwired.
>
> **The one live source of both — the Author state.** `ShellState::author`
> (`AuthorState`, [shell.h:219](../../../desktop/src/ui/shell.h#L219)) retains, across
> frames, the last authored run's assembled `image` bytes + `image_base` AND its
> `args[6]`/`nargs`. Reweave is intrinsically an author-time what-if ("I authored
> this routine and ran it — now show me the counterfactual"), so the Author state is
> the correct and only source. This is *why* the review pointed at the
> `ASMTEST_DESKTOP_CAN_AUTHOR` seam without realising the seam is also the data source.
>
> Authored 2026-07-30 against HEAD `4e06409`. If a cited file:line disagrees with the
> code when you implement, the code wins — re-verify, then fix this doc in the same
> change.
>
> **Status (2026-07-30) — ✅ 5/5, adversarially reviewed.** T1-T5 landed. All
> three variants build green (`make desktop`, `desktop-render`, `desktop-test`)
> and `ldd` confirms `asmtest-viewer` still links no unicorn/keystone/capstone.
>
> A 4-dimension Workflow review (fidelity / undo-lockstep / build-graph /
> coverage, each independently adversarially verified) ran against the first
> landing and confirmed 17 real findings — the design above held, but the
> FIRST implementation pass had four real bugs, all fixed in the same change:
>
> 1. **Author `args`/`nargs` were never frozen** alongside `image`/`image_sha`
>    — they are live, always-editable widget fields, so dragging the arg
>    slider after a Run (with no re-Run) let a reweave replay args that never
>    produced the identity-matched recording. Fixed: `AuthorState` gained
>    `frozen_args`/`frozen_nargs`, captured at the exact point `image_sha` is
>    computed; `shell_reweave_source` reads the frozen copies, never the live
>    ones.
> 2. **`L.takes`/`L.take_views` survived a Loom tab switch** — `ShellState::loom`
>    is a single, non-per-recording state, and its `source_id`-change reset
>    block never cleared them, so switching to an unrelated recording painted
>    a stale take's patient-zero/cone overlay onto a fabric it had nothing to
>    do with. Fixed: both clear in that reset block (not undo-tracked, like the
>    other per-recording resets there). This reopened a second-order hazard —
>    `Ctrl+Z` could resurrect the cleared recording's takes via a stale
>    `TakeSet` command — closed by stamping `UndoCommand::take_rec` (the
>    recording id at push time, mirroring the existing `Filter`/`filter_rec`
>    pattern) and skipping the restore in `undo_apply` when it doesn't match
>    the Loom's current `source_id`.
> 3. **A refused reweave (K past the run's length, an unknown register) still
>    painted a fabricated whole-canvas divergence** — `loom_take_view` computed
>    real-looking alignment/cone/steps against an intentionally-EMPTY take
>    fabric, so `dt_first_divergence` reported "diverged at step 0" and every
>    base step got marked unaligned, painting a dashed band across the entire
>    canvas plus a patient-zero line for a take that never ran. Fixed:
>    `loom_take_node_t` gained `has_fabric` (`take.steps > 0`, computed inside
>    `loom_take_view`); a hard refusal now short-circuits before computing any
>    alignment/cone/steps, and the T4 paint loop skips a view with
>    `!has_fabric`.
> 4. **`reweave_apply.cpp`'s success path discarded `take.err`** — a genuine
>    caveat ("faulted after the edit at step N" / "operand buffers filled: a
>    lower bound") alongside a REAL, non-empty fabric was silently dropped
>    (hardcoded to `std::string()`), so a partially-faulted counterfactual was
>    indistinguishable from a clean one. Fixed: `take.err` now reaches
>    `node.err` verbatim on the success path too. `fault_card()`/`node.fault`
>    stay honestly empty for a reweave (`loom_take_run_from_step` never
>    populates `result` — only `loom_take_run`'s session-bracketed leg does);
>    fabricating fault_kind/addr without real data would be worse than no
>    fault card, so that gap is recorded below, not papered over.
>
> Also fixed: the disabled-reweave copy said "the render-only viewer" even in
> test/uitest builds that are not the viewer (reworded to name no specific
> binary). Test coverage added for all four: `test_loom_reweave.cpp` (empty-
> vs-real fabric, zero-prims on a refusal, a deterministically-reproducible
> faulted reweave) and `test_shell.cpp` (`take_rec` cross-recording undo
> guard, mirroring the existing Filter/`filter_rec` test).
>
> **Recorded, not fixed here** (lower severity / larger or orthogonal scope):
> - Rich `fault_card()` for a reweave (fault_kind/addr/disassembly) needs
>   `loom_take_run_from_step` to populate `result`, which needs the resume
>   seam's C API (`src/dataflow_resume.c`) to expose fault details it does not
>   today — a C-library signature change, out of scope here.
> - CI's `desktop` job installs no Keystone, so `DESKTOP_ENGINE_TESTS`
>   (`desktop_test_loom_forks`/`desktop_test_regsynth`/the new
>   `desktop_test_loom_reweave`) likely self-skip there silently — inherited by
>   this change, not introduced by it. `ci.yml` was being concurrently edited
>   by another agent during this pass; not touched here.
> - No automated `ldd`/`nm` check mechanically re-proves `asmtest-viewer` stays
>   engine-free on every build — today it rests on object-list membership plus
>   this pass's one-off manual verification. A `desktop-addon-compile-check`-
>   style Makefile target would close this.
> - `reweave_apply.o` joining `forks.o` under `DESKTOP_LOOM_APP` widens (1→2)
>   the set of `desktop/src/loom/*.o` basenames that are silently NOT
>   engine-free despite sharing a directory with five that are; not exploitable
>   today since every link line enumerates objects explicitly (no wildcard
>   aggregation exists), but worth a maintainer's eye if that ever changes.

## Why this is not a one-shot change (the two real hazards)

1. **The engine-free viewer must stay engine-free.** `fabric_imgui.o` and `shell.o`
   are compiled into THREE binaries: `asmtest-desktop` (full, GPL-2.0, links the
   engines), `asmtest-viewer` (render-only, **permissively licensed, links no
   engine**), and the null-backend tests. `loom_take_run_from_step` pulls in
   `asmtest_emu.h` → Unicorn/Keystone. The engine call and its include MUST sit under
   `#ifdef ASMTEST_DESKTOP_CAN_AUTHOR`, and only the app-variant object may define it
   — exactly the pattern [`author_door.cpp:30/183`](../../../desktop/src/ui/author_door.cpp#L30)
   already uses. A stray engine symbol in `asmtest-viewer` is a link failure AND a
   licensing violation.

2. **A wrong-routine reweave is a D7 lie.** The Author state holds ONE routine's
   inputs; the Loom is shown per recording tab. If the active tab's recording is not
   the one the Author state produced, running `loom_take_run_from_step` with
   `s.author.image`/`args` paints a counterfactual of a DIFFERENT program as if it
   were this recording's — a fabricated fabric, the precise failure the whole app is
   built to refuse. The consumption MUST be latched on a `code_sha` identity match
   (Streams already carries `code_sha`; SHA the `s.author.image` with the same helper
   that produced it) and REFUSE loudly (a `loom_take_node_t` with a verbatim `err`,
   never an empty or invented fabric) when it does not match.

## Tasks

- **T1 — thread the reweave inputs into the loom draw.** Give `draw_loom` a small
  engine-free input struct (`const uint8_t* code; size_t code_len; const long* args;
  int nargs; std::string code_sha; uint64_t base;`) sourced from `s.author` at the
  two call sites ([shell.cpp:1331, :2649](../../../desktop/src/ui/shell.cpp#L1331)).
  All fields are plain data — no engine header crosses the seam here. Null/empty when
  no authored run is live.

- **T2 — the identity latch (pure, tested).** A pure predicate
  `loom_reweave_available(streams_code_sha, author_image_sha, nargs)` → bool, so the
  "may this recording be reweaved?" rule is a headless assertion (D4), not buried in
  the draw. Drives whether the form's button is enabled and whether a request is even
  built.

- **T3 — consume the request behind the seam.** In `fabric_imgui.cpp`, under
  `#ifdef ASMTEST_DESKTOP_CAN_AUTHOR`: when `L.reweave.requested` and T2 passes, build
  the `loom_from_step_edit_t`, call `loom_take_run_from_step`, weave the take fabric
  (`loom_fabric_build(take.vt(), take.g(), take.provenance(), …)`), build the
  `loom_take_view`, and append a `loom_take_node_t` to `L.takes` as a reversible
  `TakeSet` command through `undo`. Under `#ifndef`, clear `requested` and append a
  node whose `err` states the viewer cannot assemble a take (the existing gutter
  copy). Always clear `requested` (one-frame latch).

- **T4 — paint the overlay (pure, both binaries).** Call `loom_take_plan` after
  `loom_plan` for the active take view(s) so `patient_zero`/`take_dim`/`take_hot`/
  `dashed_tail`/`fault_card` finally paint. Pure — it ships in `asmtest-viewer` too,
  so a recorded take somebody else authored still renders. Requires `LoomState` to
  hold the built `loom_take_view_t`(s) alongside the gutter `loom_take_node_t`s.

- **T5 — the Makefile seam + tests.** Add the app-variant `fabric_imgui.o` (and
  `shell.o` if T1's source read needs it) to the `ASMTEST_DESKTOP_CAN_AUTHOR=1` flag
  list in [mk/desktop.mk:849](../../../mk/desktop.mk#L849); confirm `forks.o` is in the
  app link closure (it is, via `author_door`). Verify ALL THREE builds green:
  `make desktop` (app), `make desktop-render` (viewer — assert no engine symbol),
  `make desktop-test` (null). Extend `test_loom_forks`/a UI-lane test to drive
  request → run → append → paint end to end, and pin the T2 refusal path.

## Out of scope / recorded

- **Reweave on a reopened or attached recording** — impossible until the schema
  records entry args (a `session.params`/header `args` field, or a new `entryargs`
  event). A separate, larger decision; note it, do not silently degrade.
- **arm64 / non-x86-64 reweave** — `loom_take_run_from_step` is x86-64-guest only
  (the resume seam's scope, [forks.h:127](../../../desktop/src/loom/forks.h#L127)).
- **Live-process reweave** — forks never touch a live process, forever, by design
  ([forks.h:14](../../../desktop/src/loom/forks.h#L14)); a reweave is always an
  isolated-guest emulator replay and is badged `kLoomForkDisclosure`.
