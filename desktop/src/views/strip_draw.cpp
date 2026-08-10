// strip_draw.cpp — the session strip's painter and panel. Walks strip_plan's
// pixel-space prims and calls ImDrawList; nothing about zoom, bucketing or
// fidelity chrome lives here (that is strip.cpp's plan, where it is
// headlessly assertable). The Loom's painter split (fabric_imgui.cpp).
#define IMGUI_DEFINE_MATH_OPERATORS // ImZoomSlider: ImRect/ImMin/operators
#include "imgui.h"
#include "imgui_internal.h"
#include "ImZoomSlider.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "views/strip.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {

// scene3d/scene.cpp's OWN tid_color palette, copied VERBATIM — the same
// mirrored-not-shared convention scene2d_draw.cpp follows. A lane's hue is a
// presentation nicety, not a fidelity fact, but using the SAME table keeps a
// thread's colour consistent across every surface.
constexpr float kTidPalette[6][3] = {
    {0.95f, 0.85f, 0.25f}, {0.40f, 0.80f, 1.00f}, {0.55f, 0.95f, 0.45f},
    {1.00f, 0.55f, 0.55f}, {0.80f, 0.60f, 1.00f}, {0.35f, 0.95f, 0.85f}};

ImU32 tid_u32(uint32_t idx, float alpha) {
    const float *c = kTidPalette[idx % 6];
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
}

// The class hue for a rail/lane tick. Other stays a VISIBLE grey — the same
// never-green-on-unknown rule the crossing legend draws by.
ImU32 class_u32(space::SyscallClass c) {
    switch (c) {
    case space::SyscallClass::File: return IM_COL32(86, 156, 214, 255);
    case space::SyscallClass::Net: return IM_COL32(78, 201, 176, 255);
    case space::SyscallClass::Process: return IM_COL32(216, 160, 223, 255);
    case space::SyscallClass::Memory: return IM_COL32(220, 205, 125, 255);
    case space::SyscallClass::Signal: return IM_COL32(224, 108, 117, 255);
    case space::SyscallClass::Time: return IM_COL32(152, 195, 121, 255);
    case space::SyscallClass::Other: break;
    }
    return IM_COL32(128, 128, 128, 255);
}

// The outcome ring: Ok green / Error red / Unknown grey — never green on a
// row that did not parse.
ImU32 outcome_u32(space::SyscallOutcome o) {
    switch (o) {
    case space::SyscallOutcome::Ok: return IM_COL32(120, 200, 120, 255);
    case space::SyscallOutcome::Error: return IM_COL32(224, 90, 90, 255);
    case space::SyscallOutcome::Unknown: break;
    }
    return IM_COL32(150, 150, 150, 255);
}

// read cool / write warm — the data-cell family's rw convention.
ImU32 rw_u32(bool is_write, int alpha) {
    return is_write ? IM_COL32(224, 108, 117, alpha)
                    : IM_COL32(86, 156, 214, alpha);
}

void clipped_text(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b,
                  ImU32 col, const std::string &text) {
    if (text.empty())
        return;
    dl->PushClipRect(a, b, true);
    dl->AddText(ImVec2(a.x + 2, a.y + 1), col, text.c_str());
    dl->PopClipRect();
}

} // namespace

void draw_strip_plan(const std::vector<strip_prim_t> &prims,
                     std::string *hover) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 dim_col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 frame_col = ImGui::GetColorU32(ImGuiCol_Border);
    for (const auto &p : prims) {
        const ImVec2 a(o.x + p.x0, o.y + p.y0), b(o.x + p.x1, o.y + p.y1);
        switch (p.kind) {
        case strip_prim::run_tint:
            // alternation by GLOBAL run ordinal — stable while panning
            dl->AddRectFilled(a, b,
                              (p.a & 1) ? IM_COL32(128, 128, 160, 14)
                                        : IM_COL32(128, 160, 128, 8));
            break;
        case strip_prim::hud_note:
            clipped_text(dl, a, b, dim_col, p.text);
            break;
        case strip_prim::channel_absent:
            dl->AddRectFilled(a, b, IM_COL32(128, 128, 128, 20));
            clipped_text(dl, a, b, dim_col, p.text);
            break;
        case strip_prim::group_header:
            dl->AddRectFilled(a, ImVec2(b.x, a.y + 2.0f),
                              IM_COL32(200, 200, 200, 90));
            clipped_text(dl, ImVec2(a.x + 124.0f, a.y),
                         ImVec2(b.x, a.y + 14.0f), dim_col, p.text);
            break;
        case strip_prim::lane_header:
            clipped_text(dl, a, b, text_col, p.text);
            break;
        case strip_prim::lane_density:
            dl->AddRectFilled(
                a, b, IM_COL32(140, 170, 220, 40 + (p.b * 200) / 255));
            break;
        case strip_prim::lane_sys_tick:
        case strip_prim::rail_tick:
            dl->AddRectFilled(
                a, b, class_u32(static_cast<space::SyscallClass>(p.b / 4)));
            dl->AddRect(a, b,
                        outcome_u32(static_cast<space::SyscallOutcome>(p.b % 4)));
            break;
        case strip_prim::rail_overflow:
            clipped_text(dl, a, b, text_col, p.text);
            break;
        case strip_prim::rail_frame:
        case strip_prim::band_frame:
            dl->AddRect(a, b, frame_col);
            break;
        case strip_prim::band_label:
            clipped_text(dl, a, b, dim_col, p.text);
            break;
        case strip_prim::gap_notch:
            // two short diagonal cuts: "addresses elided here", never a
            // to-scale gap
            dl->AddLine(ImVec2(a.x + 4, a.y + 2), ImVec2(a.x + 10, b.y - 2),
                        dim_col);
            dl->AddLine(ImVec2(a.x + 10, a.y + 2), ImVec2(a.x + 16, b.y - 2),
                        dim_col);
            break;
        case strip_prim::mem_mark:
            dl->AddRectFilled(a, b, rw_u32(p.b & 1, 255));
            break;
        case strip_prim::mem_envelope:
            dl->AddRectFilled(a, b, rw_u32(p.b & 1, 96));
            break;
        case strip_prim::pc_mark:
            dl->AddRectFilled(a, b, tid_u32(p.b, 1.0f));
            break;
        case strip_prim::run_seam:
            dl->AddRectFilled(a, b,
                              p.b ? IM_COL32(200, 200, 120, 140)
                                  : IM_COL32(220, 220, 220, 140));
            clipped_text(dl, ImVec2(a.x + 3, a.y + 2),
                         ImVec2(a.x + 160, a.y + 16), dim_col, p.text);
            break;
        case strip_prim::torn_edge:
            // the Loom's torn meaning: the recorded window ended before the
            // run did — a jagged warning edge, not a wall
            for (float y = a.y; y + 8.0f <= b.y; y += 8.0f) {
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y + 4.0f),
                            IM_COL32(230, 200, 90, 200));
                dl->AddLine(ImVec2(b.x, y + 4.0f), ImVec2(a.x, y + 8.0f),
                            IM_COL32(230, 200, 90, 200));
            }
            break;
        }
        if (hover && mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y &&
            mouse.y < b.y && !p.text.empty())
            *hover = p.text; // topmost wins: later prims overwrite
    }
}

void draw_strip(StripState &st, const std::string &rec_id,
                const std::function<void(const dt_link &)> &go) {
    const StripModel &m = st.model;
    ImGui::TextUnformatted(m.hud.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(st.cam.follow_tail ? "following" : "follow"))
        st.cam.follow_tail = true;
    ImGui::SameLine();
    // The posture toggle: simplified (top-N + counted aggregates) is the
    // default; detail is one click away and back. Keyed by strip_plan_key,
    // so the flip re-plans exactly once.
    if (ImGui::SmallButton(st.cam.detail ? "simplify" : "detail"))
        st.cam.detail = !st.cam.detail;

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    st.cam.px_w = std::max(64.0f, avail.x);
    st.cam.px_h = std::max(96.0f, avail.y - 40.0f); // room for the zoom slider
    if (st.cam.seq_per_px <= 0) // first sight: fit the whole session
        st.cam.seq_per_px = std::max(
            1e-6, static_cast<double>(m.seq_end ? m.seq_end : 1) /
                      static_cast<double>(st.cam.px_w));
    if (st.cam.follow_tail)
        strip_view_follow(st.cam, m.seq_end);

    // The plan cache: a static frame — no growth, no interaction — walks the
    // cached vector and plans nothing (the Firefox-scale per-frame fix).
    const uint64_t key = strip_plan_key(st.cam, st.model_gen);
    if (key != st.plan_key) {
        strip_plan(m, st.cam, &st.plan_cache);
        st.plan_key = key;
    }
    const std::vector<strip_prim_t> &prims = st.plan_cache;

    ImGui::BeginChild("strip-canvas", ImVec2(st.cam.px_w, st.cam.px_h), true,
                      ImGuiWindowFlags_NoScrollbar);
    const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    std::string hover;
    draw_strip_plan(prims, &hover);
    if (ImGui::IsWindowHovered()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        // pick: TOPMOST prim under the mouse — walk the plan in reverse, and
        // resolve through the pure hover/link functions so a test can pin the
        // exact strings and links without a mouse
        for (auto it = prims.rbegin(); it != prims.rend(); ++it) {
            if (mp.x < canvas_origin.x + it->x0 ||
                mp.x >= canvas_origin.x + it->x1 ||
                mp.y < canvas_origin.y + it->y0 ||
                mp.y >= canvas_origin.y + it->y1)
                continue;
            const std::string tip = strip_hover_text(m, *it);
            if (tip.empty())
                continue;
            ImGui::SetTooltip("%s", tip.c_str());
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && go)
                if (auto lk = strip_click_link(m, *it, rec_id))
                    go(*lk);
            break;
        }
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            const StripLayout L = strip_layout(m, st.cam);
            strip_view_scroll_lanes(st.cam, static_cast<int>(m.lanes.size()),
                                    L.deck_h, wheel > 0 ? -1 : 1);
        }
    }
    ImGui::EndChild();

    // The windowed pan+zoom over the whole seq range (the Loom's ImZoomSlider
    // idiom): drag the middle to PAN, an edge to ZOOM. A window whose right
    // edge moved off the tail is a deliberate look-back: follow drops, and
    // the button / End re-arm it.
    {
        double wlo, whi;
        strip_view_window(st.cam, &wlo, &whi);
        const float fmax = static_cast<float>(m.seq_end ? m.seq_end : 1);
        float flo = static_cast<float>(wlo);
        float fhi = static_cast<float>(whi);
        if (flo < 0)
            flo = 0;
        if (fhi > fmax || fhi <= flo)
            fhi = fmax;
        ImGui::TextDisabled("%s", StripModel::axis_label());
        ImGui::PushID("strip-hzoom");
        if (ImZoomSlider::ImZoomSlider(0.0f, fmax, flo, fhi)) {
            strip_view_set_window(st.cam, flo, fhi);
            if (static_cast<double>(fhi) <
                static_cast<double>(m.seq_end) - 0.5)
                st.cam.follow_tail = false;
        }
        ImGui::PopID();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_End))
        st.cam.follow_tail = true;
}

} // namespace asmdesk
