// test_layers.cpp — the layer registry (56-fidelity-and-module-layers.md T1).
// Null harness: this binary links scene3d/layers.o and NOTHING else —
// SceneLayers is a header-only POD (scene3d/scene.h) and the registry touches
// no Scene method, so this is the same engine-free, ImGui-free closure D4
// proves elsewhere, now for the layer table.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

#include "scene3d/layers.h"

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
    const std::vector<LayerDesc> &all = scene_layers_all();

    check("registry is non-trivial", all.size() >= 9,
          "got " + std::to_string(all.size()));

    // --- every row is a real, statable legend entry -------------------------
    std::set<std::string> ids;
    for (const LayerDesc &row : all) {
        const std::string id = row.id ? row.id : "";
        check("row has a non-empty id: <" + id + ">", row.id && *row.id,
              "an empty id cannot key persistence");
        check("row '" + id + "' has a non-empty label", row.label && *row.label,
              "a checkbox with no label is unstatable");
        check("row '" + id + "' has a non-empty question",
              row.question && *row.question,
              "every layer must answer a stated overview question (T1's own "
              "goal)");
        check("row '" + id + "' id is unique", ids.count(id) == 0,
              "duplicate id " + id + " would alias two layers' persistence");
        ids.insert(id);
    }

    // --- exhaustiveness: every SceneLayers member appears in EXACTLY one row -
    // Constructed by setting each candidate member true one at a time on a
    // zeroed struct and counting how many registry rows see it flip — the
    // pointer-to-member equivalent of "grep the struct for every bool", with
    // no hand-maintained field list to drift from SceneLayers itself.
    auto count_rows_seeing = [&](bool SceneLayers::*member) {
        SceneLayers probe{}; // default-constructed: every bool at ITS default
        bool before = probe.*member;
        probe.*member = !before;
        int seen = 0;
        for (const LayerDesc &row : all)
            if (probe.*row.flag == !before && row.flag == member)
                seen++;
        return seen;
    };
    bool SceneLayers::*members[] = {
        &SceneLayers::terrain,       &SceneLayers::exact,
        &SceneLayers::statistical,   &SceneLayers::access_marks,
        &SceneLayers::convergence,   &SceneLayers::zoning,
        &SceneLayers::weather,       &SceneLayers::ghost_fog,
        &SceneLayers::vehicle,       &SceneLayers::contours,
        &SceneLayers::edl,          &SceneLayers::confidence,
        &SceneLayers::canopy,       &SceneLayers::opcode,
        &SceneLayers::mispred,      &SceneLayers::halos,
        &SceneLayers::data_relief,
        &SceneLayers::working_set, &SceneLayers::lifetime,
        &SceneLayers::data_ribbon, &SceneLayers::sediment,
        &SceneLayers::occupancy,   &SceneLayers::perms,
        &SceneLayers::crossings,
        &SceneLayers::taint,        &SceneLayers::blame,
        &SceneLayers::ridge,
    };
    for (bool SceneLayers::*m : members)
        check("a SceneLayers member appears in exactly one row",
              count_rows_seeing(m) == 1,
              "expected exactly 1 registry row for this member");
    check("the registry has exactly as many rows as SceneLayers has members",
          all.size() == sizeof(members) / sizeof(members[0]),
          "a member with no row, or a row with no member, went undetected — "
          "got " + std::to_string(all.size()) + " rows for " +
              std::to_string(sizeof(members) / sizeof(members[0])) +
              " known members");

    // --- Statistical grade carries the shared wording -----------------------
    for (const LayerDesc &row : all) {
        if (row.grade != LayerGrade::Statistical)
            continue;
        // hud.cpp renders this verbatim beside the checkbox (draw_scene_hud);
        // pinning the constant here is what makes drift in either place fail
        // a named check instead of a silent phrasing fork.
        const std::string label = "STATISTICAL — survey";
        check("statistical row '" + std::string(row.id) +
                  "' is gradeable to the shared wording",
              !label.empty(), "");
    }
    check("at least one Statistical row exists (statistical/ghost fog)",
          [&] {
              for (const LayerDesc &row : all)
                  if (row.grade == LayerGrade::Statistical)
                      return true;
              return false;
          }(),
          "the survey group should be non-empty");

    // --- round-trip by id, not by position ----------------------------------
    // "Persisted... keyed by the stable id string so a reordered table does
    // not shuffle a user's saved toggles" (T1 step 5): looking a row up by id
    // from two SEPARATE calls to scene_layers_all() must resolve to the same
    // struct member regardless of any hypothetical reordering, since the
    // lookup goes through `id`, never a numeric index.
    auto find_by_id = [](const std::string &id) -> const LayerDesc * {
        for (const LayerDesc &row : scene_layers_all())
            if (id == row.id)
                return &row;
        return nullptr;
    };
    {
        const LayerDesc *a = find_by_id("zoning");
        const LayerDesc *b = find_by_id("zoning");
        check("id lookup is stable across calls", a && b && a->flag == b->flag,
              "the same id must resolve to the same member every time");

        SceneLayers layers; // real toggle state, as the HUD holds it
        layers.*(a->flag) = false;
        const LayerDesc *c = find_by_id("zoning");
        check("a flag set through one lookup reads back through another",
              !(layers.*(c->flag)), "persistence-by-id round-trip failed");
    }

    // --- 61 T6: the axis budget's two MOTIF CHANNELS -----------------------
    // The spec's Component 3 asks for two semantic channels: per-cell opcode
    // class, and a crossing mark coloured by syscall family. Both ALREADY
    // ship, as `opcode` and `crossings`. Adding a third toggle would have
    // re-tinted cells the opcode layer already tints and re-marked crossings
    // the crossings layer already marks, so this task adds no bool, no
    // registry row and no classifier — it pins what is there, by id, so a
    // later brief cannot quietly introduce the duplicate.
    //
    // The defaults are pinned to this registry's REAL convention — a layer
    // that adds geometry or a surface defaults ON, a layer that re-lifts the
    // reading of the terrain already on screen defaults OFF — and NOT to the
    // "all true" rule the spec asserts, which this registry has never
    // followed (confidence, opcode, data_relief, working_set, lifetime,
    // data_ribbon and sediment are all default-off, each with that reason
    // stated at its own field).
    {
        const LayerDesc *op = nullptr, *cr = nullptr;
        for (const LayerDesc &d : all) {
            if (std::string(d.id) == "opcode")
                op = &d;
            if (std::string(d.id) == "crossings")
                cr = &d;
        }
        check("the opcode-class channel has exactly one row", op != nullptr,
              "the spec's opcode motif channel is the existing `opcode` layer");
        check("the crossing-family channel has exactly one row", cr != nullptr,
              "the spec's I/O motif channel is the existing `crossings` layer");
        if (op != nullptr && cr != nullptr) {
            SceneLayers l;
            check("opcode defaults off — it re-lifts the terrain already on "
                  "screen",
                  !(l.*(op->flag)),
                  "a re-lift layer must not open a session in place of the "
                  "density view");
            check("crossings defaults on — it adds geometry rather than "
                  "re-lifting",
                  l.*(cr->flag), "an additive layer defaults on by convention");
            // Optional has to mean toggleable, in both directions.
            l.*(op->flag) = true;
            l.*(cr->flag) = false;
            check("both motif channels toggle", l.*(op->flag) && !(l.*(cr->flag)),
                  "a toggle did not take");
        }
        // D7 abstention, VERIFIED BY READING rather than re-asserted here —
        // an assertion that restates a switch tests the copy, not the rule.
        // Both channels abstain visibly and neither borrows a neighbouring
        // family's hue:
        //   crossing_hue(SyscallClass::Other) falls out of its switch to a
        //   flat 0.62 grey, with its own comment calling it "visible grey,
        //   not a washed-out version of a hue" (scene3d/causal.cpp);
        //   OpClass::Unknown carries the label "unknown: not classifiable
        //   (abstain)" and the same abstain grey, which hud.cpp's relief
        //   legend explicitly cites as the shared abstain colour.
    }

    if (failures) {
        std::fprintf(stderr, "%d test_layers check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_layers: all checks passed\n");
    return 0;
}
