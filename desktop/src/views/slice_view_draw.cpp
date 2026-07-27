// slice_view_draw.cpp — the ImGui half of the slice explorer. Draws only.
//
// Layered, not force-directed: the x position is the node's column (its rank by
// step index) and the y position is its arc lane, both decided by the builder.
// Nothing here iterates, settles, or randomises, so the same recording draws
// the same picture on every frame and in every session.
//
// Wrapped in an ImGuiEx::Canvas (15-plotting-and-graph-nav.md T2) for pan/zoom:
// the canvas only transforms the view, so the layered layout above stays
// deterministic — it just fixes the old unbounded rightward overflow by clipping
// to the viewport and letting the user navigate. Standalone canvas, no node
// semantics (the node-editor de-risk step).
#define IMGUI_DEFINE_MATH_OPERATORS // imgui_canvas.h uses ImRect / math operators
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_canvas.h"

#include "ui/legend.h"
#include "ui/theme.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {

// The four dependency-cone hues now live in ui/theme.h (T1) — this routes each
// case through the shared accessor, so a cone colour cannot drift from the
// legend that explains it (dt_cone_legend, drawn from the same accessors).
ImU32 cone_colour(dt_cone c) {
    switch (c) {
    case dt_cone::back:
        return dt_cone_back_u32(); // what produced the value
    case dt_cone::fwd:
        return dt_cone_fwd_u32(); // what it goes on to affect
    case dt_cone::both:
        return dt_cone_both_u32(); // the selection itself
    case dt_cone::dimmed:
        return dt_cone_dim_u32();
    case dt_cone::none:
        break;
    }
    return IM_COL32(200, 200, 200, 255); // defensive fallback (unreached)
}

// The SECOND CHANNEL for cone direction (24 T2 / F15): a shape glyph so a
// monochrome reader — or one who cannot separate the back-blue from the
// fwd-orange — still reads which way the dependency flows. The glyphs match
// dt_cone_legend / the encoding table exactly (back inflow, fwd outflow, both,
// off-cone), so the node and its legend can never disagree.
const char *cone_glyph(dt_cone c) {
    switch (c) {
    case dt_cone::back:
        return "\xE2\x97\x84"; // back-cone: produced it (inflow)
    case dt_cone::fwd:
        return "\xE2\x96\xBA"; // fwd-cone: affects (outflow)
    case dt_cone::both:
        return "\xE2\x97\x8F"; // the selection itself
    case dt_cone::dimmed:
        return "\xC2\xB7"; // off-cone
    case dt_cone::none:
        break;
    }
    return "";
}

} // namespace

void draw_slice_view(const dt_slice_view &v) {
    draw_banner(v.banner.c_str(), false);
    if (v.nodes.empty()) {
        ImGui::TextDisabled(
            "this recording carries no def-use edges — nothing to slice");
        return;
    }
    ImGui::Text("selected step: %s",
                v.selected_step ? std::to_string(*v.selected_step).c_str()
                                : "(none)");
    ImGui::TextDisabled("drag to pan, wheel to zoom");
    // The cone legend, drawn from the SAME ui/theme.h accessors cone_colour uses
    // — so the words below can never disagree with the hues above them (T1), and
    // each carries a second-channel glyph so the axis reads without colour (T2).
    dt_cone_legend();

    // A persistent canvas holds the pan/zoom view across frames (single ImGui
    // context, like the other addon draws). Its viewport is whatever space is
    // left; the content is navigated, not stretched — so a wide slice no longer
    // overflows the pane.
    static ImGuiEx::Canvas canvas;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 16.0f)
        avail.x = 16.0f;
    if (avail.y < 16.0f)
        avail.y = 16.0f;
    if (canvas.Begin("slice-canvas", avail)) {
        const float col_w = 120.0f, lane_h = 18.0f, node_r = 5.0f;
        const int max_lane = [&] {
            int m = 0;
            for (const dt_slice_edge &e : v.edges)
                m = e.lane > m ? e.lane : m;
            return m;
        }();
        const float height = lane_h * static_cast<float>(max_lane + 3);
        // GetCursorScreenPos is canvas-local here — the canvas has transformed the
        // coordinate system, so the existing layered drawing is unchanged.
        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float node_y = origin.y + height - lane_h;

        auto x_of_step = [&](uint32_t step) {
            for (const dt_slice_node &n : v.nodes)
                if (n.step == step)
                    return origin.x + col_w * static_cast<float>(n.column) +
                           node_r;
            return origin.x;
        };

        for (const dt_slice_edge &e : v.edges) {
            float x0 = x_of_step(e.from_step), x1 = x_of_step(e.to_step);
            float y = node_y - lane_h * static_cast<float>(e.lane + 1);
            ImU32 col = IM_COL32(150, 150, 150, 200);
            dl->AddLine(ImVec2(x0, node_y), ImVec2(x0, y), col);
            dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col);
            dl->AddLine(ImVec2(x1, y), ImVec2(x1, node_y), col);
        }
        for (const dt_slice_node &n : v.nodes) {
            float x = origin.x + col_w * static_cast<float>(n.column) + node_r;
            ImU32 col = cone_colour(n.style);
            dl->AddCircleFilled(ImVec2(x, node_y), node_r, col);
            // The direction glyph rides just above the node (T2 second channel):
            // the axis reads without depending on the back/fwd hue difference.
            dl->AddText(ImVec2(x - node_r, node_y - node_r - 14), col,
                        cone_glyph(n.style));
            dl->AddText(ImVec2(x - node_r, node_y + node_r + 2), col,
                        n.label.c_str());
        }
        canvas.End();
    }
    // Pan on left-drag, zoom on wheel, while the canvas is hovered.
    if (ImGui::IsItemHovered()) {
        ImGuiIO &io = ImGui::GetIO();
        ImVec2 vorigin = canvas.ViewOrigin();
        float vscale = canvas.ViewScale();
        if (io.MouseWheel != 0.0f) {
            float ns = vscale * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f);
            ns = ns < 0.1f ? 0.1f : (ns > 8.0f ? 8.0f : ns);
            canvas.SetView(vorigin, ns);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            canvas.SetView(
                ImVec2(vorigin.x + io.MouseDelta.x, vorigin.y + io.MouseDelta.y),
                vscale);
        }
    }
}

} // namespace asmdesk
