# Strip Simplified LOD + Plan Cache + Clear-Previous Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the session strip usable and fast at Firefox scale — a simplified default (top-N lanes/bands + counted aggregate rows) with a one-click detailed view, a plan cache so static frames re-plan nothing, density run-length merging, and a "clear previous sessions" affordance on the Live-capture pane.

**Architecture:** All view-side: the model stays byte-identical; `strip_view_t` gains a `detail` reading-posture flag; pure selection helpers shared by layout and plan decide what is drawn; `StripState` caches the prim vector keyed by a pure hash. The session-history affordance is one `LiveSession::clear_completed()` + a two-step button + a self-healing clamp in `shell_sync_live_tab`.

**Tech Stack:** C++17, Dear ImGui 1.91.9b (null-backend testable), hand-rolled `check()` harness, `mk/desktop.mk` (`make desktop-test`, authoritative `make docker-desktop`).

**Spec:** `docs/superpowers/specs/2026-08-10-strip-simplified-lod-design.md`.

## Global Constraints

- **No model change**: `strip_build` output byte-identical; no wire/schema/3D change; no new Settings field.
- **At/below thresholds (`kStripSimplifiedLanes = 8`, `kStripSimplifiedBands = 6`) simplified == detailed, byte-identical `strip_plan_dump`** — existing small-fixture tests must stay green UNMODIFIED.
- Hidden remainders are COUNTED on screen: aggregate rows carry `"(+N lanes, M events)"` / `"(+N regions — M access(es), counts only)"`; the HUD appends `"simplified — top 8 of N lanes, top 6 of R regions"` only when something was actually aggregated. Pinned verbatim by tests.
- Aggregate rows use existing prim kinds with `a = kStripAggRow` (0xFFFFFFFFu) — no new prim kind, painter reads only `b` for density.
- Determinism everywhere: ranking ties break by ascending tid / ascending base; RLE and merges are pure functions of the same inputs.
- `clear_completed()` refuses (returns false, no-op) while a capture is growing; `reset()` stays the Disconnect-only teardown.
- Shared-tree discipline: work in a detached worktree off origin/main; `git add` only your paths; push every commit (`git push origin HEAD:main`; on reject: `git fetch && git rebase origin/main && push`).
- Verify with the binaries directly; `make desktop-test` full lane + `make docker-desktop` before the final claim.

---

### Task 1: `detail` flag, `strip_plan_key`, plan cache

**Files:**
- Modify: `desktop/src/views/strip.h` (`strip_view_t`, `StripState`, new decls ~lines 136/221)
- Modify: `desktop/src/views/strip.cpp`, `desktop/src/views/strip_draw.cpp` (`draw_strip` plan call)
- Test: `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Produces: `strip_view_t::detail` (bool, default false); `uint64_t strip_plan_key(const strip_view_t &v, uint64_t model_gen)`; `StripState::{plan_cache, plan_key, model_gen}`.

- [ ] **Step 1: Write the failing key tests** (append fn + `main` call in `test_strip_model.cpp`):

```cpp
static void plan_key_sensitivity() {
    strip_view_t v; v.px_w = 800; v.px_h = 400; v.seq_per_px = 1.0;
    const uint64_t k0 = strip_plan_key(v, 7);
    check("key stable", strip_plan_key(v, 7) == k0,
          "identical inputs → identical key (the cache's whole premise)");
    strip_view_t w;
    w = v; w.seq0 = 1;          check("key: seq0", strip_plan_key(w, 7) != k0, "");
    w = v; w.seq_per_px = 2;    check("key: zoom", strip_plan_key(w, 7) != k0, "");
    w = v; w.lane0 = 1;         check("key: lane0", strip_plan_key(w, 7) != k0, "");
    w = v; w.lane_h = 20;       check("key: lane_h", strip_plan_key(w, 7) != k0, "");
    w = v; w.px_w = 801;        check("key: px_w", strip_plan_key(w, 7) != k0, "");
    w = v; w.px_h = 401;        check("key: px_h", strip_plan_key(w, 7) != k0, "");
    w = v; w.detail = true;     check("key: detail", strip_plan_key(w, 7) != k0, "");
    check("key: model_gen", strip_plan_key(v, 8) != k0, "");
    check("key: follow does NOT key", [&]{ strip_view_t f = v; f.follow_tail = false;
          return strip_plan_key(f, 7) == k0; }(),
          "follow only moves seq0, which is keyed on its own");
}
```

- [ ] **Step 2: Run to verify it fails** — `make build/desktop_test_strip_model` → compile error (`detail`/`strip_plan_key` unknown).

- [ ] **Step 3: Implement** — in `strip.h`: add `bool detail = false;` to `strip_view_t` (after `follow_tail`, comment: "the reading posture: false = simplified top-N + aggregates (default), true = every lane/band"); declare `uint64_t strip_plan_key(const strip_view_t &v, uint64_t model_gen);` beside `strip_plan`; add to `StripState`: `std::vector<strip_prim_t> plan_cache; uint64_t plan_key = 0; uint64_t model_gen = 0;` (comment: cache is valid iff plan_key matches; model_gen increments on every rebuild). In `strip.cpp` (FNV-1a over bit patterns):

```cpp
uint64_t strip_plan_key(const strip_view_t &v, uint64_t model_gen) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    auto bits_d = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
    auto bits_f = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return uint64_t(u); };
    mix(bits_d(v.seq0));
    mix(bits_d(v.seq_per_px));
    mix(static_cast<uint64_t>(v.lane0));
    mix(bits_f(v.lane_h));
    mix(bits_f(v.px_w));
    mix(bits_f(v.px_h));
    mix(v.detail ? 1 : 0);
    mix(model_gen);
    return h;
}
```
(add `#include <cstring>`). In `strip_draw.cpp`'s `draw_strip`, replace the unconditional plan with the cache (and bump nothing here — `model_gen` is bumped where the model is REBUILT, next step):

```cpp
    const uint64_t key = strip_plan_key(st.cam, st.model_gen);
    if (key != st.plan_key) {
        strip_plan(m, st.cam, &st.plan_cache);
        st.plan_key = key;
    }
    const std::vector<strip_prim_t> &prims = st.plan_cache;
```
(delete the local `prims` vector; downstream uses the reference.) In `shell.cpp`'s `shell_strip_body`, bump the generation when the model is rebuilt: after `st.built = true;` add `st.model_gen++;`.

- [ ] **Step 4: Run** — model test `ok`; also `./build/desktop_test_strip_draw` still `ok` (panel now caches).

- [ ] **Step 5: Commit + push** — `git add desktop/src/views/strip.h desktop/src/views/strip.cpp desktop/src/views/strip_draw.cpp desktop/src/ui/shell.cpp desktop/test/test_strip_model.cpp` · msg `desktop/views: strip plan cache — pure key, static frames re-plan nothing`.

---

### Task 2: selection helpers + aggregate rows + HUD phrase

**Files:**
- Modify: `desktop/src/views/strip.h/.cpp`; Test: `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Produces:
  - `inline constexpr uint32_t kStripAggRow = 0xFFFFFFFFu;`
  - `inline constexpr size_t kStripSimplifiedLanes = 8;` / `kStripSimplifiedBands = 6;`
  - `struct StripSelection { std::vector<size_t> keep; size_t hidden = 0; uint64_t hidden_events = 0; };`
  - `StripSelection strip_selected_lanes(const StripModel &m, bool detail);`
  - `StripSelection strip_selected_bands(const StripModel &m, bool detail);`
- Consumes: Task 1's `detail`.

**Rules (from the spec, exact):** lanes ranked by `lane_activity[i].size()` + count of `sys` rows with `lane == i`, ties → ascending tid; bands ranked by placed `mem`+`pc` marks with `band == i`, ties → ascending base; `keep` holds the winners IN MODEL ORDER; `hidden_events` sums the losers' same metric. Detail or count ≤ threshold → every index, `hidden == 0`.

- [ ] **Step 1: Failing tests** — a 12-lane, 9-band synthetic model (built directly, no NDJSON — selection is pure over the model):

```cpp
static StripModel big_model() {
    StripModel m;
    for (int i = 0; i < 12; i++) {
        StripLane ln; ln.tid = 100 + i; ln.label = "[" + std::to_string(100 + i) + "]";
        m.lanes.push_back(ln);
        // lane i gets i activity events at seqs 0..i-1  (lane 11 busiest)
        std::vector<uint64_t> act; for (int k = 0; k < i; k++) act.push_back(k);
        m.lane_activity.push_back(act);
    }
    for (int b = 0; b < 9; b++) {
        StripBand bd; bd.region.base = 0x1000 * (b + 1); bd.region.len = 0x1000;
        bd.region.label = "r" + std::to_string(b); m.bands.push_back(bd);
    }
    // band b gets b placed mem marks (band 8 busiest)
    for (int b = 0; b < 9; b++)
        for (int k = 0; k < b; k++) {
            StripMemMark mk; mk.seq = k; mk.addr = 0x1000 * (b + 1) + k;
            mk.band = b; m.mem.push_back(mk);
        }
    std::sort(m.mem.begin(), m.mem.end(),
              [](const StripMemMark &a, const StripMemMark &b) { return a.seq < b.seq; });
    m.deck_enabled = m.bands_enabled = true;
    m.seq_end = 16;
    return m;
}

static void simplified_selection() {
    StripModel m = big_model();
    StripSelection ls = strip_selected_lanes(m, false);
    check("lanes: top 8 kept", ls.keep.size() == 8 && ls.hidden == 4,
          "12 lanes, threshold 8");
    check("lanes: busiest kept, model order", ls.keep.front() == 4 && ls.keep.back() == 11,
          "lanes 4..11 are the 8 most active; keep[] stays in model order");
    check("lanes: hidden events summed", ls.hidden_events == 0 + 1 + 2 + 3,
          "lanes 0-3 hide");
    StripSelection ld = strip_selected_lanes(m, true);
    check("lanes: detail keeps all", ld.keep.size() == 12 && ld.hidden == 0, "");
    StripSelection bs = strip_selected_bands(m, false);
    check("bands: top 6 kept", bs.keep.size() == 6 && bs.hidden == 3 &&
              bs.keep.front() == 3, "bands 3..8; base order preserved");
    check("bands: hidden accesses summed", bs.hidden_events == 0 + 1 + 2, "");
    StripModel small; small.lanes.resize(3); small.lane_activity.resize(3);
    small.deck_enabled = true;
    check("threshold no-op", strip_selected_lanes(small, false).keep.size() == 3 &&
              strip_selected_lanes(small, false).hidden == 0, "");
}

static void simplified_plan_rows() {
    StripModel m = big_model();
    strip_view_t v; v.px_w = 300; v.px_h = 400; v.seq_per_px = 100.0; // envelope
    std::vector<strip_prim_t> simp, det;
    strip_plan(m, v, &simp);
    strip_view_t vd = v; vd.detail = true;
    strip_plan(m, vd, &det);
    size_t sh = 0, dh = 0, agg_lane = 0, agg_band = 0;
    for (auto &p : simp) {
        if (p.kind == strip_prim::lane_header) { sh++; if (p.a == kStripAggRow) agg_lane++; }
        if (p.kind == strip_prim::band_label && p.a == kStripAggRow) agg_band++;
    }
    for (auto &p : det)
        if (p.kind == strip_prim::lane_header) dh++;
    check("simplified: 8 lanes + 1 aggregate", sh == 9 && agg_lane == 1, "");
    check("detailed: all 12 lanes, no aggregate", dh == 12, "");
    check("aggregate lane label", [&]{ for (auto &p : simp)
        if (p.kind == strip_prim::lane_header && p.a == kStripAggRow)
            return p.text == "(+4 lanes, 6 events)"; return false; }(), "counted, never vanished");
    check("elsewhere band label", [&]{ for (auto &p : simp)
        if (p.kind == strip_prim::band_label && p.a == kStripAggRow)
            return p.text == "(+3 regions — 3 access(es), counts only)"; return false; }(), "");
    check("simplified fewer prims", simp.size() < det.size(), "the budget claim");
    check("hud states the posture", [&]{ for (auto &p : simp)
        if (p.kind == strip_prim::hud_note)
            return p.text.find("simplified — top 8 of 12 lanes, top 6 of 9 regions")
                   != std::string::npos; return false; }(), "pinned like the axis label");
    check("hud silent when nothing hidden", [&]{ for (auto &p : det)
        if (p.kind == strip_prim::hud_note)
            return p.text.find("simplified") == std::string::npos; return true; }(), "");
    // byte-identical at/below thresholds: the existing small fixtures
    Recording r = mk_rec({R"({"k":"trace","basis":"rel","off":16,"tid":10})",
                          R"({"k":"end","events":1})"});
    StripModel sm = strip_build(r, two_bands(), {});
    strip_view_t sv; sv.px_w = 200; sv.px_h = 300; sv.seq_per_px = 0.05;
    std::vector<strip_prim_t> a2, b2;
    strip_plan(sm, sv, &a2);
    strip_view_t svd = sv; svd.detail = true;
    strip_plan(sm, svd, &b2);
    check("small model: simplified == detailed byte-identical",
          strip_plan_dump(a2) == strip_plan_dump(b2), "zero change below thresholds");
}
```

- [ ] **Step 2: Run to verify failure** (unknown symbols; then row/label mismatches).

- [ ] **Step 3: Implement** — in `strip.h` add the three constants + `StripSelection` + the two decls (doc comments per spec). In `strip.cpp`:

```cpp
StripSelection strip_selected_lanes(const StripModel &m, bool detail) {
    StripSelection s;
    const size_t n = m.lanes.size();
    std::vector<uint64_t> score(n, 0);
    for (size_t i = 0; i < n; i++)
        score[i] = m.lane_activity[i].size();
    for (const StripSys &sy : m.sys)
        if (sy.lane >= 0 && static_cast<size_t>(sy.lane) < n)
            score[static_cast<size_t>(sy.lane)]++;
    if (detail || n <= kStripSimplifiedLanes) {
        for (size_t i = 0; i < n; i++) s.keep.push_back(i);
        return s;
    }
    std::vector<size_t> rank(n);
    for (size_t i = 0; i < n; i++) rank[i] = i;
    std::sort(rank.begin(), rank.end(), [&](size_t A, size_t B) {
        if (score[A] != score[B]) return score[A] > score[B];
        return m.lanes[A].tid < m.lanes[B].tid; // ties: ascending tid
    });
    std::vector<char> kept(n, 0);
    for (size_t i = 0; i < kStripSimplifiedLanes; i++) kept[rank[i]] = 1;
    for (size_t i = 0; i < n; i++)
        if (kept[i]) s.keep.push_back(i);           // MODEL order survives
        else { s.hidden++; s.hidden_events += score[i]; }
    return s;
}
```
`strip_selected_bands` identical shape: score = placed mem+pc counts per band; ties `m.bands[A].region.base < m.bands[B].region.base`; threshold `kStripSimplifiedBands`.

Then thread the selection through `strip_layout` and `strip_plan` (`strip.cpp`):
- `strip_layout`: replace `m.lanes.size()` with `strip_selected_lanes(m, v.detail)` count + (hidden ? 1 : 0) for the deck rows, and `m.bands.size()` with kept + (hidden ? 1 : 0) for `band_h` — one extra row each hosts the aggregates. (Sum invariant unchanged; the existing layout tests use ≤3 lanes/2 bands → identical numbers.)
- `strip_plan` deck section: iterate `sel.keep` (visible slice `[v.lane0, v.lane0+L.lanes_visible)`) instead of raw indices; row index within the deck is position in `keep`. Group separators (`group_header`) emit ONLY when `v.detail` (spec: flat top lanes when simplified). After the kept lanes, when `sel.hidden`, emit the aggregate row at the next deck row: `lane_header{a = kStripAggRow, text = "(+" + N + " lanes, " + M + " events)"}` and per-column density = sum over hidden lanes' `col_range` counts (same quantization vs `density_max`, which must now be computed over kept+aggregate rows). `lane_sys_tick` emits only for sys whose lane is KEPT (a hidden lane's syscalls still show on the rail — nothing vanishes from the rail).
- `strip_plan` bands section: iterate kept bands for frames/labels/gap notches/marks/envelopes; marks/envelopes of hidden bands are NOT placed; instead the elsewhere row (below the kept bands, height `band_h`) gets `band_label{a = kStripAggRow, text = "(+N regions — M access(es), counts only)"}` + per-column `lane_density{a = kStripAggRow}` count ribbon over the hidden bands' mem seqs (quantized against that row's own max — state it in the dump via the row label only). `band_y` for kept bands maps by kept POSITION, not model index.
- HUD: when either selection hid something, append `" · simplified — top " + kept_lanes + " of " + total + " lanes, top " + kept_bands + " of " + total + " regions"` — with the exact phrase from the test; when nothing hidden, append nothing.
- `strip_hover_text` `lane_header`/`band_label`: `p.a == kStripAggRow` → return `p.text`.

- [ ] **Step 4: Run** — model test `ok`; existing checks untouched and green.

- [ ] **Step 5: Commit + push** — msg `desktop/views: strip simplified default — top-N lanes/bands, counted aggregates, pinned HUD posture`.

---

### Task 3: density RLE + envelope merge

**Files:** `desktop/src/views/strip.cpp`; Test: `desktop/test/test_strip_model.cpp`.

- [ ] **Step 1: Failing tests**:

```cpp
static void density_rle() {
    StripModel m;
    m.deck_enabled = true;
    StripLane ln; ln.tid = 1; ln.label = "[1]"; m.lanes.push_back(ln);
    // 4 events in col 0-1 territory, then a gap, then 4 more: with
    // seq_per_px=2 and px_w=8, cols 0,1 have 2 each; cols 4,5 have 2 each
    m.lane_activity.push_back({0, 1, 2, 3, 8, 9, 10, 11});
    m.seq_end = 16;
    strip_view_t v; v.px_w = 8; v.px_h = 200; v.seq_per_px = 2.0;
    std::vector<strip_prim_t> p;
    strip_plan(m, v, &p);
    size_t density = 0;
    for (auto &q : p)
        if (q.kind == strip_prim::lane_density) {
            density++;
            check("rle: run spans whole equal stretch",
                  q.x1 - q.x0 >= 2.0f, "adjacent equal columns merged");
        }
    check("rle: two runs, not four columns", density == 2,
          "equal-intensity neighbours collapse; the gap breaks the run");
}
```

- [ ] **Step 2: Run to verify failure** (4 density prims today).

- [ ] **Step 3: Implement** — in the deck density loop (and the aggregate row's, and the elsewhere ribbon's — extract one lambda `emit_density_rle(seqs_source, row_y0, row_y1, row_a)`): compute each column's quantized `b`; extend a pending run while the next column's `b` is equal AND non-zero; flush on change/zero/end as ONE prim spanning `[c0, c1)`. For `mem_envelope`: keep a pending prim per (band, rw); when the next column's rect (y0, y1 after the float math) is EXACTLY equal, extend `x1` by one column; else flush. Deterministic: pure function of the same inputs.

- [ ] **Step 4: Run** — new checks green; determinism check (`plan a == plan b`) still green; `simplified fewer prims` still green.

- [ ] **Step 5: Commit + push** — msg `desktop/views: strip density RLE + envelope merge — equal neighbours collapse to runs`.

---

### Task 4: the panel toggle + draw coverage

**Files:** `desktop/src/views/strip_draw.cpp`; Test: `desktop/test/test_strip_draw.cpp`.

- [ ] **Step 1: Failing draw test** — extend `test_strip_draw.cpp`: build a 20-lane model (loop `feed` strings or construct directly like `big_model()` — construct directly; it needs no Recording), draw the panel at `detail=false` then `detail=true`, geometry oracle both ways:

```cpp
    // 3) simplified vs detailed posture both rasterise
    st.model = StripModel{};
    for (int i = 0; i < 20; i++) {
        StripLane ln; ln.tid = i; ln.label = "[" + std::to_string(i) + "]";
        st.model.lanes.push_back(ln);
        std::vector<uint64_t> act; for (int k = 0; k <= i; k++) act.push_back(k);
        st.model.lane_activity.push_back(act);
    }
    st.model.deck_enabled = true;
    st.model.seq_end = 32;
    st.model.hud = "x";
    st.cam = strip_view_t{}; st.cam.seq_per_px = 8.0; // envelope mode
    frame([] { draw_strip(st, "rec-x", [](const dt_link &) {}); });
    check("simplified posture draws", ImGui::GetDrawData()->TotalVtxCount > 0, "");
    st.cam.detail = true;
    st.plan_key = 0; // a posture flip keys a re-plan (strip_plan_key covers it)
    frame([] { draw_strip(st, "rec-x", [](const dt_link &) {}); });
    check("detailed posture draws", ImGui::GetDrawData()->TotalVtxCount > 0, "");
```
(`st.plan_key = 0;` is belt-and-braces; the key covers `detail` already — keep the line out if the first run proves it unnecessary. It is: drop it.)

- [ ] **Step 2: Run to verify failure** — compiles but no button yet: also add the button presence check via the header-row draw (geometry-only harness cannot click; assert by drawing and relying on Task 2's plan tests for behavior — the BUTTON itself is the only new draw code):

In `draw_strip`'s header row (after the follow button):

```cpp
    ImGui::SameLine();
    if (ImGui::SmallButton(st.cam.detail ? "simplify" : "detail"))
        st.cam.detail = !st.cam.detail; // keyed into strip_plan_key → re-plans
```

- [ ] **Step 3: Run** — both postures draw, all strip tests green.

- [ ] **Step 4: Commit + push** — msg `desktop/views: strip detail/simplify toggle on the panel header`.

---

### Task 5: clear previous sessions

**Files:**
- Modify: `desktop/src/live/session.h` (~after `reset()`), `desktop/src/live/session.cpp`
- Modify: `desktop/src/ui/doors.h` (`InspectState`: `bool clear_prev_armed = false;`)
- Modify: `desktop/src/ui/inspect_door.cpp` (beside the `"N completed recording(s) this session"` line, ~845)
- Modify: `desktop/src/ui/shell.cpp` (`shell_sync_live_tab` clamp, ~233)
- Test: `desktop/test/test_live_session.cpp`, `desktop/test/test_shell.cpp`

**Interfaces:**
- Produces: `bool LiveSession::clear_completed();` — false + no-op while `growing() != nullptr`; else clears `done_` + `notes_`, keeps status/pipes/malformed counter, returns true.

- [ ] **Step 1: Failing session test** — in `test_live_session.cpp`, mimic `test_state_machine`'s feed shapes (copy its exact header/session lines) to complete TWO captures, then:

```cpp
    check("clear: refused while growing", /* feed a third 'session started'
          + one event so growing() != nullptr */ !s.clear_completed() &&
              s.recordings().size() == 2,
          "previous means FINISHED; the open capture pins history");
    /* feed the stop/end lines to close capture 3 */
    check("clear: drops completed", s.clear_completed() &&
              s.recordings().empty() && s.notes().empty(),
          "done_ and notes_ empty; the affordance's whole contract");
    check("clear: session still usable", true, "");
    /* feed a fourth capture start+event+stop; then: */
    check("clear: later capture records", s.recordings().size() == 1,
          "clearing history must not wedge the state machine");
```
(Write the feed lines by copying the fixture strings already in this file — session started/stopped and a minimal event — they exist verbatim in `test_state_machine`.)

- [ ] **Step 2: Run to verify failure** — compile error (`clear_completed` unknown).

- [ ] **Step 3: Implement**:

`session.h`, after `reset()`:
```cpp
    // Drop the COMPLETED recordings and the notes that accompanied them,
    // keeping the host, the pipes, the status and any still-growing capture
    // — the "clear previous sessions" affordance: the union weave and every
    // accumulating scene start again from what is still live. Returns false
    // and does NOTHING while a capture is growing: "previous" means
    // finished, and refusing while open keeps the lifecycle notes trivially
    // attributable. reset() remains the Disconnect-only full teardown.
    bool clear_completed();
```
`session.cpp`:
```cpp
bool LiveSession::clear_completed() {
    if (open_)
        return false;
    done_.clear();
    notes_.clear();
    return true;
}
```
`doors.h` (`InspectState`): `bool clear_prev_armed = false; // two-step arm for "clear previous captures"`.

`inspect_door.cpp`, replacing the bare count line:
```cpp
    if (nrec) {
        ImGui::Text("%zu completed recording(s) this session", nrec);
        ImGui::SameLine();
        const bool growing_now = s.session.growing() != nullptr;
        if (growing_now) {
            ImGui::TextDisabled("(stop the capture to clear them)");
            s.clear_prev_armed = false;
        } else if (!s.clear_prev_armed) {
            if (ImGui::SmallButton("clear previous"))
                s.clear_prev_armed = true; // first click only arms
        } else {
            char lbl[64];
            std::snprintf(lbl, sizeof lbl, "really clear %zu? not saved", nrec);
            const bool go2 = ImGui::SmallButton(lbl);
            ImGui::SameLine();
            const bool keep = ImGui::SmallButton("keep");
            if (go2)
                s.session.clear_completed();
            if (go2 || keep)
                s.clear_prev_armed = false;
        }
    } else {
        s.clear_prev_armed = false;
    }
```
(Check the surrounding code's actual variable names — `nrec` exists at the anchor; keep its spelling.)

`shell.cpp`, in `shell_sync_live_tab` right after `LiveSession &sess = s.inspect.session;`:
```cpp
    // Self-healing dedup watermark: "clear previous" (or any future shrink)
    // can drop completed captures below the adopted-tab count; a stale
    // watermark would then block the NEXT capture's promotion forever.
    if (s.live_dismissed_done > sess.recordings().size())
        s.live_dismissed_done = 0;
```
(Verify the member spelling `live_dismissed_done` against `shell.h` — the code wins.)

- [ ] **Step 4: Failing→green shell test** — in `test_shell.cpp`, find the live-tab feed block (the `s.feed_line(R"({"k":"syscall"...)` cluster) and append after its existing checks: drive two full captures, set `ds.live_dismissed_done = 5;` (a stale adopted count), call `sess.clear_completed()`, run `shell_sync_live_tab(ds)`, assert `ds.live_dismissed_done == 0` and the live tab was dropped (`ds.live_tab == -1` — the "No capture to show" path) or, if a growing capture was left, the union event count equals the growing capture's. Use the file's existing helpers/spellings.

- [ ] **Step 5: Run both** — `./build/desktop_test_live_session` and `./build/desktop_test_shell` → all checks pass.

- [ ] **Step 6: Commit + push** — `git add desktop/src/live/session.h desktop/src/live/session.cpp desktop/src/ui/doors.h desktop/src/ui/inspect_door.cpp desktop/src/ui/shell.cpp desktop/test/test_live_session.cpp desktop/test/test_shell.cpp` · msg `desktop/live: clear previous sessions — two-step affordance, self-healing tab watermark`.

---

### Task 6: full-lane verification

- [ ] **Step 1:** `make desktop-test` in the worktree → exit 0, judge only NEW failures (none expected — thresholds keep old fixtures byte-identical).
- [ ] **Step 2:** `make docker-desktop` → exit 0 (headless + ui-test + GL + Xvfb).
- [ ] **Step 3:** Push any fixes; confirm `origin/main` tip contains every task commit.
