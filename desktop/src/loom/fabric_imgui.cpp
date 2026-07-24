// fabric_imgui.cpp — the ImGui painter for the Loom's draw plan. Draws only.
#include "imgui.h"

#include "loom/loom_draw.h"

namespace asmdesk {

namespace {

// Every honesty prim shares the warn colour with the rest of the app's placards
// (views/canvas_draw.cpp), so a torn edge cannot be mistaken for ordinary
// chrome and ordinary chrome cannot be mistaken for a warning.
constexpr ImU32 kWarn = IM_COL32(242, 191, 64, 255);
constexpr ImU32 kSpan = IM_COL32(90, 150, 220, 220);
constexpr ImU32 kSpanDim = IM_COL32(90, 150, 220, 60);
constexpr ImU32 kHollow = IM_COL32(150, 150, 160, 200);
constexpr ImU32 kHop = IM_COL32(220, 220, 230, 180);
constexpr ImU32 kHopDim = IM_COL32(220, 220, 230, 45);
constexpr ImU32 kKnot = IM_COL32(255, 255, 255, 40);
constexpr ImU32 kText = IM_COL32(20, 22, 28, 255);
constexpr ImU32 kLabel = IM_COL32(210, 214, 224, 255);
constexpr ImU32 kRibbon = IM_COL32(90, 150, 220, 30);
constexpr ImU32 kHot = IM_COL32(230, 110, 90, 220);

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

void draw_loom(LoomState &L, const Streams &s, const Workspace &ws, int self) {
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
    if (!L.err.empty()) {
        // A refusal is a full-pane placard and nothing else: there is no fabric,
        // and half a fabric would be worse than none.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
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
    // Zoom + scrub live in the panel; the plan is a pure function of them.
    float spp = static_cast<float>(L.cam.steps_per_px);
    if (spp <= 0)
        spp = 0.05f;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat("steps/px", &spp, 0.005f, 8.0f, "%.3f",
                           ImGuiSliderFlags_Logarithmic))
        L.cam.steps_per_px = spp;
    ImGui::SameLine();
    int ph = static_cast<int>(L.playhead);
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderInt("playhead", &ph, 0,
                         f.steps ? static_cast<int>(f.steps) - 1 : 0))
        L.playhead = static_cast<uint32_t>(ph);
    ImGui::SameLine();
    ImGui::Checkbox(kLoomAuditTitle, &L.audit);
    if (L.audit && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", kLoomAuditHover);

    ImVec2 avail = ImGui::GetContentRegionAvail();
    L.cam.px_w = avail.x > 64 ? avail.x : 64;
    L.cam.px_h = avail.y > 64 ? avail.y * 0.62f : 64;
    L.cam.lane_h = 18.0f;
    L.cam.selected_steps =
        L.has_selection ? L.sel.steps : std::vector<uint32_t>();

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
        if (lane >= 0 && step >= 0)
            L.has_selection =
                loom_select(f, L.feed.edges.data(), L.feed.edges.size(),
                            static_cast<uint32_t>(lane),
                            static_cast<uint32_t>(step), &L.sel);
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

} // namespace asmdesk
