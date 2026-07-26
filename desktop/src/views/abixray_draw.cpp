// abixray_draw.cpp — the ImGui half of the ABI x-ray. Draws only: every
// decision is in abixray.cpp, asserted headlessly. The two panes ARE two
// scrubber decks (dt_scrubber, via the built model) locked to ONE playhead: the
// walkthrough rail advances that playhead, the shared slider and `[` / `]` keys
// move it, and the register deltas do the animation.
#include "imgui.h"

#include "views/abixray.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {

// The scrubber's per-step highlight colour (a register changed vs the previous
// held step), reused so the x-ray's panes light exactly as the standalone
// scrubber does.
const ImVec4 kChanged(1.0f, 0.85f, 0.3f, 1.0f);
// A register whose value DIFFERS across the two conventions at this step — the
// marshalling contrast. Tinted on the register NAME in both panes so the eye
// pairs them.
const ImVec4 kDiffer(0.45f, 0.85f, 1.0f, 1.0f);

// One locked pane: the register deck of ONE convention, read from the built
// cross-pane rows so the DIFF tint and the per-pane highlight stay in sync.
void draw_pane(const char *id, const char *label, const dt_abixray &x,
               bool sysv, float width) {
    ImGui::BeginChild(id, ImVec2(width, 0), true);
    ImGui::TextUnformatted(label);
    ImGui::Separator();

    const dt_scrubber &deck = sysv ? x.sysv : x.win64;
    if (deck.torn_here) {
        ImGui::TextDisabled("step %llu dropped by the ring — register file "
                            "UNKNOWN, not zero",
                            static_cast<unsigned long long>(x.playhead));
        ImGui::EndChild();
        return;
    }
    if (!deck.has_prev)
        ImGui::TextDisabled("no previous held step — nothing to diff against");

    if (ImGui::BeginTable("regfile", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("register", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const dt_abixray_row &r : x.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (r.differs)
                ImGui::TextColored(kDiffer, "%s", r.name.c_str());
            else
                ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            bool known = sysv ? r.sysv_known : r.win64_known;
            bool changed = sysv ? r.sysv_changed : r.win64_changed;
            unsigned long long v =
                static_cast<unsigned long long>(sysv ? r.sysv : r.win64);
            if (!known)
                ImGui::TextDisabled("UNKNOWN");
            else if (changed)
                ImGui::TextColored(kChanged, "0x%llx", v);
            else
                ImGui::Text("0x%llx", v);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

void draw_abixray(const StepIndex &sysv_idx, const StepIndex &win64_idx,
                  wt_model &walk, uint64_t &playhead) {
    dt_abixray x = dt_abixray_seek(sysv_idx, win64_idx, walk, playhead);

    if (!x.present) {
        draw_banner(x.message.c_str(), true);
        ImGui::TextDisabled("producer docs: %s", x.docs.c_str());
        return;
    }

    if (!x.banner.empty())
        draw_banner(x.banner.c_str(), false);
    if (!x.title.empty())
        ImGui::TextWrapped("%s", x.title.c_str());
    ImGui::Text("descriptor: %s", x.desc.c_str());
    ImGui::Separator();

    // The walkthrough rail drives the animation. Advancing a stop SNAPS the
    // locked playhead to that stop's anchor, seeking both panes together.
    if (x.has_stop) {
        ImGui::Text("stop %d of %d", x.stop_ordinal, x.stop_count);
        ImGui::SameLine();
        bool moved = false;
        if (ImGui::SmallButton("prev stop")) {
            walk.prev();
            moved = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("next stop")) {
            walk.next();
            moved = true;
        }
        if (moved) {
            playhead = dt_abixray_playhead(walk);
            x = dt_abixray_seek(sysv_idx, win64_idx, walk, playhead);
        }
        if (!x.stop_title.empty())
            ImGui::TextWrapped("%s", x.stop_title.c_str());
        ImGui::TextWrapped("%s", x.stop_body.c_str());
        if (x.stop_beyond_window)
            // NEVER clamp — the same refusal the walkthrough player raises.
            draw_banner(kWtBeyondWindow, true);
        else if (x.stop_anchor < 0)
            ImGui::TextDisabled("this stop is about the call, not a step");
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("this pair carries no walkthrough stops");
    }

    // The shared controls — one playhead for BOTH panes. `[` / `]` step keys
    // (04's bindings) and the slider move it directly, decoupled from the rail.
    uint64_t np = playhead;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
        np = dt_scrubber_prev(sysv_idx, playhead);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
        np = dt_scrubber_next(sysv_idx, playhead);
    int cur = static_cast<int>(playhead);
    int lo = 0, hi = static_cast<int>(x.total > 0 ? x.total - 1 : 0);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##abixray-playhead", &cur, lo, hi, "step %d") &&
        cur >= 0)
        np = static_cast<uint64_t>(cur);
    if (np != playhead) {
        playhead = np;
        x = dt_abixray_seek(sysv_idx, win64_idx, walk, playhead);
    }

    // The two LOCKED panes, side by side.
    ImGuiStyle &style = ImGui::GetStyle();
    float half =
        (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    if (half < 1.0f)
        half = 1.0f;
    draw_pane("abixray-sysv", x.sysv_label.c_str(), x, true, half);
    ImGui::SameLine();
    draw_pane("abixray-win64", x.win64_label.c_str(), x, false, 0.0f);
}

} // namespace asmdesk
