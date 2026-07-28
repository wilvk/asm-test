# Anchor the 3D plane — place a routine-relative path, or say why not — implementation

> **A tail follow-on to the [extension roadmap](27-extension-roadmap.md)**, and the
> repair of a seam left by [10](10-spacetime-3d-overview.md) T3/T5 and
> [25](25-live-model-wiring.md) T6. It removes a single structural limit: the 3D
> overview places geometry **only** for a `basis:"abs"` recording, and the only
> producer of `basis:"abs"` in the tree is the synthetic golden-scene generator
> ([`record_scene_abs`](../../../tools/asmtrace_record.c#L969)). Every real
> capture — live `trace`, live `dataflow`/`auto`, and every recorded corpus file —
> is `basis:"rel"` ([asmspy.c:1800](../../../cli/asmspy.c#L1800),
> [asmtrace_record.c:1314](../../../tools/asmtrace_record.c#L1314)), so its plane
> comes up **empty, and unlabelled**. This brief anchors a relative path to the
> absolute span the recording itself states, and makes every unplaced vertex say
> why.
>
> **This is not "draw a path we cannot place."** [10](10-spacetime-3d-overview.md)
> T3's rule — a rel path is *"never projected as a true absolute path"* — was
> correct against the fact available then: a bare offset has no anchor. It is
> narrowed, not repealed. When a recording carries exactly one `codeimage` code
> span, `base + off` **is** the true address — a derivation from a fact the
> recording states, not a guess — and refusing it discards a measurement rather
> than protecting one. When the span is absent or ambiguous, this brief refuses
> **louder** than today: no placed geometry *and* a stated reason.
>
> Authored 2026-07-29, verified against HEAD `24778e4`. If a cited file:line
> disagrees with the code when you implement, the code wins — re-verify, then fix
> this doc in the same change.
>
> **Status (2026-07-29) — ✅ 5/5. COMPLETE.** T1 `resolve_anchor` (`38d05d6`);
> T2 the anchored rel PC path + placement count (`1622aaa`); T3 the labelled
> `df_step` height rung + anchored trace rung (`ff650c3`); T4 the honesty chrome,
> `scene-df-loop` golden, and end-to-end tests (`a3b2247`); T5 convergence admits
> an anchored rel path (this commit). Both docker lanes green; the golden is
> byte-stable under `docker-cli`.

## Why this work exists

Double-click a process in Inspect → Start → "Arm it anyway" → **View in the 3D
overview**, and the tab opens onto nothing. No path, no terrain relief, no error.
The capture is running and healthy; the 3D pane is simply unable to place any of
it, and says nothing about that.

Three independent defects stack to produce it, and only the third is a design
question.

**1. The promised honesty chip never fires (a D7 breach against a landed claim).**
[25](25-live-model-wiring.md) T6 states that a live single-step capture *"shows an
honest routine-relative execution path … and the HUD shows the existing "rel:
routine-relative (not a true path)" chip."* It does not. That chip is keyed on
`terr.basis` ([hud.cpp:49-53](../../../desktop/src/scene3d/hud.cpp#L49)), which is
the **canvas** basis — assigned from `trace` events at
[terrain.cpp:122](../../../desktop/src/space/terrain.cpp#L122). A live
`dataflow`/`auto` session emits **no** `trace`
([dataflow_record](../../../cli/asmspy.c#L2304) writes `df_invocation` / `df_step`
/ `regstate` / `fpenv` / `mem` / `df_edge` and never `trace` or `coverage`), so
`terr.basis` is `""`, the `else if` is skipped, and **no basis chip is drawn at
all**. `TrajectorySet::basis` — which does say `"rel"`
([trajectory.cpp:128](../../../desktop/src/space/trajectory.cpp#L128)) — is never
read by the HUD. The rel-ness is measured, recorded, and then dropped on the floor
between the model and the chrome.

**2. Nothing counts, so a total placement failure is indistinguishable from
success.** `Scene::set_trajectories`
([scene.cpp:257](../../../desktop/src/scene3d/scene.cpp#L257)) calls
`proj.project(pt.addr, &u, &v)` per point and appends a vertex only on success;
[`Projection::project`](../../../desktop/src/space/projection.cpp#L102) returns
false for any address below every region
([:116-117](../../../desktop/src/space/projection.cpp#L116) — `if (lo == 0) return
false; // addr is below every region`), which is every small routine-relative
offset against a real (often ASLR'd) base. Every vertex is dropped, the
`line.size() >= 6` gate ([scene.cpp:280](../../../desktop/src/scene3d/scene.cpp#L280))
fails, and no polyline is built. `TrajectorySet::refused()`
([trajectory.h:59](../../../desktop/src/space/trajectory.h#L59)) stays **false** the
whole time — nothing was *detected* as mismatched, each point merely failed
individually — so the "trajectory refused" chip
([hud.cpp:54-55](../../../desktop/src/scene3d/hud.cpp#L54)) does not fire either.
The model reports a healthy set of trajectories that the renderer silently cannot
draw. The same silence covers the terrain: `cell_of` is called on raw offsets at
[terrain.cpp:155](../../../desktop/src/space/terrain.cpp#L155) and
[:214](../../../desktop/src/space/terrain.cpp#L214) with no basis check, so a
rel-basis `trace` recording — i.e. **live `trace` mode**, the region engine, which
*does* emit `trace`+`coverage` ([region_record :1852](../../../cli/asmspy.c#L1852))
— places zero cells too, while `m.nsteps` ([:219](../../../desktop/src/space/terrain.cpp#L219))
happily reports a non-zero step count. Steps without cells, unexplained.

**3. `dataflow`/`auto` cannot produce a height field at all.** The terrain's relief
comes from `trace`+`coverage` via `decode_streams` → `dt_canvas_build`
([terrain.cpp:113](../../../desktop/src/space/terrain.cpp#L113)), and the value
producer structurally has none — a deliberate, documented schema gap
([asmtrace-schema.md](asmtrace-schema.md) *"No block starts from the L0
producer"*; [asmtrace_record.c:721-725](../../../tools/asmtrace_record.c#L721)
declines to guess block starts from an offset stream). So even a correctly
*anchored* live dataflow capture draws a flat plane. The per-step residency to
build a coarse rung from is already decoded and sitting in `s.df.insn_off`.

The net effect is that doc 10's entire growth rung — seven landed tasks — renders
its own test fixture and nothing else. The tab is present (its gate is only
`regions_from_codeimage` non-empty,
[view_presence.cpp:121-124](../../../desktop/src/ui/view_presence.cpp#L121)), which
is exactly the wrong signal: present, openable, and mute.

## What already exists (verified 2026-07-29)

- **The anchor fact, in the recording.** `serve_codeimage_emit`
  ([asmspy.c:3219](../../../cli/asmspy.c#L3219)) writes the scoped region's real
  absolute `base` (and `len`, clamped to `SERVE_CI_MAX_BYTES` = 4096,
  [:3189-3196](../../../cli/asmspy.c#L3189)) into a `codeimage` event, from the same
  session that emits the offsets. `regions_from_codeimage`
  ([terrain.cpp:77-107](../../../desktop/src/space/terrain.cpp#L77)) already
  collapses repeat versions of one base into a single `Region`, taking the widest
  `len`.
- **The offset's exact provenance.** `dfp_step_loop` computes `pc - base_ip`
  ([dataflow_ptrace.c:1168](../../../src/dataflow_ptrace.c#L1168)) — `df_step.off`
  is mechanically an offset from the same `base` the `codeimage` event states.
  `df_step` carries **no** `basis` and no region tag on the wire
  ([asmtrace_ndjson.c:276-277](../../../cli/asmtrace_ndjson.c#L276)).
- **The rel path builder** — [trajectory.cpp:109-130](../../../desktop/src/space/trajectory.cpp#L109)
  (the `df_step` fallback, `p.addr` raw at
  [:121](../../../desktop/src/space/trajectory.cpp#L121)) and the trace loop at
  [:57-95](../../../desktop/src/space/trajectory.cpp#L57); `TRAJ_RELATIVE_BASIS` is
  applied at [:135-137](../../../desktop/src/space/trajectory.cpp#L135) and then read
  **only** by [converge.cpp:51](../../../desktop/src/space/converge.cpp#L51) — never
  by `scene.cpp` or `projection.cpp`.
- **Two address families already share one set, correctly.** `mem.ea`
  ([trajectory.cpp:166](../../../desktop/src/space/trajectory.cpp#L166)) and the
  `survey` endpoints ([:209](../../../desktop/src/space/trajectory.cpp#L209)) are
  **absolute** by construction. Today a rel recording therefore mixes bases inside
  one `TrajectorySet` — anchoring the PC path is what makes them consistent.
- **The refusal idiom to mirror** — `basis_error`
  ([terrain.cpp:137-144](../../../desktop/src/space/terrain.cpp#L137)) refuses a
  mixed-basis canvas with a flat plane *plus* a stated reason, and `mem_present` /
  `mem_note` ([:128-130](../../../desktop/src/space/terrain.cpp#L128)) is the
  graded "present-or-say-why" pattern for a whole rung.
- **The one production caller** — [shell.cpp:653-663](../../../desktop/src/ui/shell.cpp#L653)
  weaves terrain, trajectory and convergences lazily; the terrain is built first,
  so its `Projection` is available to the trajectory builder at
  [:658](../../../desktop/src/ui/shell.cpp#L658).
- **The lane.** Everything here is pure `space/` + `scene3d` HUD; the null-backend
  desktop lane runs it all. Only the optional pixel bar in T4 needs the GL lane,
  which `make docker-desktop` provides for real (llvmpipe), not as a self-skip.

## Tasks

### T1 — the anchor: one shared rel→abs resolution in `space/`  (S)

**Goal.** One pure function answering *"what absolute span is a routine-relative
offset relative to, in this recording?"* — or refusing with the reason.

**Steps.**
1. In [`space/projection.h`](../../../desktop/src/space/projection.h), beside
   `build_projection`, add `struct Anchor { bool ok = false; uint64_t base = 0,
   len = 0; std::string reason; bool place(uint64_t off, uint64_t *abs) const; };`
   and `Anchor resolve_anchor(const std::vector<Region> &regions);`.
2. Implement in [`projection.cpp`](../../../desktop/src/space/projection.cpp) after
   `build_projection` (ends [:100](../../../desktop/src/space/projection.cpp#L100)),
   before `Projection::project` ([:102](../../../desktop/src/space/projection.cpp#L102)).
   Filter to `kind == Region::Code`, then: **exactly one** ⇒ `ok`, base/len from it;
   **zero** ⇒ `reason = "no codeimage code span — a routine-relative offset has
   nothing to anchor to"`; **two or more** ⇒ `reason` naming each hex base and
   stating that a rel offset carries no region tag, so the span is unrecoverable.
3. `place(off, abs)`: `if (!ok || off >= len) return false; *abs = base + off;` —
   an out-of-span offset returns false so the caller **counts** it rather than
   dropping it. This is the 4096-byte clamp case
   ([asmspy.c:3189-3196](../../../cli/asmspy.c#L3189)), which is common, not exotic.

**Code.** `projection.{h,cpp}` only — no new TU, so no `mk/desktop.mk` link-rule
churn, and `test_trajectory` (which already links `projection.o`) gains it free.
`terrain.cpp` deliberately does **not** host it: that would drag `terrain.o` into
`desktop_test_trajectory`.

**Tests.** `desktop/test/test_projection.cpp`, after the existing plane cases: one
code region anchors; zero refuses with a reason; two refuse with a reason naming
both bases; a non-code region does not make an anchor ambiguous; `place(len)` is
false while `place(len-1)` is true and equals `base+len-1`.

**Docs.** A comment block on `Anchor` citing the wire facts it derives from
([asmtest_trace.h:41-42](../../../include/asmtest_trace.h#L41),
[asmtrace_ndjson.c:276-277](../../../cli/asmtrace_ndjson.c#L276)).

**Done when.** `make desktop-test` passes; reverting the two-region branch to "pick
the first" fails a named check.

### T2 — trajectory: anchor the rel PC path, and count what lands  (M, depends on: T1)

**Goal.** A rel PC path is *placed* when the recording pins the span down, and when
it is not, the model **says so** instead of handing the renderer vertices nobody
can draw.

**Steps.**
1. [`trajectory.h`](../../../desktop/src/space/trajectory.h): add
   `TRAJ_ANCHORED = 1u << 2` to `TrajFlag`
   ([:25-34](../../../desktop/src/space/trajectory.h#L25)), documented as *"rel
   offsets PLACED against the recording's single codeimage span — a derived
   placement, not a measured absolute address"*. Add to `TrajectorySet`
   ([:48-60](../../../desktop/src/space/trajectory.h#L48)): `uint64_t pc_points`,
   `uint64_t pc_placed`, `bool anchored`, `std::string placement_note`. Change the
   declaration ([:76](../../../desktop/src/space/trajectory.h#L76)) to
   `build_trajectories(const Recording &r, const Projection &proj)` and keep a
   one-argument overload — documented **test/plane-free only** — so the ~20 existing
   test call sites compile unchanged.
2. [`trajectory.cpp:40`](../../../desktop/src/space/trajectory.cpp#L40): take the new
   parameter. After the `TRAJ_RELATIVE_BASIS` block
   ([:135-137](../../../desktop/src/space/trajectory.cpp#L135)) and **before** the
   refusal early-return ([:142-145](../../../desktop/src/space/trajectory.cpp#L142)),
   resolve the anchor. When `set.basis == "rel"` and the anchor is `ok`, rewrite each
   PC vertex through `place()`, set `anchored`, and OR in `TRAJ_ANCHORED` while
   **keeping** `TRAJ_RELATIVE_BASIS` — the wire basis is still rel and must keep
   saying so. When it is not `ok`, leave the offsets verbatim (never fabricate) and
   set `placement_note` from the reason.
3. Count placement before the emit loop
   ([:176-184](../../../desktop/src/space/trajectory.cpp#L176)): for each PC vertex,
   `pc_points++` and `pc_placed += proj.project(...)`. If `pc_placed < pc_points` and
   no note is set, state the shortfall **and name the 4096-byte codeimage clamp** as
   its cause — otherwise an honest partial placement reads as a regression.
4. Do **not** touch the `mem` spur loop
   ([:153-172](../../../desktop/src/space/trajectory.cpp#L153)) or the survey loop
   ([:193-219](../../../desktop/src/space/trajectory.cpp#L193)) — both are absolute by
   construction and become *consistent* with the PC path once it is anchored. Record
   that in the comment.
5. [`shell.cpp:658`](../../../desktop/src/ui/shell.cpp#L658) passes `sv.terr.proj`
   (valid: `build_terrain` moves the projection into the model on both exits, and
   the terrain is built first at [:656-657](../../../desktop/src/ui/shell.cpp#L656)).
6. Rewrite the fallback comment at
   [trajectory.cpp:97-108](../../../desktop/src/space/trajectory.cpp#L97) — its
   sentence *"the renderer never places it on the absolute plane"* is the line that
   made the silent drop look intentional, and it is what this brief narrows.

**Tests.** `desktop/test/test_trajectory.cpp`, inline NDJSON, building the `Region`
exactly as the existing abs case does: a df_step path anchors to the single span
(`addr == base + off`, both flags set, `basis` still `"rel"`); **every anchored
vertex projects onto the plane** — the assertion whose absence hid this bug;
placement is counted; a rel *`trace`* anchors too; two code spans refuse honestly
(`pc_placed == 0`, note set, no `TRAJ_ANCHORED`, `refused()` still false, addrs
unchanged); an offset past the span is *counted, not placed*; no code span leaves
the path unanchored and says so. Then a **regression bar applied at the end of
every case**: a built vertex is either placed or explained.

**Done when.** `make desktop-test` green; reverting the anchoring pass fails "every
anchored vertex projects onto the plane"; reverting the counter fails the
regression bar.

### T3 — terrain: a labelled `df_step` height rung, and the same anchor for the trace rung  (M, depends on: T1)

**Goal.** An Auto/Dataflow capture gets real relief, labelled as step residency and
never as block coverage; a rel-basis `trace` recording stops silently placing zero
cells.

**Steps.**
1. [`terrain.h`](../../../desktop/src/space/terrain.h): add `height_source`
   (`"trace"` | `"df_step"` | `""`), `height_note`, and `anchor_error` to
   `TerrainModel`. Extend the header's honesty-rules block with a third rule: **a df
   height is not block coverage.**
2. In `build_terrain` ([:109](../../../desktop/src/space/terrain.cpp#L109)), after the
   `basis_error` refusal ([:137-144](../../../desktop/src/space/terrain.cpp#L137)),
   resolve the anchor and route both `cell_of` call sites
   ([:155](../../../desktop/src/space/terrain.cpp#L155),
   [:214](../../../desktop/src/space/terrain.cpp#L214)) through it. For `basis ==
   "abs"` the placement is the identity, so every existing fixture and both scene
   goldens are behaviourally unchanged.
3. Rel offsets with no resolvable anchor ⇒ set `anchor_error`, leave `m.code`
   empty, but still fill `m.nsteps` — the time axis is real even when placement is
   refused. Do **not** reuse `basis_error`; that is reserved for mixed bases.
4. New rung: when the canvas placed nothing, drive the same walk from
   `s.df.insn_off` (already decoded at
   [:113](../../../desktop/src/space/terrain.cpp#L113)), set `m.nsteps` from it,
   `height_source = "df_step"`, and `height_note = "coarse: heights from
   single-step residency (df_step) — no block coverage"`. Per-cell `full_heat` comes
   from the step count, not the canvas. Synthesize **no** block semantics: `blocks`
   stays empty, nothing is marked covered, and nothing is flagged `TF_STAT` — this
   stream is exact, not sampled.
5. Leave the churn walk ([:160-206](../../../desktop/src/space/terrain.cpp#L160)) and
   the absolute `mem` loop ([:241-276](../../../desktop/src/space/terrain.cpp#L241))
   alone. Cite the churn walk in-code as the precedent to reuse **if and when** the
   schema tags `df_step` with a region — it is the right shape (a seq-merge with a
   step counter) but unusable today: it retains only the first bump per base and
   never walks `df_step`.

**Tests.** `desktop/test/test_terrain.cpp`: **G** df heights (one codeimage + three
`df_step`, no `trace` ⇒ non-zero `nsteps`, `height_source == "df_step"`, note set,
cells at `base+off` with the expected relief, `slice(0)` shows only the first hit);
**G2** a df height never claims block coverage; **H** two codeimage bases ⇒ no cells,
`anchor_error` set, `nsteps` still real — a flat plane that *says why*; **I** rel
`trace` + one codeimage places cells at `base+off`. Plus a regression bar in every
fixture: **steps without cells must be explained.** The existing mixed-basis fixture
stays refused (that check precedes anchoring).

**Docs.** Record the df rung in [10](10-spacetime-3d-overview.md) T2 as a second,
labelled height source.

**Done when.** Existing fixtures unchanged in behaviour; G/H/I pass; removing
`height_note` or `anchor_error` fails a named check.

### T4 — the honesty chrome, the end-to-end bars, and a live-shaped golden  (L, depends on: T2, T3)

**Goal.** Nothing about placement is silent, and one generated golden proves the
real Auto/Dataflow shape renders end to end.

**Steps.**
1. [`hud.cpp`](../../../desktop/src/scene3d/hud.cpp): insert at
   [:56](../../../desktop/src/scene3d/hud.cpp#L56), between the `traj.refused()` chip
   and the coarse/rich chip — `anchor_error` ⇒ a `kBad` "HEIGHTS NOT PLACED" chip;
   `height_source == "df_step"` ⇒ a `kWarn` "heights: single-step residency
   (df_step), not block coverage"; `pc_placed == 0` with points ⇒ `kBad` "PATH NOT
   PLACED"; a partial ⇒ `kWarn` "K of N path vertices off-plane"; fully anchored ⇒
   `kWarn` "rel: anchored to the codeimage span (derived placement)". House style:
   ALL CAPS only for refusals, lowercase `label: explanation` for graded facts,
   parenthetical caveats, no trailing punctuation.
2. **Fix the dropped chip from defect 1** in the same pass: the existing basis chip
   ([:49-53](../../../desktop/src/scene3d/hud.cpp#L49)) falls back to
   `traj.basis` when `terr.basis` is empty, so a df-only recording finally shows the
   rel label [25](25-live-model-wiring.md) T6 already promised. Echo `height_note`,
   `placement_note` and `anchor_error` as dim asides beside `mem_note`
   ([:68-72](../../../desktop/src/scene3d/hud.cpp#L68)). No `HudState` field is
   needed — chips derive from `terr`/`traj` each frame.
3. [`shell.cpp`](../../../desktop/src/ui/shell.cpp): after the slice block, when the
   terrain has no cells **and** nothing was placed, name the reason in the pane
   itself — the HUD is a separate window, and the same rule as the existing
   "no address-space regions" placard ([:728-737](../../../desktop/src/ui/shell.cpp#L728))
   applies: never present an empty plane unlabelled.
4. **No GL change.** Stipple is already the statistical channel; reusing it for
   "anchored" would conflate a derived placement with a sampled one. An anchored path
   is exact execution, honestly placed — its caveat rides in the HUD, where every
   other provenance fact rides. This keeps all four tasks off the GL-gated lane.
5. **New golden** `tests/golden-asmtrace/scene-df-loop.asmtrace` — the live shape no
   existing golden carries (absolute `codeimage` + region-relative `df_step`, **no**
   `trace`; the continuous fixture has df_step but no codeimage, and every other df
   golden also carries a rel `trace`, so the fallback never fires). Add
   `record_scene_df()` beside [`record_scene_abs`](../../../tools/asmtrace_record.c#L989),
   reusing its `codeimage` emission verbatim. Regenerate **only inside `docker-cli`**
   (`make asmtrace-golden`; the host's Capstone would churn the whole corpus) and
   commit only the new file. The committed file is then read unconditionally by the
   desktop lane, so no desktop test self-skips.
6. `desktop/test/test_shell.cpp`: extend the wired-pane block that already opens
   `scene-abs-loop.asmtrace` to open the new golden and assert the terrain has
   steps, `height_source == "df_step"`, and `pc_placed == pc_points > 0`.
7. `desktop/test/test_scene_fbo.cpp`: in the pure half, assert at least two vertices
   project so the `line.size() >= 6` gate
   ([scene.cpp:280](../../../desktop/src/scene3d/scene.cpp#L280)) cannot drop the
   tube; in the GL half, mirror the existing layer-toggle pixel diff — *"the anchored
   trajectory puts pixels on screen"* — which runs for real under
   `make docker-desktop`.
8. Docs: `CHANGELOG.md` (`Fixed`); [10](10-spacetime-3d-overview.md) T3 and
   [25](25-live-model-wiring.md) T6 amended to record the narrowed rule; and a
   **doc-only** clarification to [asmtrace-schema.md](asmtrace-schema.md) stating
   normatively that `df_step.off` is region-relative to the session's scoped span and
   carries no `basis` field. The wire is unchanged — the gap is now written down.

**Done when.** `make desktop-test` and `make docker-desktop` green; opening
`scene-df-loop.asmtrace` yields a non-empty terrain **and** a drawn trajectory;
deleting any one chip branch fails a named check; `make asmtrace-golden-check`
passes in `docker-cli`.

### T5 — convergence admits an anchored rel path  (S, depends on: T2)

**Goal.** Once a rel path is genuinely *placed* (T2), two anchored paths in the
same span are two paths on the same plane, and
[converge.cpp:50-51](../../../desktop/src/space/converge.cpp#L50) is refusing a
measurement rather than protecting one — the same narrowing this brief already made
for the renderer. Admit them, **and only them**.

**The soundness bar.** `detect_convergences` never compares an address to an
address: it projects each `TrajPoint::addr` once, buckets by **cell**, and joins on
cell identity + a step-gap window + `tid_a != tid_b`. So admission need only
establish that both `addr` values are legal inputs to that one projection:

- **S1 — same address space.** Guaranteed structurally, not by luck: T1's anchor
  base comes from `regions_from_codeimage`, which is the *same* region vector
  `build_projection` consumed, and [shell.cpp:653-663](../../../desktop/src/ui/shell.cpp#L653)
  hands the *same* `sv.terr.proj` to both builders. `base + off` projects exactly as
  a measured absolute PC at that address would. There is one anchor per recording,
  so two anchored trajectories are anchored to the *same* base by construction —
  cross-anchor comparison cannot arise.
- **S2 — individually placed.** Each vertex must be the *output of a successful
  placement*, not a raw offset left behind. This is the sharp one: T1's `place()`
  fails for `off >= len` and T2 leaves those vertices verbatim inside an otherwise
  anchored vector, and the 4096-byte clamp makes that common.
- **S3 — per-thread.** `TRAJ_STATISTICAL` stays excluded unconditionally.
- **S4 — one clock family.** Holds by construction: the `df_step` fallback runs only
  when the trace loop placed nothing, so one set has exactly one PC source. Worth a
  sentence in `converge.h`, not a guard.

**Steps.**
1. [`space/types.h`](../../../desktop/src/space/types.h): add `bool placed = true;`
   to `TrajPoint`, documented as *"`addr` is an address in the recording's address
   space — true for a measured absolute vertex, true for a rel offset the anchor
   placed, **false** for one it could not place, where `addr` is still the raw wire
   offset."* Defaulting `true` leaves every existing producer and hand-built test
   point unchanged. This is also the natural source for T2's `pc_placed` counter.
2. In T2's anchoring pass, set `p.placed` from the `place()` result. If T5 lands
   **with** T2, fold this in and drop this step.
3. [`converge.cpp:50-51`](../../../desktop/src/space/converge.cpp#L50): narrow the
   mask — keep `TRAJ_STATISTICAL` excluded unconditionally, exclude
   `TRAJ_RELATIVE_BASIS` **only when `TRAJ_ANCHORED` is absent**, and skip any
   `!p.placed` vertex. Apply the per-point skip to *every* admitted trajectory, not
   only anchored ones, so it cannot rot. The rel test is **narrowed, never dropped**:
   deleting the rel bit outright would readmit raw offsets, which is exactly the
   false address-space claim the file's comment forbids.
4. Rewrite the comment at [converge.cpp:41-43](../../../desktop/src/space/converge.cpp#L41)
   and the doc block in [`converge.h`](../../../desktop/src/space/converge.h) to state
   the four-condition bar, plus two inherited caveats: a mark over an anchored path is
   a hint over a **derived** placement (doubly not a proof), and widest-`len` aliasing
   means the shared cell may name bytes never at that offset in that version. The hint
   grade itself is unchanged — admitting anchored paths does not upgrade it.

**Honest limit, state it plainly.** `df_step` carries **no `tid`** on the wire
([asmtrace_ndjson.c:269-290](../../../cli/asmtrace_ndjson.c#L269)), so a live
single-step dataflow capture reads `tid = -1` and is **one** trajectory — it yields
zero marks whatever the flags say. T5 produces marks only for a rel **`trace`**
recording whose events carry `tid`. Without this sentence the task reads as promising
arcs on the Auto/Dataflow pane that will never appear.

**No change elsewhere.** `Scene::set_convergences` re-projects the mark addresses, so
an anchored absolute address lands on the drawn path with no `scene3d` edit; the HUD
does not consume `ConvergenceSet` at all, so there is no chip to add — the
derived-placement caveat already rides on T4's path chip. `desktop_test_converge`
already links what it needs, so no `mk/desktop.mk` edit.

**Tests.** `desktop/test/test_converge.cpp`: two anchored rel paths in one span
converge; the mark carries the **absolute** address (`base + off`, not `off`); an
anchored path and an equivalent abs path yield **identical** `ConvergenceSet`s (the S1
assertion); an anchored path never converges with an unanchored one; a shared cell
reached only by `placed == false` vertices yields zero marks (the clamp case);
same-tid and window rules unchanged; and an end-to-end case over inline NDJSON proving
the predicate matches what the builder actually emits. The **existing** unanchored-rel
and statistical exclusion cases stay untouched — they are the canaries that fail if
someone "fixes" the mask by deleting the rel bit.

**Docs.** The exclusion is currently asserted in three places — this brief's own
non-goals, `desktop/README.md`, and [10](10-spacetime-3d-overview.md) — and all three
become false; reconcile them in the same change.

**Done when.** `make desktop-test` green; reverting the predicate fails the first
case; deleting `TRAJ_RELATIVE_BASIS` from it outright fails the unanchored canary;
deleting the `!p.placed` skip fails the clamp case; removing the `placed` assignment
fails it too (the bit must be *measured*, not defaulted).

## Task order & parallelism

`T1` → (`T2` ∥ `T3`) → `T4`, with `T5` after `T2` — it can land in parallel with
`T4`. T2 and T3 touch disjoint files and share only the T1 helper. T5 cannot start
before T2: `TRAJ_ANCHORED` does not exist in the tree today.

## Constraints & gates

- **The 4096-byte clamp is the common case, not an edge case.** Any routine larger
  than `SERVE_CI_MAX_BYTES` ([asmspy.c:3189-3196](../../../cli/asmspy.c#L3189))
  yields genuinely out-of-span offsets, so the "K of N off-plane" chip will fire on
  real captures. That is honest — but the note must name the clamp, or it reads as a
  regression.
- **Widest-`len` aliasing.** `regions_from_codeimage` keeps the widest `len` across
  versions of a base ([terrain.cpp:97-99](../../../desktop/src/space/terrain.cpp#L97)),
  so an offset valid only in a shorter version still lands inside the span. The base
  is right, so it is not a mis-attribution, but the cell may correspond to bytes never
  at that offset in that version. A real fix needs per-`(base, version)` regions —
  out of scope; record it as a known limit.
- **`TrajPoint::addr` changes meaning under the flag.** After T2 it is absolute for an
  anchored path while `set.basis` still reads `"rel"`. Audited consumers:
  [converge.cpp:51](../../../desktop/src/space/converge.cpp#L51) (skips rel by flag —
  still correct, merely conservative), [scene.cpp:257](../../../desktop/src/scene3d/scene.cpp#L257)
  (now correct), `hud.cpp` (reads `terr.basis`). Update the comment on `addr` in
  `space/types.h` in the same change.
- **Mid-session anchor flip.** A capture that re-arms on a second candidate span
  ([asmspy.c:3733-3761](../../../cli/asmspy.c#L3733)) flips from anchored to refused
  on the next live re-weave: the plane goes from drawn to labelled-empty in front of
  the user. Honest, but the note should say the session re-armed on a second span.
- **D4 / D9 unchanged.** Everything is pure `space/` + HUD; no engine, no GL, no wire
  change, no regeneration of any existing golden.

## Non-goals / honest limits

- **Not a schema change.** `df_step` gains no `basis` and no region tag here; the
  producers keep emitting `"rel"`. Tagging `df_step` with its region is the real fix
  for the multi-span ambiguity — with it, the churn walk becomes a sound "region
  as-of this step" resolver and the refusal branch mostly disappears — and it is a
  producer + schema change with its own brief:
  [37](37-region-tag-on-df-step.md). **T1's `resolve_anchor` remains the permanent
  documented fallback** for every recording produced before 37 lands, and for rel
  `trace` recordings, which 37 deliberately does not tag.
- **Not "region as-of this step".** That rule is **not soundly recoverable** today
  and must not be attempted: `df_step` carries no region tag; the candidate walk
  resets the version counter and restarts the `when` timeline; and the refresh path
  emits `codeimage` *after* the invocation it belongs to, so seq order is "steps then
  image" — the opposite of what a naive as-of rule assumes. Ambiguity is refused, not
  guessed.
- **Not block coverage.** The df height rung is single-step residency. It never
  synthesizes `coverage`, never fills `blocks`, and is labelled at every surface. The
  L0 producer's absence of block starts stays a documented gap
  ([asmtrace-schema.md](asmtrace-schema.md)).
- **Not a new visual channel.** An anchored path draws exactly like any exact path;
  its derivation rides in the HUD. Stipple stays the statistical channel.
- **The statistical layer is still never flagged rel or anchored.** It is built after
  the flag pass and uses absolute survey endpoints, so it is not mis-placed — but a
  rel recording carrying `survey` still holds two address families in one set,
  distinguished only by `TRAJ_STATISTICAL`. Documented, not fixed.
- **Convergence admits anchored paths, but only in T5 and only for `trace`.** T5
  narrows [converge.cpp:50-51](../../../desktop/src/space/converge.cpp#L50) so an
  *anchored* rel path takes part while an unanchored one still cannot. It changes
  nothing for the live Auto/Dataflow pane: `df_step` carries no `tid`, so such a
  capture is a single trajectory and convergence needs two.

## Cross-references

Repairs the seam left by [10](10-spacetime-3d-overview.md) T3/T5 (the projection
rule and the live overlay) and [25](25-live-model-wiring.md) T6 (which promised the
rel chip that never fired). Consumes the live substrate of
[07](07-serve-live-host.md)/[08](08-observer-views.md) and the `codeimage` kind
defined there. Sits directly downstream of [35](35-continuous-live-dataflow.md) —
a continuous capture is exactly the session whose 3D pane stays empty longest — and
beside [34](34-playhead-and-scene-reach.md), whose two-axis honesty note governs the
terrain-time axis this brief now derives from `df_step`. Its producer-side sequel is
[37](37-region-tag-on-df-step.md), which states the region on the wire so the
multi-span case resolves instead of refusing; land **36 in full, then 37**. Schema/D5
[01](01-asmtrace-format.md) + [asmtrace-schema.md](asmtrace-schema.md) (doc-only
clarification). Honesty chrome D7 / [23](23-graded-truth-layer.md); palette and
wording D7 / [24](24-one-visual-language.md). Engine-free closure D4; capture host
D9.
