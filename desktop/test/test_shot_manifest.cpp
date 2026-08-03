// test_shot_manifest.cpp — the screenshot manifest, and the rule that keeps the
// documentation honest: EVERY SceneKind must be covered by at least one shot.
//
// That exhaustiveness walk is the point of this file. A new scene kind that
// nobody screenshotted fails here rather than silently shipping a documented set
// that no longer documents everything — the same anti-drift discipline
// scene_axes() enforces for axis labels with its default-less switch.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "scene3d/scene_kind.h"
#include "ui/shot.h"

static int g_fail = 0;
static void check(const std::string &what, bool ok, const std::string &why) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) {
        std::printf("     %s\n", why.c_str());
        g_fail = 1;
    }
}

static std::string slurp(const char *p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    using namespace asmdesk;

    // --- parsing ------------------------------------------------------------
    {
        std::vector<ShotSpec> v;
        std::string err;
        const bool ok = shot_manifest_parse(
            R"([{"name":"a","open":["x.asmtrace"],"view":"scene3d",
                 "scene":"LanePrism","layers":["terrain"],
                 "size":[800,600],"warmup":7}])",
            v, err);
        check("parse: a well-formed entry is accepted", ok, err);
        check("parse: name survives", ok && v.size() == 1 && v[0].name == "a",
              "name lost");
        check("parse: size and warmup survive",
              ok && v.size() == 1 && v[0].width == 800 && v[0].height == 600 &&
                  v[0].warmup == 7,
              "size/warmup not carried");
        check("parse: scene name maps to its SceneKind",
              ok && v.size() == 1 && v[0].scene == scene3d::SceneKind::LanePrism,
              "scene name did not map");
        check("parse: open list survives",
              ok && v.size() == 1 && v[0].open.size() == 1 &&
                  v[0].open[0] == "x.asmtrace",
              "open list lost");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: an unknown scene name is REFUSED, not defaulted",
              !shot_manifest_parse(R"([{"name":"a","scene":"NoSuchScene"}])", v,
                                   err) &&
                  err.find("NoSuchScene") != std::string::npos,
              "an unknown scene silently became Plane — the shot would then "
              "document the wrong substrate with nothing to reveal it");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: an unknown view name is REFUSED",
              !shot_manifest_parse(R"([{"name":"a","view":"nope"}])", v, err) &&
                  err.find("nope") != std::string::npos,
              "an unknown view was silently defaulted");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: an unknown layer id is REFUSED at manifest level",
              !shot_manifest_parse(
                  R"([{"name":"a","layers":["no-such-layer"]}])", v, err) &&
                  err.find("no-such-layer") != std::string::npos,
              "a layer typo would silently produce an image missing that layer");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: a missing name is REFUSED",
              !shot_manifest_parse(R"([{"view":"canvas"}])", v, err) &&
                  !err.empty(),
              "an entry with no name has no output path");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: a non-positive size is REFUSED",
              !shot_manifest_parse(R"([{"name":"a","size":[0,100]}])", v, err) &&
                  !err.empty(),
              "a zero dimension would make an unusable framebuffer");
    }
    {
        std::vector<ShotSpec> v;
        std::string err;
        check("parse: malformed JSON fails with a message",
              !shot_manifest_parse("{ not json", v, err) && !err.empty(),
              "a corrupt manifest must not parse to an empty shot list");
    }

    // --- layers -------------------------------------------------------------
    {
        scene3d::SceneLayers L;
        L.terrain = false;
        std::string err;
        check("layers: a known id is applied",
              shot_apply_layers({"terrain"}, L, err) && L.terrain, err);
        check("layers: an unknown id is REFUSED",
              !shot_apply_layers({"no-such-layer"}, L, err) &&
                  err.find("no-such-layer") != std::string::npos,
              "an unknown layer id was ignored instead of reported");
    }
    {
        // SceneLayers defaults every flag TRUE, so "only these" needs a clear
        // first. Without it a two-layer shot is identical to any other, which
        // is exactly what happened before this was added.
        scene3d::SceneLayers L;
        check("layers: SceneLayers really does default everything on",
              L.terrain && L.exact && L.weather,
              "the premise of shot_clear_layers no longer holds");
        shot_clear_layers(L);
        check("layers: clear turns every registry layer off",
              !L.terrain && !L.exact && !L.weather && !L.zoning && !L.contours,
              "a layer survived shot_clear_layers");
        std::string err;
        shot_apply_layers({"terrain", "exact"}, L, err);
        check("layers: clear-then-apply leaves ONLY the named layers",
              L.terrain && L.exact && !L.weather && !L.zoning,
              "an unnamed layer is still lit, so shots of different layer sets "
              "would render identically");
    }

    // --- the committed manifest --------------------------------------------
    {
        const std::string text = slurp("desktop/shots.json");
        check("manifest: desktop/shots.json is readable", !text.empty(),
              "run this from the repo root");

        std::vector<ShotSpec> shots;
        std::string err;
        const bool ok = shot_manifest_parse(text, shots, err);
        check("manifest: desktop/shots.json parses", ok, err);

        // THE GATE: every SceneKind must appear in some 3D shot.
        for (scene3d::SceneKind k : scene3d::all_scene_kinds()) {
            bool covered = false;
            for (const ShotSpec &s : shots)
                if (s.view == ViewId::Scene3D && s.scene == k)
                    covered = true;
            check(std::string("manifest: SceneKind '") +
                      scene3d::scene_kind_name(k) + "' has a shot",
                  covered,
                  std::string("no shot covers '") + scene3d::scene_kind_name(k) +
                      "' — add one to desktop/shots.json, or the documented set "
                      "no longer documents every scene");
        }

        // Names must be unique, or one shot silently overwrites another's PNG.
        bool uniq = true;
        std::string dup;
        for (size_t i = 0; i < shots.size(); i++)
            for (size_t j = i + 1; j < shots.size(); j++)
                if (shots[i].name == shots[j].name) {
                    uniq = false;
                    dup = shots[i].name;
                }
        check("manifest: shot names are unique", uniq,
              "two shots share the name \"" + dup +
                  "\" and would overwrite each other's PNG");

        // Every shot must name at least one recording, or it photographs an
        // empty workspace.
        bool have_open = true;
        std::string bare;
        for (const ShotSpec &s : shots)
            if (s.open.empty()) {
                have_open = false;
                bare = s.name;
            }
        check("manifest: every shot opens a recording", have_open,
              "\"" + bare + "\" opens nothing and would photograph an empty app");
    }

    std::printf("%s\n", g_fail ? "test_shot_manifest: FAILURES"
                               : "test_shot_manifest: all ok");
    return g_fail;
}
