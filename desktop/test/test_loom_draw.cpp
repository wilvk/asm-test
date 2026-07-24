// test_loom_draw.cpp — the Loom painter's smoke under ImGui's null backend
// (05-loom-day-one.md T2 step 3). No display, no GL, no engines: it proves the
// painter walks every prim kind the planner can emit without asserting inside
// ImGui, which is the only thing a painter test can honestly claim.
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "loom/loom_draw.h"
#include "loom_fixture.h"

using namespace asmdesk;
using namespace loomfx;

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    Fixture fx;
    loom_provenance_t p = prov();
    p.truncated = true;
    p.steps_total = 19;
    loom_fabric_t f = build(fx, p);

    // Three cameras so the painter sees rectangles, byte rows AND ribbon.
    std::vector<loom_view_t> cams;
    {
        loom_view_t v;
        v.steps_per_px = 0.05;
        v.px_w = 800;
        v.px_h = 400;
        v.lane_h = 18;
        cams.push_back(v);
        v.steps_per_px = 4.0; // collapsed
        cams.push_back(v);
        v.steps_per_px = 0.01; // byte rows
        v.lane_h = 200;
        v.lane0 = band_lane(f);
        cams.push_back(v);
    }

    // Plus one hand-built plan carrying the fork prims (T6) so no prim kind in
    // the enum goes unpainted by this smoke.
    std::vector<loom_prim_t> forks;
    for (loom_prim k :
         {loom_prim::take_dim, loom_prim::take_hot, loom_prim::take_dashed_tail,
          loom_prim::patient_zero, loom_prim::fault_card}) {
        loom_prim_t x;
        x.kind = k;
        x.x0 = 10;
        x.y0 = 10;
        x.x1 = 120;
        x.y1 = 40;
        x.text = loom_prim_name(k);
        forks.push_back(x);
    }

    size_t painted = 0;
    for (size_t n = 0; n < cams.size() + 1; n++) {
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Begin("loom");
        std::vector<loom_prim_t> prims;
        if (n < cams.size())
            loom_plan(f, cams[n], &prims);
        else
            prims = forks;
        painted += prims.size();
        std::string hover;
        draw_loom_plan(prims, &hover);
        ImGui::End();
        ImGui::Render();
        if (ImGui::GetDrawData() == nullptr) {
            std::fprintf(stderr, "test_loom_draw: FAIL: null draw data\n");
            return 1;
        }
    }
    // The PANEL itself, over real recordings: one that weaves and one that must
    // refuse. A refusal that crashed the panel would be worse than a wrong
    // fabric, so it is driven here rather than only reasoned about.
    for (const char *name :
         {"loom-df-chain.asmtrace", "loom-truncated.asmtrace",
          "export/survey-only.asmtrace"}) {
        std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
        std::string err;
        auto rec = load_recording_file(path, err);
        if (!rec) {
            std::fprintf(stderr, "test_loom_draw: FAIL: %s: %s\n", name,
                         err.c_str());
            return 1;
        }
        Workspace ws;
        ws.recordings.push_back(*rec);
        Streams st = decode_streams(*rec);
        LoomState L;
        for (int n = 0; n < 2; n++) {
            io.DisplaySize = ImVec2(1280, 720);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            ImGui::Begin("loom panel");
            draw_loom(L, st, ws, 0);
            ImGui::End();
            ImGui::Render();
        }
        const bool should_weave =
            std::string(name).find("survey-only") == std::string::npos;
        if (should_weave == !L.err.empty()) {
            std::fprintf(stderr, "test_loom_draw: FAIL: %s wove=%d err='%s'\n",
                         name, !L.err.empty() ? 0 : 1, L.err.c_str());
            return 1;
        }
    }

    ImGui::DestroyContext();

    if (painted == 0) {
        std::fprintf(stderr, "test_loom_draw: FAIL: nothing was painted\n");
        return 1;
    }
    std::printf("test_loom_draw: PASS (%zu prims painted)\n", painted);
    return 0;
}
