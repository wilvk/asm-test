// slice_view_draw.cpp — the ImGui half of the slice explorer. Draws only.
//
// Layered, not force-directed: the x position is the node's column (its rank by
// step index) and the y position is its arc lane, both decided by the builder.
// Nothing here iterates, settles, or randomises, so the same recording draws
// the same picture on every frame and in every session.
#include "imgui.h"

#include "views/views_draw.h"

namespace asmdesk {

namespace {

ImU32 cone_colour(dt_cone c) {
    switch (c) {
    case dt_cone::back:
        return IM_COL32(120, 170, 255, 255); // what produced the value
    case dt_cone::fwd:
        return IM_COL32(255, 180, 110, 255); // what it goes on to affect
    case dt_cone::both:
        return IM_COL32(255, 255, 255, 255); // the selection itself
    case dt_cone::dimmed:
        return IM_COL32(110, 110, 110, 255);
    case dt_cone::none:
        break;
    }
    return IM_COL32(200, 200, 200, 255);
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
    ImGui::TextDisabled("blue = what produced this value; orange = what it "
                        "affects; grey = outside both cones");

    const float col_w = 120.0f, lane_h = 18.0f, node_r = 5.0f;
    const int max_lane = [&] {
        int m = 0;
        for (const dt_slice_edge &e : v.edges)
            m = e.lane > m ? e.lane : m;
        return m;
    }();
    const float height = lane_h * static_cast<float>(max_lane + 3);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float node_y = origin.y + height - lane_h;

    auto x_of_step = [&](uint32_t step) {
        for (const dt_slice_node &n : v.nodes)
            if (n.step == step)
                return origin.x + col_w * static_cast<float>(n.column) + node_r;
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
        dl->AddCircleFilled(ImVec2(x, node_y), node_r, cone_colour(n.style));
        dl->AddText(ImVec2(x - node_r, node_y + node_r + 2),
                    cone_colour(n.style), n.label.c_str());
    }
    ImGui::Dummy(ImVec2(col_w * static_cast<float>(v.nodes.size()),
                        height + lane_h * 2));
}

} // namespace asmdesk
