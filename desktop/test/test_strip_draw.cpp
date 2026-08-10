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

// one prim of EVERY kind, in a synthetic pixel-space plan — the painter must
// rasterise all of them without a model in sight
static std::vector<strip_prim_t> every_prim() {
    std::vector<strip_prim_t> v;
    auto add = [&](strip_prim k, float x0, float y0, float x1, float y1,
                   const char *t) {
        v.push_back(strip_prim_t{k, x0, y0, x1, y1, 0, 0, t});
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

static StripState st; // shared with the capture-less frame bodies below

static void frame(void (*body)()) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.f / 60.f;
    ImGui::NewFrame();
    // a first-frame window is auto-fit-pending (zero content size) and its
    // draw commands would be clipped to nothing — give it a real size so the
    // geometry oracle sees the vertices
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1000, 640), ImGuiCond_Always);
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

    // 1) painter smoke: every prim kind rasterises
    frame([] {
        auto prims = every_prim();
        std::string hover;
        draw_strip_plan(prims, &hover);
    });
    ImDrawData *dd = ImGui::GetDrawData();
    check("painter smoke: draw data exists", dd != nullptr, "");
    check("painter smoke: geometry emitted", dd && dd->TotalVtxCount > 0,
          "every prim kind must rasterise to vertices");

    // 2) the full panel over a tiny real model, three cameras (mark mode,
    //    the threshold edge, deep envelope mode)
    {
        std::string nd =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"})"
            "\n"
            R"({"k":"trace","basis":"abs","off":4112,"tid":10})"
            "\n"
            R"({"k":"syscall","line":"openat(AT_FDCWD, <path>) = 3","tid":10})"
            "\n"
            R"({"k":"mem","step":0,"ea":4200,"size":8,"rw":"w","space":"abs"})"
            "\n"
            R"({"k":"end","events":3})"
            "\n";
        std::istringstream in(nd);
        std::string err;
        auto rec = load_recording(in, err);
        check("panel fixture loads", rec.has_value(), err);
        space::Region code;
        code.base = 0x1000;
        code.len = 0x1000;
        code.kind = space::Region::Code;
        code.label = "code";
        if (rec)
            st.model = strip_build(*rec, {code}, {});
    }
    const double zooms[3] = {0.05, 4.0, 400.0};
    for (double z : zooms) {
        st.cam = strip_view_t{};
        st.cam.seq_per_px = z;
        frame([] { draw_strip(st, "rec-x", [](const dt_link &) {}); });
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
