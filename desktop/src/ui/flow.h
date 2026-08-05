// flow.h — the shell's narrow-pane layout primitives.
//
// Every pane in the shell is dockable, and the shipped split makes two of them
// NARROW by default (ui/layout.cpp: the left rail takes 0.20 of the window, the
// right inspector 0.28) — before the user drags a splitter anywhere. Two draw
// idioms break at that width, and both break SILENTLY:
//
//   1. A row of controls glued with ImGui::SameLine(). Past the content edge
//      ImGui clips the item, and no window here enables a horizontal scrollbar,
//      so an overflowing button is not merely off-screen — it is UNREACHABLE.
//   2. A fixed-width side rail: BeginChild(w) + SameLine() + BeginChild(main).
//      Once the pane is narrower than `w`, the main child starts past the right
//      edge and never draws at all.
//
// The fix for both is the same shape: measure BEFORE drawing, and spend the
// window's one free axis — vertical, which every pane already scrolls — instead
// of the horizontal one, which nothing scrolls. So a too-wide row becomes a
// too-tall one, and a too-narrow rail stacks above its main area.
//
// Nothing here decides a product rule; it is pure measurement over ImGui's
// current cursor and content region, which is what lets test_flow drive it
// headless under the null backend with no display.
#ifndef ASMDESK_UI_FLOW_H
#define ASMDESK_UI_FLOW_H

#include "imgui.h" // ImVec2, IM_FMTARGS

namespace asmdesk {

// --- measuring the NEXT item ------------------------------------------------
// The wrap decision has to happen before the item is drawn, so the width comes
// from the label rather than from a just-submitted item rect. These mirror
// ImGui's own sizing (CalcTextSize + the frame/button padding it adds), and
// honour the "##id" / "###id" label suffixes by hiding them like ImGui does.

// Width of a Button/SmallButton drawn with `label`.
float flow_button_w(const char *label);
float flow_small_button_w(const char *label);
// Width of a Text/TextDisabled/TextUnformatted run.
float flow_text_w(const char *text);
// Width of a Checkbox with `label` (box + inner spacing + label).
float flow_checkbox_w(const char *label);
// Width of a RadioButton — ImGui sizes it exactly as a Checkbox, but the call
// sites read better naming the widget they actually measure.
float flow_radio_w(const char *label);
// Width of a printf-formatted Text/TextDisabled run, for the many call sites
// whose trailing annotation is built from a format string.
float flow_textf_w(const char *fmt, ...) IM_FMTARGS(1);

// --- wrapping a row of controls ---------------------------------------------

// Continue the current line if an item `next_w` wide still fits within the
// window's content region; otherwise leave the cursor where it is, so the item
// falls onto a new line by itself. Call INSTEAD of ImGui::SameLine().
//
// Returns true when it wrapped (the caller starts a new line), which lets a
// caller drop a separator that would otherwise lead a line: `if
// (!flow_same_line(w)) ImGui::TextDisabled("|");`
//
// `spacing` < 0 uses the style's ItemSpacing.x, matching bare SameLine().
bool flow_same_line(float next_w, float spacing = -1.0f);

// The same decision without issuing SameLine — for a caller that must know
// whether a run of several items will fit before it commits to drawing any.
bool flow_fits(float next_w, float spacing = -1.0f);

// Content width available to the current window/child, in pixels.
float flow_avail_w();

// Some widgets size themselves from the ITEM WIDTH rather than from their
// content — a combo, a progress bar drawn at -FLT_MIN. There is no label to
// measure, and they do not clip when the line runs short: they starve, shrinking
// to a sliver that is still technically on screen and no longer readable. For
// those the wrap threshold is the width below which the widget stops doing its
// job, so it is a stated minimum rather than a measurement.
constexpr float kMinComboW = 180.0f;
constexpr float kMinProgressBarW = 120.0f;

// --- the side-rail pattern ---------------------------------------------------

// How a rail + main-area pair should be laid out at the current width. `rail`
// and `main` are ready to pass straight to ImGui::BeginChild.
//
// Side by side when there is room for both (`stacked == false`): the rail keeps
// its requested width, the main area takes the rest, and the caller issues the
// SameLine between them. Stacked when there is not (`stacked == true`): the
// rail spans the full width with a BOUNDED height and the main area follows
// BELOW it — no SameLine — so both stay on screen and each keeps its own
// vertical scroll. Nothing is ever pushed off the right edge.
struct FlowRail {
    ImVec2 rail{0, 0};    // size for the rail child
    ImVec2 main{0, 0};    // size for the main-area child
    bool stacked = false; // true -> do NOT SameLine between them
};

// Decide the split for a rail that wants `want_w` beside a main area needing at
// least `min_main_w`. Stacks when the content region cannot seat both.
//
// When stacked the rail is given `rail_frac` of the available HEIGHT (clamped to
// at least `min_rail_h`, and never so much that the main area is squeezed to
// nothing), and the main area takes the remainder — the 0-height "fill the rest"
// convention ImGui already uses. The defaults are the shell's: a rail may take
// up to 40% of a narrow pane, but never less than 120px of it.
FlowRail flow_rail(float want_w, float min_main_w, float rail_frac = 0.40f,
                   float min_rail_h = 120.0f);

} // namespace asmdesk
#endif // ASMDESK_UI_FLOW_H
