// learn_door.cpp — the Learn door: bundled walkthroughs, played
// (06-doors-and-learning.md T4). Draws and discovers files; decides nothing.
#include "imgui.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "ui/doors.h"
#include "views/views_draw.h"

namespace fs = std::filesystem;

namespace asmdesk {

#ifndef ASMTEST_WALKTHROUGH_DIR
#define ASMTEST_WALKTHROUGH_DIR "tests/golden-asmtrace/walkthroughs"
#endif

std::string learn_dir() {
    const char *env = std::getenv("ASMTRACE_LEARN_DIR");
    if (env != nullptr && *env)
        return env;
    return ASMTEST_WALKTHROUGH_DIR;
}

void learn_scan(LearnState &s, const std::string &dir) {
    s.cards.clear();
    s.scan_error.clear();
    s.dir = dir;
    s.scanned = true;
    s.open_card = -1;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        s.scan_error = "no walkthroughs at " + dir +
                       " — set ASMTRACE_LEARN_DIR to where they were bundled";
        return;
    }
    std::vector<std::string> files;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".asmtrace")
            files.push_back(e.path().string());
    // Deterministic order: a door whose list reshuffles per filesystem is a
    // door people cannot give directions to.
    std::sort(files.begin(), files.end());

    for (const std::string &path : files) {
        LearnCard c;
        c.path = path;
        c.id = recording_id(path);
        std::string err;
        auto rec = load_recording_file(path, err);
        if (!rec) {
            // A card that names its own failure, rather than a missing row.
            c.error = err;
            c.title = c.id;
            s.cards.push_back(std::move(c));
            continue;
        }
        wt_model m = wt_build(*rec);
        c.title = m.title.empty() ? c.id : m.title;
        c.stops = static_cast<int>(m.stops.size());
        c.truncated = m.truncated;
        c.provenance = rec->provenance.backend + " / " +
                       (rec->provenance.exact ? "exact" : "statistical");
        s.cards.push_back(std::move(c));
    }
}

void draw_learn_door(LearnState &s,
                     const std::function<void(const std::string &, long)> &go) {
    if (!s.scanned)
        learn_scan(s, learn_dir());

    ImGui::TextUnformatted("Learn — bundled walkthroughs");
    ImGui::TextDisabled("%s", s.dir.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("rescan"))
        learn_scan(s, learn_dir());
    ImGui::Separator();

    if (!s.scan_error.empty()) {
        draw_banner(s.scan_error.c_str(), true);
        return;
    }

    ImGui::BeginChild("learn-cards", ImVec2(300, 0), true);
    for (size_t i = 0; i < s.cards.size(); i++) {
        const LearnCard &c = s.cards[i];
        bool sel = static_cast<int>(i) == s.open_card;
        if (ImGui::Selectable(c.title.c_str(), sel)) {
            s.open_card = static_cast<int>(i);
            std::string err;
            auto rec = load_recording_file(c.path, err);
            s.player = rec ? wt_build(*rec) : wt_model{};
        }
        if (!c.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
            ImGui::TextWrapped("  %s", c.error.c_str());
            ImGui::PopStyleColor();
            continue;
        }
        ImGui::TextDisabled("  %d stop(s) · %s%s", c.stops,
                            c.provenance.c_str(),
                            c.truncated ? " · TRUNCATED" : "");
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("learn-player", ImVec2(0, 0), false);
    if (s.open_card < 0) {
        ImGui::TextDisabled("pick a walkthrough");
        ImGui::EndChild();
        return;
    }
    wt_model &m = s.player;

    // D7: the truncation banner is the MODEL's fact, so the player cannot
    // forget to show it.
    if (m.truncated)
        draw_banner("TRUNCATED recording — this walkthrough was recorded with "
                    "the window cut short; stops past it cannot be played",
                    false);
    if (m.torn)
        draw_banner("TORN recording — no end footer; the producer died "
                    "mid-record",
                    false);

    const wt_stop *stop = m.current();
    if (stop == nullptr) {
        ImGui::TextDisabled("this recording carries no stops — it is a "
                            "recording, just not a walkthrough");
        ImGui::EndChild();
        return;
    }

    ImGui::Text("stop %d of %d", stop->ordinal, (int)m.stops.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("prev"))
        m.prev();
    ImGui::SameLine();
    if (ImGui::SmallButton("next"))
        m.next();
    ImGui::Separator();

    if (!stop->title.empty()) {
        ImGui::PushFont(nullptr);
        ImGui::TextWrapped("%s", stop->title.c_str());
        ImGui::PopFont();
        ImGui::Separator();
    }
    ImGui::TextWrapped("%s", stop->body.c_str());

    if (stop->has_framing()) {
        ImGui::Separator();
        if (ImGui::BeginTable("framing", 2,
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableSetupColumn("expected");
            ImGui::TableSetupColumn("got");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", stop->expected.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", stop->got.c_str());
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    if (stop->step_anchor < 0) {
        ImGui::TextDisabled("this stop is about the run, not a step");
    } else if (!m.anchor_in_window(stop->step_anchor)) {
        // NEVER clamp. Showing the last recorded step while narrating a
        // different one is exactly the quiet wrong answer this tree exists to
        // avoid.
        draw_banner(kWtBeyondWindow, true);
    } else if (ImGui::Button("go to this step in the replay views")) {
        go(s.cards[static_cast<size_t>(s.open_card)].path, stop->step_anchor);
    }
    ImGui::EndChild();
}

} // namespace asmdesk
