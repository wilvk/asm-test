# Live Union Weave + Stable Plane Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make repeated live captures of one process *accumulate* in the 3D address scene (union weave) and keep already-placed regions from jumping around between captures (stable layout), both as Settings toggles defaulting ON.

**Architecture:** A pure `merge_session_recordings()` mirrors what `--serve --record` already tees to disk (one header, concatenated events), so the live tab's 3D pane can weave the session union in memory. A `keep_order` flag on `build_projection()` plus a first-seen-order `regions_from_codeimage_seen()` makes the compacted domain append-only, so an existing code region's domain slot never moves when a later capture adds regions. Two new `Settings` bools (default **true** — "all options start as checked", which covers `auto`-mode sessions from the first Start) gate both behaviors; the pane weave re-fires when either toggles.

**Tech Stack:** C++17 desktop tree (`desktop/src`), hand-rolled `check()` test harness (NO gtest), `mk/desktop.mk` explicit per-test link lines.

## Global Constraints

- Desktop tests are standalone binaries with `check(what, cond, why)` + their own `main()` — never gtest (`desktop/test/view_test.h` idiom).
- `vmmap` spans must NEVER become Projection regions (`space/vmmap.h` load-bearing constraint; `test_vmmap` pins it). Nothing in this plan feeds vmmap into `build_projection`.
- Shared tree worked by concurrent agents: commit with a private `GIT_INDEX_FILE` + `write-tree`/`commit-tree`/CAS `update-ref`, path-scoped; push each commit to origin/main immediately; repair the shared index with `git reset -- <paths>` afterwards.
- Verify each touched test binary INDIVIDUALLY (`make build/desktop_test_X && ./build/desktop_test_X`) — a for-loop dies on the pre-existing test_shell attach/no-host failures.
- New capability = new standalone files where possible (concurrent-agent clobber hazard).

---

### Task 1: `merge_session_recordings` (pure union Recording)

**Files:**
- Create: `desktop/src/doc/recording_union.h`, `desktop/src/doc/recording_union.cpp`
- Create: `desktop/test/test_recording_union.cpp`
- Modify: `mk/desktop.mk` (app object list ~line 599, `DESKTOP_TEST_DOC` area ~line 726, `DESKTOP_TESTS` list ~line 1212, new link rule near `desktop_test_recording` ~line 2180)

**Interfaces:**
- Produces: `asmdesk::Recording merge_session_recordings(const std::vector<Recording> &done, const Recording *growing);` in namespace `asmdesk`, header `doc/recording_union.h`. Task 4 consumes it from `shell_sync_live_tab`.

Merge semantics (mirror the single-header `--serve --record` tee, `cli/asmspy.c` "One writer for however many engines"):
- 0 parts → `Recording{}`. 1 part → verbatim copy.
- Identity from FIRST part: `version`, `producer`, `arch`, `provenance.raw`. `provenance.backend`: distinct values joined with `+` in part order. `provenance.exact`: AND. `provenance.trust`: weakest present, rank `exact(3) > strong(2) > statistical(1) > weak(0)`. `redacted`: OR.
- `code`: present iff every part present with equal `sha256`; else absent.
- Events: per part in order, append every `by_kind` vector's events with `seq += offset`, then `offset += part.next_seq`; `next_seq` = final offset; `unknown_kinds` summed. (Per-kind vectors stay seq-ascending because parts are appended in order.)
- Fidelity: `has_end` = LAST part's; `torn` = OR; `end_truncated` = OR; `drops_lost` summed; `drops_throttled` = OR; `declared_events` summed; `has_steps_total` = OR with `steps_total` summed over parts that have it; `skipped` = AND over parts (first skipped part's `skip_code`/`skip_reason` when set); `path` = `""`, `dirty` = false.

- [ ] **Step 1: Write the failing test** — `desktop/test/test_recording_union.cpp`, loading parts through the real loader:

```cpp
// test_recording_union.cpp — the session-union Recording (live union weave).
// Pure document model: links doc/recording.o + doc/streams.o and nothing else.
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/recording_union.h"

static int failures = 0;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        failures++;
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
    }
}

static asmdesk::Recording load(const std::string &text) {
    std::istringstream in(text);
    std::string err;
    auto r = asmdesk::load_recording(in, err);
    check("fixture loads", r.has_value(), err.c_str());
    return r.value_or(asmdesk::Recording{});
}

static const char *kHdrExact =
    R"({"asmtrace":1,"producer":{"name":"asmspy","version":"1.1.0"},)"
    R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
static const char *kHdrStat =
    R"({"asmtrace":1,"producer":{"name":"asmspy","version":"1.1.0"},)"
    R"("provenance":{"backend":"ibs-sample","exact":false,"trust":"statistical"},"arch":"x86_64"})";

int main() {
    using asmdesk::Recording;
    // Two ended captures over DIFFERENT regions of one process.
    Recording a = load(std::string(kHdrExact) + "\n" +
        R"({"k":"codeimage","base":4096,"len":64,"version":0})" + "\n" +
        R"({"k":"df_step","step":0,"off":0})" + "\n" +
        R"({"k":"end","events":2})" + "\n");
    Recording b = load(std::string(kHdrExact) + "\n" +
        R"({"k":"codeimage","base":8192,"len":32,"version":0})" + "\n" +
        R"({"k":"df_step","step":0,"off":4})" + "\n" +
        R"({"k":"df_step","step":1,"off":8})" + "\n" +
        R"({"k":"end","events":3})" + "\n");

    { // empty + identity
        Recording u0 = asmdesk::merge_session_recordings({}, nullptr);
        check("empty union has no events", u0.event_count() == 0, "0 parts");
        Recording u1 = asmdesk::merge_session_recordings({a}, nullptr);
        check("single-part union is the part",
              u1.event_count() == a.event_count() && u1.arch == a.arch &&
                  u1.has_end,
              "1 part must merge to itself");
    }
    { // two ended parts: events concatenate, seq stays strictly increasing
        Recording u = asmdesk::merge_session_recordings({a, b}, nullptr);
        check("union event_count sums",
              u.event_count() == a.event_count() + b.event_count(),
              "2+3 events expected");
        check("union keeps both code regions",
              u.by_kind.at("codeimage").size() == 2,
              "codeimage from BOTH captures must survive");
        const auto &df = u.by_kind.at("df_step");
        bool inc = true;
        for (size_t i = 1; i < df.size(); i++)
            inc = inc && df[i - 1].seq < df[i].seq;
        check("union seq strictly increases across the boundary", inc,
              "reassigned seq must preserve stream order");
        check("union of ended parts has an end", u.has_end && !u.torn,
              "last part ended cleanly");
    }
    { // a growing tail: the union is open (no end), never falsely complete
        Recording g = load(std::string(kHdrExact) + "\n" +
                           R"({"k":"df_step","step":0,"off":0})" + "\n");
        g.torn = false; // a growing live capture is open, not torn
        g.has_end = false;
        Recording u = asmdesk::merge_session_recordings({a}, &g);
        check("union with growing tail is open", !u.has_end,
              "has_end must come from the LAST part");
        check("growing events included",
              u.event_count() == a.event_count() + g.event_count(),
              "growing part's events must be in the union");
    }
    { // provenance honesty: exact AND, weakest trust, backends joined
        Recording s = load(std::string(kHdrStat) + "\n" +
                           R"({"k":"end","events":0})" + "\n");
        Recording u = asmdesk::merge_session_recordings({a, s}, nullptr);
        check("union of exact+statistical is NOT exact", !u.provenance.exact,
              "exact must AND");
        check("union trust is the weakest",
              u.provenance.trust == "statistical", "weakest rank wins");
        check("union backend names both",
              u.provenance.backend == "ptrace-dataflow+ibs-sample",
              "distinct backends join with +");
    }
    { // a torn part taints the union
        Recording t = load(std::string(kHdrExact) + "\n" +
                           R"({"k":"df_step","step":0,"off":0})" + "\n");
        check("fixture is torn", t.torn, "no end -> torn");
        Recording u = asmdesk::merge_session_recordings({t, b}, nullptr);
        check("torn part keeps the union truncated", u.torn,
              "a torn capture's absence of footer must not vanish");
    }
    if (failures == 0)
        std::printf("test_recording_union: OK\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run to verify it fails** — `make build/desktop_test_recording_union` → FAILS: `doc/recording_union.h` not found.

- [ ] **Step 3: Implement** — `recording_union.h` declares the function with a doc comment stating it mirrors the `--record` tee (one header, concatenated events) and naming the honesty rules (exact ANDs, trust weakens, torn ORs). `recording_union.cpp` implements the semantics above; collect `const Recording*` parts (done in order, then growing), early-return the 0/1-part cases, then fold.

- [ ] **Step 4: Register in `mk/desktop.mk`** — add `doc/recording_union.o` beside `doc/recording.o` in the app template (~599) and `DESKTOP_TEST_DOC` (~726); add `$(BUILD)/desktop_test_recording_union` to `DESKTOP_TESTS`; add the link rule beside `desktop_test_recording` (~2180): test object + `$(DESKTOP_TEST_DOC)` + the union object.

- [ ] **Step 5: Run to verify it passes** — build + run `./build/desktop_test_recording_union`. Also `make build/desktop_test_recording && ./build/desktop_test_recording` (untouched neighbor still green).

- [ ] **Step 6: Commit + push** (private-index technique; paths: the two new src files, the test, `mk/desktop.mk`) — `desktop/doc: add merge_session_recordings — the live session union Recording`.

---

### Task 2: append-only projection order (`keep_order`) + first-seen code regions

**Files:**
- Modify: `desktop/src/space/projection.h` (~line 24 `build_projection` decl), `desktop/src/space/projection.cpp` (~line 222)
- Modify: `desktop/src/space/terrain.h` (~line 249), `desktop/src/space/terrain.cpp` (~line 77)
- Test: `desktop/test/test_projection.cpp`, `desktop/test/test_terrain.cpp`

**Interfaces:**
- Produces: `Projection build_projection(std::vector<Region> regions, bool keep_order = false);` — `keep_order=true` skips the by-base sort, so callers control domain order and appends never move existing slots. Default `false` is byte-identical to today.
- Produces: `std::vector<Region> regions_from_codeimage_seen(const Recording &rec);` — same dedup as `regions_from_codeimage` (one Region per base, widest len, latest version, `"code@"+hex(base)` label) but output in FIRST-SEEN event order.

- [ ] **Step 1: Failing tests.** In `test_projection.cpp` (match its existing harness style) add:

```cpp
// keep_order: the caller's order IS the domain order, and appending a region
// never moves an existing region's domain slot (the stable-layout hinge).
{
    using asmdesk::space::Region;
    Region hi;  hi.base = 0x2000; hi.len = 64;  hi.kind = Region::Code;
    Region lo;  lo.base = 0x1000; lo.len = 128; lo.kind = Region::Code;
    Region add; add.base = 0x1800; add.len = 32; add.kind = Region::Code;
    auto p1 = asmdesk::space::build_projection({hi, lo}, /*keep_order=*/true);
    check("keep_order preserves caller order",
          p1.regions.size() == 2 && p1.regions[0].base == 0x2000,
          "first-given must own the first domain slot");
    auto p2 = asmdesk::space::build_projection({hi, lo, add), /*keep_order=*/true);
    check("append keeps existing domain slots",
          p2.domain_off[0] == p1.domain_off[0] &&
              p2.domain_off[1] == p1.domain_off[1] &&
              p2.regions[2].base == 0x1800,
          "an appended region must land AFTER the existing slots");
    auto ps = asmdesk::space::build_projection({hi, lo});
    check("default still sorts by base", ps.regions[0].base == 0x1000,
          "the two-arg default must stay byte-identical to today");
}
```

(Fix the deliberate `{hi, lo, add)` typo to `}` when writing — the vector literal needs explicit `std::vector<Region>{...}` if brace deduction fails.)

In `test_terrain.cpp` add:

```cpp
// regions_from_codeimage_seen: first-seen order, same dedup as the sorted twin.
{
    std::istringstream in(std::string(kHdr) + "\n" +
        R"({"k":"codeimage","base":8192,"len":32,"version":0})" + "\n" +
        R"({"k":"codeimage","base":4096,"len":64,"version":0})" + "\n" +
        R"({"k":"codeimage","base":8192,"len":48,"version":1})" + "\n" +
        R"({"k":"end","events":3})" + "\n");
    std::string err;
    auto rec = asmdesk::load_recording(in, err);
    auto seen = asmdesk::space::regions_from_codeimage_seen(*rec);
    check("seen order is first-encounter",
          seen.size() == 2 && seen[0].base == 8192 && seen[1].base == 4096,
          "8192 arrived first and must stay first");
    check("seen keeps widest len + latest version",
          seen[0].len == 48 && seen[0].version == 1,
          "dedup must match regions_from_codeimage");
}
```

(reuse the file's existing header fixture name if it differs from `kHdr`.)

- [ ] **Step 2: Run both to verify failure** — no `keep_order` overload / no `_seen` symbol.
- [ ] **Step 3: Implement.** `build_projection`: add the parameter, wrap only the `std::sort` in `if (!keep_order)`, extend the doc comment: caller-ordered packing is what makes a grown region set append-only (the stable-layout setting), at the stated cost that memory neighbours are plane neighbours only per the caller's order. `regions_from_codeimage_seen`: same loop as `regions_from_codeimage` but accumulate into `std::vector<Region>` + `std::map<uint64_t,size_t>` index instead of ordered map.
- [ ] **Step 4: Run to verify pass** — `desktop_test_projection`, `desktop_test_terrain`, plus `desktop_test_vmmap` (layout invariance untouched).
- [ ] **Step 5: Commit + push** — `desktop/space: keep_order projection packing + first-seen codeimage regions`.

---

### Task 3: Settings toggles (default ON)

**Files:**
- Modify: `desktop/src/ui/settings.h` (struct), `desktop/src/ui/settings.cpp` (serialize/parse)
- Modify: `desktop/src/ui/shell.cpp` `draw_settings` (~line 4526)
- Test: `desktop/test/test_settings.cpp`

**Interfaces:**
- Produces: `Settings::live_union_weave` and `Settings::stable_plane_layout`, both `bool`, both default `true`. Task 4 reads them from `s.settings`.

- [ ] **Step 1: Failing test** in `test_settings.cpp` (its existing style):

```cpp
{
    asmdesk::Settings d;
    check("union weave defaults ON", d.live_union_weave,
          "all options start as checked");
    check("stable layout defaults ON", d.stable_plane_layout,
          "all options start as checked");
    asmdesk::Settings s;
    s.live_union_weave = false;
    s.stable_plane_layout = false;
    asmdesk::Settings back;
    check("scene toggles round-trip",
          asmdesk::settings_parse(asmdesk::settings_serialize(s), back) &&
              !back.live_union_weave && !back.stable_plane_layout,
          "serialize/parse must carry both bools");
    asmdesk::Settings legacy;
    check("absent keys stay checked",
          asmdesk::settings_parse("{}", legacy) && legacy.live_union_weave &&
              legacy.stable_plane_layout,
          "an old store must not uncheck the new options");
}
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** — struct fields with a doc comment ("the 3D pane's session-union weave and append-only plane layout; ON by default so an `auto`-led session gets both from the first Start"); serialize both keys; parse with `value(key, true)` defaulting true.
- [ ] **Step 4: In `draw_settings`**, after the theme block: `ImGui::SeparatorText("3D scene")` (or `ImGui::TextDisabled` heading if SeparatorText is absent from the pinned ImGui), then two `ImGui::Checkbox` calls ("Live capture: weave session union", "Stable plane layout (append-only regions)"), each setting `s.settings_dirty = true`.
- [ ] **Step 5: Run to verify pass** — `desktop_test_settings`.
- [ ] **Step 6: Commit + push** — `desktop/ui: settings toggles for union weave + stable plane layout (default ON)`.

---

### Task 4: shell wiring — union maintenance, scene source seam, weave paths

**Files:**
- Modify: `desktop/src/ui/shell.h` (ShellState fields near `live_built_events`; `SceneView` gets `bool woven_union = false; bool woven_stable = false;`; declare the seam)
- Modify: `desktop/src/ui/shell.cpp` (`shell_sync_live_tab` ~line 280; Scene3D dispatch ~line 2852; weave block ~line 1330)
- Test: `desktop/test/test_shell.cpp` (mimic the two-session pattern near lines 1989–2031)

**Interfaces:**
- Consumes: `merge_session_recordings` (Task 1), `build_projection(..., keep_order)` + `regions_from_codeimage_seen` (Task 2), the two Settings bools (Task 3).
- Produces:

```cpp
// The recording+streams the 3D pane weaves for the ACTIVE tab: the session
// union when this is the live tab and the setting is on, else the tab's own.
struct SceneSource {
    const Recording *rec;
    const Streams *streams;
    bool is_union = false;
};
SceneSource shell_scene_source(const ShellState &s, const Recording &r,
                               const Streams &a);
```

- [ ] **Step 1: Failing wiring test** in `test_shell.cpp` (pure model, no ImGui — same posture as the 25-live tests): drive TWO dataflow sessions through `sess.feed_line` (start cmd, `session started`, header, two `df_step` + a `codeimage` at base 4096; `{"k":"end","events":3}`; `session ended` line copied from the existing re-arm test), `shell_sync_live_tab`, then the second session with a `codeimage` at base 8192 + one `df_step`, `shell_sync_live_tab` again. Assert:

```cpp
size_t i = static_cast<size_t>(ls.live_tab);
SceneSource src = shell_scene_source(ls, ls.ws.recordings[i], ls.streams[i]);
check("union source on by default", src.is_union,
      "live tab + default settings must weave the session union");
check("union spans both captures",
      src.rec->by_kind.at("codeimage").size() == 2,
      "capture A's region must still be in the weave after Start #2");
ls.settings.live_union_weave = false;
SceneSource cur = shell_scene_source(ls, ls.ws.recordings[i], ls.streams[i]);
check("setting off falls back to the current capture",
      !cur.is_union && cur.rec == &ls.ws.recordings[i],
      "the toggle must restore today's per-capture weave");
```

Also assert `ls.live_union.event_count()` equals the sum of both sessions' event counts.

- [ ] **Step 2: Run to verify failure** (`make build/desktop_test_shell`; run the binary — the two pre-existing attach/no-host FAILs are known, only the new checks matter).
- [ ] **Step 3: Implement.**
  - `shell.h`: `Recording live_union;` + `Streams live_union_streams;` next to `live_built_events` (doc comment: the session union the 3D pane weaves — recordings() ∪ growing, rebuilt on the same growth watermark; mirrors the `--record` tee in memory). `SceneView::woven_union/woven_stable` (doc comment: which settings the cached weave used, so a toggle re-weaves).
  - `shell_sync_live_tab`: after `s.ws.recordings[i] = *live;` add `s.live_union = merge_session_recordings(sess.recordings(), sess.growing()); s.live_union_streams = decode_streams(s.live_union);` (same watermark gate — no new state).
  - `shell_scene_source` (shell.cpp, near the Scene3D dispatch): `is_union` when `s.active_tab == s.live_tab && s.live_tab >= 0 && s.settings.live_union_weave`; returns `{&s.live_union, &s.live_union_streams, true}` then, else `{&r, &a, false}`.
  - Scene3D dispatch: fetch `SceneSource src = shell_scene_source(s, r, *a);`, then `SceneView &sv = s.scenes[i];` — if `sv.built` and (`sv.woven_union != src.is_union || sv.woven_stable != s.settings.stable_plane_layout`) set `sv.built = false;` then stamp both and call `draw_scene_overview(s, *src.rec, *src.streams)`.
  - Weave block (~1330): `const bool stable = s.settings.stable_plane_layout;` → `regs = stable ? space::regions_from_codeimage_seen(r) : space::regions_from_codeimage(r);` and `build_projection(std::move(regs), stable)`. (Observed data spans still append after the code prefix; vmmap still names only.)
- [ ] **Step 4: Run to verify pass** — `desktop_test_shell` (new checks green, pre-existing failures unchanged), then re-run `desktop_test_projection`, `desktop_test_terrain`, `desktop_test_settings`, `desktop_test_recording_union`, and build the full app objects via the desktop build to catch link breaks.
- [ ] **Step 5: Commit + push** — `desktop/ui: 3D pane weaves the live session union with an append-only plane (settings-gated)`.

---

## Explicitly out of scope

- Item 3 from the discussion (use `continuous` mode) — usage guidance, nothing to build.
- Item 4 (vmmap-as-geometry) — design-rejected in `space/vmmap.h`, pinned by `test_vmmap`; not implemented.
- Union weave for the flat views — the merged `--record` file via File ▸ Open remains the way to get the union everywhere; this plan scopes the union to the 3D pane's weave inputs.

## Self-Review notes

- Spec coverage: union weave (Task 1+4), stable layout (Task 2+4), settings + defaults ON (Task 3), auto-mode "checked from the first Start" (defaults true are global, so `auto` sessions inherit them).
- Type consistency: `merge_session_recordings(const std::vector<Recording>&, const Recording*)` used identically in Tasks 1 and 4; `SceneSource` defined once in shell.h.
- No placeholders: every step carries code or an exact edit location.
