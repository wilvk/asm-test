// test_scene_kind.cpp — the SCENE-KIND contract of
// 59-standalone-scenes.md T1: every kind names its axes, the per-kind pick id
// bands cannot overlap, the Plane path is byte-identical to what it was before
// the brief, and each kind has a framing chosen for its own extents.
//
// Null harness, no display: this binary links the GL-free pick id space
// (s3/pick.o) + the pure standalone models (s3/standalone.o) and their pure
// closure — no ImGui, no GL, no engine (D4). A STANDALONE TU rather than an
// addition to test_drillin.cpp, so it never collides with that file's own
// concurrent work (the same reason test_goto.cpp is its own binary).
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "scene3d/camera.h"
#include "scene3d/pick.h"
#include "scene3d/scene_kind.h"
#include "scene3d/standalone.h"

using namespace asmdesk;
using namespace asmdesk::scene3d;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

int main() {
    // --- T1 step 4: EVERY kind labels EVERY axis --------------------------
    // The exhaustiveness assertion. A scene cannot ship an unlabelled axis
    // because the field is required; this is what makes "required" checkable
    // rather than aspirational.
    {
        const std::vector<SceneKind> &kinds = all_scene_kinds();
        check("axes/enumeration is complete",
              kinds.size() ==
                  static_cast<size_t>(SceneKind::LanePrism) + 1u,
              "all_scene_kinds() has " + std::to_string(kinds.size()) +
                  " entries but the enum's last value is " +
                  std::to_string(
                      static_cast<int>(SceneKind::LanePrism)));
        std::set<std::string> names, ys;
        for (SceneKind k : kinds) {
            const std::string name = scene_kind_name(k);
            const SceneAxes ax = scene_axes(k);
            check("axes/" + name + ": X is labelled",
                  ax.x != nullptr && ax.x[0] != '\0', "empty X label");
            check("axes/" + name + ": Y is labelled",
                  ax.y != nullptr && ax.y[0] != '\0', "empty Y label");
            check("axes/" + name + ": Z is labelled",
                  ax.z != nullptr && ax.z[0] != '\0', "empty Z label");
            // D7: the vertical axis must state what it is NOT. A new substrate
            // is a new opportunity to imply a correspondence, and the positive
            // claim alone is exactly the fabrication this family avoids.
            check("axes/" + name + ": Y states what it is NOT",
                  ax.y_not != nullptr && ax.y_not[0] != '\0',
                  "empty y_not — the vertical axis makes only a positive claim");
            check("axes/" + name + ": the denial is a denial",
                  std::string(ax.y_not).find("NOT") != std::string::npos ||
                      std::string(ax.y_not).find("not") != std::string::npos,
                  "y_not does not actually deny anything: '" +
                      std::string(ax.y_not) + "'");
            check("axes/" + name + ": four lines",
                  scene_axis_lines(k).size() == 4,
                  "scene_axis_lines is not X/Y/Z/denial");
            names.insert(name);
            ys.insert(ax.y);
        }
        check("axes: every kind has a distinct name",
              names.size() == kinds.size(), "two kinds share a name");
        // The whole reason these are standalone: their VERTICAL axes differ.
        // Two kinds with the same vertical axis would belong on one substrate.
        check("axes: every kind has a distinct vertical axis",
              ys.size() == kinds.size(),
              "two kinds claim the same vertical axis — they would compose, "
              "and this brief exists for the ones that cannot");
    }

    // --- T1 step 3: pick bands, ALLOCATED not inferred --------------------
    {
        // The plane's id space is unchanged: kind 0 shifts to 0, so every
        // pre-59 id is byte-identical. This is the check that a change to the
        // band scheme cannot quietly break every existing golden pick.
        for (uint32_t n : {1u, 8u, 64u, 256u}) {
            check("bands/plane cell ids unchanged (n=" + std::to_string(n) +
                      ")",
                  pick_id_cell(0) == 1u && pick_id_cell(n * n - 1) == n * n,
                  "pick_id_cell drifted");
            check("bands/plane vertex ids unchanged (n=" + std::to_string(n) +
                      ")",
                  pick_id_vertex(n, 0) == n * n + 1u,
                  "pick_id_vertex drifted");
            check("bands/plane ids stay in the Plane band (n=" +
                      std::to_string(n) + ")",
                  pick_id_kind(pick_id_cell(n * n - 1)) == SceneKind::Plane &&
                      pick_id_kind(pick_id_vertex(n, 1000)) == SceneKind::Plane,
                  "a plane id escaped into another kind's band");
        }

        // Two kinds' bands do not overlap, at SEVERAL sizes INCLUDING EMPTY.
        const std::vector<SceneKind> &kinds = all_scene_kinds();
        for (uint64_t count : {uint64_t{0}, uint64_t{1}, uint64_t{7},
                               uint64_t{1000}, uint64_t{100000}}) {
            std::set<uint32_t> seen;
            bool overlap = false;
            for (SceneKind k : kinds) {
                if (k == SceneKind::Plane)
                    continue; // the plane's own bands are 47 T3's, tested there
                for (uint64_t i = 0; i < count; i++) {
                    const uint32_t id = pick_id_element(k, i);
                    check("bands/id is non-zero", id != 0,
                          "element " + std::to_string(i) + " of " +
                              scene_kind_name(k) + " encoded as background");
                    if (!seen.insert(id).second)
                        overlap = true;
                }
            }
            check("bands/no overlap at count=" + std::to_string(count),
                  !overlap,
                  "two kinds produced the same pick id — the aliasing bug the "
                  "brief names");
        }

        // A decode is bounded by what was REALLY uploaded, and refuses an id
        // from a different substrate outright.
        PickBands b;
        b.kind = SceneKind::Invocation;
        b.nelem = 3;
        check("bands/decode: in range",
              decode_pick_standalone(pick_id_element(SceneKind::Invocation, 2),
                                     b) == std::optional<uint64_t>(2),
              "element 2 did not round-trip");
        check("bands/decode: past the live geometry refuses",
              !decode_pick_standalone(
                  pick_id_element(SceneKind::Invocation, 3), b),
              "an id past nelem decoded to an element");
        check("bands/decode: another kind's id refuses",
              !decode_pick_standalone(
                  pick_id_element(SceneKind::LanePrism, 0), b),
              "a LanePrism id decoded as an Invocation element — exactly the "
              "aliasing an inferred boundary produces");
        check("bands/decode: background refuses",
              !decode_pick_standalone(0, b), "id 0 decoded to an element");
        PickBands empty;
        empty.kind = SceneKind::Divergence;
        check("bands/decode: an empty scene has no picks",
              !decode_pick_standalone(
                  pick_id_element(SceneKind::Divergence, 0), empty),
              "an empty band still decoded");

        // The plane's own decoder rejects a foreign-kind id rather than
        // aliasing it onto a cell.
        const Pick p =
            decode_pick(pick_id_element(SceneKind::ModuleRibbon, 4), 64);
        check("bands/decode_pick refuses a foreign kind", p.kind == Pick::None,
              "a ModuleRibbon id decoded as a plane cell/vertex");

        // A local id past the band's capacity is refused, not wrapped.
        check("bands/capacity is stated, not assumed",
              pick_id_fits(kPickKindCapacity) &&
                  !pick_id_fits(uint64_t{kPickKindCapacity} + 1),
              "pick_id_fits does not bound the band");
        check("bands/an over-capacity element emits nothing",
              pick_id_element(SceneKind::LanePrism,
                              uint64_t{kPickKindCapacity}) == 0,
              "an over-capacity element wrapped into the next kind's band");
    }

    // --- T1: per-kind camera framing (48's movable target, not a 2nd camera)
    {
        const Camera plane = standalone_default_camera(SceneKind::Plane);
        const Camera fresh;
        check("camera/Plane keeps its own default untouched",
              plane.target[0] == fresh.target[0] &&
                  plane.target[1] == fresh.target[1] &&
                  plane.target[2] == fresh.target[2] &&
                  plane.radius == fresh.radius && plane.yaw == fresh.yaw &&
                  plane.pitch == fresh.pitch,
              "the plane's framing changed — 56/57/58 ride it");
        for (SceneKind k : all_scene_kinds()) {
            if (k == SceneKind::Plane)
                continue;
            const Camera c = standalone_default_camera(k);
            check(std::string("camera/") + scene_kind_name(k) +
                      ": radius is in the dolly range",
                  c.radius >= Camera::kMinRadius && c.radius <= Camera::kMaxRadius,
                  "radius " + std::to_string(c.radius) + " is out of range");
            check(std::string("camera/") + scene_kind_name(k) +
                      ": pitch is inside the pole clamp",
                  c.pitch > -Camera::kPitchLimit && c.pitch < Camera::kPitchLimit,
                  "pitch would degenerate look_at");
            check(std::string("camera/") + scene_kind_name(k) +
                      ": the target is lifted off the plane floor",
                  c.target[1] > 0.0f,
                  "a standalone scene's mass sits at mid height; the plane "
                  "orbit's y=0 target was not chosen for it");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d scene-kind check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_scene_kind: all checks passed\n");
    return 0;
}
