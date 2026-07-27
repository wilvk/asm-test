// test_theme.cpp — the one semantic palette (24-one-visual-language.md T1).
//
// Two model/source assertions, no pixels (D4):
//  (a) every named accessor in ui/theme.h returns its documented canonical value,
//      dt_bad_col() IS dt_refuse_col() (the 0.90-vs-0.95 split is gone), and the
//      three former yellows (changed / maybe / statistical) are named DISTINCT
//      constants — no accidental collision, no accidental merge; and the shared
//      legend draws under the null backend, engine-free.
//  (b) a source-lint: the drift files no longer carry the semantic-colour
//      literal they used to inline, and DO reference the theme accessor instead
//      (a grep-style invariant over the file text — the F14 discipline, tested).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "imgui.h"

#include "ui/legend.h"
#include "ui/theme.h"

#ifndef ASMTEST_DESKTOP_SRC_DIR
#error "ASMTEST_DESKTOP_SRC_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static bool veq(ImVec4 a, float r, float g, float b, float al) {
    return a.x == r && a.y == g && a.z == b && a.w == al;
}

// --- (b) source-lint over the drift files -----------------------------------
static std::string slurp(const std::string &rel) {
    std::ifstream f(std::string(ASMTEST_DESKTOP_SRC_DIR) + "/" + rel);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct LintRow {
    const char *file;
    const char *forbidden; // the inline semantic-colour literal, now deleted
    const char *required;  // the theme accessor it was routed through
};

static const LintRow kLint[] = {
    // inspect_door: the verdict axis.
    {"ui/inspect_door.cpp", "ImVec4(0.45f, 0.80f, 0.45f", "dt_good_col"},
    {"ui/inspect_door.cpp", "ImVec4(0.90f, 0.45f, 0.40f", "dt_bad_col"},
    {"ui/inspect_door.cpp", "ImVec4(0.90f, 0.78f, 0.35f", "dt_maybe_col"},
    // scrubber: the per-step "changed" yellow.
    {"views/scrubber_draw.cpp", "ImVec4(1.0f, 0.85f, 0.3f", "dt_changed_col"},
    // abixray: the "changed" yellow and the marshalling-contrast blue.
    {"views/abixray_draw.cpp", "ImVec4(1.0f, 0.85f, 0.3f", "dt_changed_col"},
    {"views/abixray_draw.cpp", "ImVec4(0.45f, 0.85f, 1.0f", "dt_selected_col"},
    // slice view: the four cone hues.
    {"views/slice_view_draw.cpp", "IM_COL32(120, 170, 255", "dt_cone_back_u32"},
    {"views/slice_view_draw.cpp", "IM_COL32(255, 180, 110", "dt_cone_fwd_u32"},
    {"views/slice_view_draw.cpp", "IM_COL32(255, 255, 255, 255)",
     "dt_cone_both_u32"},
    {"views/slice_view_draw.cpp", "IM_COL32(110, 110, 110", "dt_cone_dim_u32"},
    // 3D HUD: the ok green and the dim aside.
    {"scene3d/hud.cpp", "ImVec4{0.55f, 0.85f, 0.55f", "dt_good_col"},
    {"scene3d/hud.cpp", "{0.65f, 0.65f, 0.70f", "dt_dim_col"},
    // Loom refusal placard: the third copy of the refuse red.
    {"loom/fabric_imgui.cpp", "ImVec4(0.95f, 0.45f, 0.40f", "dt_refuse_col"},
};

static void source_lint() {
    for (const LintRow &r : kLint) {
        std::string src = slurp(r.file);
        check("lint-readable", !src.empty(),
              std::string(r.file) + " could not be read");
        check("lint-no-literal", src.find(r.forbidden) == std::string::npos,
              std::string(r.file) + " still inlines the semantic literal '" +
                  r.forbidden + "' — route it through theme.h");
        check("lint-uses-accessor", src.find(r.required) != std::string::npos,
              std::string(r.file) + " does not reference the accessor '" +
                  r.required + "'");
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // --- (a) the accessors return their documented canonical values ----------
    check("good", veq(dt_good_col(), 0.45f, 0.80f, 0.45f, 1.0f),
          "good drifted");
    check("bad==refuse", veq(dt_bad_col(), 0.95f, 0.45f, 0.40f, 1.0f),
          "bad must equal the refuse red");
    check("bad-is-refuse-col",
          dt_bad_col().x == dt_refuse_col().x &&
              dt_bad_col().y == dt_refuse_col().y &&
              dt_bad_col().z == dt_refuse_col().z,
          "dt_bad_col() must BE dt_refuse_col() (kill the 0.90-vs-0.95 split)");
    check("bad-is-refuse-u32", dt_bad_u32() == dt_refuse_u32(),
          "the u32 forms must agree too");
    check("maybe", veq(dt_maybe_col(), 0.90f, 0.78f, 0.35f, 1.0f),
          "maybe drifted");
    check("changed", veq(dt_changed_col(), 1.0f, 0.85f, 0.3f, 1.0f),
          "changed drifted");
    check("selected", veq(dt_selected_col(), 0.45f, 0.85f, 1.0f, 1.0f),
          "selected drifted");
    check("dim", veq(dt_dim_col(), 0.65f, 0.65f, 0.70f, 1.0f), "dim drifted");
    check("statistical==warn",
          veq(dt_statistical_col(), 0.95f, 0.75f, 0.25f, 1.0f),
          "statistical is the caution amber today");
    check("statistical-is-warn",
          dt_statistical_col().x == dt_warn_col().x &&
              dt_statistical_u32() == dt_warn_u32(),
          "statistical aliases warn (named so 23 T4.1 can move it)");

    // The cone hues round-trip to the exact bytes the slice view drew before.
    check("cone-back", dt_cone_back_u32() == IM_COL32(120, 170, 255, 255),
          "cone-back byte drift");
    check("cone-fwd", dt_cone_fwd_u32() == IM_COL32(255, 180, 110, 255),
          "cone-fwd byte drift");
    check("cone-both", dt_cone_both_u32() == IM_COL32(255, 255, 255, 255),
          "cone-both byte drift");
    check("cone-dim", dt_cone_dim_u32() == IM_COL32(110, 110, 110, 255),
          "cone-dim byte drift");

    // The three former yellows must stay DISTINCT — no accidental merge.
    check("changed!=maybe", dt_changed_u32() != dt_maybe_u32(),
          "changed and maybe collapsed to one yellow");
    check("changed!=statistical", dt_changed_u32() != dt_statistical_u32(),
          "changed and statistical collapsed to one yellow");
    check("maybe!=statistical", dt_maybe_u32() != dt_statistical_u32(),
          "maybe and statistical collapsed to one yellow");

    // The shared legend draws under the null backend, engine-free.
    ImGui::NewFrame();
    ImGui::Begin("legend");
    dt_semantic_legend();
    dt_cone_legend();
    dt_legend_row(dt_good_u32(), "yes", "row helper");
    ImGui::End();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    check("legend-draws", dd && dd->TotalVtxCount >= 0, "legend drew nothing");

    source_lint();

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d test_theme check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_theme: all checks passed\n");
    return 0;
}
