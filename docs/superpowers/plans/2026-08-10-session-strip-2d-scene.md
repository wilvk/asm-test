# Session Strip 2D Scene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new 2D "Session strip" view — stream-order X axis; stacked thread-deck / kernel-rail / address-band / run-ribbon channels — showing memory accesses, threads & processes, syscalls, and run boundaries for a whole session, growing live at the right edge.

**Architecture:** The Loom split, exactly: a pure model (`strip_build`) + pure pixel-space planner (`strip_plan`) + pure camera/layout math in `desktop/src/views/strip.h/.cpp`, and a dumb ImGui painter in `desktop/src/views/strip_draw.cpp`. Registered as `ViewId::SessionStrip` in the recording tab bar. Live tabs rebuild on the existing growth watermark and build over the live-union Recording.

**Tech Stack:** C++17, Dear ImGui 1.91.9b (docking), nlohmann json (already vendored), hand-rolled desktop test harness (`check(what, cond, why)`, no gtest), `mk/desktop.mk` + `make desktop-test` / `make docker-desktop`.

**Spec:** `docs/superpowers/specs/2026-08-10-session-strip-2d-scene-design.md` — read it first; its honesty rules are requirements, not flavor.

## Global Constraints

- **No engine, wire, or schema change.** Desktop-only; consumes existing kinds: `mem`, `syscall`, `trace`, `call`, `watch`, `df_step`, `df_invocation`, `coverage`, `topo`.
- **X axis is `Event::seq`** (stream position, `desktop/src/doc/recording.h:66-75`) — NEVER `step` (per-pass, restarts at 0 every `df_invocation`) and NEVER presented as time/duration. Syscall ticks are points; no prim has along-axis extent derived from a gap between two events.
- **Pinned strings, verbatim** (each has a test asserting it): axis label `"stream order — not time"`; mem legend `"mem carries no tid — access marks are r/w-hued, never thread-hued"`; presence reason `"recording carries no mem/syscall/trace/call/watch/df_step events"`.
- **Syscall payload BYTES are never copied into the model** — count only (`space/crossing.h:121-126` precedent). The payload-free `line` IS safe to copy (schema contract).
- **Every disabled channel carries a non-empty verbatim reason** (`space/crossing.h:139-144` precedent).
- **Deterministic plan:** same (model, camera) → byte-identical `strip_plan_dump`. No wall-clock reads, no unordered-container iteration in build or plan (use `std::map` / sorted vectors).
- **Off-model events are COUNTED, never silently dropped** (`off_band_mem`, `off_band_pc`).
- **Style:** match `desktop/src/views/scene2d.h` / `loom/fabric_plan.h` voice — file-top comment states the claims; `namespace asmdesk`; no gtest; no new dependencies. desktop/ is outside the clang-format CI gate but keep the local style.
- **Shared tree discipline:** this repo is worked by concurrent agents. `git add` ONLY the paths you touched (never `-a`/`-A`); if `git status` shows staged files you did not stage, commit via a private index (`GIT_INDEX_FILE` + `read-tree HEAD` + `git add <paths>` + `write-tree`/`commit-tree`/`update-ref`, then repair the shared index with `git reset -- <your paths>`). Push after every commit: `git pull --rebase origin main && git push origin main`.
- **Verify tests individually** — `make desktop-test` runs a for-loop that stops at the first pre-existing failure (`desktop/test_shell` attach/no-host FAILs are known pre-existing); run your own test binaries directly.

---

### Task 1: Extract the syscall classify helper

The class/outcome parse is file-local in `desktop/src/views/crossing.cpp` (`syscall_name_of` ~line 19, `class_of` ~line 45, `outcome_of` ~line 126). The strip needs the same parse; extract it so two consumers cannot drift. Behaviour-preserving — the existing crossing tests are the regression net.

**Files:**
- Create: `desktop/src/views/syscall_classify.h` (header-only, inline fns)
- Modify: `desktop/src/views/crossing.cpp` (delete the three local fns; include + call the header's)
- Test: extend `desktop/test/test_crossing.cpp` (direct assertions on the now-public functions)

**Interfaces:**
- Produces (used by Task 4 and by crossing.cpp):
  - `std::string asmdesk::syscall_name_of(const std::string &line)`
  - `asmdesk::space::SyscallClass asmdesk::syscall_class_of(const std::string &name)`
  - `asmdesk::space::SyscallOutcome asmdesk::syscall_outcome_of(const std::string &line)`

- [ ] **Step 1: Add failing direct checks to test_crossing.cpp**

Append to `desktop/test/test_crossing.cpp` (before `main`'s return / summary, following its local `check` idiom):

```cpp
#include "views/syscall_classify.h" // add with the other includes

static void classify_helper_direct() {
    using space::SyscallClass;
    using space::SyscallOutcome;
    check("classify: name skips tid prefix",
          syscall_name_of("[4242] openat(AT_FDCWD, <path>) = 3") == "openat",
          "the engine's \"[tid] \" prefix must be skipped before the name");
    check("classify: malformed prefix reads no name",
          syscall_name_of("[4242 openat(...) = 3").empty(),
          "an unclosed [ must not be guessed around");
    check("classify: openat is File",
          syscall_class_of("openat") == SyscallClass::File, "table entry");
    check("classify: clone3 is Process",
          syscall_class_of("clone3") == SyscallClass::Process, "table entry");
    check("classify: unknown name is Other",
          syscall_class_of("zzz_not_a_syscall") == SyscallClass::Other,
          "misses land in the visible grey bucket, never a guessed family");
    check("classify: '= 3' is Ok",
          syscall_outcome_of("openat(...) = 3") == SyscallOutcome::Ok, "");
    check("classify: '= -2' is Error",
          syscall_outcome_of("openat(...) = -2") == SyscallOutcome::Error, "");
    check("classify: '= ?' is Unknown",
          syscall_outcome_of("openat(...) = ?") == SyscallOutcome::Unknown,
          "\"could not tell\" and \"it worked\" are different facts");
}
```

Call `classify_helper_direct();` from `main()` alongside the existing test fns.

- [ ] **Step 2: Build the test to verify it fails**

Run: `make build/desktop_test_crossing`
Expected: COMPILE FAILURE — `views/syscall_classify.h: No such file or directory`.

- [ ] **Step 3: Write the header and switch crossing.cpp to it**

Create `desktop/src/views/syscall_classify.h`:

```cpp
// syscall_classify.h — the ONE parse of a payload-free syscall `line` into its
// derived family and outcome. Extracted verbatim from crossing.cpp so the
// crossing spurs (3D) and the session strip (2D) classify from the same table
// and can never drift. The rules are the crossing layer's, unchanged:
//   - a name that is not listed lands in SyscallClass::Other, the VISIBLE grey
//     bucket, and is never folded into a neighbouring family on a guess;
//   - an outcome that does not parse ("= ?", missing "=", "unfinished") is
//     Unknown and is NEVER read as success;
//   - the engine's "[tid] " line prefix is skipped before the name; a
//     malformed prefix reads NO name at all rather than a wrong one.
// Header-only (C++17 inline): links nowhere, so no mk/ churn and both
// consumers share the single function-local table instance.
#ifndef ASMDESK_VIEWS_SYSCALL_CLASSIFY_H
#define ASMDESK_VIEWS_SYSCALL_CLASSIFY_H

#include <cctype>
#include <map>
#include <string>

#include "space/crossing.h" // SyscallClass, SyscallOutcome

namespace asmdesk {

inline std::string syscall_name_of(const std::string &line) {
    /* MOVE crossing.cpp's syscall_name_of body here UNCHANGED */
}

inline space::SyscallClass syscall_class_of(const std::string &name) {
    /* MOVE crossing.cpp's class_of body here UNCHANGED (incl. the whole
       static kTable) */
}

inline space::SyscallOutcome syscall_outcome_of(const std::string &line) {
    /* MOVE crossing.cpp's outcome_of body here UNCHANGED */
}

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SYSCALL_CLASSIFY_H
```

The three `/* MOVE ... */` bodies are cut-and-paste moves of the exact code at `crossing.cpp:19-44` (`syscall_name_of`), `:45-120` (`class_of` incl. table), `:126-141` (`outcome_of`) — keep every comment. In `crossing.cpp`: delete the three moved functions from the anonymous namespace, add `#include "views/syscall_classify.h"`, and rename the two call sites `class_of(` → `syscall_class_of(` and `outcome_of(` → `syscall_outcome_of(` (`syscall_name_of` keeps its name). `<map>`/`<cctype>` includes in crossing.cpp can stay (harmless) — do not churn unrelated lines.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make build/desktop_test_crossing && ./build/desktop_test_crossing`
Expected: PASS, zero failures (existing checks + the 8 new ones).

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/syscall_classify.h desktop/src/views/crossing.cpp desktop/test/test_crossing.cpp
git commit -m "desktop/views: extract syscall classify parse to a shared header"
git pull --rebase origin main && git push origin main
```

---

### Task 2: strip.h — types, camera math, vertical layout (pure)

The complete public surface, written once so every later task compiles against it. Camera/layout math is implemented and tested in THIS task; builders/planner are declared here but implemented in Tasks 3–7 (stub bodies returning empty results are fine until then — the tests added per task are what force each piece).

**Files:**
- Create: `desktop/src/views/strip.h`, `desktop/src/views/strip.cpp`
- Create: `desktop/test/test_strip_model.cpp`
- Modify: `mk/desktop.mk` (DESKTOP_TESTS entry + link rule)

**Interfaces (produced — later tasks and the painter consume EXACTLY these):**

Create `desktop/src/views/strip.h`:

```cpp
// strip.h — the session strip: the whole session on ONE stream-order axis
// (2026-08-10 session-strip spec). Memory accesses, thread activity, kernel
// crossings and run boundaries, stacked as channels over Event::seq — the only
// ordering primitive every consumed kind carries. NOT time: seq orders events
// and measures nothing (space/crossing.h's ban), so the axis label rides in
// the model and no prim may read as a duration.
//
// The Loom split (loom/fabric_plan.h): strip_build → StripModel (pure, from a
// Recording + a caller-supplied region list + caller-supplied capture seams);
// strip_plan → pixel-space prims (pure, deterministic, byte-stable dump);
// strip_draw.cpp walks the prims. Distinct from the timeline's per-recording
// "overview strip" (doc 65) — the two never share an identifier.
#ifndef ASMDESK_VIEWS_STRIP_H
#define ASMDESK_VIEWS_STRIP_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "nav.h"
#include "space/types.h" // space::Region

namespace asmdesk {

// ---- model -----------------------------------------------------------------

// A caller-provided capture seam (live sessions only): `seq` is the union seq
// of the FIRST event of the new capture; label uses the existing capture
// ordinal convention ("capture 2").
struct StripSeam {
    uint64_t seq = 0;
    std::string label;
};

enum class StripSeamKind { Invocation, CoverageClose, Capture };

struct StripRunSeam {
    StripSeamKind kind = StripSeamKind::Invocation;
    uint64_t seq = 0;
    std::string label;          // "pass 3 = 42, 8 steps" / "coverage close" / "capture 2"
    bool armed_waiting = false; // df_invocation steps==0 — a marker, not a verdict
    bool truncated = false;     // that pass's own truncation
};

struct StripLane {
    int64_t tid = -1; // -1 = the single-stream lane
    long tgid = -1;   // -1 = unknown (no topo task for this tid)
    bool leader = false;
    std::string label;       // "comm [tid]" / "[tid]" / "(single stream)"
    bool group_head = false; // first lane of a tgid group when >1 tgid known
    std::string group_label; // "comm [tgid]" on group_head rows, else ""
};

struct StripSys {
    size_t row = 0;   // order of appearance among syscall events
    uint64_t seq = 0;
    int64_t tid = -1; // -1 = wire carried no tid: rail only, NEVER a lane tick
    int lane = -1;    // index into lanes when tid known, else -1
    space::SyscallClass cls = space::SyscallClass::Other;
    space::SyscallOutcome outcome = space::SyscallOutcome::Unknown;
    bool has_payload = false;
    uint64_t payload_bytes = 0; // COUNT only; bytes are never copied here
    std::string line;           // payload-free by schema; safe to show
};

struct StripMemMark {
    uint64_t seq = 0;
    uint64_t addr = 0;
    uint64_t size = 0;
    bool is_write = false;
    uint32_t step = 0;  // per-PASS step (restarts each df_invocation)
    int32_t pass = -1;  // 0-based df_invocation ordinal owning this seq; -1 none
    int band = -1;      // index into bands (placed marks only)
};

struct StripPcMark {
    uint64_t seq = 0;
    uint64_t addr = 0;
    int64_t tid = -1;
    int band = -1;
};

struct StripBand {
    space::Region region; // base/len/kind/label — y maps [base, base+len) linearly
};

struct StripModel {
    std::vector<StripLane> lanes;
    // parallel to lanes: that lane's activity seqs (trace/call/watch), sorted
    std::vector<std::vector<uint64_t>> lane_activity;
    std::vector<StripSys> sys;       // sorted by seq
    std::vector<StripBand> bands;    // sorted by region base
    std::vector<StripMemMark> mem;   // placed only, sorted by seq
    std::vector<StripPcMark> pc;     // placed only, sorted by seq
    std::vector<StripRunSeam> seams; // sorted by seq (ties: input order)
    uint64_t seq_end = 0;            // r.event_count() — the axis extent
    uint32_t off_band_mem = 0;       // counted, never silently dropped
    uint32_t off_band_pc = 0;
    bool multi_tgid = false; // >1 known tgid → group separators draw

    bool deck_enabled = false;
    std::string deck_reason; // verbatim, non-empty when disabled
    bool rail_enabled = false;
    std::string rail_reason;
    bool bands_enabled = false;
    std::string bands_reason;

    bool torn = false;
    bool truncated = false;
    uint64_t drops_lost = 0, drops_throttled = 0;
    std::string hud; // the one-line honesty summary (built by strip_build)

    static const char *axis_label() { return "stream order — not time"; }
    static const char *mem_tid_note() {
        return "mem carries no tid — access marks are r/w-hued, never "
               "thread-hued";
    }
};

// Build the model. Engine-free and session-free: `regions` is the SAME list
// the 3D weave assembles (codeimage → observed spans → vmmap names), passed in
// so the strip and the 3D pane cannot disagree; `capture_seams` come from the
// live session's parts (empty for a replayed file).
StripModel strip_build(const Recording &r,
                       const std::vector<space::Region> &regions,
                       const std::vector<StripSeam> &capture_seams);

// ---- camera ----------------------------------------------------------------

struct strip_view_t {
    double seq0 = 0;
    double seq_per_px = 0; // <= 0 means "fit whole session" (resolved at draw)
    int lane0 = 0;         // deck scroll, in lanes
    float lane_h = 18.0f;
    float px_w = 800.0f, px_h = 400.0f;
    bool follow_tail = true; // reading posture, not a Settings field
};

void strip_view_window(const strip_view_t &v, double *lo, double *hi);
void strip_view_set_window(strip_view_t &v, double lo, double hi);
// Pin the window's right edge to the growing tail.
void strip_view_follow(strip_view_t &v, uint64_t seq_end);
int strip_view_lanes_full(const strip_view_t &v, float deck_h);
int strip_view_lane_max(const strip_view_t &v, int lane_count, float deck_h);
void strip_view_scroll_lanes(strip_view_t &v, int lane_count, float deck_h,
                             int delta);

// ---- vertical layout (pure) --------------------------------------------------

// Fixed stacking, top→bottom: thread deck, kernel rail, address bands, run
// ribbon. Heights are deterministic; deck_h + rail_h + bands_h + ribbon_h ==
// px_h exactly. Band heights are EQUAL — a band's height encodes nothing.
struct StripLayout {
    float deck_y0 = 0, deck_h = 0;
    float rail_y0 = 0, rail_h = 0;
    float bands_y0 = 0, bands_h = 0;
    float ribbon_y0 = 0, ribbon_h = 0;
    float band_h = 0; // bands_h / max(1, band_count)
    int lanes_visible = 0;
};
StripLayout strip_layout(const StripModel &m, const strip_view_t &v);

// ---- plan --------------------------------------------------------------------

enum class strip_prim {
    lane_header,    // a=lane
    group_header,   // a=lane (separator + group_label at a tgid boundary)
    lane_density,   // a=lane, b=quantized 0..255 intensity, one per px col
    lane_sys_tick,  // a=index into sys
    rail_frame,     //
    rail_tick,      // a=index into sys
    rail_overflow,  // a=first sys index in the column, text="+N"
    band_frame,     // a=band
    band_label,     // a=band
    gap_notch,      // a=band (the boundary ABOVE band a elides a gap)
    mem_mark,       // a=index into mem, b bit0 = is_write
    mem_envelope,   // a=band, b bit0 = is_write (one per px col per band per rw)
    pc_mark,        // a=index into pc
    run_seam,       // a=index into seams
    run_tint,       // a=run ordinal (global parity — stable while panning)
    torn_edge,      //
    hud_note,       // the HUD line + axis label + mem tid note
    channel_absent, // a: 0=deck 1=rail 2=bands; text=verbatim reason
};
const char *strip_prim_name(strip_prim k);

struct strip_prim_t {
    strip_prim kind = strip_prim::hud_note;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t a = 0, b = 0;
    std::string text;
};

// Deterministic: same (model, view) → byte-identical vector. Individual marks
// when seq_per_px <= kStripEnvelopeSeqPerPx, per-pixel-column envelopes above
// it (the doc-65 lesson: bucket in pixel space, never one drawable per event).
inline constexpr double kStripEnvelopeSeqPerPx = 4.0;
inline constexpr int kStripRailTicksPerCol = 3;
size_t strip_plan(const StripModel &m, const strip_view_t &v,
                  std::vector<strip_prim_t> *out);
std::string strip_plan_dump(const std::vector<strip_prim_t> &prims);

// ---- hover / drill-in (pure) ---------------------------------------------------

std::string strip_hover_text(const StripModel &m, const strip_prim_t &p);
// rail_tick → the syscalls view (pid set when the tick's tid maps to a known
// tgid); mem_mark → the timeline at that step (invocation set when the mark
// falls inside a df_invocation pass). Everything else: nullopt (hover only).
std::optional<dt_link> strip_click_link(const StripModel &m,
                                        const strip_prim_t &p,
                                        const std::string &rec_id);

// ---- per-recording UI state (shell-owned) --------------------------------------

struct StripState {
    StripModel model;
    strip_view_t cam;
    bool built = false;
};

} // namespace asmdesk
#endif // ASMDESK_VIEWS_STRIP_H
```

- [ ] **Step 1: Write the failing camera/layout tests**

Create `desktop/test/test_strip_model.cpp`:

```cpp
// test_strip_model.cpp — the session strip's pure closure: camera math,
// layout, build, plan. No ImGui, no GL, no engine (test_scene2d.cpp idiom).
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "views/strip.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static void camera_math() {
    strip_view_t v;
    v.px_w = 800;
    v.seq0 = 100;
    v.seq_per_px = 2.0;
    double lo = 0, hi = 0;
    strip_view_window(v, &lo, &hi);
    check("window: lo", lo == 100.0, "lo is seq0");
    check("window: hi", hi == 100.0 + 2.0 * 800, "hi is seq0 + seq_per_px*px_w");
    strip_view_set_window(v, 50, 850);
    strip_view_window(v, &lo, &hi);
    check("set_window round-trips", lo == 50.0 && hi == 850.0,
          "set then get must return the same window");
    check("set_window zoom", v.seq_per_px == (850.0 - 50.0) / 800.0, "");
    strip_view_set_window(v, -10, 790);
    strip_view_window(v, &lo, &hi);
    check("set_window clamps lo to 0", lo == 0.0, "no negative stream position");

    v.seq_per_px = 1.0;
    strip_view_follow(v, 10000);
    strip_view_window(v, &lo, &hi);
    check("follow pins tail", hi == 10000.0,
          "follow puts seq_end at the right edge");
    strip_view_follow(v, 10); // shorter than a window
    check("follow clamps at 0", v.seq0 == 0.0, "never a negative origin");

    strip_view_t d;
    d.lane_h = 18;
    check("lanes_full", strip_view_lanes_full(d, 90.0f) == 5, "90/18");
    check("lane_max small deck", strip_view_lane_max(d, 3, 90.0f) == 0,
          "3 lanes fit in 5 rows: no scroll");
    check("lane_max big deck", strip_view_lane_max(d, 12, 90.0f) == 7,
          "12 lanes, 5 visible: max lane0 is 7");
    d.lane0 = 0;
    strip_view_scroll_lanes(d, 12, 90.0f, 100);
    check("scroll clamps high", d.lane0 == 7, "");
    strip_view_scroll_lanes(d, 12, 90.0f, -100);
    check("scroll clamps low", d.lane0 == 0, "");
}

static void layout_sums() {
    StripModel m;
    m.deck_enabled = true;
    m.rail_enabled = true;
    m.bands_enabled = true;
    m.lanes.resize(3);
    m.bands.resize(2);
    strip_view_t v;
    v.px_h = 400;
    v.lane_h = 18;
    StripLayout L = strip_layout(m, v);
    check("layout: channels sum to px_h",
          L.deck_h + L.rail_h + L.bands_h + L.ribbon_h == v.px_h,
          "no unowned pixels");
    check("layout: order", L.deck_y0 == 0 && L.rail_y0 == L.deck_h &&
                               L.bands_y0 == L.rail_y0 + L.rail_h &&
                               L.ribbon_y0 == L.bands_y0 + L.bands_h,
          "deck, rail, bands, ribbon — top to bottom");
    check("layout: equal band heights", L.band_h == L.bands_h / 2.0f,
          "a band's height encodes nothing");
    check("layout: deck fits its lanes", L.deck_h == 3 * v.lane_h,
          "3 lanes need no cap at 400px");

    StripModel none;
    none.deck_enabled = false;
    none.deck_reason = "x";
    none.rail_enabled = false;
    none.rail_reason = "x";
    none.bands_enabled = false;
    none.bands_reason = "x";
    StripLayout L2 = strip_layout(none, v);
    check("layout: disabled channels still sum",
          L2.deck_h + L2.rail_h + L2.bands_h + L2.ribbon_h == v.px_h,
          "absent channels shrink to note rows; the sum invariant holds");
}

static void pinned_strings() {
    check("axis label pinned",
          std::string(StripModel::axis_label()) == "stream order — not time",
          "the axis's own honesty claim, verbatim");
    check("mem tid note pinned",
          std::string(StripModel::mem_tid_note()) ==
              "mem carries no tid — access marks are r/w-hued, never "
              "thread-hued",
          "the legend's no-inference claim, verbatim");
}

int main() {
    camera_math();
    layout_sums();
    pinned_strings();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
```

- [ ] **Step 2: Register the test in mk/desktop.mk and verify it fails**

In `mk/desktop.mk`: append `$(BUILD)/desktop_test_strip_model \` to the `DESKTOP_TESTS` list (alphabetical-ish, near `desktop_test_scene2d`, ~line 1250), and add a link rule next to the `desktop_test_scene2d` rule (~line 1655):

```make
$(BUILD)/desktop_test_strip_model: $(BUILD)/desktop/test/t/test_strip_model.o \
    $(BUILD)/desktop/test/vw/strip.o \
    $(BUILD)/desktop/test/src/nav.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

(The `vw/%.o` pattern rule at ~line 397 already covers `desktop/src/views/strip.cpp`; `$(DESKTOP_TEST_DOC)` brings recording/streams/union; `nav.o` is for `dt_link` in `strip_click_link`.)

Run: `make build/desktop_test_strip_model`
Expected: COMPILE FAILURE — `views/strip.h` does not exist.

- [ ] **Step 3: Write strip.h (the full header above) and strip.cpp with camera/layout implemented, builders stubbed**

`desktop/src/views/strip.cpp` — implement camera + layout + prim name now; leave `strip_build` / `strip_plan` / `strip_hover_text` / `strip_click_link` as minimal stubs (empty model with reasons unset, empty plan) — Tasks 3–7 replace them test-first:

```cpp
// strip.cpp — the pure builder, planner and camera math of strip.h. No ImGui,
// no GL, no I/O, no engine (D4).
#include "views/strip.h"

#include <algorithm>
#include <map>

namespace asmdesk {

void strip_view_window(const strip_view_t &v, double *lo, double *hi) {
    *lo = v.seq0;
    *hi = v.seq0 + v.seq_per_px * static_cast<double>(v.px_w);
}

void strip_view_set_window(strip_view_t &v, double lo, double hi) {
    if (lo < 0)
        lo = 0;
    if (hi <= lo || v.px_w <= 0)
        return; // an empty or inverted window is a no-op, never a div-by-zero
    v.seq0 = lo;
    v.seq_per_px = (hi - lo) / static_cast<double>(v.px_w);
}

void strip_view_follow(strip_view_t &v, uint64_t seq_end) {
    const double span = v.seq_per_px * static_cast<double>(v.px_w);
    v.seq0 = std::max(0.0, static_cast<double>(seq_end) - span);
}

int strip_view_lanes_full(const strip_view_t &v, float deck_h) {
    if (v.lane_h <= 0)
        return 0;
    return static_cast<int>(deck_h / v.lane_h);
}

int strip_view_lane_max(const strip_view_t &v, int lane_count, float deck_h) {
    const int full = strip_view_lanes_full(v, deck_h);
    return std::max(0, lane_count - full);
}

void strip_view_scroll_lanes(strip_view_t &v, int lane_count, float deck_h,
                             int delta) {
    const int mx = strip_view_lane_max(v, lane_count, deck_h);
    v.lane0 = std::min(mx, std::max(0, v.lane0 + delta));
}

// Channel heights: a disabled channel is a 14px note row (its verbatim reason
// draws there — quietly absent is indistinguishable from "nothing happened");
// the rail is a fixed 24px band; the ribbon a fixed 14px; the deck takes
// lane_h per lane up to 40% of px_h; the bands take every remaining pixel.
StripLayout strip_layout(const StripModel &m, const strip_view_t &v) {
    StripLayout L;
    const float note_h = 14.0f;
    if (m.deck_enabled) {
        const int cap = std::max(
            1, static_cast<int>((0.4f * v.px_h) / std::max(1.0f, v.lane_h)));
        L.lanes_visible =
            std::min<int>(static_cast<int>(m.lanes.size()), cap);
        L.deck_h = static_cast<float>(L.lanes_visible) * v.lane_h;
    } else {
        L.deck_h = note_h;
    }
    L.rail_h = m.rail_enabled ? 24.0f : note_h;
    L.ribbon_h = note_h;
    L.deck_y0 = 0;
    L.rail_y0 = L.deck_h;
    L.bands_y0 = L.rail_y0 + L.rail_h;
    L.bands_h = std::max(0.0f, v.px_h - L.deck_h - L.rail_h - L.ribbon_h);
    L.ribbon_y0 = L.bands_y0 + L.bands_h;
    L.band_h =
        L.bands_h / static_cast<float>(std::max<size_t>(1, m.bands.size()));
    return L;
}

const char *strip_prim_name(strip_prim k) {
    switch (k) {
    case strip_prim::lane_header: return "lane_header";
    case strip_prim::group_header: return "group_header";
    case strip_prim::lane_density: return "lane_density";
    case strip_prim::lane_sys_tick: return "lane_sys_tick";
    case strip_prim::rail_frame: return "rail_frame";
    case strip_prim::rail_tick: return "rail_tick";
    case strip_prim::rail_overflow: return "rail_overflow";
    case strip_prim::band_frame: return "band_frame";
    case strip_prim::band_label: return "band_label";
    case strip_prim::gap_notch: return "gap_notch";
    case strip_prim::mem_mark: return "mem_mark";
    case strip_prim::mem_envelope: return "mem_envelope";
    case strip_prim::pc_mark: return "pc_mark";
    case strip_prim::run_seam: return "run_seam";
    case strip_prim::run_tint: return "run_tint";
    case strip_prim::torn_edge: return "torn_edge";
    case strip_prim::hud_note: return "hud_note";
    case strip_prim::channel_absent: return "channel_absent";
    }
    return "?";
}

StripModel strip_build(const Recording &, const std::vector<space::Region> &,
                       const std::vector<StripSeam> &) {
    return StripModel{}; // Task 3-6 build this test-first
}

size_t strip_plan(const StripModel &, const strip_view_t &,
                  std::vector<strip_prim_t> *out) {
    out->clear(); // Task 7 builds this test-first
    return 0;
}

std::string strip_plan_dump(const std::vector<strip_prim_t> &prims) {
    std::string s;
    char buf[160];
    for (const auto &p : prims) {
        std::snprintf(buf, sizeof buf, "%s %.1f,%.1f..%.1f,%.1f a=%u b=%u %s\n",
                      strip_prim_name(p.kind), p.x0, p.y0, p.x1, p.y1, p.a, p.b,
                      p.text.c_str());
        s += buf;
    }
    return s;
}

std::string strip_hover_text(const StripModel &, const strip_prim_t &) {
    return std::string(); // Task 7
}

std::optional<dt_link> strip_click_link(const StripModel &,
                                        const strip_prim_t &,
                                        const std::string &) {
    return std::nullopt; // Task 7
}

} // namespace asmdesk
```

(`run_tint` intentionally missing from a switch would warn — the repo builds with `-Werror=switch-enum` in cli/; desktop may not, but keep the switch total anyway.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `make build/desktop_test_strip_model && ./build/desktop_test_strip_model`
Expected: `ok`, exit 0.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.h desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp mk/desktop.mk
git commit -m "desktop/views: session strip skeleton — types, camera math, layout (pure)"
git pull --rebase origin main && git push origin main
```

---

### Task 3: strip_build — thread lanes, tid discovery, topo grouping

**Files:**
- Modify: `desktop/src/views/strip.cpp` (replace the `strip_build` stub incrementally)
- Modify: `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Consumes: `StripModel`/`StripLane` from Task 2; `Recording::by_kind`, `Event::seq/body` (`doc/recording.h`).
- Produces: `strip_build` fills `lanes`, `lane_activity`, `multi_tgid`, `deck_enabled/deck_reason`, `seq_end`.

- [ ] **Step 1: Write the failing tests**

Add to `test_strip_model.cpp` (and call from `main`). The NDJSON helper mirrors `test_scene2d.cpp`'s `mk_rec`, with the golden corpus's real header line:

```cpp
static const char *kHdr =
    R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"})";

static Recording mk_rec(std::initializer_list<const char *> lines) {
    std::string nd = std::string(kHdr) + "\n";
    for (const char *l : lines) {
        nd += l;
        nd += "\n";
    }
    std::istringstream in(nd);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

static void lanes_discovery_and_grouping() {
    // Two processes (tgid 10: tids 10,11; tgid 20: tid 20), discovered from
    // tid-bearing events, labelled from the LAST topo snapshot.
    Recording r = mk_rec({
        R"({"k":"trace","basis":"abs","off":4096,"tid":10})",
        R"({"k":"trace","basis":"abs","off":4100,"tid":11})",
        R"({"k":"trace","basis":"abs","off":4104,"tid":20})",
        R"({"k":"topo","mode":"syscalls","tasks":[{"tid":99,"tgid":99,"ppid":1,"leader":true,"comm":"stale","exe":"stale","inv":1}]})",
        R"({"k":"topo","mode":"syscalls","tasks":[{"tid":10,"tgid":10,"ppid":1,"leader":true,"comm":"alpha","exe":"alpha","inv":3},{"tid":11,"tgid":10,"ppid":1,"leader":false,"comm":"alpha-w","exe":"","inv":2},{"tid":20,"tgid":20,"ppid":10,"leader":true,"comm":"beta","exe":"beta","inv":1}]})",
        R"({"k":"end","events":5})",
    });
    StripModel m = strip_build(r, {}, {});
    check("deck enabled", m.deck_enabled, "tid-bearing events exist");
    check("three lanes", m.lanes.size() == 3, "tids 10, 11, 20");
    check("lane order (tgid, leader, tid)",
          m.lanes[0].tid == 10 && m.lanes[1].tid == 11 && m.lanes[2].tid == 20,
          "grouped by tgid; leader first inside a group");
    check("labels from LAST topo snapshot",
          m.lanes[0].label == "alpha [10]" && m.lanes[2].label == "beta [20]",
          "the stale first snapshot must not label anything");
    check("multi tgid flags grouping", m.multi_tgid, "two known tgids");
    check("group heads", m.lanes[0].group_head && m.lanes[2].group_head &&
                             !m.lanes[1].group_head,
          "first lane of each tgid group");
    check("group label", m.lanes[0].group_label == "alpha [10]",
          "comm [tgid] on the head row");
    check("activity recorded per lane",
          m.lane_activity.size() == 3 && m.lane_activity[0].size() == 1,
          "one trace event for tid 10");
    check("seq_end covers the stream", m.seq_end == r.event_count(),
          "the axis extent is the whole stream");
}

static void lanes_single_stream_and_unknown() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"trace","basis":"rel","off":4})",
        R"({"k":"end","events":2})",
    });
    StripModel m = strip_build(r, {}, {});
    check("single-stream lane", m.lanes.size() == 1 && m.lanes[0].tid == -1,
          "no tids anywhere → ONE lane, never hidden");
    check("single-stream label", m.lanes[0].label == "(single stream)", "");
    check("no grouping", !m.multi_tgid, "");

    Recording bare = mk_rec({R"({"k":"note","text":"x"})", R"({"k":"end","events":1})"});
    StripModel mb = strip_build(bare, {}, {});
    check("deck disabled without activity", !mb.deck_enabled,
          "no trace/call/watch events at all");
    check("deck reason verbatim",
          mb.deck_reason ==
              "no trace/call/watch events in this recording — there is no "
              "thread activity to lane",
          "a quietly absent channel is indistinguishable from one that found "
          "nothing");
    check("unknown-tgid label is bare tid", true,
          "covered by grouping test's absence: a tid with no topo task keeps "
          "label \"[tid]\"");
}
```

Also add one check with a tid that no topo task names (e.g. a fourth `trace` with `"tid":30` in the first fixture): expect `label == "[30]"`, `tgid == -1`, sorted AFTER known groups (unknown-tgid lanes go last, ascending tid).

- [ ] **Step 2: Run to verify the new checks fail**

Run: `make build/desktop_test_strip_model && ./build/desktop_test_strip_model`
Expected: FAIL lines for every new check (stub returns an empty model).

- [ ] **Step 3: Implement lane discovery in strip_build**

Replace the stub's lane portion in `strip.cpp`:

```cpp
namespace {
// json field helpers, mirroring the get()-style reads the other views use
inline bool jint(const nlohmann::json &b, const char *k, int64_t *out) {
    auto it = b.find(k);
    if (it == b.end() || !it->is_number_integer())
        return false;
    *out = it->get<int64_t>();
    return true;
}
inline bool juint(const nlohmann::json &b, const char *k, uint64_t *out) {
    auto it = b.find(k);
    if (it == b.end() || !it->is_number())
        return false;
    *out = it->get<uint64_t>();
    return true;
}
inline std::string jstr(const nlohmann::json &b, const char *k) {
    auto it = b.find(k);
    return (it != b.end() && it->is_string()) ? it->get<std::string>()
                                              : std::string();
}
} // namespace

StripModel strip_build(const Recording &r,
                       const std::vector<space::Region> &regions,
                       const std::vector<StripSeam> &capture_seams) {
    StripModel m;
    m.seq_end = r.event_count();
    m.torn = r.torn;
    m.truncated = r.truncated;             // verify member names against
    m.drops_lost = r.drops_lost;           // doc/recording.h — the footer
    m.drops_throttled = r.drops_throttled; // lift (recording.cpp:183-213);
                                           // adjust spellings to the code.

    // --- lanes: tids discovered from what the strip draws ------------------
    std::map<int64_t, std::vector<uint64_t>> activity; // tid → sorted seqs
    size_t activity_events = 0;
    for (const char *kind : {"trace", "call", "watch"}) {
        auto it = r.by_kind.find(kind);
        if (it == r.by_kind.end())
            continue;
        for (const Event &e : it->second) {
            activity_events++;
            int64_t tid = -1;
            jint(e.body, "tid", &tid);
            activity[tid].push_back(e.seq);
        }
    }
    // syscalls with a tid create a lane too (a v2 writer), but carry no
    // activity count — the rail tick is their mark. Collected in Task 4;
    // here only ensure the lane exists.
    if (auto it = r.by_kind.find("syscall"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            int64_t tid = -1;
            if (jint(e.body, "tid", &tid))
                activity.emplace(tid, std::vector<uint64_t>{});
        }

    if (activity_events == 0 && activity.empty()) {
        m.deck_reason = "no trace/call/watch events in this recording — "
                        "there is no thread activity to lane";
    } else {
        m.deck_enabled = true;
        // the LAST topo snapshot names tasks (topo.h:64-67 — merging several
        // would invent a tree that never existed at any one time)
        struct Task { long tgid = -1; bool leader = false; std::string comm; };
        std::map<int64_t, Task> tasks;
        if (auto it = r.by_kind.find("topo");
            it != r.by_kind.end() && !it->second.empty()) {
            const Event &last = it->second.back();
            auto ts = last.body.find("tasks");
            if (ts != last.body.end() && ts->is_array())
                for (const auto &t : *ts) {
                    int64_t tid = -1;
                    if (!jint(t, "tid", &tid))
                        continue;
                    Task k;
                    int64_t tg = -1;
                    jint(t, "tgid", &tg);
                    k.tgid = static_cast<long>(tg);
                    auto ld = t.find("leader");
                    k.leader = ld != t.end() && ld->is_boolean() &&
                               ld->get<bool>();
                    k.comm = jstr(t, "comm");
                    tasks[tid] = k;
                }
        }
        // sort key: known tgids first (grouped, leader first, tid asc),
        // then unknown-tgid lanes ascending tid, then the -1 stream lane last
        std::vector<int64_t> tids;
        for (auto &kv : activity)
            tids.push_back(kv.first);
        std::sort(tids.begin(), tids.end(), [&](int64_t A, int64_t B) {
            auto ka = tasks.find(A), kb = tasks.find(B);
            const bool ha = ka != tasks.end(), hb = kb != tasks.end();
            if (ha != hb)
                return ha; // known before unknown
            if (ha) {
                if (ka->second.tgid != kb->second.tgid)
                    return ka->second.tgid < kb->second.tgid;
                if (ka->second.leader != kb->second.leader)
                    return ka->second.leader; // leader first
            }
            return A < B;
        });
        long seen_tgid = -2;
        std::size_t known_tgids = 0;
        {
            std::vector<long> gs;
            for (auto &kv : tasks)
                gs.push_back(kv.second.tgid);
            std::sort(gs.begin(), gs.end());
            known_tgids = std::unique(gs.begin(), gs.end()) - gs.begin();
        }
        m.multi_tgid = known_tgids > 1;
        for (int64_t tid : tids) {
            StripLane ln;
            ln.tid = tid;
            auto k = tasks.find(tid);
            if (tid == -1) {
                ln.label = "(single stream)";
            } else if (k != tasks.end()) {
                ln.tgid = k->second.tgid;
                ln.leader = k->second.leader;
                ln.label = k->second.comm + " [" + std::to_string(tid) + "]";
                if (k->second.tgid != seen_tgid) {
                    ln.group_head = true;
                    ln.group_label =
                        k->second.comm + " [" + std::to_string(k->second.tgid) + "]";
                    seen_tgid = k->second.tgid;
                }
            } else {
                ln.label = "[" + std::to_string(tid) + "]";
            }
            m.lanes.push_back(std::move(ln));
            auto &v = activity[tid];
            std::sort(v.begin(), v.end());
            m.lane_activity.push_back(std::move(v));
        }
    }
    // (rail / bands / seams / hud: Tasks 4-6)
    return m;
}
```

**Verify against the real code as you go:** the exact `Recording` member spellings for the footer lift (`torn`, `truncated`, drops) live at `desktop/src/doc/recording.h` / `recording.cpp:183-223` — use the code's names, and if a field (e.g. drops) is nested, read it the way `shell.cpp` does. The json type is whatever `Event::body` is (`nlohmann::json` — see `crossing.cpp`'s `e.body.find`); mirror its accessor style.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make build/desktop_test_strip_model && ./build/desktop_test_strip_model`
Expected: `ok`.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp
git commit -m "desktop/views: strip lanes — tid discovery, last-topo labels, tgid grouping"
git pull --rebase origin main && git push origin main
```

---

### Task 4: strip_build — kernel rail

**Files:**
- Modify: `desktop/src/views/strip.cpp`, `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Consumes: `syscall_name_of` / `syscall_class_of` / `syscall_outcome_of` (Task 1); `StripSys` (Task 2).
- Produces: `m.sys` (sorted by seq), `rail_enabled/rail_reason`, `StripSys::lane` back-refs.

- [ ] **Step 1: Write the failing tests**

```cpp
static void rail_rows() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"abs","off":4096,"tid":10})",
        R"({"k":"syscall","line":"[10] openat(AT_FDCWD, <path>) = 3","tid":10})",
        R"({"k":"syscall","line":"write(1, <14 bytes>) = -9","payload":"AAAABBBBCCCCDD"})",
        R"({"k":"syscall","line":"zzz_mystery(1) = ?"})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, {}, {});
    check("rail enabled", m.rail_enabled, "syscall events exist and have seq");
    check("three rows", m.sys.size() == 3, "");
    check("rows sorted by seq",
          m.sys[0].seq < m.sys[1].seq && m.sys[1].seq < m.sys[2].seq, "");
    check("class from shared parse",
          m.sys[0].cls == space::SyscallClass::File &&
              m.sys[2].cls == space::SyscallClass::Other,
          "openat → File; a miss stays in the visible grey bucket");
    check("outcome from shared parse",
          m.sys[0].outcome == space::SyscallOutcome::Ok &&
              m.sys[1].outcome == space::SyscallOutcome::Error &&
              m.sys[2].outcome == space::SyscallOutcome::Unknown,
          "");
    check("tid-ful syscall maps to its lane",
          m.sys[0].tid == 10 && m.sys[0].lane == 0,
          "a v2 writer's tid joins the thread lane");
    check("tid-less syscall is rail-only",
          m.sys[1].tid == -1 && m.sys[1].lane == -1,
          "v1 writers omit tid — never guessed into a lane");
    check("payload counted, never copied",
          m.sys[1].has_payload && m.sys[1].payload_bytes == 14 &&
              m.sys[1].line.find("AAAA") == std::string::npos,
          "count only; the line is the payload-free rendering");

    Recording none = mk_rec({R"({"k":"trace","basis":"rel","off":0})",
                             R"({"k":"end","events":1})"});
    StripModel mn = strip_build(none, {}, {});
    check("rail disabled without syscalls", !mn.rail_enabled, "");
    check("rail reason verbatim",
          mn.rail_reason == "no syscall events in this recording",
          "stated, never quietly absent");
}
```

Note on the seq-present rule: `Event::seq` is assigned by the LOADER as stream position (`recording.h:66-75`), so a loaded recording always has real seqs — the `SyscallView::seq_present` guard exists for view-level rows whose *recorded* seq field may predate the schema. The strip reads loader seqs directly, so its rail is orderable whenever syscall events exist; no seq_present reason is needed. State this in a comment in `strip.cpp` so a reviewer doesn't re-add the guard.

- [ ] **Step 2: Run to verify the new checks fail**

Run: `./build/desktop_test_strip_model` after `make build/desktop_test_strip_model`
Expected: FAIL lines for the rail checks.

- [ ] **Step 3: Implement the rail in strip_build**

Insert after the lane block (before `return m;`):

```cpp
    // --- kernel rail: every syscall, at its OWN Event::seq ------------------
    // (loader-assigned stream position — no anchor approximation needed; the
    // crossing layer's seq_present guard is about a RECORDED seq field on
    // rows, which the strip does not read.)
    if (auto it = r.by_kind.find("syscall");
        it != r.by_kind.end() && !it->second.empty()) {
        m.rail_enabled = true;
        size_t row = 0;
        for (const Event &e : it->second) {
            StripSys s;
            s.row = row++;
            s.seq = e.seq;
            s.line = jstr(e.body, "line");
            int64_t tid = -1;
            if (jint(e.body, "tid", &tid))
                s.tid = tid;
            auto pl = e.body.find("payload");
            if (pl != e.body.end() && pl->is_string()) {
                s.has_payload = true;
                s.payload_bytes = pl->get<std::string>().size();
            }
            s.cls = syscall_class_of(syscall_name_of(s.line));
            s.outcome = syscall_outcome_of(s.line);
            if (s.tid != -1)
                for (size_t i = 0; i < m.lanes.size(); i++)
                    if (m.lanes[i].tid == s.tid) {
                        s.lane = static_cast<int>(i);
                        break;
                    }
            m.sys.push_back(std::move(s));
        }
        std::sort(m.sys.begin(), m.sys.end(),
                  [](const StripSys &a, const StripSys &b) {
                      return a.seq < b.seq;
                  });
    } else {
        m.rail_reason = "no syscall events in this recording";
    }
```

Add `#include "views/syscall_classify.h"` to strip.cpp.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/desktop_test_strip_model` → `ok`.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp
git commit -m "desktop/views: strip kernel rail — shared classify, tid-less rows stay rail-only"
git pull --rebase origin main && git push origin main
```

---

### Task 5: strip_build — address bands, mem/pc marks, off_band counting

**Files:**
- Modify: `desktop/src/views/strip.cpp`, `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Consumes: `space::Region` (`space/types.h:19-48` — `base`, `len`, `kind`, `label`); `StripBand`/`StripMemMark`/`StripPcMark` (Task 2).
- Produces: `m.bands` (sorted by base), `m.mem`, `m.pc` (placed, sorted by seq), `off_band_mem/off_band_pc`, `bands_enabled/bands_reason`, `m.hud`.

**Placement rules (the projection.cpp:445-450 precedent — count, never place raw):**
- `mem` with `space=="abs"`: place `ea` into the band containing it; no band → `off_band_mem++`. `mem` with `space=="off"`: region-relative with no region identity on the event → `off_band_mem++` (counted, never placed raw).
- `trace` with `basis=="abs"`: place `off` as an absolute pc. `basis=="rel"`: place at `code.base + off` iff EXACTLY ONE `Code`-kind band exists, else `off_band_pc++`.
- `df_step`: place `rbase + off` when the event carries `rbase`; else the same single-Code-band rule.

- [ ] **Step 1: Write the failing tests**

```cpp
static std::vector<space::Region> two_bands() {
    space::Region code;
    code.base = 0x1000;
    code.len = 0x1000;
    code.kind = space::Region::Kind::Code; // verify enum spelling in types.h
    code.label = "code";
    space::Region data;
    data.base = 0x200000;
    data.len = 0x10000;
    data.kind = space::Region::Kind::Data;
    data.label = "observed data";
    return {code, data};
}

static void bands_and_marks() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"rel","off":16,"tid":10})",
        R"({"k":"df_step","step":0,"off":20,"rbase":4096,"ops":[]})",
        R"({"k":"mem","step":0,"ea":2097160,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"mem","step":0,"ea":16,"size":4,"rw":"r","space":"off"})",
        R"({"k":"mem","step":0,"ea":999999999,"size":8,"rw":"r","space":"abs"})",
        R"({"k":"end","events":5})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    check("bands enabled", m.bands_enabled, "regions were passed");
    check("bands sorted by base",
          m.bands.size() == 2 && m.bands[0].region.base == 0x1000, "");
    check("abs mem placed",
          m.mem.size() == 1 && m.mem[0].band == 1 && m.mem[0].is_write &&
              m.mem[0].addr == 2097160,
          "ea 0x200008 lands in the data band");
    check("off-space and off-band mem COUNTED", m.off_band_mem == 2,
          "space:\"off\" is counted, never placed raw; an unmapped abs "
          "address is counted too");
    check("rel pc placed via the single Code band",
          !m.pc.empty() && m.pc[0].band == 0 && m.pc[0].addr == 0x1000 + 16,
          "basis:rel resolves against the ONE Code band");
    check("df_step pc placed via rbase",
          m.pc.size() == 2 && m.pc[1].addr == 4096 + 20,
          "rbase+off, the df_step's own region identity");
    check("pc marks keep tid", m.pc[0].tid == 10 && m.pc[1].tid == -1,
          "df_step never has a tid");
    check("hud states the counts",
          m.hud.find("2 mem access(es) off-band") != std::string::npos,
          "counted facts are stated, not dropped");

    StripModel nb = strip_build(r, {}, {});
    check("no regions → bands disabled", !nb.bands_enabled, "");
    check("bands reason verbatim",
          nb.bands_reason ==
              "no regions to band — the caller assembled no codeimage, "
              "observed-data or vmmap regions",
          "");
    check("disabled bands still count", nb.off_band_mem == 3 && nb.pc.empty(),
          "nothing places when no band exists; everything is counted");
}
```

- [ ] **Step 2: Run to verify the new checks fail**

Run: `./build/desktop_test_strip_model` → FAIL lines for bands checks.

- [ ] **Step 3: Implement bands + marks in strip_build**

```cpp
    // --- address bands + marks ----------------------------------------------
    for (const auto &rg : regions)
        m.bands.push_back(StripBand{rg});
    std::sort(m.bands.begin(), m.bands.end(),
              [](const StripBand &a, const StripBand &b) {
                  return a.region.base < b.region.base;
              });
    auto band_of = [&](uint64_t addr) -> int {
        for (size_t i = 0; i < m.bands.size(); i++)
            if (addr >= m.bands[i].region.base &&
                addr < m.bands[i].region.base + m.bands[i].region.len)
                return static_cast<int>(i);
        return -1;
    };
    int code_band = -1, code_bands = 0;
    for (size_t i = 0; i < m.bands.size(); i++)
        if (m.bands[i].region.kind == space::Region::Kind::Code) {
            code_band = static_cast<int>(i);
            code_bands++;
        }
    if (code_bands != 1)
        code_band = -1; // the single-Code-band rule: 0 or >1 → no rel placement
    if (m.bands.empty())
        m.bands_reason = "no regions to band — the caller assembled no "
                         "codeimage, observed-data or vmmap regions";
    else
        m.bands_enabled = true;

    if (auto it = r.by_kind.find("mem"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            uint64_t ea = 0, size = 0, step = 0;
            juint(e.body, "ea", &ea);
            juint(e.body, "size", &size);
            juint(e.body, "step", &step);
            const bool abs = jstr(e.body, "space") == "abs";
            const int band = abs ? band_of(ea) : -1;
            if (!abs || band < 0) {
                m.off_band_mem++; // counted, never placed raw
                continue;
            }
            StripMemMark mk;
            mk.seq = e.seq;
            mk.addr = ea;
            mk.size = size;
            mk.step = static_cast<uint32_t>(step);
            mk.is_write = jstr(e.body, "rw") == "w";
            mk.band = band;
            m.mem.push_back(mk);
        }
    auto place_pc = [&](const Event &e, bool is_df) {
        uint64_t off = 0;
        if (!juint(e.body, "off", &off))
            return;
        uint64_t abs_addr = 0;
        bool placed = false;
        uint64_t rbase = 0;
        if (is_df && juint(e.body, "rbase", &rbase)) {
            abs_addr = rbase + off;
            placed = true;
        } else if (!is_df && jstr(e.body, "basis") == "abs") {
            abs_addr = off;
            placed = true;
        } else if (code_band >= 0) {
            abs_addr = m.bands[static_cast<size_t>(code_band)].region.base + off;
            placed = true;
        }
        const int band = placed ? band_of(abs_addr) : -1;
        if (band < 0) {
            m.off_band_pc++;
            return;
        }
        StripPcMark p;
        p.seq = e.seq;
        p.addr = abs_addr;
        p.band = band;
        int64_t tid = -1;
        if (!is_df && jint(e.body, "tid", &tid))
            p.tid = tid;
        m.pc.push_back(p);
    };
    if (auto it = r.by_kind.find("trace"); it != r.by_kind.end())
        for (const Event &e : it->second)
            place_pc(e, false);
    if (auto it = r.by_kind.find("df_step"); it != r.by_kind.end())
        for (const Event &e : it->second)
            place_pc(e, true);
    std::sort(m.mem.begin(), m.mem.end(),
              [](const StripMemMark &a, const StripMemMark &b) {
                  return a.seq < b.seq;
              });
    std::sort(m.pc.begin(), m.pc.end(),
              [](const StripPcMark &a, const StripPcMark &b) {
                  return a.seq < b.seq;
              });
```

And build the HUD line at the END of strip_build (after Task 6's seams, keep it last; grow it now with what exists):

```cpp
    // --- the one-line honesty summary ---------------------------------------
    {
        std::string h;
        h += std::to_string(m.mem.size()) + " access(es), " +
             std::to_string(m.sys.size()) + " syscall(s), " +
             std::to_string(m.lanes.size()) + " lane(s)";
        if (m.off_band_mem)
            h += " · " + std::to_string(m.off_band_mem) +
                 " mem access(es) off-band";
        if (m.off_band_pc)
            h += " · " + std::to_string(m.off_band_pc) + " pc mark(s) off-band";
        if (m.truncated)
            h += " · truncated";
        if (m.drops_lost || m.drops_throttled)
            h += " · ring dropped " +
                 std::to_string(m.drops_lost + m.drops_throttled) +
                 " (tail-drop)";
        if (m.torn)
            h += " · torn (no footer)";
        m.hud = h;
    }
```

Check `space::Region`'s actual kind enum spelling (`space/types.h:19-48` says `kind{Code,Stack,Heap,Data,Mmap,Unknown}`) and member names before compiling; the test fixture must construct it the way `shell.cpp:1345-1385` does.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/desktop_test_strip_model` → `ok`.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp
git commit -m "desktop/views: strip address bands — placed marks, counted off-band"
git pull --rebase origin main && git push origin main
```

---

### Task 6: strip_build — run seams

**Files:**
- Modify: `desktop/src/views/strip.cpp`, `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Consumes: `StripSeam`/`StripRunSeam`/`StripSeamKind` (Task 2).
- Produces: `m.seams` sorted by seq; `StripMemMark::pass` back-fill.

**Seam derivations (exactly three, labelled by kind, never blurred):**
1. `df_invocation` events: seam at the MARKER's seq (the marker precedes its pass's block); label `"pass <pass> = <result>, <steps> steps"`; `steps==0` → `armed_waiting=true`, label `"pass <pass> — armed, region quiet"`. `result` is a NUMBER (the routine return value — schema `df_invocation`).
2. `coverage` events: a coverage event CLOSES the `[trace…]` block before it (`region.h:11-16`) — seam at the coverage event's seq + 1 (the boundary is AFTER the closer), label `"coverage close"`, kind `CoverageClose`.
3. `capture_seams` input: kind `Capture`, verbatim label.

- [ ] **Step 1: Write the failing tests**

```cpp
static void run_seams() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":42,"steps":8,"truncated":false})",
        R"({"k":"df_step","step":0,"off":0,"ops":[]})",
        R"({"k":"df_invocation","pass":1,"result":0,"steps":0,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"coverage","basis":"rel","blocks":[0]})",
        R"({"k":"end","events":5})",
    });
    StripModel m = strip_build(r, {}, {{3, "capture 2"}});
    check("three derivations present", m.seams.size() == 4,
          "2 invocation + 1 coverage-close + 1 capture");
    check("seams sorted by seq",
          std::is_sorted(m.seams.begin(), m.seams.end(),
                         [](const StripRunSeam &a, const StripRunSeam &b) {
                             return a.seq < b.seq;
                         }),
          "");
    check("invocation label carries pass/result/steps",
          m.seams[0].kind == StripSeamKind::Invocation &&
              m.seams[0].label == "pass 0 = 42, 8 steps",
          "result is a NUMBER (routine return), not a status word");
    check("steps:0 is armed-and-waiting, not a verdict",
          m.seams[1].armed_waiting &&
              m.seams[1].label == "pass 1 — armed, region quiet",
          "39 T4");
    check("capture seam verbatim", [&] {
        for (auto &s : m.seams)
            if (s.kind == StripSeamKind::Capture)
                return s.label == std::string("capture 2");
        return false;
    }(), "the caller's ordinal label passes through");
    check("coverage closes the block BEFORE it", [&] {
        for (auto &s : m.seams)
            if (s.kind == StripSeamKind::CoverageClose)
                return s.seq > 0;
        return false;
    }(), "seam sits after the closer");

    // pass back-fill on mem marks: a mem event after marker 0 and before
    // marker 1 belongs to pass 0
    Recording r2 = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":1,"steps":2,"truncated":false})",
        R"({"k":"mem","step":1,"ea":4200,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"df_invocation","pass":1,"result":1,"steps":2,"truncated":false})",
        R"({"k":"mem","step":0,"ea":4208,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"end","events":4})",
    });
    StripModel m2 = strip_build(r2, two_bands(), {});
    check("mem pass back-fill", m2.mem.size() == 2 && m2.mem[0].pass == 0 &&
                                    m2.mem[1].pass == 1,
          "same step value, different pass — the marker is the discriminator");
}
```

(Adjust `two_bands()`'s code band so `ea` 4200/4208 land in it: they do — code is [0x1000, 0x2000).)

- [ ] **Step 2: Run to verify the new checks fail**

Run: `./build/desktop_test_strip_model` → FAIL lines for seam checks.

- [ ] **Step 3: Implement seams in strip_build**

Insert before the HUD block:

```cpp
    // --- run seams: three derivations, labelled by kind ---------------------
    if (auto it = r.by_kind.find("df_invocation"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            StripRunSeam s;
            s.kind = StripSeamKind::Invocation;
            s.seq = e.seq;
            uint64_t pass = 0, steps = 0;
            int64_t result = 0;
            juint(e.body, "pass", &pass);
            juint(e.body, "steps", &steps);
            jint(e.body, "result", &result);
            auto tr = e.body.find("truncated");
            s.truncated =
                tr != e.body.end() && tr->is_boolean() && tr->get<bool>();
            if (steps == 0) {
                s.armed_waiting = true;
                s.label = "pass " + std::to_string(pass) +
                          " — armed, region quiet";
            } else {
                s.label = "pass " + std::to_string(pass) + " = " +
                          std::to_string(result) + ", " +
                          std::to_string(steps) + " steps";
            }
            m.seams.push_back(std::move(s));
        }
    if (auto it = r.by_kind.find("coverage"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            StripRunSeam s;
            s.kind = StripSeamKind::CoverageClose;
            s.seq = e.seq + 1; // the boundary is AFTER the closer
            s.label = "coverage close";
            m.seams.push_back(std::move(s));
        }
    for (const StripSeam &cs : capture_seams) {
        StripRunSeam s;
        s.kind = StripSeamKind::Capture;
        s.seq = cs.seq;
        s.label = cs.label;
        m.seams.push_back(std::move(s));
    }
    std::stable_sort(m.seams.begin(), m.seams.end(),
                     [](const StripRunSeam &a, const StripRunSeam &b) {
                         return a.seq < b.seq;
                     });
    // pass back-fill: a mem mark belongs to the last Invocation seam ≤ its seq
    {
        std::vector<std::pair<uint64_t, int32_t>> inv; // seq → pass ordinal
        int32_t ord = 0;
        for (const auto &s : m.seams)
            if (s.kind == StripSeamKind::Invocation)
                inv.emplace_back(s.seq, ord++);
        for (auto &mk : m.mem) {
            auto it2 = std::upper_bound(
                inv.begin(), inv.end(),
                std::make_pair(mk.seq, std::numeric_limits<int32_t>::max()));
            mk.pass = it2 == inv.begin() ? -1 : std::prev(it2)->second;
        }
    }
```

Add `#include <limits>` to strip.cpp. Note the pass ordinal is the Nth invocation MARKER in stream order (0-based), not the marker's own `pass` field — a live union concatenates captures whose `pass` fields both restart at 0, and `dt_link.invocation` wants the segmented index's ordinal; `desktop/src/analysis/stepindex.cpp:139` keys segments the same stream-order way. State this in a comment.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/desktop_test_strip_model` → `ok`.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp
git commit -m "desktop/views: strip run seams — invocation/coverage-close/capture, pass back-fill"
git pull --rebase origin main && git push origin main
```

---

### Task 7: strip_plan + dump + hover/click (pure)

**Files:**
- Modify: `desktop/src/views/strip.cpp`, `desktop/test/test_strip_model.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces: `strip_plan`, `strip_plan_dump`, `strip_hover_text`, `strip_click_link` — the painter (Task 8) consumes these EXACTLY as declared in Task 2.

**Plan rules (all deterministic):**
- x mapping: `x = (seq - v.seq0) / v.seq_per_px`; only prims intersecting `[0, px_w]` are emitted; iterate events via `lower_bound` on the sorted seq vectors.
- Emission order (fixed, so the dump is stable): `hud_note`, `channel_absent`*, `run_tint`*, group/lane headers, `lane_density`*, `lane_sys_tick`*, `rail_frame`, `rail_tick`/`rail_overflow`*, `band_frame`/`band_label`/`gap_notch`*, `mem_envelope`*/`mem_mark`*, `pc_mark`*, `run_seam`*, `torn_edge`.
- Mark mode iff `v.seq_per_px <= kStripEnvelopeSeqPerPx` (4.0): individual `mem_mark` (y from addr within its band, height = clamp(size px, 1..6)), `pc_mark` (2px), `lane_sys_tick`. Envelope mode: per pixel column per band per rw, one `mem_envelope` spanning min..max addr touched that column; per lane per column one `lane_density` with `b = round(count * 255 / max_count_in_window)` (max over the visible window, recomputed per plan — announce it in the hud_note text as `density max N/col`); pc collapses into the density (no pc prims in envelope mode).
- Rail: per column, up to `kStripRailTicksPerCol` (3) individual `rail_tick`s (2px wide, full rail height, `a`=sys index); more → 3 ticks + one `rail_overflow` with `text="+N"`.
- `run_tint`: background rect per inter-seam interval intersecting the window; `a` = GLOBAL run ordinal (index over all seam intervals, not window-relative) — parity must be stable while panning.
- `run_seam`: 1px full-height vertical at its x; `text` = label. `torn_edge`: at x(seq_end) when `m.torn` and it is inside the window.
- `gap_notch`: at each band's top edge when the previous band's `base+len != this band's base` (an elided gap).
- `hud_note`: ONE prim, text = `m.hud + " · " + axis_label() + " · " + mem_tid_note()`.
- `strip_hover_text`: `rail_tick`/`lane_sys_tick` → `sys[a].line + " — " + syscall_class_name(cls) + ", " + syscall_outcome_name(outcome) + ", seq N"` (+ ` [tid T]` when known); `mem_mark` → `"w 8B @ 0x200008, seq N (pass 2)"` form; `pc_mark`, `lane_header`, `run_seam`, `band_label` → their model strings; others → `""`.
- `strip_click_link`: `rail_tick`/`lane_sys_tick` → `dt_link{rec=rec_id, view=dt_view::syscalls, pid=lanes[sys[a].lane].tgid when lane>=0 && tgid!=-1}`; `mem_mark` → `dt_link{rec=rec_id, view=dt_view::timeline, step=mem[a].step, invocation=mem[a].pass when pass>=0}`; else `nullopt`.

- [ ] **Step 1: Write the failing tests**

```cpp
static void plan_determinism_and_modes() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":42,"steps":8,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":16,"tid":10})",
        R"({"k":"syscall","line":"openat(AT_FDCWD, <path>) = 3","tid":10})",
        R"({"k":"mem","step":0,"ea":4200,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    strip_view_t v;
    v.px_w = 400;
    v.px_h = 300;
    v.seq_per_px = 0.05; // mark mode (≤ 4)
    std::vector<strip_prim_t> a, b;
    strip_plan(m, v, &a);
    strip_plan(m, v, &b);
    check("plan deterministic", strip_plan_dump(a) == strip_plan_dump(b),
          "same (model, view) → byte-identical plans");
    auto has = [&](strip_prim k) {
        for (auto &p : a)
            if (p.kind == k)
                return true;
        return false;
    };
    check("mark mode emits marks",
          has(strip_prim::mem_mark) && has(strip_prim::rail_tick) &&
              has(strip_prim::pc_mark),
          "");
    check("hud carries the pinned notes", [&] {
        for (auto &p : a)
            if (p.kind == strip_prim::hud_note)
                return p.text.find(StripModel::axis_label()) != std::string::npos &&
                       p.text.find(StripModel::mem_tid_note()) != std::string::npos;
        return false;
    }(), "the axis claim and the no-tid claim ride in the plan itself");
    check("run seam emitted with label", [&] {
        for (auto &p : a)
            if (p.kind == strip_prim::run_seam)
                return p.text == "pass 0 = 42, 8 steps";
        return false;
    }(), "");
    check("syscall tick is a POINT", [&] {
        for (auto &p : a)
            if (p.kind == strip_prim::rail_tick)
                return (p.x1 - p.x0) <= 2.0f;
        return true;
    }(), "no along-axis extent a reader could mistake for a duration");

    strip_view_t vz = v;
    vz.seq_per_px = 100.0; // envelope mode
    std::vector<strip_prim_t> c;
    strip_plan(m, vz, &c);
    bool env_ok = true;
    for (auto &p : c)
        if (p.kind == strip_prim::mem_mark || p.kind == strip_prim::pc_mark)
            env_ok = false;
    check("envelope mode has no per-event marks", env_ok,
          "the doc-65 lesson: never one drawable per event above threshold");

    // hover + click
    for (auto &p : a)
        if (p.kind == strip_prim::rail_tick) {
            check("rail hover text",
                  strip_hover_text(m, p).find("openat") != std::string::npos,
                  "");
            auto lk = strip_click_link(m, p, "rec-x");
            check("rail click links to syscalls view",
                  lk && lk->view == dt_view::syscalls && lk->rec == "rec-x",
                  "");
        }
    for (auto &p : a)
        if (p.kind == strip_prim::mem_mark) {
            auto lk = strip_click_link(m, p, "rec-x");
            check("mem click links to timeline step+invocation",
                  lk && lk->view == dt_view::timeline && lk->step &&
                      *lk->step == 0 && lk->invocation && *lk->invocation == 0,
                  "step is per-pass; the invocation ordinal disambiguates");
        }
}

static void plan_stable_tint_parity() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":1,"steps":1,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"df_invocation","pass":1,"result":1,"steps":1,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":4})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    strip_view_t v;
    v.px_w = 100;
    v.px_h = 300;
    v.seq_per_px = 0.05;
    v.seq0 = 3.0; // window shows only the SECOND run
    std::vector<strip_prim_t> p;
    strip_plan(m, v, &p);
    for (auto &q : p)
        if (q.kind == strip_prim::run_tint)
            check("tint ordinal is global", q.a == 2,
                  "interval index over ALL seams (0: pre-pass0, 1: pass0, "
                  "2: pass1) — parity stable while panning");
}
```

- [ ] **Step 2: Run to verify the new checks fail**

Run: `./build/desktop_test_strip_model` → FAIL lines (stub plan is empty).

- [ ] **Step 3: Implement strip_plan / hover / click**

Structure (implement fully, following the rules above — the emission-order list is the function's section order):

```cpp
size_t strip_plan(const StripModel &m, const strip_view_t &v,
                  std::vector<strip_prim_t> *out) {
    out->clear();
    if (v.seq_per_px <= 0 || v.px_w <= 0)
        return 0;
    const StripLayout L = strip_layout(m, v);
    const double lo = v.seq0, hi = v.seq0 + v.seq_per_px * v.px_w;
    auto X = [&](double seq) {
        return static_cast<float>((seq - lo) / v.seq_per_px);
    };
    auto push = [&](strip_prim k, float x0, float y0, float x1, float y1,
                    uint32_t a, uint32_t b, std::string text) {
        out->push_back({k, x0, y0, x1, y1, a, b, std::move(text)});
    };
    const bool marks = v.seq_per_px <= kStripEnvelopeSeqPerPx;
    // 1) hud_note (+ density max when in envelope mode — compute it first)
    // 2) channel_absent rows for any disabled channel (a: 0=deck 1=rail 2=bands)
    // 3) run_tint: walk seam intervals [prev.seq, s.seq) over [0,seq_end],
    //    global ordinal a; emit only intervals intersecting [lo,hi]
    // 4) deck: for visible lanes [v.lane0, v.lane0+L.lanes_visible):
    //    group_header (2px separator + group_label) when lane.group_head &&
    //    m.multi_tgid; lane_header (text=label, left edge); then per-column
    //    lane_density from lane_activity[i] via lower_bound (envelope mode)
    //    or nothing (mark mode draws activity as pc marks in the bands);
    //    lane_sys_tick for each sys with s.lane == lane index in window
    // 5) rail: rail_frame; per-column group of sys in window (lower_bound on
    //    m.sys), ≤3 → rail_tick each (width 2px, x=X(s.seq)), else 3 +
    //    rail_overflow "+N"
    // 6) bands: band_frame + band_label per band; gap_notch when previous
    //    band's base+len != base; then mem: mark mode → mem_mark per event in
    //    window (y from addr: band_y0 + (1 - (addr-base)/len) * band_h... use
    //    TOP-DOWN: y = band_y0 + ((addr - base) / (double)len) * band_h,
    //    height clamp(size / 16.0 * 4 + 1, 1, 6) px, b bit0 = is_write);
    //    envelope mode → per column per band per rw min/max addr → one
    //    mem_envelope; pc_mark (2px) in mark mode only
    // 7) run_seam per seam in window (x 1px, full height, text=label)
    // 8) torn_edge at X(seq_end) when m.torn && seq_end in [lo,hi]
    return out->size();
}
```

Every `/* section */` above is prose describing exact geometry — write the loops out; no section may be skipped. `strip_hover_text` / `strip_click_link` per the rules in the task header:

```cpp
std::string strip_hover_text(const StripModel &m, const strip_prim_t &p) {
    switch (p.kind) {
    case strip_prim::rail_tick:
    case strip_prim::lane_sys_tick: {
        const StripSys &s = m.sys[p.a];
        std::string t = s.line + " — " +
                        space::syscall_class_name(s.cls) + ", " +
                        space::syscall_outcome_name(s.outcome) + ", seq " +
                        std::to_string(s.seq);
        if (s.tid != -1)
            t += " [tid " + std::to_string(s.tid) + "]";
        return t;
    }
    case strip_prim::mem_mark: {
        const StripMemMark &k = m.mem[p.a];
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %lluB @ 0x%llx, seq %llu",
                      k.is_write ? "w" : "r",
                      static_cast<unsigned long long>(k.size),
                      static_cast<unsigned long long>(k.addr),
                      static_cast<unsigned long long>(k.seq));
        std::string t = buf;
        if (k.pass >= 0)
            t += " (pass " + std::to_string(k.pass) + ")";
        return t;
    }
    case strip_prim::pc_mark: {
        const StripPcMark &k = m.pc[p.a];
        char buf[64];
        std::snprintf(buf, sizeof buf, "pc 0x%llx, seq %llu",
                      static_cast<unsigned long long>(k.addr),
                      static_cast<unsigned long long>(k.seq));
        std::string t = buf;
        if (k.tid != -1)
            t += " [tid " + std::to_string(k.tid) + "]";
        return t;
    }
    case strip_prim::lane_header: return m.lanes[p.a].label;
    case strip_prim::band_label: return m.bands[p.a].region.label;
    case strip_prim::run_seam: return m.seams[p.a].label;
    default: return std::string();
    }
}

std::optional<dt_link> strip_click_link(const StripModel &m,
                                        const strip_prim_t &p,
                                        const std::string &rec_id) {
    if (p.kind == strip_prim::rail_tick ||
        p.kind == strip_prim::lane_sys_tick) {
        dt_link l;
        l.rec = rec_id;
        l.view = dt_view::syscalls;
        const StripSys &s = m.sys[p.a];
        if (s.lane >= 0 && m.lanes[static_cast<size_t>(s.lane)].tgid != -1)
            l.pid = m.lanes[static_cast<size_t>(s.lane)].tgid;
        return l;
    }
    if (p.kind == strip_prim::mem_mark) {
        dt_link l;
        l.rec = rec_id;
        l.view = dt_view::timeline;
        l.step = m.mem[p.a].step;
        if (m.mem[p.a].pass >= 0)
            l.invocation = static_cast<uint32_t>(m.mem[p.a].pass);
        return l;
    }
    return std::nullopt;
}
```

(`space/crossing.h` provides `syscall_class_name`/`syscall_outcome_name` — include it via `views/syscall_classify.h`, already included.)

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./build/desktop_test_strip_model` → `ok`.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp
git commit -m "desktop/views: strip plan — pixel-space buckets, deterministic dump, hover/drill links"
git pull --rebase origin main && git push origin main
```

---

### Task 8: The painter — strip_draw.cpp + headless draw test

**Files:**
- Create: `desktop/src/views/strip_draw.cpp`
- Create: `desktop/test/test_strip_draw.cpp`
- Modify: `desktop/src/views/views_draw.h` (declare the two entry points), `mk/desktop.mk`

**Interfaces:**
- Consumes: `strip_plan` outputs, `StripState`, `strip_hover_text`, `strip_click_link` (Task 7).
- Produces (Task 9 calls the second):
  - `void draw_strip_plan(const std::vector<strip_prim_t> &prims, std::string *hover)` — pure walk, the `draw_loom_plan` twin (`fabric_imgui.cpp:54`).
  - `void draw_strip(StripState &st, const std::string &rec_id, const std::function<void(const dt_link &)> &go)` — the panel: camera controls + plan + paint + pick.

- [ ] **Step 1: Declare in views_draw.h and write the failing draw test**

Add to `desktop/src/views/views_draw.h` next to the `draw_scene2d` declaration (`views_draw.h:122-127`), with matching includes (`views/strip.h`):

```cpp
// The session strip (2026-08-10 spec): draw_strip_plan is the pure painter —
// walks prims, writes the topmost hovered prim's hover text; draw_strip is
// the panel (camera controls + plan + paint + pick), the Loom's shape.
void draw_strip_plan(const std::vector<strip_prim_t> &prims,
                     std::string *hover);
void draw_strip(StripState &st, const std::string &rec_id,
                const std::function<void(const dt_link &)> &go);
```

Create `desktop/test/test_strip_draw.cpp` (the `test_loom_draw.cpp` shape — headless context boilerplate from `test_loom_draw.cpp:17-24`):

```cpp
// test_strip_draw.cpp — the strip painter under a headless ImGui context.
// Geometry oracle (draw-data vertex counts), never LogToClipboard: a text
// oracle cannot see geometry (desktop draw-test oracle blindness).
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"

#include "doc/recording.h"
#include "views/strip.h"
#include "views/views_draw.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// one prim of EVERY kind, in a synthetic pixel-space plan
static std::vector<strip_prim_t> every_prim() {
    std::vector<strip_prim_t> v;
    auto add = [&](strip_prim k, float x0, float y0, float x1, float y1,
                   const char *t) {
        v.push_back({k, x0, y0, x1, y1, 0, 0, t});
    };
    add(strip_prim::hud_note, 0, 0, 400, 12, "hud");
    add(strip_prim::channel_absent, 0, 12, 400, 26, "why");
    add(strip_prim::run_tint, 0, 0, 200, 300, "");
    add(strip_prim::group_header, 0, 26, 400, 28, "alpha [10]");
    add(strip_prim::lane_header, 0, 28, 80, 46, "alpha [10]");
    add(strip_prim::lane_density, 100, 28, 101, 46, "");
    add(strip_prim::lane_sys_tick, 120, 28, 122, 46, "");
    add(strip_prim::rail_frame, 0, 46, 400, 70, "");
    add(strip_prim::rail_tick, 130, 46, 132, 70, "");
    add(strip_prim::rail_overflow, 140, 46, 152, 70, "+9");
    add(strip_prim::band_frame, 0, 70, 400, 170, "");
    add(strip_prim::band_label, 2, 70, 60, 82, "code");
    add(strip_prim::gap_notch, 0, 170, 400, 172, "");
    add(strip_prim::mem_mark, 200, 90, 202, 94, "");
    add(strip_prim::mem_envelope, 210, 80, 211, 160, "");
    add(strip_prim::pc_mark, 220, 100, 222, 102, "");
    add(strip_prim::run_seam, 200, 0, 201, 300, "pass 0 = 42, 8 steps");
    add(strip_prim::torn_edge, 396, 0, 400, 300, "");
    return v;
}

static void frame(void (*body)()) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    ImGui::Begin("strip-test");
    body();
    ImGui::End();
    ImGui::Render();
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    frame([] {
        auto prims = every_prim();
        std::string hover;
        draw_strip_plan(prims, &hover);
    });
    ImDrawData *dd = ImGui::GetDrawData();
    check("painter smoke: draw data exists", dd != nullptr, "");
    check("painter smoke: geometry emitted", dd && dd->TotalVtxCount > 0,
          "every prim kind must rasterise to vertices");

    // the full panel over a tiny real model, three cameras
    static StripState st; // static: lambda-to-fn-pointer needs no capture
    {
        std::string nd =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"})"
            "\n"
            R"({"k":"trace","basis":"abs","off":4112,"tid":10})" "\n"
            R"({"k":"syscall","line":"openat(AT_FDCWD, <path>) = 3","tid":10})" "\n"
            R"({"k":"mem","step":0,"ea":4200,"size":8,"rw":"w","space":"abs"})" "\n"
            R"({"k":"end","events":3})" "\n";
        std::istringstream in(nd);
        std::string err;
        auto rec = load_recording(in, err);
        check("panel fixture loads", rec.has_value(), err);
        space::Region code;
        code.base = 0x1000;
        code.len = 0x1000;
        code.kind = space::Region::Kind::Code;
        code.label = "code";
        st.model = strip_build(*rec, {code}, {});
    }
    for (double zoom : {0.05, 4.0, 400.0}) {
        st.cam = strip_view_t{};
        st.cam.seq_per_px = zoom;
        frame([] {
            draw_strip(st, "rec-x", [](const dt_link &) {});
        });
        ImDrawData *d2 = ImGui::GetDrawData();
        check("panel draws at zoom", d2 && d2->TotalVtxCount > 0,
              "mark mode, threshold edge, and deep envelope mode all draw");
    }

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
```

(If a capture-less lambda can't reach `st`/zoom, restructure with a small static — shown above; match `test_loom_draw.cpp`'s exact frame-loop idiom if it differs.)

Register in `mk/desktop.mk`: append `$(BUILD)/desktop_test_strip_draw \` to `DESKTOP_TESTS`, and a link rule next to `desktop_test_loom_draw` (~line 1540), mirroring its variable set:

```make
$(BUILD)/desktop_test_strip_draw: $(BUILD)/desktop/test/t/test_strip_draw.o \
    $(BUILD)/desktop/test/vw/strip.o $(BUILD)/desktop/test/vw/strip_draw.o \
    $(BUILD)/desktop/test/src/nav.o \
    $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

(If the link fails on ImGui-helper symbols the Loom painter also uses — e.g. `$(DESKTOP_TEST_UI_OBJ)` — add exactly the missing variable, mirroring `desktop_test_loom_draw`'s list.)

- [ ] **Step 2: Run to verify it fails**

Run: `make build/desktop_test_strip_draw`
Expected: LINK FAILURE — `draw_strip_plan` / `draw_strip` undefined.

- [ ] **Step 3: Write strip_draw.cpp**

```cpp
// strip_draw.cpp — the session strip's painter and panel. Walks strip_plan's
// pixel-space prims and calls ImDrawList; nothing about zoom, bucketing or
// fidelity chrome lives here (that is strip.cpp's plan, where it is
// headlessly assertable). The Loom's painter split (fabric_imgui.cpp).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"

#include "views/strip.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {
// per-tid hue, THE shared palette (scene2d_draw.cpp:32-34 / scene3d's
// tid_color) — copy the six rows verbatim from scene2d_draw.cpp
extern const float kTidPalette[6][3]; // replace with the literal table

ImU32 class_color(space::SyscallClass c) {
    switch (c) {
    case space::SyscallClass::File: return IM_COL32(86, 156, 214, 255);
    case space::SyscallClass::Net: return IM_COL32(78, 201, 176, 255);
    case space::SyscallClass::Process: return IM_COL32(216, 160, 223, 255);
    case space::SyscallClass::Memory: return IM_COL32(220, 205, 125, 255);
    case space::SyscallClass::Signal: return IM_COL32(224, 108, 117, 255);
    case space::SyscallClass::Time: return IM_COL32(152, 195, 121, 255);
    case space::SyscallClass::Other: break;
    }
    return IM_COL32(128, 128, 128, 255); // the visible grey bucket
}
} // namespace

void draw_strip_plan(const std::vector<strip_prim_t> &prims,
                     std::string *hover) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const auto &p : prims) {
        const ImVec2 a(o.x + p.x0, o.y + p.y0), b(o.x + p.x1, o.y + p.y1);
        switch (p.kind) {
        // filled rects: run_tint (very faint), lane_density (alpha by p.b),
        // rail_tick (class color via caller-precomputed? NO — the painter has
        // no model; rail ticks carry their color decision as prim data? They
        // do not: paint rail_tick neutral and let hover text carry the class
        // words? NO — hue IS the encoding. RESOLUTION: the planner writes the
        // class ordinal into prim `b` for rail_tick/lane_sys_tick, and
        // is_write already rides bit0 of `b` for mem prims. Paint from `b`.
        // (Adjust strip.cpp in this task: rail_tick/lane_sys_tick set
        //  b = (uint32_t)cls * 4 + (uint32_t)outcome; a one-line change +
        //  one model-test check.)
        ...
        }
        if (hover && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y &&
            mouse.y < b.y && !p.text.empty())
            *hover = p.text; // topmost wins: later prims overwrite
    }
}
```

**Resolve the note above as part of this step** (it is the one place plan data was insufficient): in `strip.cpp`, set `b = static_cast<uint32_t>(cls) * 4 + static_cast<uint32_t>(outcome)` on `rail_tick`/`lane_sys_tick` prims, add a model-test check for it, and paint: fill = `class_color(cls from b/4)`, ring/border = green (Ok) / red (Error) / grey (Unknown) from `b%4`. Then the full painter switch, one case per prim kind:

- `run_tint`: `dl->AddRectFilled` alpha 10, alternating by `p.a & 1` (two greys).
- `hud_note` / `channel_absent` / `band_label` / `lane_header` / `group_header` / `run_seam` / `rail_overflow`: `dl->AddText` (clip long text to the rect with `ImGui::PushClipRect`); `group_header` and `run_seam` also draw their 1-2px line (`AddRectFilled`).
- `lane_density`: `AddRectFilled` with alpha = `p.b` (0..255), neutral foreground grey-blue.
- `rail_frame` / `band_frame`: `AddRect` 1px border, subtle.
- `rail_tick` / `lane_sys_tick`: filled 2px vertical with class fill + outcome border (from `p.b` as above).
- `mem_mark` / `mem_envelope`: `AddRectFilled`; `p.b & 1` ? warm (write, e.g. `IM_COL32(224,108,117,···)`) : cool (read, `IM_COL32(86,156,214,···)`); envelope alpha 96, mark alpha 255.
- `pc_mark`: 2px square, `kTidPalette[tid ordinal % 6]` — the planner writes the lane ordinal into `p.b` for pc_mark (same one-line pattern as rail ticks; add the check).
- `gap_notch`: two short diagonal lines (`AddLine` ×2) across the band boundary — visually "cut here".
- `torn_edge`: a 4px jagged vertical (`AddLine` zigzag, 8px teeth) in warning yellow, the Loom's torn meaning.

Then the panel:

```cpp
void draw_strip(StripState &st, const std::string &rec_id,
                const std::function<void(const dt_link &)> &go) {
    const StripModel &m = st.model;
    ImGui::TextUnformatted(m.hud.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(st.cam.follow_tail ? "following ▶" : "follow ▶"))
        st.cam.follow_tail = true;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    st.cam.px_w = std::max(64.0f, avail.x);
    st.cam.px_h = std::max(96.0f, avail.y - 24.0f); // room for the zoom slider
    if (st.cam.seq_per_px <= 0) // first sight: fit the whole session
        st.cam.seq_per_px =
            std::max(1e-6, static_cast<double>(m.seq_end) / st.cam.px_w);
    if (st.cam.follow_tail)
        strip_view_follow(st.cam, m.seq_end);

    std::vector<strip_prim_t> prims;
    strip_plan(m, st.cam, &prims);

    ImGui::BeginChild("strip-canvas", ImVec2(st.cam.px_w, st.cam.px_h), true,
                      ImGuiWindowFlags_NoScrollbar);
    std::string hover;
    draw_strip_plan(prims, &hover);
    // pick: topmost prim under the mouse (walk prims in REVERSE), tooltip
    // from strip_hover_text, click → strip_click_link → go
    if (ImGui::IsWindowHovered()) {
        const ImVec2 o = ImGui::GetWindowPos(); // matches the canvas origin
        const ImVec2 mp = ImGui::GetIO().MousePos;
        for (auto it = prims.rbegin(); it != prims.rend(); ++it) {
            if (mp.x < o.x + it->x0 || mp.x >= o.x + it->x1 ||
                mp.y < o.y + it->y0 || mp.y >= o.y + it->y1)
                continue;
            const std::string tip = strip_hover_text(m, *it);
            if (tip.empty())
                continue;
            ImGui::SetTooltip("%s", tip.c_str());
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                if (auto lk = strip_click_link(m, *it, rec_id))
                    go(*lk);
            break;
        }
        // wheel scrolls the deck
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            const StripLayout L = strip_layout(m, st.cam);
            strip_view_scroll_lanes(st.cam, static_cast<int>(m.lanes.size()),
                                    L.deck_h, wheel > 0 ? -1 : 1);
        }
    }
    ImGui::EndChild();

    // pan+zoom: the Loom's window round-trip (fabric_imgui.cpp:286-306) —
    // mirror its exact ImZoomSlider usage; a window whose right edge moved
    // off the tail drops follow
    double lo, hi;
    strip_view_window(st.cam, &lo, &hi);
    const double lo0 = lo, hi0 = hi;
    // <ImZoomSlider over [0, max(seq_end, hi)] — copy the Loom's call>
    if (lo != lo0 || hi != hi0) {
        strip_view_set_window(st.cam, lo, hi);
        if (hi < static_cast<double>(m.seq_end))
            st.cam.follow_tail = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End))
        st.cam.follow_tail = true;
    ImGui::TextDisabled("%s", StripModel::axis_label());
}
```

Copy the `ImZoomSlider` block verbatim-shaped from `fabric_imgui.cpp:286-306` (same header, same flags). Copy `kTidPalette`'s six rows verbatim from `scene2d_draw.cpp:32-34`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make build/desktop_test_strip_draw && ./build/desktop_test_strip_draw` → `ok`.
Also rerun the model test (`./build/desktop_test_strip_model`) — the `b`-encoding additions changed the plan.

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/views/strip_draw.cpp desktop/test/test_strip_draw.cpp desktop/src/views/views_draw.h desktop/src/views/strip.cpp desktop/test/test_strip_model.cpp mk/desktop.mk
git commit -m "desktop/views: strip painter + panel — headless draw test, class/outcome hues"
git pull --rebase origin main && git push origin main
```

---

### Task 9: Registration — ViewId, presence, shell wiring, live rebuild

**Files:**
- Modify: `desktop/src/ui/view_presence.h` (enum), `desktop/src/ui/view_presence.cpp` (rule), `desktop/src/ui/shell.h` (state vector), `desktop/src/ui/shell.cpp` (tab case, rebuild, region assembly helper), `desktop/test/test_view_presence.cpp`
- Modify: `mk/desktop.mk` ONLY if the app link list needs the new objects named (check how `vw/scene2d_draw.o` reaches the app binary — the same list gets `vw/strip.o` + `vw/strip_draw.o`).

**Interfaces:**
- Consumes: `StripState`, `strip_build`, `draw_strip` (Tasks 2–8).
- Produces: `ViewId::SessionStrip`; `ShellState::strips` (`std::vector<StripState>`, parallel to `ws.recordings`); `shell_assemble_regions(const Recording &, const Streams &)` (static in shell.cpp, shared by the 3D weave and the strip).

- [ ] **Step 1: Write the failing presence tests**

In `desktop/test/test_view_presence.cpp`, following its existing row-check idiom (read the file first; add checks in its style):

```cpp
// Session strip: present iff ANY strip channel exists
{
    // reuse the file's existing helpers for building Streams/Recording;
    // a recording with one mem event → present
    // a recording with only a note event → absent, reason verbatim:
    //   "recording carries no mem/syscall/trace/call/watch/df_step events"
    // (write both checks with the file's own fixture helpers)
}
```

Concretely: find how the file constructs its `Recording`/`Streams`/`ObserverState`/`StepIndex` inputs for other rows, build (a) a recording containing exactly one `mem` event and (b) one containing only a `note` event, call `view_presence(...)`, and assert a `ViewId::SessionStrip` entry exists with `present == true` for (a), and for (b) `present == false` with `reason == "recording carries no mem/syscall/trace/call/watch/df_step events"` and label `"Session strip"`.

- [ ] **Step 2: Run to verify it fails**

Run: `make build/desktop_test_view_presence && ./build/desktop_test_view_presence`
Expected: COMPILE FAILURE (`ViewId::SessionStrip` unknown).

- [ ] **Step 3: Implement registration + shell wiring**

1. `view_presence.h`: append `SessionStrip,` LAST in `enum class ViewId` (enum order is not tab order; append-last keeps any index assumptions safe).
2. `view_presence.cpp`: insert the `add(...)` between the Loom and Scrubber rows (~line 137 region) — tab reading order puts the strip beside the Loom:

```cpp
    {
        static const char *kKinds[] = {"mem",  "syscall", "trace",
                                       "call", "watch",   "df_step"};
        bool present = false;
        for (const char *k : kKinds) {
            auto it = r.by_kind.find(k);
            if (it != r.by_kind.end() && !it->second.empty()) {
                present = true;
                break;
            }
        }
        add(ViewId::SessionStrip, "Session strip", present,
            present ? std::string()
                    : std::string("recording carries no "
                                  "mem/syscall/trace/call/watch/df_step "
                                  "events"),
            std::nullopt);
    }
```

3. `shell.h`: add `#include "views/strip.h"` and, next to the other per-recording parallel vectors (near `scenes`, ~line 519): `std::vector<StripState> strips;`.
4. `shell.cpp` — every site that resizes/erases `s.scenes` gets the twin line (found at 151, 185, 259; grep `scenes.` to catch all):
   `s.strips.resize(s.ws.recordings.size());` / `s.strips.erase(s.strips.begin() + static_cast<long>(idx));`
5. `shell.cpp` — extract the region assembly (~lines 1345-1385, the codeimage → `observed_data_spans` → `vmmap_apply_names` block inside the weave) into
   `static std::vector<space::Region> shell_assemble_regions(const Recording &rec, const Streams &st)` and call it from BOTH the weave site (behaviour-identical — the weave's own tests hold) and the strip build below.
6. `shell.cpp` — the growth-watermark rebuild block (~283-310): after the seg-index rebuilds, invalidate the live tab's strip but KEEP its camera (the SceneView carry-over policy at ~315-366):

```cpp
    if (idx >= 0 && static_cast<size_t>(idx) < s.strips.size()) {
        s.strips[static_cast<size_t>(idx)].built = false; // model rebuilds
        // lazily at draw; cam (zoom, lane scroll, follow) is deliberately
        // NOT reset — the reading posture survives growth ticks
    }
```

7. `shell.cpp` — the recording tab-bar switch (~2872, beside `case ViewId::Loom:`):

```cpp
    case ViewId::SessionStrip: {
        StripState &st = s.strips[static_cast<size_t>(tab)];
        if (!st.built) {
            const bool live_union_tab =
                s.live_tab == tab && s.settings.live_union_weave &&
                s.live_union.event_count() > 0;
            const Recording &sub =
                live_union_tab ? s.live_union : s.ws.recordings[tab];
            const Streams &stst =
                live_union_tab ? s.live_union_streams : s.streams[tab];
            std::vector<StripSeam> seams;
            if (live_union_tab && s.live) {
                uint64_t off = 0;
                size_t ord = 1;
                for (const Recording &part : s.live->recordings()) {
                    off += part.event_count();
                    ord++;
                    seams.push_back(
                        {off, "capture " + std::to_string(ord)});
                }
                if (!s.live->growing() && !seams.empty())
                    seams.pop_back(); // no growing tail: the last "seam"
                                      // would sit at the stream's end
            }
            st.model =
                strip_build(sub, shell_assemble_regions(sub, stst), seams);
            st.built = true;
        }
        draw_strip(st, s.streams[tab].id, go);
        break;
    }
```

**Verify the exact member spellings against shell.h/shell.cpp while wiring** (`s.live`, `s.live_tab`, `s.live_union`, `s.live_union_streams`, `s.streams[tab].id`, the `go` lambda the other cases pass — e.g. the `draw_flat_surface` call site at ~1946-1951). Use whatever the neighbouring cases actually name; the code wins over this plan's spellings.
8. App link: if `mk/desktop.mk` names view objects for the app binary explicitly, add `vw/strip.o` and `vw/strip_draw.o` beside `vw/scene2d.o`/`vw/scene2d_draw.o` in the app's list (grep `app/vw/scene2d`); if a wildcard covers views/, nothing to do.

- [ ] **Step 4: Run the tests**

Run:
```bash
make build/desktop_test_view_presence && ./build/desktop_test_view_presence
make build/desktop_test_strip_model && ./build/desktop_test_strip_model
make build/desktop_test_strip_draw && ./build/desktop_test_strip_draw
make desktop 2>&1 | tail -3   # the full app must link
```
Expected: all `ok`; app links. (`test_shell`'s attach/no-host FAILs are PRE-EXISTING — do not chase them; verify only that no NEW test_shell failure names the strip.)

- [ ] **Step 5: Commit and push**

```bash
git add desktop/src/ui/view_presence.h desktop/src/ui/view_presence.cpp desktop/src/ui/shell.h desktop/src/ui/shell.cpp desktop/test/test_view_presence.cpp mk/desktop.mk
git commit -m "desktop/ui: register the Session strip view — presence rule, live-union substrate, growth-tick rebuild"
git pull --rebase origin main && git push origin main
```

---

### Task 10: Full-lane verification

**Files:** none new — this task is evidence.

- [ ] **Step 1: Run the whole desktop test set individually**

```bash
for t in strip_model strip_draw crossing view_presence scene2d loom_draw; do
  make build/desktop_test_$t && ./build/desktop_test_$t || echo "FAILED: $t";
done
```
Expected: every one `ok`. (These are the touched/adjacent suites; the full `make desktop-test` for-loop stops at the pre-existing `test_shell` failures, so run it too but judge only NEW failures.)

- [ ] **Step 2: Run the authoritative Docker lane**

```bash
make docker-desktop 2>&1 | tail -20
```
Expected: builds both binaries, headless + GL + Xvfb sublanes pass with no NEW failure. This is the lane CI runs; do not claim done on host-only results.

- [ ] **Step 3: Eyeball it live (evidence, not vibes)**

```bash
make desktop && ./build/desktop  # open a golden recording with mem+syscalls;
                                 # confirm: strip tab present, marks/rail/seams
                                 # draw, zoom slider pans, End re-follows
```
If a golden with all channels is not at hand, `asmspy --serve` + the Live capture pane exercises the live path (follow-tail + growth rebuild).

- [ ] **Step 4: Final commit (only if fixes were needed) and push**

```bash
git add <exactly the files you fixed>
git commit -m "desktop/views: strip verification fixes"
git pull --rebase origin main && git push origin main
```
