// timeline_draw.cpp — the ImGui half of the operand timeline. Draws only.
#define IMGUI_DEFINE_MATH_OPERATORS // ImZoomSlider (14 T5): ImRect/ImMin/operators
#include "imgui.h"
#include "imgui_internal.h"
#include "ImZoomSlider.h"
#include "implot.h" // whole-trace density strip (15 T1), context-guarded

#include <algorithm>
#include <cmath>
#include <vector>

#include "views/overview.h" // the pure minimap projection (21 T3)
#include "views/views_draw.h"

namespace asmdesk {

// The always-visible overview/minimap strip (21-spine-navigation.md T3). It is a
// compressed projection of the SAME rows the table below shows (Tracy's
// TimelineController idea) — never a separate layout, so it cannot fabricate
// structure the recording does not hold (docs 04/08 ban; overview_from_timeline
// keeps exactly the real steps). The ImPlot density degrades to a text ratio
// where no ImPlot context exists (the null test backend / the render-only viewer
// with plots off), so the model + its tests never depend on a context.
void draw_timeline_overview(const dt_timeline &t, uint32_t nsteps, double *lo,
                            double *hi,
                            const std::function<void(uint32_t)> &jump) {
    OverviewStrip strip = overview_from_timeline(t, *lo, *hi);

    ImGui::TextDisabled("overview (whole trace) — click to jump");
    if (ImPlot::GetCurrentContext() && !strip.buckets.empty()) {
        std::vector<double> xs, ys;
        xs.reserve(strip.buckets.size());
        ys.reserve(strip.buckets.size());
        double maxw = 1.0;
        for (const OverviewBucket &b : strip.buckets) {
            xs.push_back(static_cast<double>(b.step));
            ys.push_back(static_cast<double>(b.weight));
            maxw = std::max(maxw, static_cast<double>(b.weight));
        }
        const double extent = strip.nsteps ? static_cast<double>(strip.nsteps) : 1;
        if (ImPlot::BeginPlot("##tl-overview", ImVec2(-1, 48),
                              ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
                                  ImPlotFlags_NoTitle | ImPlotFlags_NoInputs)) {
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoDecorations,
                              ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxesLimits(0, extent, 0, maxw * 1.05, ImPlotCond_Always);
            ImPlot::PlotBars("density", xs.data(), ys.data(),
                             static_cast<int>(xs.size()), 0.9);
            // The current viewport, as two vertical markers over the whole-trace
            // density: the window the ImZoomSlider set (never a fabricated span).
            double bounds[2] = {*lo, *hi};
            ImPlot::PlotInfLines("viewport", bounds, 2);
            // Click-to-jump: the axis spans [0, extent] across the plot rect, so
            // the click's x-fraction maps straight to a step. Computed from the
            // plot rect (not ImPlot input, which NoInputs suppresses) so it is
            // robust, then routed out through the caller's dt_nav_go seam.
            if (ImGui::IsMouseClicked(0)) {
                ImVec2 pp = ImPlot::GetPlotPos();
                ImVec2 ps = ImPlot::GetPlotSize();
                ImVec2 m = ImGui::GetIO().MousePos;
                if (ps.x > 0 && m.x >= pp.x && m.x <= pp.x + ps.x &&
                    m.y >= pp.y && m.y <= pp.y + ps.y) {
                    double frac = static_cast<double>(m.x - pp.x) /
                                  static_cast<double>(ps.x);
                    if (jump)
                        jump(overview_click_step(strip, frac));
                }
            }
            ImPlot::EndPlot();
        }
    } else {
        // Null backend / plots off: the honest text ratio, still whole-trace.
        ImGui::TextDisabled(
            "  %zu step(s) recorded of %u; viewport [%.0f, %.0f]",
            strip.buckets.size(), strip.nsteps, *lo, *hi);
    }

    // The ImZoomSlider window control — this is precisely doc 14 T5's "timeline
    // windowing" follow-on: a pan/zoom over the whole step range writing the
    // [lo,hi] the strip draws. Its label is fixed, so PushID keeps it unique.
    float fmax = static_cast<float>(nsteps ? nsteps : 1);
    float flo = static_cast<float>(*lo);
    float fhi = static_cast<float>(*hi);
    if (flo < 0)
        flo = 0;
    if (fhi > fmax || fhi <= flo)
        fhi = fmax;
    ImGui::PushID("tl-hzoom");
    if (ImZoomSlider::ImZoomSlider(0.0f, fmax, flo, fhi)) {
        *lo = flo;
        *hi = fhi;
    }
    ImGui::PopID();
    ImGui::Separator();
}

void draw_timeline(const dt_timeline &t) {
    draw_banner(t.banner.c_str(), false);
    if (!ImGui::BeginTable("timeline", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("step", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("instruction");
    ImGui::TableSetupColumn("values");
    ImGui::TableSetupColumn("in/out", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    bool drew_separator = false;
    for (const dt_timeline_row &r : t.rows) {
        // The unaligned treatment: one visible break where the two runs stop
        // being comparable. Everything below it is A's alone.
        if (r.unaligned && !drew_separator) {
            drew_separator = true;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("- - -");
            ImGui::TableNextColumn();
            ImGui::TextDisabled(
                "the two recordings diverge here; rows below are A only");
        }
        ImGui::TableNextRow();
        // 22 T1: the brushed entity is cross-highlighted in place (the same entity
        // the other panes brush). 22 T3: every global-find hit is MARKED — find
        // measures, never hides, so the row stays and is merely lit. Selection wins
        // over a find tint when a row is both.
        const bool is_sel = t.selected_step && *t.selected_step == r.step;
        bool is_hit = false;
        for (uint32_t h : t.find_hits)
            if (h == r.step) {
                is_hit = true;
                break;
            }
        if (is_sel)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImGuiCol_Header));
        else if (is_hit)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(120, 100, 0, 90));
        const bool dim = r.style == dt_rowstyle::dimmed || r.missing;
        if (dim)
            ImGui::BeginDisabled();
        ImGui::TableNextColumn();
        ImGui::Text("%u", r.step);
        ImGui::TableNextColumn();
        if (r.missing)
            ImGui::TextUnformatted("(unknown)");
        else
            ImGui::Text("0x%llx", (unsigned long long)r.off);
        ImGui::TableNextColumn();
        if (r.missing)
            // A dropped step is UNKNOWN, not an instruction at offset 0.
            ImGui::TextUnformatted(
                "(no df_step event - this step was dropped)");
        else if (r.disasm.empty())
            ImGui::Text("0x%llx", (unsigned long long)r.off);
        else
            ImGui::TextUnformatted(r.disasm.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.ann.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%zu/%zu", r.n_in, r.n_out);
        if (dim)
            ImGui::EndDisabled();
    }
    ImGui::EndTable();
}

} // namespace asmdesk
