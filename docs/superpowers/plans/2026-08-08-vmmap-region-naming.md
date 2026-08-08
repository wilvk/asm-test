# vmmap Region Naming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the 3D overview's floor say what its regions *are* — `libc.so.6 .text`, `[heap]`, `[stack]` — instead of labelling nearly all of them `observed data (unknown)`.

**Architecture:** The serve host emits a new `vmmap` event carrying `/proc/<pid>/maps`. The viewer consumes it as a **naming overlay**: it rewrites `label`/`kind` on the observed-data spans the viewer already derives, and never becomes a region itself. That single constraint is what keeps the plane's layout byte-identical and provably un-moved under a reader.

**Tech Stack:** C11 (`cli/`, header-only unit under `cli/asmspy_vmmap.h`), C++17 (`desktop/src/`), the repo's hand-rolled test harnesses (`vt::` in `desktop/test/view_test.h`; `fail()` in `cli/cli_smoke.sh`). No new dependencies.

**Spec:** [`docs/superpowers/specs/2026-08-08-vmmap-region-naming-design.md`](../specs/2026-08-08-vmmap-region-naming-design.md). Read it first — this plan implements it and does not restate its reasoning.

## Global Constraints

- **`vmmap` is a naming overlay, never a Projection region.** Feeding vmmap spans into `build_projection` pins `order` at 12 (16.7 M `unproject` calls per weave) and extinguishes every atlas label under the min-side legibility skip. Write this into the code comment.
- **Never use `rec_emitf` for the vmmap body.** It formats into `char body[16384]` on the **stack** and discards `vsnprintf`'s return (`cli/asmspy.c:195-204`) — an oversized body emits invalid NDJSON with no flag. Use the heap-buffer + sticky-`overflow` + loud-refusal pattern from `info_emit_json` (`cli/asmspy.c:8609`).
- **Emission is serve-only.** Headless `--record` and `tools/asmtrace_record.c` must not emit it, so the golden corpus stays byte-stable.
- **Never coalesce maps rows across the executable bit.** Measured: `(lo,hi,name)` coalescing fuses exec with non-exec in 102 of 212 spans and hides a real `PROT_NONE → rw-p` mprotect.
- **Cap = 256 spans. Rank first, then cap.** Rank executable-first, then by descending length. Emit `spans_total` **and** `spans_truncated` — cap, flag, and total, all three.
- **Addresses are hex strings; lengths are numbers.** The schema's 64-bit-address rule.
- **`maps_readable:false` classifies nothing and says so.** Absent measurement, never measured zero.
- Every new `desktop/test/*.cpp` returns `vt::report("name")` or the hand-rolled `check()` idiom. **There is no gtest in this tree.**

## File structure

| File | Responsibility |
|---|---|
| `cli/asmspy_vmmap.h` (new, header-only) | Parse a maps stream → rows; rank; build the JSON body. Pure, no `/proc`, no globals — so it is unit-testable from a C test with a string fixture. |
| `cli/asmspy.c` (modify) | Call it: emit at attach, change-gated refresh. Owns the snapshot digest. |
| `cli/test_vmmap.c` (new) | C unit test for the parse/rank/emit unit. |
| `cli/cli_smoke.sh` (modify) | One serve lane asserting a real capture carries a `vmmap` with `[heap]`/`[stack]`. |
| `desktop/src/space/vmmap.h/.cpp` (new) | Decode the `vmmap` event → `VmMap`; apply names to a `std::vector<Region>`. Pure. |
| `desktop/src/space/types.h` (modify) | `Region` gains `extent_base`/`extent_len`/`perms`/`path`. |
| `desktop/src/ui/shell.cpp` (modify) | Call the overlay after `observed_data_spans`. |
| `desktop/src/doc/recording.cpp` (modify) | `kKnownKinds` 24 → 25. |
| `desktop/src/scene3d/hud.cpp`, `hud.h`, `focus.h` (modify) | Stale-index fix, pick readout, region roster. |
| `desktop/src/scene3d/scene.h`, `layers.cpp` (modify) | Two new default-off layers. |
| `desktop/test/test_vmmap.cpp` (new) | Overlay + layout-invariance tests. |
| `desktop/test/fixtures/vmmap-named.asmtrace` (new) | Hand-authored fixture. |
| `docs/internal/gui/asmtrace-schema.md` (modify) | The `vmmap` section + reserved-kinds row. |

---

### Task 1: Fix the stale region-index bug

A live bug today, independent of everything else, and relabeling makes it easier to hit. `SceneFocus::region` (`desktop/src/scene3d/focus.h:53-54`, *"An index into Projection::regions, or -1"*) and `HudState::goto_region_sel` (`desktop/src/scene3d/hud.h:262-264`) survive `shell_sync_live_tab`'s reset as **raw indices** and are only ever bounds-checked. After any weave that changes the region set they silently retarget a different region.

**Files:**
- Modify: `desktop/src/scene3d/focus.h`, `desktop/src/scene3d/focus.cpp`
- Modify: `desktop/src/ui/shell.cpp` (the weave block, ~line 1330)
- Test: `desktop/test/test_focus.cpp`

**Interfaces:**
- Produces: `int32_t scene3d::reresolve_region(const std::vector<space::Region> &regions, uint64_t base, uint64_t len);` — returns the index of the region with exactly that `(base,len)`, or `-1`.
- Produces: `SceneFocus::region_base`, `SceneFocus::region_len` (the identity the index is re-resolved against).

- [ ] **Step 1: Write the failing test** — append to `desktop/test/test_focus.cpp`, inside `main()`:

```cpp
    // doc: a region index must survive a weave by IDENTITY, not by ordinal.
    {
        std::vector<space::Region> before;
        space::Region a; a.base = 0x1000; a.len = 0x1000; before.push_back(a);
        space::Region b; b.base = 0x9000; b.len = 0x2000; before.push_back(b);
        check("reresolve: finds an unmoved region",
              scene3d::reresolve_region(before, 0x9000, 0x2000) == 1,
              "region 1 is (0x9000,0x2000)");
        // A weave that INSERTS a region ahead of it shifts the ordinal.
        std::vector<space::Region> after;
        space::Region z; z.base = 0x0500; z.len = 0x0100; after.push_back(z);
        after.push_back(a);
        after.push_back(b);
        check("reresolve: survives an insertion that shifts the ordinal",
              scene3d::reresolve_region(after, 0x9000, 0x2000) == 2,
              "the same region is now at index 2");
        check("reresolve: refuses a region that went away",
              scene3d::reresolve_region(after, 0xdead0000, 0x10) == -1,
              "a vanished region must clear, never retarget");
    }
```

- [ ] **Step 2: Run it and watch it fail**

```
make build/desktop_test_focus && ./build/desktop_test_focus
```
Expected: compile error, `reresolve_region` is not a member of `scene3d`.

- [ ] **Step 3: Add the declaration** to `desktop/src/scene3d/focus.h`, after the `SceneFocus` struct:

```cpp
// doc 68-followup: resolve a region by IDENTITY, not ordinal. Projection::regions
// is rebuilt on every weave, and a growing live capture gains regions — so an
// index held across a weave (SceneFocus::region, HudState::goto_region_sel) names
// a DIFFERENT region afterwards. Both were only ever bounds-checked, which cannot
// catch that: the stale index is in range, it is just wrong. Returns -1 when no
// region has exactly this (base,len), which the caller renders as "cleared",
// never as "region 0".
int32_t reresolve_region(const std::vector<space::Region> &regions,
                         uint64_t base, uint64_t len);
```

Add `#include <vector>` and `#include "space/types.h"` to `focus.h` if absent.

- [ ] **Step 4: Implement it** in `desktop/src/scene3d/focus.cpp`:

```cpp
int32_t reresolve_region(const std::vector<space::Region> &regions,
                         uint64_t base, uint64_t len) {
    for (size_t i = 0; i < regions.size(); i++)
        if (regions[i].base == base && regions[i].len == len)
            return static_cast<int32_t>(i);
    return -1;
}
```

- [ ] **Step 5: Carry the identity on SceneFocus** — add to the `SceneFocus` struct in `focus.h`, directly under `region`:

```cpp
    // The (base,len) `region` was chosen for. Written whenever `region` is set;
    // read after a weave to re-resolve the ordinal. 0/0 means "never set".
    uint64_t region_base = 0, region_len = 0;
```

- [ ] **Step 6: Re-resolve after the weave** — in `desktop/src/ui/shell.cpp`, immediately after `sv.built = true;` in `draw_scene_overview`:

```cpp
        // doc 68-followup: the weave just rebuilt Projection::regions, so every
        // held region ORDINAL is now suspect. Re-resolve by identity and clear
        // what went away — a bounds check cannot catch this, because a stale
        // index is in range and simply names someone else.
        if (sv.hud.focus.region >= 0) {
            sv.hud.focus.region = scene3d::reresolve_region(
                sv.terr.proj.regions, sv.hud.focus.region_base,
                sv.hud.focus.region_len);
        }
        if (sv.hud.goto_region_sel >= 0 &&
            static_cast<size_t>(sv.hud.goto_region_sel) >=
                sv.terr.proj.regions.size())
            sv.hud.goto_region_sel = -1;
```

- [ ] **Step 7: Set the identity at every write site of `focus.region`.** Find them with:

```
rg -n "focus\.region = |focus\.region=" desktop/src
```

At each site that assigns a non-negative index `i`, add beside it:

```cpp
        sv.hud.focus.region_base = sv.terr.proj.regions[i].base;
        sv.hud.focus.region_len  = sv.terr.proj.regions[i].len;
```

- [ ] **Step 8: Run the tests**

```
make build/desktop_test_focus && ./build/desktop_test_focus
make build/desktop_test_shell && ./build/desktop_test_shell
```
Expected: both pass.

- [ ] **Step 9: Commit**

```bash
git add desktop/src/scene3d/focus.h desktop/src/scene3d/focus.cpp \
        desktop/src/ui/shell.cpp desktop/test/test_focus.cpp
git commit -m "desktop: a held region index survives a weave by identity, not ordinal"
```

---

### Task 2: `Region` metadata + the pure naming overlay

**Files:**
- Modify: `desktop/src/space/types.h` (the `Region` struct)
- Create: `desktop/src/space/vmmap.h`, `desktop/src/space/vmmap.cpp`
- Create: `desktop/test/test_vmmap.cpp`
- Modify: `mk/desktop.mk`

**Interfaces:**
- Consumes: `space::Region` (`base`, `len`, `kind`, `label`, `version`), `asmdesk::Recording`.
- Produces:
  - `struct space::VmSpan { uint64_t base, len; std::string perms, name, path; };`
  - `struct space::VmMap { std::vector<VmSpan> spans; bool readable = false; uint64_t spans_total = 0; bool truncated = false; bool present = false; };`
  - `VmMap space::vmmap_from_recording(const Recording &rec);`
  - `Region::Kind space::vmmap_kind_of(const std::string &perms, const std::string &name);`
  - `size_t space::vmmap_apply_names(std::vector<Region> &regions, const VmMap &map);` → count relabelled.

- [ ] **Step 1: Extend `Region`** — in `desktop/src/space/types.h`, inside `struct Region`, after `version`:

```cpp
    // doc 68-followup (vmmap): the MAPPING this region falls inside, when a
    // `vmmap` event named one. base/len above stay what was TOUCHED; these are
    // what it IS. 0/0 when unknown — the overlay never invents an extent, and
    // an extent is not an allocation (the same refusal observed_data_spans makes).
    uint64_t extent_base = 0, extent_len = 0;
    std::string perms; // "r-xp" verbatim from maps; "" when unknown
    std::string path;  // "/usr/lib/libc.so.6"; "" for anonymous or unknown
```

- [ ] **Step 2: Write the failing test** — create `desktop/test/test_vmmap.cpp`:

```cpp
// test_vmmap.cpp — the address-space naming overlay (vmmap design, 2026-08-08).
// Pure: no ImGui, no GL. The load-bearing assertion is LAYOUT INVARIANCE — the
// overlay may change what a region is CALLED and never where it sits.
#include <cstdio>
#include <string>
#include <vector>

#include "space/projection.h"
#include "space/vmmap.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static space::VmMap fixture_map() {
    space::VmMap m;
    m.present = true;
    m.readable = true;
    m.spans_total = 4;
    m.spans = {
        {0x400000, 0x10000, "r-xp", "firefox", "/usr/lib/firefox/firefox"},
        {0x500000, 0x40000, "rw-p", "[heap]", ""},
        {0x7ffd0000, 0x21000, "rw-p", "[stack]", ""},
        {0x7f0000000000, 0x100000, "rw-p", "", ""},
    };
    return m;
}

int main() {
    // --- kind resolution: a pure function of (perms, name) ------------------
    using K = space::Region::Kind;
    check("kind/heap", space::vmmap_kind_of("rw-p", "[heap]") == K::Heap, "");
    check("kind/stack", space::vmmap_kind_of("rw-p", "[stack]") == K::Stack, "");
    check("kind/stack-tid", space::vmmap_kind_of("rw-p", "[stack:7]") == K::Stack, "");
    check("kind/code", space::vmmap_kind_of("r-xp", "libc.so.6") == K::Code, "");
    check("kind/data", space::vmmap_kind_of("rw-p", "libc.so.6") == K::Data, "");
    check("kind/anon", space::vmmap_kind_of("rw-p", "") == K::Mmap, "");
    // An anonymous EXECUTABLE mapping is a JIT arena — still Mmap by kind (the
    // perms layer marks it); it must NOT be promoted to Code, which would claim
    // a module that does not exist.
    check("kind/anon-exec-is-not-code",
          space::vmmap_kind_of("rwxp", "") == K::Mmap, "");

    // --- the overlay renames, and only renames -----------------------------
    {
        std::vector<space::Region> regs;
        space::Region touched;         // an observed-data span inside [heap]
        touched.base = 0x501000;
        touched.len = 0x2000;
        touched.kind = space::Region::Unknown;
        touched.label = space::kObservedDataLabel;
        regs.push_back(touched);

        const size_t n = space::vmmap_apply_names(regs, fixture_map());
        check("overlay/renamed-one", n == 1, "expected exactly one relabel");
        check("overlay/kind", regs[0].kind == K::Heap, "kind must become Heap");
        check("overlay/label", regs[0].label == "[heap]", regs[0].label);
        check("overlay/base-untouched", regs[0].base == 0x501000, "base moved");
        check("overlay/len-untouched", regs[0].len == 0x2000, "len moved");
        check("overlay/extent", regs[0].extent_base == 0x500000 &&
                                    regs[0].extent_len == 0x40000,
              "the mapping's true extent must be carried");
        check("overlay/perms", regs[0].perms == "rw-p", regs[0].perms);
    }

    // --- what it must NOT touch --------------------------------------------
    {
        std::vector<space::Region> regs;
        space::Region code;            // a codeimage region: already named
        code.base = 0x400000;
        code.len = 0x100;
        code.kind = space::Region::Code;
        code.label = "code@0x400000";
        regs.push_back(code);
        space::Region orphan;          // covered by no mapping
        orphan.base = 0xdead0000;
        orphan.len = 0x1000;
        orphan.kind = space::Region::Unknown;
        orphan.label = space::kObservedDataLabel;
        regs.push_back(orphan);

        space::vmmap_apply_names(regs, fixture_map());
        check("overlay/leaves-codeimage-alone", regs[0].label == "code@0x400000",
              "a codeimage region already states its own provenance");
        check("overlay/uncovered-stays-unknown",
              regs[1].kind == K::Unknown &&
                  regs[1].label == space::kObservedDataLabel,
              "a span no mapping covers is still unknown — never guessed");
    }

    // --- an absent or unreadable map changes NOTHING ------------------------
    {
        std::vector<space::Region> regs;
        space::Region s;
        s.base = 0x501000;
        s.len = 0x2000;
        s.kind = space::Region::Unknown;
        s.label = space::kObservedDataLabel;
        regs.push_back(s);

        check("overlay/absent-map-is-a-noop",
              space::vmmap_apply_names(regs, space::VmMap{}) == 0 &&
                  regs[0].label == space::kObservedDataLabel,
              "no vmmap event -> byte-identical behaviour");

        space::VmMap unreadable = fixture_map();
        unreadable.readable = false;
        unreadable.spans.clear();
        check("overlay/unreadable-map-is-a-noop",
              space::vmmap_apply_names(regs, unreadable) == 0 &&
                  regs[0].label == space::kObservedDataLabel,
              "maps_readable:false is ABSENT MEASUREMENT, never measured zero");
    }

    // --- LAYOUT INVARIANCE: the whole safety argument, in one assertion -----
    {
        std::vector<space::Region> plain;
        space::Region r0;
        r0.base = 0x400000; r0.len = 0x1000; r0.kind = space::Region::Code;
        space::Region r1;
        r1.base = 0x501000; r1.len = 0x2000;
        r1.kind = space::Region::Unknown; r1.label = space::kObservedDataLabel;
        plain.push_back(r0);
        plain.push_back(r1);
        std::vector<space::Region> named = plain;
        space::vmmap_apply_names(named, fixture_map());

        space::Projection pa = space::build_projection(std::move(plain));
        space::Projection pb = space::build_projection(std::move(named));
        const space::LayoutFingerprint fa = space::layout_fingerprint(pa);
        const space::LayoutFingerprint fb = space::layout_fingerprint(pb);
        check("layout/invariant", fa.digest == fb.digest,
              "naming a region must NOT move the floor — if this fails, the "
              "overlay is mutating base/len and the reader's mental map breaks");
        check("layout/reflow-note-silent",
              space::layout_reflow_note(fa, fb).empty(),
              "an unmoved floor must not announce a reflow");
    }

    if (failures) {
        std::fprintf(stderr, "test_vmmap: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_vmmap: all checks passed\n");
    return 0;
}
```

- [ ] **Step 3: Register the test in the build** — in `mk/desktop.mk`, add to the `DESKTOP_TESTS :=` list (near line 1210):

```make
                 $(BUILD)/desktop_test_vmmap \
```

and add the link rule beside `desktop_test_projection` (~line 1561):

```make
# The vmmap naming overlay: sp/vmmap.o + sp/projection.o (for the layout-invariance
# assertion) + the document model it decodes from. No ImGui, no GL.
$(BUILD)/desktop_test_vmmap: $(BUILD)/desktop/test/t/test_vmmap.o \
    $(BUILD)/desktop/test/sp/vmmap.o $(BUILD)/desktop/test/sp/projection.o \
    $(BUILD)/desktop/test/doc/recording.o $(BUILD)/desktop/test/doc/streams.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

Also add `sp/vmmap.o` to the shared app+test object bundle (~line 646), beside `sp/projection.o`:

```make
  $(BUILD)/desktop/$(1)/sp/vmmap.o \
```

- [ ] **Step 4: Run it and watch it fail**

```
make build/desktop_test_vmmap
```
Expected: `fatal error: space/vmmap.h: No such file or directory`.

- [ ] **Step 5: Write the header** — `desktop/src/space/vmmap.h`:

```cpp
// vmmap.h — the address-space NAMING OVERLAY (vmmap design, 2026-08-08).
//
// The 3D plane has exactly two region sources, and only one is a map: codeimage
// events (one span, the exe text) and observed_data_spans (every address the
// trace was seen touching, clustered, Kind::Unknown, "observed data"). So nearly
// the whole floor reads "unknown" — honestly, because a touch says the bytes were
// touched and nothing about what they are.
//
// A `vmmap` event carries /proc/<pid>/maps into the recording. This turns it into
// names.
//
// THE LOAD-BEARING CONSTRAINT: a vmmap span is NEVER a Projection region. It only
// rewrites `label`/`kind` (and records the mapping's extent) on regions the viewer
// already derived. Feed these spans to build_projection and a 1 GiB anonymous
// reservation enters the COMPACTED domain, squashing the actual routine window to
// its guaranteed one cell, pinning `order` at 12 (16.7 M unproject calls per weave)
// and extinguishing every atlas label under the min-side legibility skip — i.e.
// destroying the very feature this exists to provide. Do not "improve" it into a
// region source.
//
// Pure: no ImGui, no GL, no engine. Links into both binaries and the null harness.
#ifndef ASMDESK_SPACE_VMMAP_H
#define ASMDESK_SPACE_VMMAP_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/types.h"

namespace asmdesk::space {

// One row of /proc/<pid>/maps, as it reached the wire.
struct VmSpan {
    uint64_t base = 0, len = 0;
    std::string perms; // "r-xp", verbatim
    std::string name;  // "libc.so.6" | "[heap]" | "[stack]" | "" for anonymous
    std::string path;  // "/usr/lib/libc.so.6"; "" when there is none
};

// A recording's address-space map. `present` distinguishes "no vmmap event"
// from "a vmmap event that measured nothing", and `readable` distinguishes
// "measured empty" from "could not read /proc/<pid>/maps" — which is the state
// of every process the running user does not own, and must never be rendered as
// "this process has no mappings".
struct VmMap {
    std::vector<VmSpan> spans; // ascending by base
    bool present = false;      // a `vmmap` event existed
    bool readable = false;     // maps_readable
    uint64_t spans_total = 0;  // rows BEFORE the cap
    bool truncated = false;    // the cap dropped rows
};

// Decode the LAST `vmmap` event in the recording. Last, not first: a capture may
// re-emit on change, and the plane flattens to last-name-wins exactly as it
// already flattens the codeimage version timeline ("widest len, latest version",
// terrain.cpp). A recording with no vmmap yields `present == false`.
VmMap vmmap_from_recording(const Recording &rec);

// The kind a mapping implies, a pure function of (perms, name).
//
// An anonymous EXECUTABLE mapping stays Mmap rather than being promoted to Code:
// it is a JIT arena, and calling it Code would claim a module that does not
// exist. The perms layer marks it instead.
Region::Kind vmmap_kind_of(const std::string &perms, const std::string &name);

// Rewrite `label` / `kind` — and record `extent_base`/`extent_len`/`perms`/`path`
// — on every region that is still an UNNAMED observed-data span and falls inside
// a mapping. Returns how many were relabelled.
//
// Touches nothing else, on purpose:
//   - a codeimage region already states its own provenance;
//   - a span no mapping covers stays Unknown, never guessed from size or
//     neighbours;
//   - `base` and `len` are NEVER modified, which is what makes the projection's
//     layout fingerprint identical before and after (it mixes order/layout/
//     domain_off/rects, not label/kind) and therefore keeps the reader's mental
//     map of the floor intact. test_vmmap asserts exactly that.
size_t vmmap_apply_names(std::vector<Region> &regions, const VmMap &map);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_VMMAP_H
```

- [ ] **Step 6: Write the implementation** — `desktop/src/space/vmmap.cpp`:

```cpp
// vmmap.cpp — see vmmap.h for the rule this must not break.
#include "space/vmmap.h"

#include <algorithm>

#include "space/projection.h" // kObservedDataLabel

namespace asmdesk::space {

namespace {
// The wire carries addresses as hex STRINGS (a JSON number is a double in many
// readers and silently rounds a 64-bit pointer). There is no shared hex parser
// reachable from space/, so this TU carries its own — as terrain.cpp, pick.cpp
// and standalone.cpp each carry their own hex formatter.
uint64_t parse_hex(const std::string &s) {
    uint64_t v = 0;
    size_t i = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 2 : 0;
    for (; i < s.size(); i++) {
        const char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}
} // namespace

Region::Kind vmmap_kind_of(const std::string &perms, const std::string &name) {
    if (name == "[heap]")
        return Region::Heap;
    if (name.rfind("[stack", 0) == 0) // "[stack]" and "[stack:7]"
        return Region::Stack;
    const bool exec = perms.size() > 2 && perms[2] == 'x';
    if (name.empty() || name[0] == '[')
        return Region::Mmap; // anonymous, or a pseudo-mapping we do not model
    return exec ? Region::Code : Region::Data;
}

VmMap vmmap_from_recording(const Recording &rec) {
    VmMap m;
    auto it = rec.by_kind.find("vmmap");
    if (it == rec.by_kind.end() || it->second.empty())
        return m; // present stays false: no event, not an empty measurement
    const Event &e = it->second.back(); // last wins; see the header
    m.present = true;
    m.readable = e.body.value("maps_readable", true);
    m.spans_total = e.body.value("spans_total", uint64_t{0});
    m.truncated = e.body.value("spans_truncated", false);
    if (!m.readable)
        return m; // absent measurement: classify nothing
    auto sp = e.body.find("spans");
    if (sp == e.body.end() || !sp->is_array())
        return m;
    for (const auto &s : *sp) {
        VmSpan v;
        v.base = parse_hex(s.value("base", std::string()));
        v.len = s.value("len", uint64_t{0});
        v.perms = s.value("perms", std::string());
        v.name = s.value("name", std::string());
        v.path = s.value("path", std::string());
        if (v.len > 0)
            m.spans.push_back(std::move(v));
    }
    std::sort(m.spans.begin(), m.spans.end(),
              [](const VmSpan &a, const VmSpan &b) { return a.base < b.base; });
    if (m.spans_total < m.spans.size())
        m.spans_total = m.spans.size();
    return m;
}

size_t vmmap_apply_names(std::vector<Region> &regions, const VmMap &map) {
    if (!map.present || !map.readable || map.spans.empty())
        return 0;
    size_t n = 0;
    for (Region &r : regions) {
        // Only an UNNAMED observed-data span. A codeimage region already states
        // its own provenance, and overwriting it would launder a captured code
        // span into a guess from a maps row.
        if (r.kind != Region::Unknown || r.label != kObservedDataLabel)
            continue;
        // The last span starting at or before r.base — the only one that can
        // contain it, since spans are sorted and non-overlapping.
        auto hi = std::upper_bound(
            map.spans.begin(), map.spans.end(), r.base,
            [](uint64_t a, const VmSpan &s) { return a < s.base; });
        if (hi == map.spans.begin())
            continue;
        const VmSpan &s = *(hi - 1);
        if (r.base < s.base || r.base >= s.base + s.len)
            continue; // covered by no mapping: still unknown, never guessed
        r.kind = vmmap_kind_of(s.perms, s.name);
        r.label = s.name.empty() ? std::string("(anonymous)") : s.name;
        r.extent_base = s.base;
        r.extent_len = s.len;
        r.perms = s.perms;
        r.path = s.path;
        n++;
    }
    return n;
}

} // namespace asmdesk::space
```

- [ ] **Step 7: Run the test**

```
make build/desktop_test_vmmap && ./build/desktop_test_vmmap
```
Expected: `test_vmmap: all checks passed`.

- [ ] **Step 8: Commit**

```bash
git add desktop/src/space/vmmap.h desktop/src/space/vmmap.cpp \
        desktop/src/space/types.h desktop/test/test_vmmap.cpp mk/desktop.mk
git commit -m "desktop: the pure vmmap naming overlay, and the layout-invariance test that guards it"
```

---

### Task 3: Wire the overlay into the weave, and teach the reader the kind

**Files:**
- Modify: `desktop/src/ui/shell.cpp` (~line 1337, after `observed_data_spans`)
- Modify: `desktop/src/doc/recording.cpp` (`kKnownKinds`)
- Create: `desktop/test/fixtures/vmmap-named.asmtrace`
- Modify: `desktop/test/test_recording.cpp`

**Interfaces:**
- Consumes: `space::vmmap_from_recording`, `space::vmmap_apply_names` (Task 2).

- [ ] **Step 1: Write the failing test** — append inside `main()` in `desktop/test/test_recording.cpp`:

```cpp
    // vmmap is a real producer kind: an unlisted kind is counted as unknown and
    // rendered as "(N event(s) of unknown kind, kept)". This is the exact defect
    // review caught for `procinfo`.
    vt::check("vmmap is a known kind", asmdesk::is_known_kind("vmmap"),
              "add \"vmmap\" to kKnownKinds and bump the std::array size");
```

- [ ] **Step 2: Run it and watch it fail**

```
make build/desktop_test_recording && ./build/desktop_test_recording
```
Expected: `FAIL vmmap is a known kind`.

- [ ] **Step 3: Extend `kKnownKinds`** in `desktop/src/doc/recording.cpp` — bump `24` to `25` and append the entry:

```cpp
static const std::array<const char *, 25> kKnownKinds = {
    {"trace",  "coverage",  "syscall", "stream",     "call",
     "graph",  "topo",      "survey",  "watch",      "df_step",
     "df_edge", "regstate", "result",  "note",       "stitch",
     "end",    "session",   "cmd",     "err",        "codeimage",
     "mem",    "blame",     "statediff", "procinfo",  "vmmap"}};
```

- [ ] **Step 4: Write the fixture** — create `desktop/test/fixtures/vmmap-named.asmtrace`:

```
{"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"}
{"k":"note","text":"HAND-AUTHORED fixture for the vmmap naming overlay. The `mem` accesses below land inside the [heap] and libc mappings the vmmap event names, so observed_data_spans derives two Unknown spans that the overlay must relabel to [heap] and libc.so.6 WITHOUT moving the floor."}
{"k":"vmmap","version":0,"maps_readable":true,"spans_total":3,"spans_truncated":false,"spans":[{"base":"0x400000","len":65536,"perms":"r-xp","name":"victim","path":"/tmp/victim"},{"base":"0x500000","len":262144,"perms":"rw-p","name":"[heap]"},{"base":"0x7f0000000000","len":2097152,"perms":"r-xp","name":"libc.so.6","path":"/usr/lib/libc.so.6"}]}
{"k":"codeimage","base":4194304,"len":64,"version":0,"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}
{"k":"df_step","step":0,"off":0,"disasm":"mov rax, [rbx]","ops":[{"space":"abs","addr":5246976,"size":8,"write":false,"value_valid":true,"value":1}]}
{"k":"df_step","step":1,"off":3,"disasm":"call libc","ops":[{"space":"abs","addr":139637976727552,"size":8,"write":false,"value_valid":true,"value":2}]}
{"k":"mem","step":0,"ea":5246976,"size":8,"rw":"r","space":"abs"}
{"k":"mem","step":1,"ea":139637976727552,"size":8,"rw":"r","space":"abs"}
{"k":"end","events":6,"truncated":false,"drops":{"lost":0,"throttled":false}}
```

- [ ] **Step 5: Assert the fixture decodes** — append to `desktop/test/test_vmmap.cpp`, before the `if (failures)` block. It needs `#include "view_test.h"`; if adding that pulls in unwanted deps, instead load with `asmdesk::load_recording_file` directly, which `doc/recording.o` already provides:

```cpp
    // The wire shape, end to end, from a hand-authored fixture.
    {
        std::string err;
        auto rec = load_recording_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/vmmap-named.asmtrace", err);
        check("fixture/loads", rec != nullptr, err);
        if (rec) {
            const space::VmMap m = space::vmmap_from_recording(*rec);
            check("fixture/present", m.present && m.readable, "");
            check("fixture/spans", m.spans.size() == 3,
                  std::to_string(m.spans.size()));
            check("fixture/hex-base-parsed",
                  m.spans.size() == 3 && m.spans[1].base == 0x500000,
                  "base is a hex STRING on the wire and must parse as one");
        }
    }
```

Add the fixture define to the test's object rule in `mk/desktop.mk` (the idiom is already used by `test_details_draw`):

```make
$(BUILD)/desktop/test/t/test_vmmap.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
```

- [ ] **Step 6: Wire the overlay into the weave** — in `desktop/src/ui/shell.cpp`, inside `draw_scene_overview`'s `if (!sv.built)` block, directly after the `regs.insert(regs.end(), obs.begin(), obs.end());` line:

```cpp
        // vmmap: name what we can. This runs AFTER observed_data_spans and
        // BEFORE build_projection, and it rewrites label/kind only — base and
        // len are untouched, which is why the floor cannot move (see
        // space/vmmap.h). A recording with no vmmap event is a no-op.
        const space::VmMap vm = space::vmmap_from_recording(r);
        const size_t named = space::vmmap_apply_names(regs, vm);
        std::string vmnote;
        if (vm.present && !vm.readable)
            vmnote = "address space NOT READABLE — no region could be named "
                     "(absent measurement, not a process without mappings)";
        else if (vm.present)
            vmnote = "address space: " + std::to_string(named) +
                     " region(s) named from " + std::to_string(vm.spans.size()) +
                     " of " + std::to_string(vm.spans_total) + " mapping(s)" +
                     (vm.truncated ? " (capped)" : "");
```

and stamp it on the projection beside the two notes that already ride there, immediately after `proj.data_span_note = std::move(span_note);`:

```cpp
        proj.vmmap_note = std::move(vmnote);
```

Add `#include "space/vmmap.h"` to `shell.cpp`'s includes, and the field to `Projection` in `desktop/src/space/types.h`, beside `data_span_note` / `layout_note`:

```cpp
    // vmmap: how far the address-space map reached, in the HUD's words. Empty
    // when the recording carried no `vmmap` (nothing to explain). Surfaced
    // exactly like data_span_note above.
    std::string vmmap_note;
```

**No new `SceneView` fields.** The HUD reads this through `Projection`, the same channel `data_span_note` and `layout_note` already use, because `placement_chips(terr, traj)` takes no `HudState`. Task 7 Step 3 only renders it.

- [ ] **Step 7: Run the tests**

```
make build/desktop_test_vmmap && ./build/desktop_test_vmmap
make build/desktop_test_recording && ./build/desktop_test_recording
make desktop-test
```
Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add desktop/src/ui/shell.cpp desktop/src/ui/shell.h \
        desktop/src/doc/recording.cpp desktop/test/test_recording.cpp \
        desktop/test/test_vmmap.cpp desktop/test/fixtures/vmmap-named.asmtrace \
        mk/desktop.mk
git commit -m "desktop: the weave names its regions from the recording's address-space map"
```

---

### Task 4: Producer — the maps walk, ranked, capped, as a JSON body

`scan_modules` (`cli/asmspy_proc.c:355`) cannot be reused: `if (path[0] != '/') continue;` **skips `[heap]`, `[stack]`, `[vdso]` and anon** — exactly the rows this needs — and `if (off != 0) continue;` keeps only offset-0 mappings, deduped by path. It is a module-base resolver, not a maps table. Write a fresh walk.

**Files:**
- Create: `cli/asmspy_vmmap.h` (header-only, the `asmspy_graphsort.h` / `asmspy_dataview.h` precedent)
- Create: `cli/test_vmmap.c`
- Modify: `mk/cli.mk`

**Interfaces:**
- Produces:
  - `typedef struct { uint64_t base, len; char perms[8]; char name[256]; char path[256]; } asmspy_vmspan_t;`
  - `int asmspy_vmmap_parse(FILE *f, asmspy_vmspan_t **out, size_t *n_total);` → rows kept (ranked, capped), `-1` on error; `*n_total` is rows seen BEFORE the cap.
  - `int asmspy_vmmap_body(const asmspy_vmspan_t *sp, size_t n, size_t total, int readable, unsigned version, char *buf, size_t cap);` → `0` ok, `-1` overflow (caller refuses loudly, never emits a half-token).
  - `#define ASMSPY_VMMAP_CAP 256`

- [ ] **Step 1: Write the failing test** — create `cli/test_vmmap.c`:

```c
/* test_vmmap.c — the /proc/<pid>/maps parse + rank + JSON body, over a STRING
 * fixture. No /proc, no ptrace: the part that can be wrong is the parse and the
 * cap discipline, and both are pure. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "asmspy_vmmap.h"

static int failures;
static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static const char *kMaps =
    "00400000-00410000 r-xp 00000000 08:01 100 /tmp/victim\n"
    "00500000-00540000 rw-p 00000000 00:00 0 [heap]\n"
    "7f0000000000-7f0000200000 r-xp 00000000 08:01 200 /usr/lib/libc.so.6\n"
    "7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 [stack]\n"
    "7f9000000000-7f9020000000 rw-p 00000000 00:00 0 \n";

int main(void) {
    FILE *f = fmemopen((void *)kMaps, strlen(kMaps), "r");
    asmspy_vmspan_t *sp = NULL;
    size_t total = 0;
    int n = asmspy_vmmap_parse(f, &sp, &total);
    fclose(f);
    check("parse/count", n == 5, "five rows in the fixture");
    check("parse/total", total == 5, "total counts rows BEFORE any cap");

    /* Anonymous rows are KEPT — they are most of what a data trace touches, and
     * dropping them is exactly the scan_modules behaviour this replaces. */
    int anon = 0, heap = 0, stack = 0;
    for (int i = 0; i < n; i++) {
        if (sp[i].name[0] == '\0') anon++;
        if (strcmp(sp[i].name, "[heap]") == 0) heap++;
        if (strcmp(sp[i].name, "[stack]") == 0) stack++;
    }
    check("parse/keeps-anon", anon == 1, "an anonymous mapping must survive");
    check("parse/keeps-heap", heap == 1, "[heap] must survive");
    check("parse/keeps-stack", stack == 1, "[stack] must survive");

    /* Ranked executable-first, so a cap can never drop libc while keeping a
     * larger anonymous reservation. */
    check("rank/exec-first", (sp[0].perms[2] == 'x'), "row 0 must be executable");
    /* NB: `sp` stays live — the body checks below still read it, and it is freed
     * once at the end. The cap block next owns its own allocation. */

    /* RANK BEFORE CAP — the assertion that matters. Build a table of CAP+1
     * enormous anonymous mappings plus ONE small executable one, so a naive
     * "cap first, then rank" drops the executable row (it is last in file order
     * and the smallest). This is the procinfo failure mode: capping first
     * "dropped libc itself while keeping dozens of zero-symbol rows". */
    {
        char *big = malloc(1 << 20);
        size_t o = 0;
        for (int i = 0; i < ASMSPY_VMMAP_CAP + 1; i++)
            o += (size_t)snprintf(big + o, (1 << 20) - o,
                                  "%08x000-%08x000 rw-p 00000000 00:00 0 \n",
                                  0x10000 + i, 0x10100 + i);
        snprintf(big + o, (1 << 20) - o,
                 "7fff00000000-7fff00001000 r-xp 00000000 08:01 9 /lib/libc.so.6\n");
        FILE *bf = fmemopen(big, strlen(big), "r");
        asmspy_vmspan_t *bs = NULL;
        size_t btotal = 0;
        int bn = asmspy_vmmap_parse(bf, &bs, &btotal);
        fclose(bf);
        check("cap/enforced", bn == ASMSPY_VMMAP_CAP,
              "the cap must bound what is kept");
        check("cap/total-is-pre-cap", btotal == (size_t)ASMSPY_VMMAP_CAP + 2,
              "spans_total must count rows SEEN, or truncation magnitude is "
              "unrecoverable");
        int kept_libc = 0;
        for (int i = 0; i < bn; i++)
            if (strcmp(bs[i].name, "libc.so.6") == 0)
                kept_libc = 1;
        check("cap/ranks-before-capping", kept_libc,
              "the one executable row must survive a cap full of larger "
              "anonymous ones — rank over the FULL table, then cap");
        free(bs);
        free(big);
    }

    /* The body is valid JSON and states cap, flag AND total. */
    char buf[64 * 1024];
    check("body/ok",
          asmspy_vmmap_body(sp, (size_t)n, total, 1, 0, buf, sizeof buf) == 0,
          "a small map must fit");
    check("body/has-total", strstr(buf, "\"spans_total\":5") != NULL, buf);
    check("body/has-flag", strstr(buf, "\"spans_truncated\":false") != NULL, buf);
    check("body/hex-base", strstr(buf, "\"base\":\"0x500000\"") != NULL,
          "addresses are hex STRINGS, never JSON numbers");
    check("body/len-number", strstr(buf, "\"len\":262144") != NULL,
          "lengths are magnitudes, so they stay numbers");

    /* Overflow REFUSES rather than truncating mid-token. */
    char tiny[64];
    check("body/refuses-overflow",
          asmspy_vmmap_body(sp, (size_t)n, total, 1, 0, tiny, sizeof tiny) == -1,
          "a body that will not fit must refuse, never emit a half-token");

    free(sp);
    if (failures) {
        fprintf(stderr, "test_vmmap: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_vmmap: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it and watch it fail**

```
make build/test_vmmap
```
Expected: `fatal error: asmspy_vmmap.h: No such file or directory`.

- [ ] **Step 3: Write `cli/asmspy_vmmap.h`**:

```c
/* asmspy_vmmap.h — /proc/<pid>/maps -> ranked, capped spans -> a JSON body.
 *
 * Header-only and PURE (it takes a FILE*, not a pid), which is the whole point:
 * the part that can be wrong is the parse and the cap discipline, and both are
 * testable from a string fixture with no /proc and no ptrace. Same extraction
 * rationale as asmspy_graphsort.h and asmspy_dataview.h.
 *
 * NOT reusable from scan_modules (asmspy_proc.c): that walks the same file but
 * drops `if (path[0] != '/')` — [heap], [stack], [vdso], anon — and
 * `if (off != 0)`, then dedups by path. It resolves module BASES. This needs the
 * table, and the rows it discards are most of what a data trace touches.
 */
#ifndef ASMSPY_VMMAP_H
#define ASMSPY_VMMAP_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtrace_ndjson.h"

/* Rank-then-cap. 256 spans is ~30 KB of body against a measured 97-118 JSON
 * bytes/span; a browser-class 5618-row map would be ~550 KB, so this is
 * mandatory rather than tidy. */
#define ASMSPY_VMMAP_CAP 256

typedef struct {
    uint64_t base, len;
    char perms[8];
    char name[256]; /* basename, or "[heap]"/"[stack]", or "" for anonymous */
    char path[256]; /* full pathname, or "" */
} asmspy_vmspan_t;

/* Rank: executable first, then descending length. Applied over the FULL table
 * before the cap — capping first is what "dropped libc itself while keeping
 * dozens of zero-symbol rows" in the procinfo modules case. */
static int asmspy_vmmap_rank(const void *a, const void *b) {
    const asmspy_vmspan_t *x = (const asmspy_vmspan_t *)a;
    const asmspy_vmspan_t *y = (const asmspy_vmspan_t *)b;
    int xe = x->perms[2] == 'x', ye = y->perms[2] == 'x';
    if (xe != ye)
        return ye - xe;
    if (x->len != y->len)
        return x->len < y->len ? 1 : -1;
    return x->base < y->base ? -1 : (x->base > y->base);
}

/* Parse every row of an open /proc/<pid>/maps. Returns rows KEPT (<= the cap),
 * or -1. *n_total is rows SEEN, so truncation magnitude is recoverable — the
 * stated v1 gap in procinfo's `modules`, fixed here rather than copied. */
static int asmspy_vmmap_parse(FILE *f, asmspy_vmspan_t **out, size_t *n_total) {
    size_t cap = 128, n = 0;
    asmspy_vmspan_t *sp = (asmspy_vmspan_t *)malloc(cap * sizeof *sp);
    char line[4096];
    if (!sp)
        return -1;
    *n_total = 0;
    while (fgets(line, sizeof line, f)) {
        uint64_t lo = 0, hi = 0, off = 0;
        char perms[8];
        int pathpos = 0;
        const char *p;
        asmspy_vmspan_t v;
        if (sscanf(line,
                   "%" SCNx64 "-%" SCNx64 " %7s %" SCNx64 " %*x:%*x %*u %n",
                   &lo, &hi, perms, &off, &pathpos) < 4)
            continue;
        if (hi <= lo)
            continue;
        (*n_total)++;
        memset(&v, 0, sizeof v);
        v.base = lo;
        v.len = hi - lo;
        snprintf(v.perms, sizeof v.perms, "%s", perms);
        /* Keep EVERY row, unlike scan_modules: an anonymous mapping is a real
         * place a trace touches, and dropping it is the defect this replaces. */
        p = pathpos > 0 ? line + pathpos : "";
        while (*p == ' ')
            p++;
        {
            char buf[256];
            size_t k = strcspn(p, "\n");
            if (k >= sizeof buf)
                k = sizeof buf - 1;
            memcpy(buf, p, k);
            buf[k] = '\0';
            if (buf[0] == '/') {
                const char *slash = strrchr(buf, '/');
                snprintf(v.path, sizeof v.path, "%s", buf);
                snprintf(v.name, sizeof v.name, "%s", slash ? slash + 1 : buf);
            } else if (buf[0] == '[') {
                snprintf(v.name, sizeof v.name, "%s", buf);
            } /* else: anonymous — name and path stay "" */
        }
        if (n == cap) {
            size_t nc = cap * 2;
            asmspy_vmspan_t *nv =
                (asmspy_vmspan_t *)realloc(sp, nc * sizeof *sp);
            if (!nv)
                break;
            sp = nv;
            cap = nc;
        }
        sp[n++] = v;
    }
    qsort(sp, n, sizeof *sp, asmspy_vmmap_rank);
    if (n > ASMSPY_VMMAP_CAP)
        n = ASMSPY_VMMAP_CAP;
    *out = sp;
    return (int)n;
}

/* Build the event BODY (no leading comma — asmtrace_emit prepends
 * {"k":"vmmap",). Returns 0, or -1 when it will not fit: the caller must refuse
 * loudly, never emit a half-token. Addresses are hex STRINGS (a JSON number is a
 * double in many readers and silently rounds a 64-bit pointer); lengths and
 * counts stay numbers. */
static int asmspy_vmmap_body(const asmspy_vmspan_t *sp, size_t n, size_t total,
                             int readable, unsigned version, char *buf,
                             size_t cap) {
    size_t o = 0;
    int w;
#define VM_APP(...)                                                            \
    do {                                                                       \
        w = snprintf(buf + o, cap - o, __VA_ARGS__);                           \
        if (w < 0 || (size_t)w >= cap - o)                                     \
            return -1;                                                         \
        o += (size_t)w;                                                        \
    } while (0)
    if (cap == 0)
        return -1;
    VM_APP("\"version\":%u,\"maps_readable\":%s,\"spans_total\":%llu,"
           "\"spans_truncated\":%s,\"spans\":[",
           version, readable ? "true" : "false", (unsigned long long)total,
           total > n ? "true" : "false");
    for (size_t i = 0; i < n; i++) {
        char en[256 * 6 + 16], ep[256 * 6 + 16];
        asmtrace_escape(en, sizeof en, sp[i].name);
        asmtrace_escape(ep, sizeof ep, sp[i].path);
        VM_APP("%s{\"base\":\"0x%llx\",\"len\":%llu,\"perms\":\"%s\"",
               i ? "," : "", (unsigned long long)sp[i].base,
               (unsigned long long)sp[i].len, sp[i].perms);
        if (en[0])
            VM_APP(",\"name\":\"%s\"", en);
        if (ep[0])
            VM_APP(",\"path\":\"%s\"", ep);
        VM_APP("}");
    }
    VM_APP("]");
#undef VM_APP
    return 0;
}

#endif /* ASMSPY_VMMAP_H */
```

- [ ] **Step 4: Register the test** in `mk/cli.mk` beside the other `cli/test_*.c` binaries, following the existing pattern for `test_arch`, and add it to `cli_smoke.sh`'s unit-test preamble near line 41:

```sh
"$BUILD/test_vmmap" || fail "test_vmmap"
```

- [ ] **Step 5: Run it**

```
make build/test_vmmap && ./build/test_vmmap
```
Expected: `test_vmmap: all checks passed`.

- [ ] **Step 6: Commit**

```bash
git add cli/asmspy_vmmap.h cli/test_vmmap.c cli/cli_smoke.sh mk/cli.mk
git commit -m "asmspy: parse, rank and encode /proc/<pid>/maps as a vmmap body"
```

---

### Task 5: Producer — emit at attach, refresh when it changes

**Files:**
- Modify: `cli/asmspy.c`
- Modify: `cli/cli_smoke.sh`

**Interfaces:**
- Consumes: `asmspy_vmmap_parse`, `asmspy_vmmap_body` (Task 4).
- Produces: `serve_vmmap_emit(serve_session_t *s)`, `serve_vmmap_refresh(serve_session_t *s)`.

- [ ] **Step 1: Add the digest field** to `serve_session_t` in `cli/asmspy.c`, beside the codeimage members (`ci_base`, `ci_len`, `img`):

```c
    /* vmmap: the SHA-256 of the last body emitted, so a refresh that finds an
     * unchanged address space emits nothing. serve_codeimage_refresh gets this
     * for free from the timeline library; a maps table has no such library, so
     * the gate lives here. Measured: 6 of 10 live processes were byte-identical
     * over 30 s, so this suppresses almost every refresh on a real target. */
    unsigned char vm_last[32];
    int vm_have;
    unsigned vm_version;
```

- [ ] **Step 2: Write the emitter** in `cli/asmspy.c`, beside `serve_codeimage_emit`:

```c
/* Emit one `vmmap`. Heap body + loud refusal, NEVER rec_emitf: that formats into
 * a 16 KB stack buffer and discards vsnprintf's return, so an oversized body
 * would emit syntactically invalid NDJSON with no flag (asmspy.c's rec_emitf).
 * This is the info_emit_json pattern. */
static void serve_vmmap_emit(serve_session_t *s) {
    char mp[64];
    FILE *f;
    asmspy_vmspan_t *sp = NULL;
    size_t total = 0;
    int n, readable = 1;
    char *body;
    unsigned char dg[32];

    if (!rec_on(&s->rec))
        return;
    snprintf(mp, sizeof mp, "/proc/%d/maps", (int)s->p.pid);
    f = fopen(mp, "r");
    if (!f) {
        /* ABSENT MEASUREMENT, not measured zero — the state of every process the
         * running user does not own. Say so; a reader must withhold every
         * conclusion drawn from an empty span list. */
        rec_emitf(&s->rec, "vmmap",
                  "\"version\":%u,\"maps_readable\":false,\"spans_total\":0,"
                  "\"spans_truncated\":false,\"spans\":[]",
                  s->vm_version);
        return;
    }
    n = asmspy_vmmap_parse(f, &sp, &total);
    fclose(f);
    if (n < 0) {
        free(sp);
        return;
    }
    body = malloc(256 * 1024);
    if (!body) {
        free(sp);
        return;
    }
    if (asmspy_vmmap_body(sp, (size_t)n, total, readable, s->vm_version, body,
                          256 * 1024) != 0) {
        /* Refuse loudly rather than emit a truncated token. */
        rec_emitf(&s->rec, "note",
                  "\"text\":\"vmmap: %d spans did not fit the body buffer; not "
                  "emitted\"",
                  n);
        free(body);
        free(sp);
        return;
    }
    /* Digest EXACTLY the payload that would be emitted — never a lossy canonical
     * form. Measured: a name-coalesced digest reported "unchanged" while the raw
     * rows moved +2/-6. */
    asmtrace_sha256((const unsigned char *)body, strlen(body), dg);
    if (s->vm_have && memcmp(dg, s->vm_last, sizeof dg) == 0) {
        free(body);
        free(sp);
        return; /* unchanged: no event */
    }
    memcpy(s->vm_last, dg, sizeof dg);
    s->vm_have = 1;
    rec_emit(&s->rec, "vmmap", body);
    s->vm_version++;
    free(body);
    free(sp);
}

/* Re-read after an invocation was captured. The address space barely moves on a
 * real target, so the digest above suppresses nearly every one of these; the
 * cost of a no-op refresh is one read of /proc/<pid>/maps. */
static void serve_vmmap_refresh(serve_session_t *s) { serve_vmmap_emit(s); }
```

Add `#include "asmspy_vmmap.h"` and confirm `asmtrace_sha256` is reachable (it is header-only in `cli/asmtrace_sha256.h`).

- [ ] **Step 3: Call it at attach** — in `serve_tracer`, in the `SM_DATAFLOW`, `SM_TRACE`, `SM_AUTO` and `SM_TREE` branches, immediately after the existing `serve_codeimage_arm(s)` call (and, where there is none, immediately after the recording is open and before the engine call). It must land **after** the header line: `LiveSession::feed_line` discards a pre-header event as malformed.

- [ ] **Step 4: Call the refresh** beside the two existing `serve_codeimage_refresh(s)` call sites in `serve_region_sink` and `serve_dataflow_sink`:

```c
    serve_vmmap_refresh(s);
```

- [ ] **Step 5: Add the smoke lane** — in `cli/cli_smoke.sh`, beside the existing `--serve dataflow (when)` block (~line 3918), following its exact idiom:

```sh
echo "--- serve: vmmap names the address space ---"
VM_OUT="$RECDIR/serve_vmmap.ndjson"
set +e
{
    printf '{"cmd":"start","mode":"dataflow","pid":%d,"func":"entered_often","max":64}\n' "$DFWPID"
    sleep 3
    printf '{"cmd":"quit"}\n'
    sleep 1
} | timeout 90 "$ASM" --serve >"$VM_OUT" 2>/dev/null
vmrc=$?
set -e
[ "$vmrc" -eq 124 ] && fail "--serve vmmap: hung"
[ "$vmrc" -eq 0 ] || fail "--serve vmmap: exited $vmrc"
nvm=$(grep -c '"k":"vmmap"' "$VM_OUT" || true)
[ "$nvm" -gt 0 ] || fail "--serve vmmap: no vmmap event emitted"
grep '"k":"vmmap"' "$VM_OUT" | grep -q '"spans_total":' \
    || fail "--serve vmmap: no spans_total (cap, flag AND total are all required)"
grep '"k":"vmmap"' "$VM_OUT" | grep -q '\[heap\]' \
    || fail "--serve vmmap: no [heap] row — anonymous mappings are being dropped"
grep '"k":"vmmap"' "$VM_OUT" | grep -q '\[stack\]' \
    || fail "--serve vmmap: no [stack] row"
grep '"k":"vmmap"' "$VM_OUT" | grep -qE '"base":"0x[0-9a-f]+"' \
    || fail "--serve vmmap: base is not a hex string"
echo "  $nvm vmmap event(s), with [heap] and [stack]"
```

- [ ] **Step 6: Run it**

```
make cli-smoke 2>&1 | tail -30
```
Expected: the new block prints its count and the smoke completes.

- [ ] **Step 7: Commit**

```bash
git add cli/asmspy.c cli/cli_smoke.sh
git commit -m "asmspy: serve emits the address-space map at attach, and again only when it changes"
```

---

### Task 6: The schema section

**Files:**
- Modify: `docs/internal/gui/asmtrace-schema.md`

- [ ] **Step 1: Add the reserved-kinds row** to the table in the *Reserved kinds* section, in the established format:

```markdown
| `vmmap` | one `/proc/<pid>/maps` address-space snapshot | [vmmap region naming](../../superpowers/specs/2026-08-08-vmmap-region-naming-design.md) |
```

- [ ] **Step 2: Add the defining section**, after the `procinfo` section. It MUST state, because a reader will otherwise assume the opposite:
  - The **stream-order forward-valid** rule: a `vmmap` describes the address space as read at its stream position and applies to events **after** it, up to the next `vmmap`. This is the `df_invocation` delimiter rule and is deliberately the **opposite** of `codeimage`, where inferring from wire order is forbidden.
  - `version` is an **ordinal, not a resolution key** — it counts emissions so a reader can say "the map changed twice"; resolution is by stream position alone.
  - `base` is a hex string; `len`, `spans_total` are numbers.
  - `maps_readable` gates the array and means **absent measurement**.
  - Cap = 256, ranked executable-first then by length, with `spans_total` **and** `spans_truncated`.
  - **Serve-only.** Headless `--record` and the golden corpus producer never emit it.
  - Not atomic: the read is 137 `read()` syscalls for a large map and the target is not quiesced, so a torn snapshot is possible.
  - Root pid only — followed children have their own address spaces and no sink surfaces them.

- [ ] **Step 3: Commit**

```bash
git add docs/internal/gui/asmtrace-schema.md
git commit -m "schema: define the vmmap kind, forward-valid by stream order"
```

---

### Task 7: The pick readout and the region roster

**Files:**
- Modify: `desktop/src/scene3d/hud.cpp` (the pick readout at ~line 346; a new roster block)
- Modify: `desktop/src/scene3d/hud.h`
- Modify: `desktop/test/test_shell.cpp`

- [ ] **Step 1: Enrich the pick readout.** At `hud.cpp:346` the readout is `r->label.empty() ? st.name : r->label`. Extend it so a region with an extent reports name, path, perms, and touched-of-extent — worded to keep the two grades distinct, since the extent is exact and the touched part is a lower bound:

```cpp
        if (r->extent_len > 0) {
            ImGui::TextColored(kDim, "%s  %s", r->perms.c_str(),
                               r->path.empty() ? "(anonymous)" : r->path.c_str());
            ImGui::TextColored(kDim, "touched at least %llu of %llu bytes",
                               (unsigned long long)r->len,
                               (unsigned long long)r->extent_len);
        }
```

- [ ] **Step 2: Add the roster.** A new HUD block listing each placed region — kind, label, extent, touched — with a button per row that sets `s.req_goto` plus `goto_u`/`goto_v` from the region's cell centre, reusing the existing framing path. It must state what it is not showing:

```cpp
        ImGui::TextColored(kDim, "%zu placed, %llu mapped-untouched",
                           terr.proj.regions.size(),
                           (unsigned long long)(s.vmmap_total >
                                                        terr.proj.regions.size()
                                                    ? s.vmmap_total -
                                                          terr.proj.regions.size()
                                                    : 0));
```

- [ ] **Step 3: Render the provenance chip.** Task 3 already computed `Projection::vmmap_note` and stamped it on the model — `placement_chips(terr, traj)` takes no `HudState`, which is why the note rides the projection like `data_span_note` and `layout_note`. This step only renders it, beside the observed-data chip at `hud.cpp:139`:

```cpp
    if (!terr.proj.vmmap_note.empty())
        out.push_back({terr.proj.vmmap_note.find("NOT READABLE") !=
                               std::string::npos
                           ? PlacementChip::Warn
                           : PlacementChip::Ok,
                       terr.proj.vmmap_note});
```

- [ ] **Step 4: Run the tests**

```
make build/desktop_test_shell && ./build/desktop_test_shell
make desktop-test
```

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/hud.cpp desktop/src/scene3d/hud.h desktop/test/test_shell.cpp
git commit -m "desktop: the 3D pick readout and a region roster read the named map"
```

---

### Task 8: The two visual layers

Both default **OFF**, joining the seven that already are (`confidence`, `opcode`, `data_relief`, `working_set`, `lifetime`, `data_ribbon`, `sediment`) — the re-lift-and-density-risk set. This is how they avoid competing with the 18 layers already on.

**Files:**
- Modify: `desktop/src/scene3d/scene.h` (the `SceneLayers` struct)
- Modify: `desktop/src/scene3d/layers.cpp` (the registry table)
- Modify: `desktop/test/test_layers.cpp`

- [ ] **Step 1: Add the flags** to `SceneLayers` in `desktop/src/scene3d/scene.h`, beside the other default-off members:

```cpp
    // vmmap: how much of each named mapping the capture actually reached. OFF by
    // default — the plane already spends colour on 18 layers, and this is a
    // re-lift of facts the roster states in words.
    bool occupancy = false;
    // vmmap: mark w+x mappings (a JIT arena) and dim read-only ones. OFF by
    // default, same reason.
    bool perms = false;
```

- [ ] **Step 2: Add the registry rows** to `desktop/src/scene3d/layers.cpp`, in the identical shape as the `sediment` row at line 67:

```cpp
        {"occupancy", "occupancy",
         "how much of this mapping did the capture actually reach?",
         G::Structure, LayerGrade::Derived, &SceneLayers::occupancy},
        {"perms", "permissions",
         "is this memory writable, executable, or both?", G::Structure,
         LayerGrade::Derived, &SceneLayers::perms},
```

- [ ] **Step 3: Run the exhaustiveness test**

```
make build/desktop_test_layers && ./build/desktop_test_layers
```
The registry test is constructed to have no hand-maintained field list, so it should pass without edits. If it fails, it is telling you a row is missing — add it rather than relaxing the test.

- [ ] **Step 4: Full suite, then commit**

```
make desktop-test
```

```bash
git add desktop/src/scene3d/scene.h desktop/src/scene3d/layers.cpp desktop/test/test_layers.cpp
git commit -m "desktop: occupancy and permissions layers, default off"
```

---

## Before you call it done

- [ ] `make desktop-test` — full suite green (100+ binaries).
- [ ] `make desktop` — the app links.
- [ ] `make cli-smoke` — including the new vmmap lane.
- [ ] `make asmtrace-golden-check` — **must be unchanged.** If a golden moved, `vmmap` escaped serve-only; find out why before regenerating.
- [ ] `make docker-docs` — the schema edit builds warnings-clean.
- [ ] Measure the one thing the spec left open: coalescing by `(name, exec-bit)`. Name-only measured 5618→212, name + full perms measured 5618→5509; the middle is a guess. If the ratio is poor, keep raw rows and lean on the 256 cap.
