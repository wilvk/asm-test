// shell.cpp — see shell.h. All draws are backend-free ImGui immediate calls; no
// GLFW, no GL, no engines are touched here, so the null backend renders every
// path in tests (03-desktop-shell.md T6).
#include "ui/shell.h"

#include <string>

#include "imgui.h"

#include "analysis/slice.h"
#include "views/views_draw.h"

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

int shell_open(ShellState &s, const std::string &path, std::string &err) {
    int idx = s.ws.open(path, err);
    if (idx < 0)
        return -1;
    // Decode once, at open. Every builder below is a pure function of Streams,
    // so no view re-parses JSON per frame.
    s.streams.resize(s.ws.recordings.size());
    s.streams[static_cast<size_t>(idx)] =
        decode_streams(s.ws.recordings[static_cast<size_t>(idx)]);
    shell_wire_nav(s);
    return idx;
}

void shell_close(ShellState &s, size_t idx) {
    if (idx >= s.ws.recordings.size())
        return;
    s.ws.close(idx);
    if (idx < s.streams.size())
        s.streams.erase(s.streams.begin() + static_cast<long>(idx));
    if (s.b_index == static_cast<int>(idx))
        s.b_index = -1;
    else if (s.b_index > static_cast<int>(idx))
        s.b_index--;
    shell_wire_nav(s);
}

const Streams *shell_a(const ShellState &s) {
    if (s.active_tab < 0 || s.active_tab >= static_cast<int>(s.streams.size()))
        return nullptr;
    return &s.streams[static_cast<size_t>(s.active_tab)];
}

const Streams *shell_b(const ShellState &s) {
    if (s.b_index < 0 || s.b_index >= static_cast<int>(s.streams.size()))
        return nullptr;
    if (s.b_index == s.active_tab)
        return nullptr; // a recording is never diffed against itself
    return &s.streams[static_cast<size_t>(s.b_index)];
}

// Find an open recording by its deep-link id (its basename).
static int index_of_id(const ShellState &s, const std::string &id) {
    for (size_t i = 0; i < s.streams.size(); i++)
        if (s.streams[i].id == id)
            return static_cast<int>(i);
    return -1;
}

void shell_wire_nav(ShellState &s) {
    s.nav.have_recording = [&s](const std::string &id) {
        return index_of_id(s, id) >= 0;
    };
    // One handler per view. They only move the SELECTION — plan D4: no view
    // keeps its own navigation state, so a link and a keypress land identically.
    auto go = [&s](dt_view v) {
        return [&s, v](const dt_link &l) {
            s.view = v;
            int a = index_of_id(s, l.rec);
            if (a >= 0)
                s.active_tab = a;
            s.b_index = l.rec_b.empty() ? -1 : index_of_id(s, l.rec_b);
            s.selected_step = l.step;
            s.selected_off = l.off;
            if (l.step)
                s.cone_active = true;
        };
    };
    for (dt_view v :
         {dt_view::canvas, dt_view::timeline, dt_view::slice, dt_view::diff})
        dt_nav_register(s.nav, v, go(v));
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

    if (ImGui::Button("Learn"))
        s.show_learn = true;
    ImGui::SameLine();
    ImGui::TextDisabled("play a bundled walkthrough — no deps, no root");

    if (ImGui::Button("Open a recording...")) {
        s.open_dialog = true;
        s.open_error.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("replay a .asmtrace you already have");

    if (ImGui::Button("Author"))
        s.show_author = true;
    ImGui::SameLine();
    ImGui::TextDisabled("type assembly, run it, see faults as data");
#ifdef ASMTEST_DESKTOP_RENDER_ONLY
    ImGui::BeginDisabled();
    ImGui::Button("Inspect");
    ImGui::EndDisabled();
    ImGui::TextUnformatted(kEngineDoorReason);
#else
    if (ImGui::Button("Inspect"))
        s.door_tabs.push_back("Inspect");
    ImGui::TextDisabled("Inspect opens an empty tab (the live views land in "
                        "doc 08)");
#endif
    ImGui::Spacing();
    if (ImGui::Button("Keyboard bindings"))
        s.show_help = !s.show_help;
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

// One open recording's pane: the summary chrome (03), then the replay views
// over it (04). Each builder is called fresh per frame from the decoded streams
// — they are pure and cheap at corpus scale; the threshold at which that stops
// being true is the worker-thread hand-off 04 describes, which lands with the
// first PT-scale recording.
static void draw_recording_tab(ShellState &s, const Recording &r) {
    if (ImGui::BeginTabBar("views")) {
        if (ImGui::BeginTabItem("Summary")) {
            draw_summary(r);
            ImGui::EndTabItem();
        }
        const Streams *a = shell_a(s);
        const Streams *b = shell_b(s);
        if (a != nullptr) {
            if (ImGui::BeginTabItem("Canvas")) {
                s.view = dt_view::canvas;
                draw_canvas(b ? dt_canvas_build2(*a, *b) : dt_canvas_build(*a));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Timeline")) {
                s.view = dt_view::timeline;
                dt_slice cone;
                if (s.cone_active && s.selected_step)
                    cone = dt_slice_backward(a->df.edges, a->df.nsteps,
                                             *s.selected_step);
                const dt_slice *lit = s.cone_active ? &cone : nullptr;
                draw_timeline(b ? dt_timeline_build2(*a, *b, lit)
                                : dt_timeline_build(*a, lit));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Slice")) {
                s.view = dt_view::slice;
                draw_slice_view(dt_slice_view_build(*a, s.selected_step));
                if (b != nullptr)
                    // Never a fake merged graph: the two-recording slice needs
                    // the Wave-2 state-diff producer, and until it exists this
                    // says so instead of inventing one.
                    ImGui::TextDisabled(
                        "showing A only — slice diff lands with the "
                        "state-diff producer (Wave 2)");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diff")) {
                s.view = dt_view::diff;
                if (b == nullptr)
                    ImGui::TextDisabled(
                        "attach a second recording (press d) to "
                        "compare");
                else
                    draw_diff_view(dt_diff_view_build(*a, *b));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Loom")) {
                // The Phase-2 flagship: the recording as a spacetime fabric.
                // It weaves from the SAME decoded streams the other views use,
                // and refuses — with its reason on screen and no partial
                // drawing — for a recording whose producer was statistical or
                // carried no per-step values.
                draw_loom(s.loom, *a, s.ws, s.active_tab);
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Backends")) {
            draw_completeness(s.completeness, s.repo_root);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("This host")) {
            // What THIS machine can do and why not, straight from the
            // library's status APIs (06 T6). The render-only viewer shows the
            // loaded recording's provenance instead and says so.
            draw_capability_panel(s.caps, &r);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!s.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.status.c_str());
    }
}

// The recording-open dialog: a text field + Open/Cancel. A Workspace::open error
// renders verbatim in the dialog — never a silent no-op.
static void draw_open_dialog(ShellState &s) {
    ImGui::Begin("Open recording", &s.open_dialog);
    ImGui::TextUnformatted("path to a .asmtrace recording:");
    ImGui::InputText("##open_path", s.open_path, sizeof s.open_path);
    if (ImGui::Button("Open")) {
        std::string err;
        int idx = shell_open(s, s.open_path, err);
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

        // The Learn door: bundled walkthroughs, in BOTH binaries (it reads
        // recordings and links no engine — D4).
        // The Author door: full app only, and the render-only build says why
        // rather than hiding the tab (D4's split has to be legible).
        if (s.show_author && ImGui::BeginTabItem("Author", &s.show_author)) {
            draw_author_door(s.author);
            ImGui::EndTabItem();
        }

        if (s.show_learn && ImGui::BeginTabItem("Learn", &s.show_learn)) {
            draw_learn_door(s.learn, [&s](const std::string &path, long step) {
                // Route through 04's router rather than reaching into the
                // views: a stop and a pasted deep link must land identically.
                std::string err;
                if (shell_open(s, path, err) < 0 && !err.empty())
                    s.status = err;
                dt_link l;
                l.rec = recording_id(path);
                l.view = dt_view::timeline;
                if (step >= 0)
                    l.step = static_cast<uint32_t>(step);
                shell_wire_nav(s);
                if (!dt_nav_go(s.nav, l))
                    s.status = s.nav.last_error;
            });
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
                draw_recording_tab(s, r);
                ImGui::EndTabItem();
            }
            if (!keep_open)
                to_close = static_cast<int>(i);
        }
        if (to_close >= 0)
            shell_close(s, static_cast<size_t>(to_close));

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
    if (s.show_help) {
        ImGui::Begin("Keyboard bindings", &s.show_help);
        draw_bindings_help();
        ImGui::End();
    }
}

} // namespace asmdesk
