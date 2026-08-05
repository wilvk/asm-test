// flow.cpp — the narrow-pane layout primitives (see ui/flow.h for why).
//
// Public ImGui API only. Unlike ui/layout.cpp this is NOT an imgui_internal.h
// consumer, so it stays outside the doc-12 repin gate's first-party list.
#include "ui/flow.h"

#include <cstdarg>
#include <cstdio>

namespace asmdesk {

namespace {

// The absolute x of the content region's right edge.
//
// Derived from the cursor rather than the deprecated GetWindowContentRegionMax:
// after an item is submitted the cursor sits at the start of the NEXT line, so
// cursor.x + avail.x is exactly that edge — and it already accounts for the
// indent, the padding and a vertical scrollbar, none of which a window-relative
// maximum would have folded in.
float content_right_abs() {
    return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
}

// Resolve the caller's spacing: < 0 means "whatever bare SameLine() would use".
float resolve_spacing(float spacing) {
    return spacing < 0.0f ? ImGui::GetStyle().ItemSpacing.x : spacing;
}

} // namespace

float flow_button_w(const char *label) {
    // ImGui sizes a button as its label plus FramePadding on both sides. The
    // `true` hides any "##id" suffix, exactly as the button itself will.
    const ImVec2 t = ImGui::CalcTextSize(label, nullptr, true);
    return t.x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float flow_small_button_w(const char *label) {
    // SmallButton zeroes FramePadding.y only — the horizontal padding, and so
    // the width, is a plain button's. Kept as its own name because the call
    // sites read better paired with the widget they measure.
    return flow_button_w(label);
}

float flow_text_w(const char *text) {
    // No "##id" hiding here: Text draws the string verbatim, hashes and all.
    return ImGui::CalcTextSize(text, nullptr, false).x;
}

float flow_checkbox_w(const char *label) {
    const ImGuiStyle &st = ImGui::GetStyle();
    const ImVec2 t = ImGui::CalcTextSize(label, nullptr, true);
    // A square box of one frame height, then the label an inner spacing away.
    const float square = ImGui::GetFrameHeight();
    return square + (t.x > 0.0f ? st.ItemInnerSpacing.x + t.x : 0.0f);
}

float flow_radio_w(const char *label) {
    // ImGui's RadioButton and Checkbox share their sizing exactly: a square of
    // one frame height, then the label an ItemInnerSpacing away.
    return flow_checkbox_w(label);
}

float flow_textf_w(const char *fmt, ...) {
    // Formatted into a bounded buffer and measured whole. A truncated tail only
    // ever makes the measurement SMALLER, which can cost a wrap that was not
    // needed — never the unreachable-control failure the module exists to stop.
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return flow_text_w(buf);
}

float flow_avail_w() { return ImGui::GetContentRegionAvail().x; }

bool flow_fits(float next_w, float spacing) {
    const float gap = resolve_spacing(spacing);
    // GetItemRectMax is the item just submitted — the same one bare SameLine()
    // would rejoin.
    return ImGui::GetItemRectMax().x + gap + next_w <= content_right_abs();
}

bool flow_same_line(float next_w, float spacing) {
    if (flow_fits(next_w, spacing)) {
        // Pass `spacing` through untouched: SameLine reads < 0 as "style
        // default", the same convention this function takes.
        ImGui::SameLine(0.0f, spacing);
        return false;
    }
    // Wrapped: leave the cursor alone. The item starts the next line by itself.
    // An item wider than the whole content region lands here too, and that is
    // still the better outcome — it begins at the left edge, so its start is
    // legible, instead of beginning past the edge and drawing nothing at all.
    return true;
}

FlowRail flow_rail(float want_w, float min_main_w, float rail_frac,
                   float min_rail_h) {
    FlowRail r;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImGuiStyle &st = ImGui::GetStyle();

    // Side by side, the pair also costs one ItemSpacing between the children.
    if (avail.x >= want_w + st.ItemSpacing.x + min_main_w) {
        r.rail = ImVec2(want_w, 0.0f); // 0 height -> fill the pane
        r.main = ImVec2(0.0f, 0.0f);   // 0 -> take what the rail leaves
        r.stacked = false;
        return r;
    }

    // Too narrow to seat both across. Stack, and spend height instead.
    r.stacked = true;
    float rail_h = avail.y * rail_frac;
    if (rail_h < min_rail_h)
        rail_h = min_rail_h;
    const float room_for_main = avail.y - st.ItemSpacing.y - rail_h;
    if (room_for_main >= min_rail_h) {
        // Both fit within the pane: the main area fills what the rail leaves.
        r.rail = ImVec2(0.0f, rail_h);
        r.main = ImVec2(0.0f, 0.0f);
    } else {
        // The pane is too SHORT for both as well. Give each its floor and let
        // the overflow fall to the pane's own vertical scrollbar — the one axis
        // that scrolls. Squeezing the main area to nothing instead would
        // reintroduce, on the other axis, exactly the vanished-pane bug this
        // whole file exists to remove.
        r.rail = ImVec2(0.0f, min_rail_h);
        r.main = ImVec2(0.0f, min_rail_h);
    }
    return r;
}

} // namespace asmdesk
