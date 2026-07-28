// scrubber_draw.cpp — the ImGui half of the register time-travel scrubber.
// Draws only: every decision is in scrubber.cpp, asserted headlessly. The one
// engine call (regsynth, behind ASMTEST_DESKTOP_CAN_AUTHOR) is the app-tree's
// only; the viewer compiles it out and shows the honest "full app only" note (D4).
#include "imgui.h"

#include <utility> // std::move (the synth swaps the index in place)

#include "ui/theme.h"
#include "views/views_draw.h"
#ifdef ASMTEST_DESKTOP_CAN_AUTHOR
#include "views/regsynth.h"
#endif

namespace asmdesk {

uint64_t draw_scrubber(StepIndex &idx, uint64_t playhead, const Recording *rec) {
    // `[` / `]` step keys (04's bindings) — the same pair the help overlay
    // lists. They walk the FULL step space, torn region included, so a reader
    // steps across the tear rather than around it.
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
        playhead = dt_scrubber_prev(idx, playhead);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket))
        playhead = dt_scrubber_next(idx, playhead);

    // 30 R3 T4: only the producer-absent path consults replayability; a present
    // deck (real or already-synthesised) needs no recording facts.
    dt_scrubber_replay replay;
    if (rec != nullptr && !idx.present())
        replay = dt_scrubber_replayable(*rec);
    dt_scrubber s = dt_scrubber_build(idx, playhead, replay);

    if (!s.present) {
        draw_banner(s.absent_message.c_str(), true);
        if (s.synthesizable) {
            // The offer + its MANDATORY re-derivation caveat (D6).
            ImGui::TextDisabled("%s", s.synth_label.c_str());
#ifdef ASMTEST_DESKTOP_CAN_AUTHOR
            if (rec != nullptr && ImGui::Button(s.synth_action.c_str())) {
                StepIndex synth;
                std::string err;
                if (regsynth_synthesize(*rec, &synth, &err))
                    idx = std::move(synth); // now present + `synthesized`
                else
                    ImGui::TextColored(dt_refuse_col(), "%s", err.c_str());
            }
#else
            ImGui::TextDisabled("(%s: full app only — the render-only viewer "
                                "runs no emulator)",
                                s.synth_action.c_str());
#endif
        }
        ImGui::TextDisabled("producer docs: %s", s.docs.c_str());
        return s.playhead;
    }

    if (!s.banner.empty())
        draw_banner(s.banner.c_str(), false);

    ImGui::Text("descriptor: %s", s.desc.c_str());

    // The playhead slider. ImGui sliders are int-based; the held step count is
    // tiny, so a plain int cursor over [0, total) is faithful.
    int cur = static_cast<int>(s.playhead);
    int lo = 0, hi = static_cast<int>(s.total > 0 ? s.total - 1 : 0);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##playhead", &cur, lo, hi, "step %d") && cur >= 0)
        s = dt_scrubber_build(idx, static_cast<uint64_t>(cur));

    if (s.torn_here) {
        ImGui::TextDisabled(
            "step %llu was dropped by the ring — its register file is UNKNOWN, "
            "not zero",
            static_cast<unsigned long long>(s.playhead));
        return s.playhead;
    }

    if (!s.has_prev)
        ImGui::TextDisabled("no previous held step — nothing to diff against "
                            "(baseline absent)");

    if (ImGui::BeginTable("regfile", 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("register", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const dt_scrubber_reg &r : s.regs) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            // A changed register is the per-step highlight: colour it so the eye
            // lands on what this instruction did, not on the 16 it did not.
            if (r.changed)
                ImGui::TextColored(dt_changed_col(), "0x%llx",
                                   static_cast<unsigned long long>(r.value));
            else
                ImGui::Text("0x%llx", static_cast<unsigned long long>(r.value));
        }
        ImGui::EndTable();
    }
    return s.playhead;
}

} // namespace asmdesk
