// fabric_imgui.cpp — the ImGui painter for the Loom's draw plan. Draws only.
#define IMGUI_DEFINE_MATH_OPERATORS // ImZoomSlider (14 T5): ImRect/ImMin/operators
#include "imgui.h"
#include "imgui_internal.h"
#include "ImZoomSlider.h"

#include <algorithm>

#include "loom/loom_draw.h"
#include "nav.h"             // dt_link — the minimap click routes through it (21 T3)
#include "ui/legend.h"
#include "ui/primer.h"
#include "ui/terms.h"
#include "ui/theme.h"
#include "ui/timepos.h"
#include "views/overview.h" // whole-fabric minimap projection (21 T3)

namespace asmdesk {

namespace {

// Every honesty prim shares the warn colour with the rest of the app's placards
// via the shared palette (ui/theme.h), so a torn edge cannot be mistaken for
// ordinary chrome and ordinary chrome cannot be mistaken for a warning. The
// draw-list ImU32 form packs from the same floats every TextColored banner uses.
const ImU32 kWarn = dt_warn_u32();
constexpr ImU32 kSpan = IM_COL32(90, 150, 220, 220);
constexpr ImU32 kSpanDim = IM_COL32(90, 150, 220, 60);
constexpr ImU32 kHollow = IM_COL32(150, 150, 160, 200);
constexpr ImU32 kHop = IM_COL32(220, 220, 230, 180);
constexpr ImU32 kHopDim = IM_COL32(220, 220, 230, 45);
constexpr ImU32 kKnot = IM_COL32(255, 255, 255, 40);
constexpr ImU32 kText = IM_COL32(20, 22, 28, 255);
constexpr ImU32 kLabel = IM_COL32(210, 214, 224, 255);
constexpr ImU32 kRibbon = IM_COL32(90, 150, 220, 30);
// The hot take / patient-zero colour now lives in the shared palette (24 T2):
// hot reads the same on the fabric as in the take legend. Its SECOND CHANNEL is
// the solid fill (dim takes are hollow), so it never rides on colour alone.
const ImU32 kHot = dt_hot_u32();

bool inside(const ImVec2 &p, float x0, float y0, float x1, float y1) {
    return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1;
}

} // namespace

void draw_loom_plan(const std::vector<loom_prim_t> &prims, std::string *hover) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (hover != nullptr)
        hover->clear();

    auto at = [&](float x, float y) { return ImVec2(o.x + x, o.y + y); };
    auto note = [&](const loom_prim_t &p) {
        if (hover != nullptr && !p.text.empty() &&
            inside(mouse, o.x + p.x0, o.y + p.y0, o.x + p.x1 + 1,
                   o.y + p.y1 + 1))
            *hover = p.text;
    };

    for (const loom_prim_t &p : prims) {
        switch (p.kind) {
        case loom_prim::lane_header:
            dl->AddText(at(p.x0 + 2, p.y0 + 1), kLabel, p.text.c_str());
            break;
        case loom_prim::span:
            dl->AddRectFilled(at(p.x0, p.y0 + 2), at(p.x1, p.y1 - 2),
                              p.b ? kSpanDim : kSpan, 2.0f);
            break;
        case loom_prim::span_hollow:
            // Outline only: the route is known, the value is not, and a filled
            // rectangle would look exactly like one that carries a measurement.
            dl->AddRect(at(p.x0, p.y0 + 2), at(p.x1, p.y1 - 2), kHollow, 2.0f,
                        0, 1.0f);
            break;
        case loom_prim::byte_row:
            dl->AddRectFilled(at(p.x0, p.y0), at(p.x1, p.y1 - 1), kSpan);
            break;
        case loom_prim::density_ribbon: {
            // Intensity IS the live-span count; nothing else modulates it.
            ImU32 c = IM_COL32(90, 150, 220,
                               static_cast<int>(30 + 40 * (p.a > 5 ? 5 : p.a)));
            (void)kRibbon;
            dl->AddRectFilled(at(p.x0, p.y0 + 2), at(p.x1, p.y1 - 2), c);
            break;
        }
        case loom_prim::value_chip:
            dl->AddText(at(p.x0 + 3, p.y0 + 1), kText, p.text.c_str());
            break;
        case loom_prim::hop:
            dl->AddBezierCubic(at(p.x0, p.y0), at(p.x0 + 12, p.y0),
                               at(p.x1 - 12, p.y1), at(p.x1, p.y1),
                               p.b ? kHopDim : kHop, 1.5f);
            break;
        case loom_prim::knot:
            dl->AddLine(at(p.x0, p.y0), at(p.x1, p.y1), kKnot, 1.0f);
            break;
        case loom_prim::torn_edge:
            dl->AddLine(at(p.x0, p.y0), at(p.x1, p.y1), kWarn, 2.0f);
            dl->AddText(at(p.x0 + 4, p.y0 + 2), kWarn, p.text.c_str());
            note(p);
            break;
        case loom_prim::fade_out:
            // A soft right edge, never a cap: the value did not end here, the
            // recording did.
            dl->AddRectFilledMultiColor(at(p.x0 - 10, p.y0 + 2),
                                        at(p.x0, p.y1 - 2), kSpan, kSpanDim,
                                        kSpanDim, kSpan);
            note(p);
            break;
        case loom_prim::born_untraced_glyph:
            dl->AddTriangleFilled(at(p.x0, p.y1 - 2), at(p.x0 + 6, p.y0 + 2),
                                  at(p.x0 + 12, p.y1 - 2), kWarn);
            note(p);
            break;
        case loom_prim::guest_badge:
            dl->AddText(at(4, 2), kWarn, p.text.c_str());
            break;
        case loom_prim::take_dim:
            dl->AddRect(at(p.x0, p.y0 + 2), at(p.x1, p.y1 - 2), kSpanDim, 2.0f,
                        0, 1.0f);
            break;
        case loom_prim::take_hot:
            dl->AddRectFilled(at(p.x0, p.y0 + 2), at(p.x1, p.y1 - 2), kHot,
                              2.0f);
            break;
        case loom_prim::take_dashed_tail: {
            // Dashes, not a line: an unaligned tail is never drawn as agreement.
            for (float x = p.x0; x < p.x1; x += 8.0f)
                dl->AddLine(at(x, p.y0), at(x + 4 > p.x1 ? p.x1 : x + 4, p.y0),
                            kHollow, 1.0f);
            note(p);
            break;
        }
        case loom_prim::patient_zero:
            dl->AddLine(at(p.x0, p.y0), at(p.x1, p.y1), kHot, 2.0f);
            dl->AddText(at(p.x0 + 4, p.y0 + 2), kHot, p.text.c_str());
            note(p);
            break;
        case loom_prim::fault_card:
            dl->AddRectFilled(at(p.x0, p.y0), at(p.x1, p.y1),
                              IM_COL32(60, 24, 24, 220), 3.0f);
            dl->AddText(at(p.x0 + 4, p.y0 + 3), kWarn, p.text.c_str());
            note(p);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------

void draw_loom(LoomState &L, const Streams &s, const Workspace &ws, int self,
               const std::function<void(const dt_link &)> &go, Selection *shared,
               UndoStack *undo) {
    if (!L.built || L.source_id != s.id) {
        L.source_id = s.id;
        L.built = true;
        L.err.clear();
        L.fabric = loom_fabric_t{};
        if (!loom_fabric_from_streams(s, &L.feed, &L.fabric, &L.err))
            L.fabric = loom_fabric_t{};
        L.has_selection = false;
        L.lane = -1;
        L.playhead = L.fabric.steps ? L.fabric.steps - 1 : 0;
    }
    // Domain-term-first heading + the "?" caveat, which also re-opens the primer
    // (24 T3/T5). "Data-flow lineage (Loom)", caveat "lineage is def-use, not
    // control flow".
    dt_view_header("loom", [&L] { dt_primer_reopen(L.primer); });

    // First-open primer (24 T5): what the fabric is + how to read its channels,
    // with the take legend, dismissible and re-openable. Front-loads nothing
    // heavier than this card until acknowledged.
    if (dt_primer_active(L.primer)) {
        dt_primer(
            "loom-primer", "Data-flow lineage (Loom)",
            "Each lane is a place a value lived; a worldline threads left-to-"
            "right through the steps that read it. This is LINEAGE (def-use) — "
            "where a value came from and what it fed — not the order code ran. "
            "Read the take channel by its PATTERN: a solid rect is a value used "
            "this step (hot), a hollow outline is present-but-unused (dim), "
            "dashes are an unaligned tail (never agreement). "
            "[ and ] walk generations.",
            [] { dt_loom_take_legend(); }, L.primer);
    }

    if (!L.err.empty()) {
        // A refusal is a full-pane placard and nothing else: there is no fabric,
        // and half a fabric would be worse than none. The refuse red is the ONE
        // shared value (ui/theme.h T1) — this was a third hard-coded copy (F14).
        ImGui::PushStyleColor(ImGuiCol_Text, dt_refuse_col());
        ImGui::TextWrapped("%s", L.err.c_str());
        ImGui::PopStyleColor();
        return;
    }

    const loom_fabric_t &f = L.fabric;

    // --- the deck, and the lane whose annex is open ------------------------
    ImGui::BeginChild("loom-deck", ImVec2(200, 0), true);
    for (size_t i = 0; i < f.lanes.size(); i++) {
        bool sel = static_cast<int>(i) == L.lane;
        if (ImGui::Selectable(f.lanes[i].name.c_str(), sel))
            L.lane = sel ? -1 : static_cast<int>(i);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("loom-fabric", ImVec2(0, 0), false);

    // Whole-fabric overview/minimap (21-spine-navigation.md T3): a compressed
    // projection of the fabric's OWN steps (overview_from_fabric — no new
    // structure), so the ImZoomSlider below rides over a visible map of what it
    // windows rather than an empty track. A click maps x to a step and jumps OUT
    // through 04's router (dt_nav_go), so a minimap click and a typed go-to land
    // identically (D4); it also moves the local playhead.
    {
        double wlo, whi;
        loom_view_step_window(L.cam, &wlo, &whi);
        OverviewStrip strip = overview_from_fabric(f, wlo, whi);
        ImGui::TextDisabled("overview (whole trace) — click to jump");
        const float mw = ImGui::GetContentRegionAvail().x;
        const float mh = 30.0f;
        const ImVec2 o = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + mw, o.y + mh),
                          IM_COL32(28, 30, 38, 255));
        if (strip.nsteps > 0 && !strip.buckets.empty()) {
            uint32_t maxw = 1;
            for (const OverviewBucket &b : strip.buckets)
                maxw = std::max(maxw, b.weight);
            for (const OverviewBucket &b : strip.buckets) {
                float x = o.x + mw * static_cast<float>(b.step) /
                                    static_cast<float>(strip.nsteps);
                float bh =
                    mh * static_cast<float>(b.weight) / static_cast<float>(maxw);
                dl->AddRectFilled(ImVec2(x, o.y + mh - bh),
                                  ImVec2(x + 1.5f, o.y + mh), kSpan);
            }
            float vx0 = o.x + mw * static_cast<float>(strip.lo) /
                                  static_cast<float>(strip.nsteps);
            float vx1 = o.x + mw * static_cast<float>(strip.hi) /
                                  static_cast<float>(strip.nsteps);
            dl->AddRect(ImVec2(vx0, o.y), ImVec2(vx1, o.y + mh), kHop, 0.0f, 0,
                        1.5f);
        }
        ImGui::InvisibleButton("loom-minimap", ImVec2(mw, mh));
        if (ImGui::IsItemClicked() && strip.nsteps > 0) {
            float frac = (ImGui::GetIO().MousePos.x - o.x) / (mw > 1 ? mw : 1);
            uint32_t step = overview_click_step(strip, frac);
            L.playhead = step;
            if (go) {
                dt_link l;
                l.view = dt_view::timeline;
                l.rec = s.id;
                l.step = step;
                go(l);
            }
        }
    }

    // Zoom + scrub live in the panel; the plan is a pure function of them.
    // A windowed pan+zoom over the whole step range: drag the middle to PAN
    // (writes step0 — the field the model carried but nothing ever set), drag an
    // edge to ZOOM (writes steps_per_px). Its label is a fixed internal
    // "ImZoomSlider", so PushID keeps it from colliding with other instances
    // (14-quick-wins.md T5). The window<->camera math is the pure, tested half.
    {
        double wlo, whi;
        loom_view_step_window(L.cam, &wlo, &whi);
        const float fmax = static_cast<float>(f.steps ? f.steps : 1);
        float flo = static_cast<float>(wlo);
        float fhi = static_cast<float>(whi);
        if (flo < 0)
            flo = 0;
        if (fhi > fmax || fhi <= flo)
            fhi = fmax; // clamp / first-frame default = whole range
        ImGui::TextDisabled("step window");
        ImGui::PushID("loom-hzoom");
        if (ImZoomSlider::ImZoomSlider(0.0f, fmax, flo, fhi))
            loom_view_set_step_window(L.cam, flo, fhi);
        ImGui::PopID();
    }
    int ph = static_cast<int>(L.playhead);
    // The ONE time-position widget, CONTINUOUS variant (24 T4): the fabric has a
    // real step total, so a scrub is honest.
    if (dt_timepos_scrub("playhead", &ph, f.steps ? static_cast<int>(f.steps) - 1
                                                  : 0))
        L.playhead = static_cast<uint32_t>(ph);
    ImGui::SameLine();
    ImGui::Checkbox(kLoomAuditTitle, &L.audit);
    if (L.audit && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", kLoomAuditHover);

    // Inline take legend (24 T3): the fabric's structural axis, named by its
    // SECOND CHANNEL (solid / hollow / dashed) so it reads without the hue (T2).
    dt_loom_take_legend();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    L.cam.px_w = avail.x > 64 ? avail.x : 64;
    L.cam.px_h = avail.y > 64 ? avail.y * 0.62f : 64;
    L.cam.lane_h = 18.0f;
    // 22 T1: the fabric dim is DERIVED from the ONE shared selection — the Loom's
    // own pick lights its worldline, a brush from another pane dims to that step,
    // nothing selected dims to nothing (never a fabricated highlight).
    L.cam.selected_steps = loom_shared_dim(L, shared);

    std::vector<loom_prim_t> prims;
    loom_plan(f, L.cam, &prims);
    std::string hover;
    ImGui::BeginChild("loom-canvas", ImVec2(L.cam.px_w, L.cam.px_h), true);
    draw_loom_plan(prims, &hover);
    // Click-to-select: a lane row plus the horizontal step under the cursor.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) {
        ImVec2 o = ImGui::GetCursorScreenPos();
        ImVec2 m = ImGui::GetIO().MousePos;
        int lane = static_cast<int>((m.y - o.y) / L.cam.lane_h) + L.cam.lane0;
        double step = L.cam.step0 + (m.x - o.x) * L.cam.steps_per_px;
        if (lane >= 0 && step >= 0) {
            L.has_selection =
                loom_select(f, L.feed.edges.data(), L.feed.edges.size(),
                            static_cast<uint32_t>(lane),
                            static_cast<uint32_t>(step), &L.sel);
            // 22 T1: a Loom pick writes the ONE shared selection, so the SAME
            // entity cross-highlights in the timeline / slice / 3D panes at once
            // (brush, not navigate — only the active pane scrolls).
            if (L.has_selection && shared != nullptr)
                shared->set(s.id, L.sel.origin_step, std::nullopt,
                            static_cast<int>(lane));
        }
    }
    ImGui::EndChild();
    if (!hover.empty())
        ImGui::SetTooltip("%s", hover.c_str());

    // `[` / `]` walk generations — the same binding the help overlay lists.
    if (L.has_selection) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
            loom_gen_step(L.sel, -1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
            loom_gen_step(L.sel, +1);
    }

    // --- biography / annex / audit ----------------------------------------
    if (ImGui::BeginTabBar("loom-detail")) {
        if (ImGui::BeginTabItem("Biography")) {
            if (!L.has_selection) {
                ImGui::TextDisabled("click a worldline to read its biography");
            } else if (L.sel.born_untraced) {
                ImGui::TextWrapped("%s", L.sel.note.c_str());
            } else {
                ImGui::Text("generation %d of [%d, %d]", L.sel.gen_view,
                            L.sel.gen_lo, L.sel.gen_hi);
                for (const loom_bio_row_t &r :
                     loom_biography(f, L.feed.edges.data(), L.feed.edges.size(),
                                    L.sel.origin_span))
                    ImGui::BulletText("%s: %s", r.kind.c_str(), r.text.c_str());
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lane annex")) {
            if (L.lane < 0) {
                ImGui::TextDisabled("click a lane header to join what the "
                                    "other recordings saw about that place");
            } else {
                std::vector<annex_entry_t> rows;
                loom_annex_join(ws, f, static_cast<uint32_t>(L.lane), self,
                                &rows);
                if (rows.empty())
                    ImGui::TextDisabled("no companion recording says anything "
                                        "about %s",
                                        f.lanes[L.lane].name.c_str());
                for (const annex_entry_t &e : rows)
                    ImGui::BulletText(
                        "%s [%s] %s — %s", annex_kind_name(e.kind),
                        e.provenance.c_str(), annex_verdict_text(e).c_str(),
                        e.text.c_str());
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Takes")) {
            // The persistent takes gutter (22 T4): per-take remove + clear forks,
            // both reversible. The accumulator is empty here until a full build
            // appends a loom_take_run result; the render-only viewer shows recorded
            // takes but assembles none.
            draw_loom_takes_gutter(L, undo);
            ImGui::EndTabItem();
        }
        if (L.audit && ImGui::BeginTabItem("Zeroization audit")) {
            ImGui::TextWrapped("%s", kLoomAuditHover);
            if (!L.has_selection) {
                ImGui::TextDisabled("select a birth to audit");
            } else {
                std::vector<loom_lit_t> lit;
                loom_audit(f, L.feed.edges.data(), L.feed.edges.size(),
                           L.sel.origin_step, L.playhead, &lit);
                if (lit.empty())
                    ImGui::TextUnformatted(
                        "no lane resident at this step holds a descendant "
                        "— within the traced window");
                for (const loom_lit_t &r : lit)
                    ImGui::BulletText(
                        "%s%s", f.lanes[r.lane].name.c_str(),
                        r.hollow ? "  (hollow — route known, value not)" : "");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// The persistent takes gutter (22-selection-and-search.md T4, F12): each
// accumulated take with a per-node [remove] and a gutter-level [clear forks],
// both reversible undo Commands (each take keeps its edit / alignment / fault /
// err / disclosure verbatim, so a remove or clear never quietly drops a take's
// loud refusal — D7). Public so the interaction lane drives its buttons directly.
void draw_loom_takes_gutter(LoomState &L, UndoStack *undo) {
    ImGui::TextDisabled("%zu take%s — one-fact counterfactual forks",
                        L.takes.size(), L.takes.size() == 1 ? "" : "s");
    if (L.takes.empty()) {
        ImGui::TextDisabled(
            "no forks yet — a take changes exactly ONE fact (an entry arg or the "
            "routine's source) and re-runs from entry. Full app only; the "
            "render-only viewer shows recorded takes but assembles none.");
        return;
    }

    // Clear forks — empties the whole set, reversibly (Ctrl+Z restores it).
    if (ImGui::SmallButton("clear forks")) {
        if (undo != nullptr) {
            UndoCommand cmd;
            cmd.kind = UndoCommand::Kind::TakeSet;
            cmd.takes_before = L.takes;
            loom_takes_clear(L.takes);
            cmd.takes_after = L.takes;
            undo->push(std::move(cmd));
        } else {
            loom_takes_clear(L.takes);
        }
    }

    int remove_idx = -1;
    for (size_t i = 0; i < L.takes.size(); i++) {
        const loom_take_node_t &n = L.takes[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::TextUnformatted(n.label.empty() ? "(take)" : n.label.c_str());
        if (!n.edit.empty())
            ImGui::BulletText("%s", n.edit.c_str());
        if (!n.alignment.empty())
            ImGui::BulletText("%s", n.alignment.c_str());
        if (!n.fault.empty())
            ImGui::TextColored(dt_refuse_col(), "%s", n.fault.c_str());
        // A take's loud refusal, verbatim — removing the whole node takes this
        // WITH it, never silently dropping the failure (D7).
        if (!n.err.empty())
            ImGui::TextColored(dt_refuse_col(), "%s", n.err.c_str());
        if (!n.disclosure.empty())
            ImGui::TextDisabled("%s", n.disclosure.c_str());
        if (ImGui::SmallButton("remove"))
            remove_idx = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove_idx >= 0) {
        if (undo != nullptr) {
            UndoCommand cmd;
            cmd.kind = UndoCommand::Kind::TakeSet;
            cmd.takes_before = L.takes;
            loom_takes_remove(L.takes, static_cast<size_t>(remove_idx));
            cmd.takes_after = L.takes;
            undo->push(std::move(cmd));
        } else {
            loom_takes_remove(L.takes, static_cast<size_t>(remove_idx));
        }
    }
}

} // namespace asmdesk
