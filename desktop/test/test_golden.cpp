// test_golden.cpp — the consumer-side schema-stability gate (03-desktop-shell.md
// T7, plan DX golden-recording tests). Proves the viewer opens every committed
// golden recording (D6): each parses, satisfies the model invariants, and — for
// the hand-authored dishonest/ fixtures — surfaces its D7 honesty fact. Then all
// of them open into one ShellState and run a 3-frame null render, so a Capstone
// bump that renames a field or an ImGui upgrade that breaks a draw fails HERE,
// in-lane, not on a user.
//
// An empty or missing corpus is a FAILURE, never a skip: the corpus is committed
// (01, D6), so absence is a broken checkout, and a test that can only self-skip
// is not a test (CLAUDE.md).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"

#include "doc/recording.h"
#include "ui/shell.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

namespace fs = std::filesystem;
using namespace asmdesk;

static int failures;
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
        failures++;
    }
}

// Collect *.asmtrace directly under `dir` (non-recursive), sorted for a stable
// report order.
static std::vector<std::string> asmtraces_in(const fs::path &dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".asmtrace")
            out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The invariants every committed recording must satisfy.
static Recording open_and_check(const std::string &path) {
    std::string err;
    auto rec = load_recording_file(path, err);
    if (!rec) {
        check(path, false, "did not load: " + err);
        return Recording{};
    }
    const std::string &p = path;
    check(p + " version", rec->version == 1, "version != 1");
    check(p + " producer", !rec->producer.name.empty(), "empty producer name");
    check(p + " backend", !rec->provenance.backend.empty(), "empty backend");
    check(p + " events", rec->event_count() > 0, "no events");
    // The footer's own count must equal the reader's count (a complete recording)
    // — the "sum of by_kind == parsed event lines" invariant, made cross-checked.
    if (rec->has_end)
        check(p + " count", rec->declared_events == rec->event_count(),
              "end.events != reader event_count");
    // D7: a recording that is less than it looks must SAY so.
    if (rec->truncated())
        check(p + " banner", shell_banner(*rec) != nullptr,
              "truncated recording has no banner");
    return *rec;
}

int main() {
    fs::path root(ASMTEST_GOLDEN_DIR);
    std::vector<std::string> flat = asmtraces_in(root);
    check("corpus/present", !flat.empty(),
          std::string("no *.asmtrace under ") + ASMTEST_GOLDEN_DIR +
              " (broken checkout — the corpus is committed)");

    ShellState s;
    for (const std::string &f : flat) {
        Recording r = open_and_check(f);
        // The honest, generated corpus is complete and clean.
        check(f + " honest", !r.truncated(),
              "a flat golden should not be torn/truncated");
        std::string err;
        s.ws.open(f, err);
    }

    // The hand-authored dishonest/ fixtures: each encodes one honesty fact, and
    // each must surface it (D7). torn loads to a torn recording, not an error.
    fs::path dishonest = root / "dishonest";
    std::vector<std::string> dfiles = asmtraces_in(dishonest);
    check("corpus/dishonest-present", !dfiles.empty(),
          "no dishonest/ fixtures (broken checkout)");
    for (const std::string &f : dfiles) {
        Recording r = open_and_check(f);
        bool dishonest_flag =
            r.truncated() || r.dropped() || r.provenance.redacted;
        check(
            f + " encodes-dishonesty", dishonest_flag,
            "a dishonest/ fixture must surface truncation, drops or redaction");
        std::string err;
        s.ws.open(f, err);
    }

    check("open/all", s.ws.recordings.size() == flat.size() + dfiles.size(),
          "every golden recording should open into the workspace");

    // 3-frame null render over the whole corpus — no path may crash headless.
    // Skipped only when the corpus is empty, which the checks above have already
    // flagged as a FAILURE (so this stays a clean exit(1), not a divide-by-zero
    // crash on an empty workspace).
    if (!s.ws.recordings.empty()) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        unsigned char *px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        for (int frame = 0; frame < 3; frame++) {
            io.DisplaySize = ImVec2(1280, 720);
            io.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            // walk the tabs so each recording's summary body actually draws
            s.active_tab = frame % static_cast<int>(s.ws.recordings.size());
            draw_shell(s);
            ImGui::Render();
            if (ImGui::GetDrawData() == nullptr) {
                std::fprintf(stderr, "FAIL render/frame %d: null draw data\n",
                             frame);
                failures++;
            }
        }
        ImGui::DestroyContext();
    }

    if (failures) {
        std::fprintf(stderr,
                     "test_golden: %d FAILURE(S) over %zu recording(s)\n",
                     failures, flat.size() + dfiles.size());
        return 1;
    }
    std::printf("test_golden: PASS (%zu golden recording(s))\n",
                flat.size() + dfiles.size());
    return 0;
}
