# 3D scene availability and mode advertising — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the verified defect family around 3D scene availability — gates that open on a scene with no geometry, a selected substrate nothing ever leaves, notes and reason strings that describe behaviour the tree does not implement, and a patch-bay table that advertises views its captures cannot fill and omits one they can — and then give the desktop a way to record a whole multi-mode serve session so one reopened file can host four of the five substrates.

**Architecture:** Two independent surfaces, joined by one user-visible question ("why is `address plane` the only scene I can pick?"). The first is `desktop/src/ui/shell.cpp`'s `shell_kind_availability` plus the four scene builders in `desktop/src/scene3d/standalone.cpp`, where availability is decided from a container's emptiness rather than from whether the scene has drawable geometry, and where three notes describe fallbacks that do not exist. The second is `desktop/src/live/budget.cpp`'s `mode_visualizations` and its pane mirror `mode_viz_panes` in `shell.cpp`, a pair of hand-maintained tables that have drifted from what each serve mode actually records. The final task adds a record path to `LiveSession`'s spawn argv so the session-level sink `asmspy --serve --record=<f>` — which already produces one loadable merged recording — becomes reachable from the GUI.

**Tech Stack:** C11 (`cli/`), C++17 + Dear ImGui 1.91.9b-docking + nlohmann/json (`desktop/`), GNU make, Docker lanes.

---

## The measurement this plan rests on

Every row below was verified against the working tree at `b3d23d01` by reading the cited lines, and each was then independently re-checked by an adversarial reviewer instructed to refute it. Two candidate defects were refuted during that pass and are recorded in Self-review as deliberate non-changes, so a later reader does not "fix" them back.

| # | Defect | Proof | Consequence |
|---|---|---|---|
| B1 | `shell_kind_availability` gates `Invocation` on `sv.invocation.slabs.empty()` only — never on cells | `desktop/src/ui/shell.cpp:957-960`; cells are built by iterating `s.blocks` at `desktop/src/scene3d/standalone.cpp:277-306`, and `s.blocks` is the union of per-invocation `blocks`, populated only by a `coverage` event (`desktop/src/views/region.cpp:120-129`) | A recording whose first invocation has no `coverage` line yields slabs with **zero cells**: the scene reports AVAILABLE with no reason text and draws a slab wireframe with nothing in it |
| B2 | Nothing returns the pane to `Plane` when the SELECTED kind becomes unavailable | The only reaction is `shell.cpp:1324` setting `hud.kind_unavailable`; `hud.cpp:504-509` disables *entering* an unavailable kind and there is no code anywhere that *leaves* one | Select `divergence worldline`, then detach B. The pane keeps drawing the divergence substrate as an empty scene — the exact fabrication its own comment promises never happens |
| B3 | A live-growth batch resets the kind and destroys the per-kind cameras while preserving the abandoned one | `s.scenes[i] = SceneView{}` wipes `kind`, `kind_cam`, `kind_cam_inited`, `prism_reg`; `hud.kind` is carried but `draw_scene_overview` overwrites it with `sv.hud.kind = sv.kind` next frame | Live `tree` capture, pick `module excursion ribbon`, orbit it — the next event batch snaps to the address plane wearing the ribbon's camera |
| B4 | `build_lane_prism` does not filter on `v.write` | `desktop/src/scene3d/standalone.cpp:633-635` filters `!v.wide \|\| v.space != "reg" \|\| v.reg != reg_id` and nothing else | Z is documented "stacked writes, oldest nearest" (`scene_kind.h:150`) but stacks **reads** too; `paddd xmm0, xmm1` draws two Z-levels for one step, and `lane_prism_registers` offers `xmm1` — never written — as a selectable register |
| B5 | The single-thread ribbon note describes a fallback with no implementation | `standalone.cpp:486-488` sets "showing the flat icicle timeline instead"; `single_thread` has zero consumers outside the model's own dump and its unit test | The user is told a 2D chart is being shown instead, and is looking at the tilted one-lane 3D chart the note says was avoided |
| B6 | The HUD's plane-coordinate navigation is drawn and applied on every substrate | `s.kind` is read nowhere in `hud.cpp` except the selector at `hud.cpp:496-522`; `camera_here_text` is called unconditionally at `hud.cpp:825` | Select `SIMD lane prism` and the HUD states "you are here: 0x… (region)" — a specific address in a specific region — for a scene whose axes are byte index / byte magnitude / write ordinal |
| B7 | `mode_visualizations(Tree)` omits the 3D overview, and `mode_viz_panes(Tree)` actively closes the pane hosting it | `desktop/src/live/budget.cpp:118-119` returns `{"Observer (call tree)"}`; `shell.cpp:3383-3402` returns `{kPaneObserver}` and `shell_apply_live_panes` closes unlisted panes | `cli/asmspy.c:4031-4040` arms a codeimage over the exe's text **specifically so the 3D pane can host the module-excursion ribbon** — and the GUI then closes that pane |
| B8 | `mode_visualizations(Sample)` advertises "3D overview" a sample capture can never fill | The serve host arms codeimage for `SM_TREE`/`SM_TRACE`/`SM_DATAFLOW`/`SM_AUTO` only; `SM_SAMPLE` emits `survey` events and nothing else. Presence is `!regions_from_codeimage(r).empty()` (`view_presence.cpp:120`) | The tooltip and the standing "shows:" line promise a tab that never appears |
| B9 | `mode_visualizations(Stream)` advertises "Slice" a stream capture can never fill | The stream sink records the engine's formatted TEXT as a `stream` event and emits no `df_step`, so `Streams::df.nsteps` stays 0 and the Slice is marked absent | Same shape as B8; the function's own header comment says "the Slice needs df_step" |
| B10 | `mode_visualizations(Log)` and `(Trace)` both advertise "Timeline", which neither can fill; `Trace` under-advertises the Canvas | The Timeline is the operand timeline, one row per `df_step` (`timeline.cpp:133-183` over `Streams::df`); `log` emits only `syscall`, `trace` only `trace`/`coverage`. `shell.cpp:3390-3393` opens `kPaneTimeline` for both | Two more promised-but-empty panes, opened rather than merely named |
| B11 | The Scene3D absent-reason promises "a live maps snapshot" with no desktop path behind it | `view_presence.cpp:122-123` and the identical placard at `shell.cpp:1649-1656`; the only `Region` producers are `regions_from_codeimage` (reads `by_kind["codeimage"]`) and `observed_data_spans`, which the gate does not consult. No `regions_from_maps` exists | An operator whose recording lacks codeimage goes looking for a maps-snapshot capture option that was never built |
| B12 | The generated keymap line conflates the two camera buttons the tree documents as distinct | `hud.cpp:365` emits "R: reset view (the landmark)", but `CamKey::Reset` is `Camera::reset()` — the literal default pose, which `hud.cpp:800-808` names "default view" | 48 T4 states "two buttons, two meanings, neither silently repurposed into the other"; the keymap repurposes one into the other for keyboard-only analysts |
| F1 | The desktop cannot produce a merged multi-mode recording, though it can consume one | `LiveSession` spawns a fixed argv that never carries `--record` (`session.cpp:81-87`); `Spec` has no record-path field; Save serializes exactly one `Recording` — `growing()` else `done.back()` (`inspect_door.cpp:865-871`) | `asmspy --serve --record=<f>` already lands `call` + `trace`/`coverage` + wide `df_step.ops` in ONE loadable file (`cli/test_serve_record.c`, `build/test-serve-record.asmtrace`), and `Workspace::open` loads it — but only if some other tool produced it |

**Why this is not a test-only shim.** B1, B2 and B3 each let the pane assert something the recording does not support: a populated scene, a live substrate, a restored view. B4 mislabels read operands as writes on a documented axis. B5, B11 and B12 are text that describes code that does not exist. B7–B10 are a table that has drifted from the producer it describes. None of these is caught by tightening an assertion; each needs the behaviour corrected at its source.

---

## Global Constraints

- **Shared tree.** Many agents work this repo concurrently and push to `main` live. Commit only your own paths using a private `GIT_INDEX_FILE` (`git write-tree` / `commit-tree` / CAS `update-ref`), never `git add -A`. Afterwards the SHARED index shows your file as a staged deletion — repair with a path-scoped `git reset -- <path>`. **Push after every commit.**
- **No gtest.** Every desktop test is a standalone `main()` over a hand-rolled `check(what, cond, why)` where the third argument is the failure explanation, printed only on failure. Copy the banner + harness from `desktop/test/test_budget.cpp:12-27` exactly. `cli/` tests are C `main()`s in the same spirit.
- **How to run things.** One desktop test: `make build/desktop_test_<name> && ./build/desktop_test_<name>`. Whole desktop suite: `make desktop-test`. Containerised desktop lane (what CI runs): `make docker-desktop`. CLI lane: `make docker-cli`. **Never `make X >/dev/null 2>&1`** — it hides compile errors and leaves a stale binary "passing".
- **Registering a new test.** A new desktop test binary needs its link rule plus an entry in `DESKTOP_TESTS` (`mk/desktop.mk:1202`). **A test not in `DESKTOP_TESTS` never runs in CI — it is not a test.** Prefer extending an existing test binary where one already covers the unit (`test_budget.cpp` for `budget.cpp`, `test_standalone.cpp` for the scene builders).
- **`cli/*.c` is inside the CI-gated `make fmt-check`.** `desktop/` is not (it has its own ungated `desktop-fmt-check`). Run `make fmt-check` after touching `cli/`. Do not let `clang-format -i` re-sort `shell.cpp`'s addon include fences — that produces an `imgui_internal.h #error`.
- **`shell_kind_availability` is file-local static** (`shell.cpp:951-952`) with exactly one call site (`shell.cpp:1324`). It cannot be reached from a test as it stands. Task 2 must either move the pure decision into a testable unit or accept that only the builder half is covered — do not paper over this by asserting on the builders and claiming the gate is tested.
- **Availability is not the same as a note.** An unavailable kind is offered DISABLED and cannot be selected (`hud.cpp:504-509`). A kind that is available but degenerate must stay SELECTABLE and carry a note. Moving a note into `kind_unavailable` is a regression — see Self-review's refuted candidates.
- **The 3D pane is gated on codeimage regions, upstream of every scene.** `draw_scene_overview` early-returns at `shell.cpp:1649` when `!sv.has_regions`, before any substrate chrome. Every fix in Tasks 2–7 is only observable for a recording that carries `codeimage`. Use a fixture that does.
- **Fixtures.** Fixtures generated from the real CLI get a Step 1 showing the exact generating command AND a shape-verification command run BEFORE committing; the fixture is then frozen. `desktop/test/fixtures/*.asmtrace` are ungated; the `build/asmtrace-rec/` golden corpus is byte-gated by `make asmtrace-golden-check`, which stops at the FIRST mismatch.
- **Pre-existing failures.** `desktop_test_shell`'s attach/no-host checks FAIL on a host with nothing to attach to and are PRE-EXISTING. They stop a naive `for` loop, so verify touched tests INDIVIDUALLY rather than trusting a suite exit code.

---

### Task 1: The advertise-vs-fill table — `desktop/src/live/budget.cpp`

**Files:**
- Modify: `desktop/src/live/budget.cpp` (`mode_visualizations`, `:99-132`)
- Modify: `desktop/src/ui/shell.cpp` (`mode_viz_panes`, `:3383-3402`)
- Test: `desktop/test/test_budget.cpp` (extend; it already walks all modes at `:295-340`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: a corrected `mode_visualizations` mapping that Task 8's manual verification reads back from the patch-bay tooltip. No signature change:

```cpp
std::vector<const char *> mode_visualizations(LiveMode m);
```

**The table is the part that can be wrong.** `test_budget.cpp`'s own banner says exactly this about the concurrency budget, and it applies with equal force here: `mode_visualizations` is a hand-maintained claim about what another translation unit records, with no mechanical link to the producer. Four of its ten rows have drifted. The fix is to correct the rows AND to add a test that pins each row against the event kinds that mode actually emits, so the next drift fails here rather than in a tooltip.

- [ ] **Step 1: Write the failing test**

Append to `desktop/test/test_budget.cpp`, before `return failures != 0;`:

```cpp
    // 3D-scene-availability plan, Task 1: the advertise-vs-fill contract.
    //
    // mode_visualizations is a CLAIM about what another TU records. Nothing
    // links it to the producer, so it drifts silently and a tooltip promises a
    // tab that never appears. These checks pin each row against the event kinds
    // the mode actually emits (cli/asmspy.c's serve_* sinks), naming the
    // producer site in the failure text so a future edit knows where to look.
    {
        auto viz_has = [](LiveMode m, const char *want) {
            for (const char *v : mode_visualizations(m))
                if (std::string(v) == want)
                    return true;
            return false;
        };

        // A mode fills the 3D overview iff the serve host arms a codeimage for
        // it — ViewId::Scene3D presence is !regions_from_codeimage(r).empty()
        // (ui/view_presence.cpp:120). asmspy.c arms it for tree/trace/dataflow/
        // auto only (asmspy.c:4039, :4062, :4067, :4155).
        const struct { const char *name; bool codeimage; } kCodeimage[] = {
            {"tree", true},   {"trace", true},  {"dataflow", true},
            {"auto", true},   {"log", false},   {"stream", false},
            {"graph", false}, {"procs", false}, {"sample", false},
            {"watch", false},
        };
        for (const auto &row : kCodeimage) {
            LiveMode m;
            check("viz/3d/mode-name", mode_from_name(row.name, &m),
                  std::string("unknown mode name in this test: ") + row.name);
            if (!mode_from_name(row.name, &m))
                continue;
            check("viz/3d/" + std::string(row.name),
                  viz_has(m, "3D overview") == row.codeimage,
                  std::string(row.name) +
                      (row.codeimage
                           ? " arms a codeimage (asmspy.c serve_codeimage_arm)"
                             " so it fills the 3D overview, but its row does not"
                             " say so"
                           : " arms no codeimage, so the 3D overview tab can"
                             " never appear — its row must not promise it"));
        }

        // The Slice and the Timeline are both built from df_step (doc/streams.cpp
        // decodes Streams::df from df_step/df_edge only), so only the two
        // dataflow-bearing modes may name them.
        const char *kDfOnly[] = {"Slice", "Timeline"};
        const struct { const char *name; bool df; } kDataflow[] = {
            {"dataflow", true}, {"auto", true},   {"log", false},
            {"stream", false},  {"trace", false}, {"tree", false},
            {"graph", false},   {"procs", false}, {"sample", false},
            {"watch", false},
        };
        for (const auto &row : kDataflow) {
            LiveMode m;
            if (!mode_from_name(row.name, &m))
                continue;
            for (const char *v : kDfOnly)
                check("viz/df/" + std::string(row.name) + "/" + v,
                      viz_has(m, v) == row.df,
                      std::string(row.name) + " emits " +
                          (row.df ? "" : "no ") +
                          "df_step, so it must " + (row.df ? "" : "not ") +
                          "advertise \"" + v + "\"");
        }
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_budget && ./build/desktop_test_budget`

Expected: exactly five FAIL lines, naming the five drifted rows —

```
FAIL viz/3d/tree: tree arms a codeimage (asmspy.c serve_codeimage_arm) so it fills the 3D overview, but its row does not say so
FAIL viz/3d/sample: sample arms no codeimage, so the 3D overview tab can never appear — its row must not promise it
FAIL viz/df/log/Timeline: log emits no df_step, so it must not advertise "Timeline"
FAIL viz/df/stream/Slice: stream emits no df_step, so it must not advertise "Slice"
FAIL viz/df/trace/Timeline: trace emits no df_step, so it must not advertise "Timeline"
```

If a sixth line appears for `stream`/`Timeline`, that is the same defect family and the row must be corrected with the rest — do not delete the check to make it green.

- [ ] **Step 3: Correct the table**

Replace `mode_visualizations`'s five drifted arms in `desktop/src/live/budget.cpp:105-130`:

```cpp
    case LiveMode::Trace:
        // `trace`/`coverage` fill the Canvas and — via the serve host's code
        // image — the 3D overview's address plane and invocation stack. They
        // carry no df_step, so the operand Timeline is NOT among them.
        return {"Canvas", "3D overview", "Observer (codeimage)"};
    case LiveMode::Log:
        return {"Observer (syscalls)"};
    case LiveMode::Stream:
        return {"Timeline (stream text)"};
    case LiveMode::Tree:
        // A tree session has no region of its own, but the serve host arms a
        // code image over the executable's text precisely so this pane can host
        // the module-excursion ribbon (cli/asmspy.c:4031-4040). Saying only
        // "Observer" here is what closed that pane.
        return {"Observer (call tree)", "3D overview"};
    case LiveMode::Sample:
        // Out-of-band statistical: hot edges only. SM_SAMPLE arms no code
        // image, so the 3D overview tab never appears for a sample capture.
        return {"Observer (hot edges)"};
```

`Stream`'s row keeps a Timeline entry only if a stream-text timeline pane genuinely exists; verify with `grep -rn "kPaneTimeline" desktop/src/ui/shell.cpp` and, if the Timeline is operand-only, return `{}` plus a `mode_jack_reason` sentence instead of inventing a facet name.

- [ ] **Step 4: Correct the pane mirror**

`mode_viz_panes` (`shell.cpp:3383-3402`) is a second copy of the same claim, and it does more damage: `shell_apply_live_panes` CLOSES panes the list omits. Bring each arm into agreement with Step 3 — `Tree` must gain `kPaneRecording` (the pane hosting the 3D tab), and `Log`/`Trace` must lose `kPaneTimeline`.

Add a comment at the head of `mode_viz_panes` pointing at `mode_visualizations` as the sibling table, so the next editor updates both.

- [ ] **Step 5: Prove the gate bites**

Revert the `Tree` arm to `{"Observer (call tree)"}`, rerun, and confirm `viz/3d/tree` fires with its full explanation. Restore. *A gate nobody has watched fail is a gate nobody knows works.*

- [ ] **Step 6: Verify**

```bash
make build/desktop_test_budget && ./build/desktop_test_budget
make desktop-test
```

Expected: `desktop_test_budget` prints no FAIL lines and exits 0. `desktop-test` shows no NEW failures (see the pre-existing `test_shell` note in Global Constraints).

- [ ] **Step 7: Commit and push**

```bash
cd /home/will/source/asm-test
export GIT_INDEX_FILE=$(mktemp /tmp/asmtest-idx.XXXXXX)
git read-tree HEAD
git update-index --add desktop/src/live/budget.cpp desktop/src/ui/shell.cpp desktop/test/test_budget.cpp
TREE=$(git write-tree)
COMMIT=$(git commit-tree "$TREE" -p HEAD -m "desktop: the patch bay stops promising views its captures cannot fill

mode_visualizations is a hand-maintained claim about what another TU
records, with nothing linking it to the producer. Four rows had drifted:
tree omitted the 3D overview the serve host arms a code image FOR, and
sample/log/trace promised panes their events can never fill. mode_viz_panes
carried the same drift and acted on it, closing the pane that hosts the 3D
tab whenever a tree capture started.

The new checks pin each row against the event kinds the mode actually
emits, so the next drift fails in test_budget rather than in a tooltip.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>")
git update-ref --create-reflog refs/heads/main "$COMMIT" HEAD
unset GIT_INDEX_FILE
git reset -- desktop/src/live/budget.cpp desktop/src/ui/shell.cpp desktop/test/test_budget.cpp
git push origin main
```

---

### Task 2: Availability means drawable — `shell_kind_availability`

**Files:**
- Modify: `desktop/src/scene3d/standalone.h` (add `InvocationScene::drawable()`)
- Modify: `desktop/src/scene3d/standalone.cpp` (`build_invocation_scene`, `:257-320`)
- Modify: `desktop/src/ui/shell.cpp` (`shell_kind_availability`, `:951-970`)
- Test: `desktop/test/test_standalone.cpp` (extend)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: a predicate the gate calls instead of `slabs.empty()`:

```cpp
// True when this scene has geometry a user can actually see. A slab with no
// cells is a wireframe with nothing in it: the union block set is empty because
// no `coverage` event described one, so the scene is "present" and blank.
bool drawable() const { return !slabs.empty() && !blocks.empty(); }
```

**The gate asks the wrong question.** `slabs.empty()` answers "did the builder produce a container", not "is there anything to draw". `blocks` is the slab's shared X/Z; when it is empty every slab gets zero cells (`standalone.cpp:277-306` iterates `s.blocks`), so the scene is offered as available, with no reason text, and renders a frame ring around nothing. The fix is to make the predicate say what the gate means, and to give the builder a note explaining the specific gap — a `trace` stream with no `coverage` footer — so the disabled entry teaches rather than merely refusing.

- [ ] **Step 1: Write the failing test**

Append to `desktop/test/test_standalone.cpp`:

```cpp
    // 3D-scene-availability plan, Task 2: a slab with no cells is not a scene.
    //
    // RegionInvocation::blocks is populated ONLY by a `coverage` event
    // (views/region.cpp:120-129). A recording whose trace events are not closed
    // by a coverage footer yields a trailing OPEN invocation with no blocks, so
    // the union set is empty and every slab gets zero cells. The gate used to
    // ask slabs.empty(), which is false here — so the scene was offered as
    // available and drew a wireframe around nothing.
    {
        RegionView rv;
        RegionInvocation inv;
        inv.number = 1;
        inv.closed = false;              // no coverage footer closed it
        inv.insns = {0x0, 0x4, 0x8};     // instructions WERE recorded
        // inv.blocks deliberately left empty — that is the whole case
        rv.invocations.push_back(inv);

        const InvocationScene s = build_invocation_scene(rv, nullptr);
        check("inv/blockless/has-a-slab", s.slabs.size() == 1,
              "the builder should still produce the slab — dropping it would "
              "hide the last thing the target did");
        check("inv/blockless/no-cells", s.slabs[0].cells.empty(),
              "with no union blocks there is nothing to place in the slab");
        check("inv/blockless/not-drawable", !s.drawable(),
              "a slab with zero cells has no geometry; drawable() is what the "
              "availability gate must ask instead of slabs.empty()");
        check("inv/blockless/says-why", !s.note.empty(),
              "an unavailable kind must state WHY (D7) — a blank refusal "
              "teaches nothing");
        check("inv/blockless/names-coverage",
              s.note.find("coverage") != std::string::npos,
              "the note must name the missing coverage footer, the actual gap: "
              "got \"" + s.note + "\"");

        // The positive control: one closed invocation WITH blocks is drawable.
        RegionView ok;
        RegionInvocation good;
        good.number = 1;
        good.closed = true;
        good.insns = {0x0, 0x4};
        good.blocks = {0x0};
        ok.invocations.push_back(good);
        const InvocationScene g = build_invocation_scene(ok, nullptr);
        check("inv/blocked/drawable", g.drawable(),
              "a closed invocation with a block set must stay available");
        check("inv/blocked/no-note", g.note.empty(),
              "an available kind carries no refusal text");
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_standalone && ./build/desktop_test_standalone`

Expected: a compile error first — `'const struct InvocationScene' has no member named 'drawable'` — because the predicate does not exist yet. Add the declaration only (returning `!slabs.empty()`), rebuild, and expect three FAILs:

```
FAIL inv/blockless/not-drawable: a slab with zero cells has no geometry; ...
FAIL inv/blockless/says-why: an unavailable kind must state WHY (D7) — ...
FAIL inv/blockless/names-coverage: the note must name the missing coverage footer, the actual gap: got ""
```

- [ ] **Step 3: Implement**

In `standalone.h`, give `InvocationScene` the `drawable()` predicate from **Interfaces** above.

In `build_invocation_scene` (`standalone.cpp`), after the union block set is assembled and before the slab loop, set the note when the set is empty:

```cpp
    if (all.empty())
        // Instructions were recorded but no `coverage` event described a block
        // set, so the slab has no X/Z to place anything on. State the gap
        // precisely: this is a capture stopped before its first footer, not a
        // recording with no region capture at all.
        s.note = "no block coverage in this recording — the invocation stack "
                 "places cells on the block set a `coverage` event carries, "
                 "and this capture's instructions were never closed by one";
```

Keep building the slabs regardless — the existing "dropping them would hide the last thing the target did" behaviour is correct and the test pins it.

In `shell_kind_availability` (`shell.cpp:957-960`), ask the new question:

```cpp
    if (!sv.invocation.drawable())
        why[scene_kind_index(scene3d::SceneKind::Invocation)] =
            sv.invocation.note.empty() ? "no region capture in this recording"
                                       : sv.invocation.note;
```

- [ ] **Step 4: Verify**

```bash
make build/desktop_test_standalone && ./build/desktop_test_standalone
make build/desktop_test_shell && ./build/desktop_test_shell
```

Expected: `desktop_test_standalone` prints no FAIL lines. `desktop_test_shell` shows only its pre-existing attach/no-host failures.

Do **not** make this green by having the builder return an empty `slabs` vector — that would restore `slabs.empty()` as the gate and silently drop the trailing invocation the region view deliberately keeps.

- [ ] **Step 5: Commit and push**

Use the Task 1 commit block, substituting the paths (`desktop/src/scene3d/standalone.h`, `desktop/src/scene3d/standalone.cpp`, `desktop/src/ui/shell.cpp`, `desktop/test/test_standalone.cpp`) and this message:

```
scene3d: the invocation stack is available only when it has cells to draw

The gate asked slabs.empty(), which answers "did the builder produce a
container" rather than "is there anything to see". A capture whose trace
events are never closed by a coverage footer yields a slab with an empty
union block set, so every cell loop runs zero times: the scene reported
available with no reason text and drew a frame ring around nothing.

drawable() asks the question the gate meant, and the builder now names the
missing coverage footer so the disabled entry teaches rather than refuses.
```

---

### Task 3: Nothing leaves an unavailable substrate

**Files:**
- Modify: `desktop/src/ui/shell.cpp` (`draw_scene_overview`, immediately after `:1324`)
- Test: `desktop/test/test_shell.cpp` (extend)

**Interfaces:**
- Consumes: Task 2's `kind_unavailable` semantics (a non-empty string means "cannot be shown").
- Produces: the invariant that `sv.kind`'s availability slot is always empty after `draw_scene_overview`'s sync block.

**Entering is guarded; staying is not.** `hud.cpp:504-509` wraps each unavailable entry in `BeginDisabled`, so you cannot *select* a kind the recording cannot show. Nothing checks the kind you are *already on*. Detach the B recording while the divergence substrate is showing and the pane keeps drawing it — `shell_standalone_chrome` prints "0 rib(s)" and the GL path draws an empty scene, which is precisely the fabrication `build_divergence_scene` refuses to perform by returning a refusal card instead of an empty scene.

- [ ] **Step 1: Write the failing test**

Add to `desktop/test/test_shell.cpp` a check over the pure decision. If the eviction is written as a free function it can be tested directly; extract it as:

```cpp
// Which kind should the pane be showing, given what it is on and what this
// recording can show? Pure so it is assertable without an ImGui frame.
scene3d::SceneKind shell_evict_unavailable_kind(
    scene3d::SceneKind cur, const std::vector<std::string> &why);
```

```cpp
    // 3D-scene-availability plan, Task 3: a kind that becomes unavailable must
    // be LEFT, not merely un-enterable. Entering is guarded by BeginDisabled;
    // staying was guarded by nothing, so detaching B while the divergence
    // substrate was showing kept drawing it as an empty scene.
    {
        std::vector<std::string> why(scene3d::all_scene_kinds().size());
        const size_t div =
            scene_kind_index(scene3d::SceneKind::Divergence);

        why[div] = "";
        check("evict/available-stays",
              shell_evict_unavailable_kind(scene3d::SceneKind::Divergence,
                                           why) ==
                  scene3d::SceneKind::Divergence,
              "an available kind must not be evicted");

        why[div] = "needs a second recording (press d to attach one)";
        check("evict/unavailable-falls-back",
              shell_evict_unavailable_kind(scene3d::SceneKind::Divergence,
                                           why) == scene3d::SceneKind::Plane,
              "a kind that became unavailable must fall back to Plane rather "
              "than keep drawing an empty substrate");

        check("evict/plane-is-terminal",
              shell_evict_unavailable_kind(scene3d::SceneKind::Plane, why) ==
                  scene3d::SceneKind::Plane,
              "Plane has no availability gate and is the fallback; it must "
              "never be evicted from");
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_shell && ./build/desktop_test_shell`

Expected: a link error — `undefined reference to shell_evict_unavailable_kind` — because the function does not exist. That is the failing state; do not stub it to return `cur`.

- [ ] **Step 3: Implement**

Add the free function beside `shell_kind_availability` in `shell.cpp`, and call it in `draw_scene_overview` right after `sv.hud.kind_unavailable` is assigned at `:1324` and BEFORE the `req_kind_change` block, so a kind evicted this frame cannot also be re-entered this frame. Restore that kind's camera exactly as the `req_kind_change` path does, so the fallback is not a second way to lose the plane orbit.

Surface the eviction: set a one-line HUD notice naming the kind that was left and why, rather than snapping silently. A silent snap-back is indistinguishable from a crash to the person watching.

- [ ] **Step 4: Verify**

Run: `make build/desktop_test_shell && ./build/desktop_test_shell`

Expected: the three `evict/*` checks pass; only the pre-existing attach/no-host failures remain.

- [ ] **Step 5: Commit and push**

Message subject: `desktop: a substrate that becomes unavailable is left, not just un-enterable`.

---

### Task 4: The lane prism stacks writes, not reads — `build_lane_prism`

**Files:**
- Modify: `desktop/src/scene3d/standalone.cpp` (`lane_prism_registers` `:620-626`, `build_lane_prism` `:628-673`)
- Test: `desktop/test/test_standalone.cpp` (extend)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: no signature change; the filter gains `v.write`.

**The axis label is a claim the filter does not keep.** `scene_kind.h:150` states Z is "stacked writes, oldest nearest" and "Z counts recorded writes, not elapsed time". The x86-64 producer captures wide XMM values for READ operands too — `df_on_code` calls `df_capture_reg_value` on every register read — so one `paddd xmm0, xmm1` yields three wide records: read `xmm0`, read `xmm1`, write `xmm0`. Today the prism draws two Z-levels for that single step and offers `xmm1`, a register never written, in its register selector.

- [ ] **Step 1: Write the failing test**

```cpp
    // 3D-scene-availability plan, Task 4: Z counts WRITES (scene_kind.h:150).
    // The producer captures wide values for read operands too, so an unfiltered
    // walk stacks the pre-state read under the post-state write for one step,
    // and offers registers that were only ever read as selectable.
    {
        DataflowStream df;
        auto wide_rec = [](uint32_t step, uint32_t reg, bool write) {
            ValRec v;
            v.step = step;
            v.space = "reg";
            v.reg = reg;
            v.size = 16;
            v.wide = true;
            v.write = write;
            v.value_valid = true;
            v.bytes.assign(16, 0u);
            return v;
        };
        // one `paddd xmm0, xmm1`: read xmm0, read xmm1, write xmm0
        df.recs.push_back(wide_rec(0, 100, false));
        df.recs.push_back(wide_rec(0, 101, false));
        df.recs.push_back(wide_rec(0, 100, true));

        const std::vector<uint32_t> regs = lane_prism_registers(df);
        check("prism/regs/only-written", regs.size() == 1 && regs[0] == 100,
              "a register that was only READ is not a prism subject — Z counts "
              "writes; got " + std::to_string(regs.size()) + " register(s)");

        const LanePrismScene s = build_lane_prism(df, 100, "x86_64");
        check("prism/writes/one-per-write", s.writes.size() == 1,
              "one step wrote xmm0 once, so there is ONE Z level; stacking the "
              "read under it draws two levels for one write");
        check("prism/writes/is-the-write", s.writes.empty() || s.writes[0].step == 0,
              "the surviving record must be the write, not the read");
    }
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make build/desktop_test_standalone && ./build/desktop_test_standalone`

Expected:

```
FAIL prism/regs/only-written: a register that was only READ is not a prism subject — Z counts writes; got 2 register(s)
FAIL prism/writes/one-per-write: one step wrote xmm0 once, so there is ONE Z level; stacking the read under it draws two levels for one write
```

- [ ] **Step 3: Implement**

Add `|| !v.write` to both filters:

```cpp
std::vector<uint32_t> lane_prism_registers(const DataflowStream &df) {
    std::set<uint32_t> regs;
    for (const ValRec &v : df.recs)
        // Z counts recorded WRITES (scene_kind.h). The producer captures wide
        // values for read operands too, so a register that was only ever read
        // is not a subject for this scene.
        if (v.wide && v.write && v.space == "reg")
            regs.insert(v.reg);
    return std::vector<uint32_t>(regs.begin(), regs.end());
}
```

and in `build_lane_prism`:

```cpp
        if (!v.wide || !v.write || v.space != "reg" || v.reg != reg_id)
            continue;
```

- [ ] **Step 4: Check the golden corpus did not move**

Run: `make asmtrace-golden-check`

Expected: `OK`. This task touches no producer, so the corpus must be byte-identical; a mismatch means something else in the tree moved and must be resolved before committing.

- [ ] **Step 5: Commit and push**

Message subject: `scene3d: the lane prism stacks writes only, as its axis says`.

---

### Task 5: Notes and reason strings describe code that exists

**Files:**
- Modify: `desktop/src/scene3d/standalone.cpp` (`build_module_ribbon`, `:486-488`)
- Modify: `desktop/src/ui/view_presence.cpp` (`:121-124`)
- Modify: `desktop/src/ui/shell.cpp` (`:1649-1656`, the in-pane placard)
- Modify: `desktop/src/scene3d/hud.cpp` (`:365`, the generated keymap line)
- Test: `desktop/test/test_standalone.cpp`, `desktop/test/test_view_presence.cpp` (extend)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: no API change — three strings and one keymap label.

**Three sentences promise behaviour the tree does not implement.** The single-thread ribbon says "showing the flat icicle timeline instead" while `single_thread` has zero consumers, so nothing is shown instead. The Scene3D absent-reason offers "a live maps snapshot" as an alternative region source; no `regions_from_maps` exists, and the only maps parsing in the tree is producer-side (`serve_exe_text_span`, which emits a `codeimage` — so the honest phrasing points at the capture mode, not at a desktop fallback). The keymap says "R: reset view (the landmark)" while `CamKey::Reset` is `Camera::reset()`, the pose `hud.cpp:800-808` calls "default view" — the two the tree documents as "two buttons, two meanings, neither silently repurposed into the other".

**This task does NOT change the module-ribbon availability gate.** A one-lane ribbon must stay selectable; see Self-review.

- [ ] **Step 1: Write the failing test**

```cpp
    // Task 5: a note may not describe a fallback nothing implements.
    {
        TreeView tv;
        TreeRow r;
        r.tid = 42; r.depth = 0; r.addr = 0x1000; r.seq = 1;
        r.name = "main"; r.module = "a.out";
        tv.rows.push_back(r);
        const ModuleRibbonScene s = build_module_ribbon(tv);
        check("ribbon/single/flagged", s.single_thread,
              "one lane must still be flagged");
        check("ribbon/single/selectable", !s.lanes.empty(),
              "a one-lane ribbon stays SELECTABLE — moving this into "
              "kind_unavailable would disable it, which 59 T4 forbids");
        check("ribbon/single/no-phantom-fallback",
              s.note.find("icicle") == std::string::npos,
              "the note claimed 'showing the flat icicle timeline instead', "
              "but single_thread has no consumer and nothing is shown "
              "instead: got \"" + s.note + "\"");
        check("ribbon/single/says-what-it-is", !s.note.empty(),
              "the degenerate shape must still be named");
    }
```

And in `test_view_presence.cpp`:

```cpp
    check("scene3d/reason/no-phantom-maps",
          reason.find("maps snapshot") == std::string::npos,
          "the absent-reason offered 'a live maps snapshot' as an alternative "
          "region source; no regions_from_maps exists, so it sends the reader "
          "looking for a capture option that was never built");
```

- [ ] **Step 2: Run it to verify it fails**

Expected: `ribbon/single/no-phantom-fallback` and `scene3d/reason/no-phantom-maps` both FAIL, quoting the current strings.

- [ ] **Step 3: Implement**

Ribbon note — describe the shape, not an imaginary substitute:

```cpp
        s.note = "single-threaded recording: one lane, so the depth axis "
                 "carries the whole finding and the thread axis carries "
                 "nothing — a 2D icicle would show this better";
```

Scene3D reason (both copies — `view_presence.cpp` and the `shell.cpp` placard must stay identical, which is why the test pins the shared substring):

```
"no address-space regions in this recording — the 3D overview places its "
"plane from `codeimage` events, which the serve host records for the "
"tree / trace / dataflow / auto modes"
```

Keymap line (`hud.cpp:365`) — name the pose the key actually applies:

```cpp
                lines.push_back("R: default view (the plane's default pose)");
```

- [ ] **Step 4: Verify**

```bash
make build/desktop_test_standalone && ./build/desktop_test_standalone
make build/desktop_test_view_presence && ./build/desktop_test_view_presence
grep -rn "maps snapshot" desktop/src/ | wc -l   # expect 0
```

- [ ] **Step 5: Commit and push**

Message subject: `desktop: three notes stop describing behaviour the tree does not implement`.

---

### Task 6: A live batch keeps the substrate and its cameras

**Files:**
- Modify: `desktop/src/ui/shell.cpp` (the live-growth `s.scenes[i] = SceneView{}` reset and its preserve list)
- Test: `desktop/test/test_shell.cpp` (extend)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `kind`, `kind_cam`, `kind_cam_inited` and `prism_reg` added to the preserve set.

**The preserve list carries the camera but not what the camera was for.** The reset wipes `kind`, `kind_cam`, `kind_cam_inited` and `prism_reg` while preserving `cam` — so the pane returns to the address plane wearing the abandoned substrate's orbit. `hud.kind` is preserved but cannot help: `draw_scene_overview` overwrites it from `sv.kind` on the next frame.

- [ ] **Step 1: Write the failing test** — assert that after a simulated growth reset the selected kind, the per-kind camera vectors and `prism_reg` all survive, and that `cam` matches the camera stored for the surviving kind rather than for a different one.
- [ ] **Step 2: Run it to verify it fails** — expect the kind to come back as `Plane` and `prism_reg` as 0.
- [ ] **Step 3: Implement** — extend the preserve list; carry `kind_cam`/`kind_cam_inited` by move so the vectors are not reallocated per batch.
- [ ] **Step 4: Verify** — `make build/desktop_test_shell && ./build/desktop_test_shell`, then a manual live check: start a `tree` capture, pick `module excursion ribbon`, orbit, and confirm the next batch neither snaps to the plane nor moves the camera.
- [ ] **Step 5: Commit and push** — subject: `desktop: a live growth batch keeps the chosen substrate and its cameras`.

---

### Task 7: The HUD is substrate-aware

**Files:**
- Modify: `desktop/src/scene3d/hud.h` (the plane-nav block's guard)
- Modify: `desktop/src/scene3d/hud.cpp` (`draw_scene_hud`, the "you are here" line at `:825` and the "go to" row)
- Test: `desktop/test/test_goto.cpp` or `desktop/test/test_hud.cpp` (extend whichever already covers `camera_here_text`)

**Interfaces:**
- Consumes: Task 3's eviction invariant (the pane's kind is always one this recording can show).
- Produces: the plane-coordinate affordances draw only when `s.kind == SceneKind::Plane`.

**An address readout on a scene with no address axis is the exact fabrication this family exists to prevent.** `camera_here_text` unprojects the camera target through `terr.proj` unconditionally, so selecting the SIMD lane prism — whose default camera target is `(0.5, 0.5)` — prints "you are here: 0x… (region)" for axes that are byte index, byte magnitude and write ordinal. The "go to" row is worse than cosmetic: it teleports the camera by plane coordinates on a substrate whose extents were never chosen for them.

- [ ] **Step 1: Write the failing test** — assert `camera_here_text` is not emitted for a non-Plane kind, and that the goto row's intent is not applied. Pin the positive control: on `Plane` both still work, unchanged.
- [ ] **Step 2: Run it to verify it fails** — expect the address string to be produced for `LanePrism`.
- [ ] **Step 3: Implement** — guard both affordances on `s.kind == SceneKind::Plane`. For a non-plane substrate, draw that kind's own axis lines (already available from `scene_axis_lines`) where the plane readout used to sit, so the space is informative rather than blank.
- [ ] **Step 4: Verify** — the new test, plus `make desktop-ui-test` if the goto row is covered there.
- [ ] **Step 5: Commit and push** — subject: `scene3d: the HUD stops fabricating an address for substrates that have none`.

---

### Task 8: The desktop can record a whole session — `LiveSession` `--record`

**Files:**
- Modify: `desktop/src/live/session.h` (`Spec` gains `record_path`)
- Modify: `desktop/src/live/session.cpp` (spawn argv, `:81-87`)
- Modify: `desktop/src/ui/inspect_door.cpp` (the Capture pane's record control; the Save pane's disclosure)
- Test: `desktop/test/test_session.cpp` (extend), plus a `cli-smoke` assertion

**Interfaces:**
- Consumes: Task 1's corrected advertising (the tooltip must now be true for `tree`).
- Produces:

```cpp
struct Spec {
    // ... existing fields ...
    // When non-empty, the serve host is spawned with --record=<path> and tees
    // every event of EVERY capture in this session into one file. That file is
    // the only artifact that can carry `call` + `trace`/`coverage` + wide
    // `df_step.ops` together, because LiveSession keeps each capture as its own
    // in-memory Recording and Save serialises exactly one of them.
    std::string record_path;
};
```

**The merged recording already works; the desktop just cannot ask for it.** `asmspy --serve --record=<f>` tees the session-level sink into one loadable file — `cli/test_serve_record.c` and `build/test-serve-record.asmtrace` prove it, and `Workspace::open` already loads such files (`desktop/test/fixtures/motif-crossings.asmtrace` is one, consumed by `test_crossing.cpp:293-316`). What the GUI lacks is a way to produce one: `LiveSession` spawns a fixed argv with no `--record` (`session.cpp:81-87`), `Spec` has no field for it, and Save serialises `growing()` else `done.back()` — one Recording, never the session.

**Use a NON-continuous dataflow leg.** A merged capture whose dataflow leg ran with `continuous:true` can land a trailing `df_invocation` pass with `steps:0` when Stop arrives just after a re-arm; `decode_streams` resolves `Streams::df` to the LATEST pass, so the lane prism comes up silently empty. This was observed during planning. Either drive the dataflow leg non-continuously or make the pass resolution skip an empty trailing pass — decide in Step 3 and state which.

- [ ] **Step 1: Write the failing test** — a `test_session` check that a `Spec` carrying `record_path` produces an argv containing `--record=<path>`, and that an empty one produces the argv unchanged (byte-identical to today, so no existing lane moves).
- [ ] **Step 2: Run it to verify it fails** — expect `--record` absent from the argv.
- [ ] **Step 3: Implement** — thread `record_path` into the argv for both the local and the `ssh` branch (the remote path writes the file on the REMOTE host; say so in the UI or refuse it, but do not let it look local). Add the Capture pane control, defaulting OFF.
- [ ] **Step 4: Prove the merge end to end** — with the control on, run `tree`, then `trace`, then a non-continuous `dataflow` against one target; stop; reopen the recorded file via Open; confirm the scene selector offers **address plane, invocation stack, module excursion ribbon and SIMD lane prism**, with only `divergence worldline` disabled ("needs a second recording"). Record the exact counts in the commit body.
- [ ] **Step 5: Add the cli-smoke assertion** — assert the merged file carries all three kind families, so the producer side cannot regress silently. Run `make cli-smoke`, then `make fmt-check` if any `cli/*.c` changed.
- [ ] **Step 6: Commit and push** — subject: `desktop: the live session can record itself, so one file can host four substrates`.

---

## Self-review

**Defect coverage.**

| Defect | Task |
|---|---|
| B1 zero-cell invocation availability | Task 2 |
| B2 no eviction from an unavailable kind | Task 3 |
| B3 live batch wipes kind + cameras | Task 6 |
| B4 prism stacks reads as writes | Task 4 |
| B5 phantom icicle-timeline fallback | Task 5 |
| B6 HUD fabricates an address off-plane | Task 7 |
| B7 Tree omits 3D overview; pane closed | Task 1 |
| B8 Sample advertises 3D overview | Task 1 |
| B9 Stream advertises Slice | Task 1 |
| B10 Log/Trace advertise Timeline | Task 1 |
| B11 phantom "live maps snapshot" | Task 5 |
| B12 keymap conflates the two camera buttons | Task 5 |
| F1 desktop cannot record a session | Task 8 |

**Two candidates this review REFUTED — do not "fix" them back.**

1. **The single-thread module ribbon's availability gate is correct as it stands.** It is tempting to move `ModuleRibbonScene::note` into `kind_unavailable` alongside the others. That would be a regression: `hud.cpp:504-509` renders `kind_unavailable` entries under `BeginDisabled` and blocks selection, and 59 T4 requires a one-lane ribbon to remain selectable. The note is already displayed in warn colour by `shell.cpp:1112-1115` on every draw path. Task 5 corrects the note's *wording* and explicitly leaves the gate alone.
2. **The producer does read `/proc/<pid>/maps`.** An earlier reading held that "a live maps snapshot" was pure prose. It is not — `serve_exe_text_span` (`cli/asmspy.c:3472`) parses maps to find the exe's text mapping for whole-process modes. What does not exist is a *desktop-side* `regions_from_maps`, so the reason string misleads by implying a viewer-side fallback. Task 5 rewords it to point at the capture modes that record `codeimage`, rather than deleting the idea.

**What this plan deliberately does NOT do.**

- **It does not change the Scene3D presence rule.** `view_presence.cpp:120` gates the whole 3D pane on `!regions_from_codeimage(r).empty()`, and `draw_scene_overview` early-returns at `shell.cpp:1649`. Three substrates — Divergence, ModuleRibbon, LanePrism — need no address plane and take no `Projection`, yet are unreachable for a codeimage-less recording. That is a real structural gap, but changing the presence rule touches the pane's whole contract and every absent-view test, and it does not block anything here: `tree`, `trace`, `dataflow` and `auto` all arm a codeimage, so every scene this plan makes reachable is reachable. **Follow-up brief required** before touching it.
- **It does not make `auto` fill every scene.** `auto` is dataflow-with-a-picker: it never calls `region_record`, so it cannot emit the `coverage` footer the invocation stack's block axis needs, and deriving blocks from an instruction stream is the greedy block-attribution rule this tree forbids (`docs/internal/gui/57-causal-layers.md:138`). Task 8's merged session is the supported route.
- **It does not address divergence.** That substrate compares two recordings and is a `d`-key action by construction. Four of five is the ceiling for any single capture.

**Open risk.**

- Task 1 changes strings that `test_budget.cpp` may already pin at `:295-340`. Read those assertions before editing and update them in the same commit; a plan that leaves a pinned string stale produces a red suite the next agent inherits.
- Task 8's `ssh` branch writes the recording on the REMOTE host. Shipping it without saying so would produce a file the user cannot open. If the UI cannot make that clear, refuse the combination rather than half-supporting it.
- Task 3 introduces the tree's first automatic change of a user-chosen mode. If the eviction notice proves noisy in practice, the fix is to make the notice quieter — never to make the eviction silent.

**Known soft spots.** `shell_kind_availability` is file-local static with one call site, so Task 2 verifies the *builder* half directly and the gate half only by inspection unless the decision is extracted. Task 3 assumes that extraction happened; if it did not, its test cannot link and the task must do the extraction first. Neither task should be reported complete on a builder-only assertion.
