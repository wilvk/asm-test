// shell.cpp — see shell.h. All draws are backend-free ImGui immediate calls; no
// GLFW, no GL, no engines are touched here, so the null backend renders every
// path in tests (03-desktop-shell.md T6).
#include "ui/shell.h"

#include <string>

#include "imgui.h"

namespace asmdesk {

// The verbatim greyed-out-shows-why reason (plan D2): an engine door disabled in
// the render-only viewer states, in place, exactly why. [[maybe_unused]] because
// only the render-only build (-DASMTEST_DESKTOP_RENDER_ONLY) references it.
[[maybe_unused]] static const char *kEngineDoorReason =
    "requires the full app (GPL-2.0; links the engines) — this is the "
    "render-only viewer";

const char *shell_banner(const Recording &r) {
    static thread_local std::string buf;
    if (!r.truncated() && !r.dropped())
        return nullptr;
    buf.clear();
    if (r.torn)
        buf = "TORN recording — no end event; shown as incomplete";
    else if (r.end_truncated)
        buf = "TRUNCATED recording — buffers filled";
    if (r.dropped()) {
        if (!buf.empty())
            buf += "; ";
        buf += "dropped " + std::to_string(r.drops_lost);
        if (r.drops_throttled)
            buf += " (throttled)";
    }
    return buf.c_str();
}

static std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    return b.empty() ? "(unnamed)" : b;
}

// The three doors on the home screen. Learn opens the open dialog; Author and
// Inspect are placeholders (behaviour in docs 06/08) — disabled with the reason
// in the render-only viewer, opening an empty named tab in the full app.
static void draw_doors(ShellState &s) {
    ImGui::TextUnformatted("asmtest desktop — choose a door");
    ImGui::Spacing();

    if (ImGui::Button("Learn")) {
        s.open_dialog = true;
        s.open_error.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("open a .asmtrace recording to replay");

#ifdef ASMTEST_DESKTOP_RENDER_ONLY
    ImGui::BeginDisabled();
    ImGui::Button("Author");
    ImGui::SameLine();
    ImGui::Button("Inspect");
    ImGui::EndDisabled();
    ImGui::TextUnformatted(kEngineDoorReason);
#else
    if (ImGui::Button("Author"))
        s.door_tabs.push_back("Author");
    ImGui::SameLine();
    if (ImGui::Button("Inspect"))
        s.door_tabs.push_back("Inspect");
    ImGui::TextDisabled(
        "Author/Inspect open an empty tab (views land in docs 06/08)");
#endif
}

// A recording's summary pane: provenance chrome + the honesty banner + per-kind
// event counts. All of it is derived from the model, so tests assert it without
// pixels via shell_banner (the banner) and the model fields (the chrome).
static void draw_summary(const Recording &r) {
    ImGui::Text("producer: %s %s",
                r.producer.name.empty() ? "(unknown)" : r.producer.name.c_str(),
                r.producer.version.c_str());
    ImGui::Text("backend: %s (%s)",
                r.provenance.backend.empty() ? "(unknown)"
                                             : r.provenance.backend.c_str(),
                r.provenance.exact ? "exact" : "statistical");
    if (!r.provenance.trust.empty())
        ImGui::Text("trust: %s", r.provenance.trust.c_str());
    if (!r.arch.empty())
        ImGui::Text("arch: %s", r.arch.c_str());
    if (r.provenance.redacted)
        ImGui::TextUnformatted(
            "payload REDACTED at record time (absent, not hidden)");

    if (const char *banner = shell_banner(r)) {
        ImGui::Separator();
        ImGui::TextUnformatted(banner);
    }

    ImGui::Separator();
    if (r.by_kind.empty()) {
        ImGui::TextDisabled("(no events)");
    } else {
        ImGui::TextUnformatted("events by kind:");
        for (const auto &kv : r.by_kind)
            ImGui::BulletText("%s: %zu", kv.first.c_str(), kv.second.size());
    }
    if (r.unknown_kinds > 0)
        ImGui::Text("(%llu event(s) of unknown kind, kept)",
                    (unsigned long long)r.unknown_kinds);
}

// The recording-open dialog: a text field + Open/Cancel. A Workspace::open error
// renders verbatim in the dialog — never a silent no-op.
static void draw_open_dialog(ShellState &s) {
    ImGui::Begin("Open recording", &s.open_dialog);
    ImGui::TextUnformatted("path to a .asmtrace recording:");
    ImGui::InputText("##open_path", s.open_path, sizeof s.open_path);
    if (ImGui::Button("Open")) {
        std::string err;
        int idx = s.ws.open(s.open_path, err);
        if (idx < 0) {
            s.open_error = err;
        } else {
            s.open_error.clear();
            s.active_tab = idx;
            s.open_dialog = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
        s.open_dialog = false;
    if (!s.open_error.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("open failed: %s", s.open_error.c_str());
    }
    ImGui::End();
}

void draw_shell(ShellState &s) {
    ImGui::Begin("asmtest");

    if (ImGui::BeginTabBar("main", ImGuiTabBarFlags_AutoSelectNewTabs)) {
        if (ImGui::BeginTabItem("Home")) {
            s.active_tab = -1;
            draw_doors(s);
            ImGui::EndTabItem();
        }

        // One tab per open recording (title = filename); ### keeps the id stable
        // across renames. A closed tab is collected and applied after the loop so
        // the index walk is never invalidated mid-iteration.
        int to_close = -1;
        for (size_t i = 0; i < s.ws.recordings.size(); ++i) {
            const Recording &r = s.ws.recordings[i];
            std::string title =
                base_name(r.path) + "###rec" + std::to_string(i);
            bool keep_open = true;
            if (ImGui::BeginTabItem(title.c_str(), &keep_open)) {
                s.active_tab = static_cast<int>(i);
                draw_summary(r);
                ImGui::EndTabItem();
            }
            if (!keep_open)
                to_close = static_cast<int>(i);
        }
        if (to_close >= 0)
            s.ws.close(static_cast<size_t>(to_close));

        // Placeholder door tabs (full app only; empty until docs 06/08).
        for (size_t i = 0; i < s.door_tabs.size(); ++i) {
            std::string title = s.door_tabs[i] + "###door" + std::to_string(i);
            if (ImGui::BeginTabItem(title.c_str())) {
                ImGui::TextDisabled("%s — view lands in a later doc",
                                    s.door_tabs[i].c_str());
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (s.open_dialog)
        draw_open_dialog(s);
}

} // namespace asmdesk
