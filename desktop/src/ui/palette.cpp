// palette.cpp — the command palette (see palette.h). The table is pure; the draw
// is backend-free ImGui with an app-only ImSearch relevance path (doc 16 T2).
#include "ui/palette.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "imgui.h"
#ifdef ASMDESK_HAVE_IMSEARCH
#include "imsearch.h"
#endif

#include "doc/recording.h" // load_recording_file — the walkthrough action
#include "ui/shell.h"      // ShellState + shell_a
#include "ui/wayfinding.h" // disambiguated_label — shared with T2's breadcrumb
#include "walkthrough.h"   // wt_build — the walkthrough action

namespace asmdesk {

bool palette_parse_goto(const ShellState &s, const std::string &text,
                        dt_link &out) {
    const Streams *a = shell_a(s);
    char *end = nullptr;
    unsigned long n = std::strtoul(text.c_str(), &end, 0);
    if (end != text.c_str() && end && *end == '\0' && a != nullptr) {
        // A bare number is a step on the active recording, in the current view —
        // exactly the go-to modal's rule, so a typed target lands identically.
        out = dt_link{};
        out.view = s.view;
        out.rec = a->id;
        out.step = static_cast<uint32_t>(n);
        return true;
    }
    std::string perr;
    return dt_nav_parse(text, out, perr);
}

std::vector<PaletteEntry> build_palette(const ShellState &s) {
    std::vector<PaletteEntry> out;

    // 1. GO TO — the live query, when it parses, then the modal fallback. The
    // live entry dispatches through dt_nav_go and lands a refusal in s.status
    // verbatim, exactly as the keymap / go-to paths do.
    {
        std::string q = s.palette_buf;
        dt_link l;
        if (!q.empty() && palette_parse_goto(s, q, l)) {
            out.push_back({"Go to " + dt_nav_format(l), "go-to",
                           PaletteCategory::GoTo, [l](ShellState &st) {
                               if (!dt_nav_go(st.nav, l))
                                   st.status = st.nav.last_error;
                           }});
        }
    }
    out.push_back({"Go to step or offset…", "Ctrl+G", PaletteCategory::GoTo,
                   [](ShellState &st) { st.show_goto = true; }});

    // 2. VIEW-SWITCH — one per view the app can actually SWITCH TO by intent.
    // `want_view` (mirroring handle_keymap's 1/2/3/4) drives the outer tab, which
    // the shell honours only for the reading views that own an outer tab/pane
    // (canvas, timeline, slice, diff). `blame` is the failure ENTRY POINT, not a
    // view; the Observer-deck sub-views (syscalls, watch, topo, hot-edges, tree,
    // region, disasm) are INNER tabs of the Observer deck with no outer switch —
    // reachable by topology/blame drill-in through dt_nav_go, but not by a bare
    // want_view. Offering them here would advertise a command the shell silently
    // drops, breaking this palette's own invariant (§8) that it never advertises
    // a key the app does not honour. So the View category lists only the
    // want_view-honoured views; test_palette pins exactly this set.
    for (dt_view v : dt_all_views()) {
        if (!dt_view_has_outer_tab(v))
            continue;
        std::string name = dt_view_name(v);
        out.push_back({"Show " + name + " view", "view", PaletteCategory::View,
                       [v](ShellState &st) { st.want_view = v; }});
    }

    // 3. OPEN-RECENT — the OPEN workspace (persisted MRU is doc 20). Labels share
    // T2's disambiguation so two same-basename recordings are distinguishable.
    for (size_t i = 0; i < s.ws.recordings.size(); i++) {
        std::string lbl = disambiguated_label(s.ws, i);
        out.push_back({"Switch to " + lbl, "recording", PaletteCategory::Recent,
                       [i](ShellState &st) {
                           if (i < st.ws.recordings.size()) {
                               st.want_open_tab = static_cast<int>(i);
                               st.active_tab = static_cast<int>(i);
                           }
                       }});
    }

    // 4. ATTACH-PID — the Inspect door (the asmspy --serve capture host, doc 07).
    out.push_back({"Attach to a running process…", "Inspect",
                   PaletteCategory::Attach,
                   [](ShellState &st) { st.show_inspect = true; }});

    // 5. RUN-WALKTHROUGH — one per known Learn card, opening the Learn door at
    // that card (loading its player exactly as the door's own click does).
    for (size_t i = 0; i < s.learn.cards.size(); i++) {
        const LearnCard &c = s.learn.cards[i];
        std::string path = c.path;
        std::string title = c.title.empty() ? c.id : c.title;
        out.push_back({"Walkthrough: " + title, "Learn",
                       PaletteCategory::Walkthrough, [i, path](ShellState &st) {
                           st.show_learn = true;
                           if (i < st.learn.cards.size())
                               st.learn.open_card = static_cast<int>(i);
                           std::string err;
                           auto rec = load_recording_file(path, err);
                           st.learn.player = rec ? wt_build(*rec) : wt_model{};
                       }});
    }

    // 6. RESET-LAYOUT — the same intent Ctrl+Shift+R / the View menu raise (the
    // dockspace build consumes it, so it fires with or without the menu bar).
    out.push_back({"Reset panel layout", "Ctrl+Shift+R", PaletteCategory::Layout,
                   [](ShellState &st) { st.want_layout_reset = true; }});

    // 7. ROUTINE / SYMBOL — the v1 `.asmtrace` header carries NO routine identity
    // (diff.cpp says so), so the faithful stand-in is the recorded code offsets and
    // their recorded disassembly: each dispatches dt_nav_go to that offset. Bound
    // so a pathological trace cannot build a million rows every frame.
    if (const Streams *a = shell_a(s)) {
        const size_t kCap = 1000;
        size_t n = 0;
        std::string rec = a->id;
        for (const auto &kv : a->trace.disasm) {
            if (n++ >= kCap)
                break;
            uint64_t off = kv.first;
            char buf[32];
            std::snprintf(buf, sizeof buf, "0x%llx",
                          static_cast<unsigned long long>(off));
            out.push_back({kv.second + "  @" + buf, "code",
                           PaletteCategory::Routine, [off, rec](ShellState &st) {
                               dt_link l;
                               l.view = dt_view::canvas;
                               l.rec = rec;
                               l.off = off;
                               if (!dt_nav_go(st.nav, l))
                                   st.status = st.nav.last_error;
                           }});
        }
        if (a->trace.disasm.empty()) {
            for (uint64_t off : a->trace.blocks) {
                if (n++ >= kCap)
                    break;
                char buf[32];
                std::snprintf(buf, sizeof buf, "0x%llx",
                              static_cast<unsigned long long>(off));
                out.push_back({std::string(buf), "code",
                               PaletteCategory::Routine,
                               [off, rec](ShellState &st) {
                                   dt_link l;
                                   l.view = dt_view::canvas;
                                   l.rec = rec;
                                   l.off = off;
                                   if (!dt_nav_go(st.nav, l))
                                       st.status = st.nav.last_error;
                               }});
            }
        }
    }

    // 8. ACCELERATOR HINTS — one NON-dispatching row per dt_nav_bindings() row, so
    // every advertised key (the doc-18 freshly-wired ones included) is surfaced by
    // typing. Enumerating the SAME table the help overlay uses means the palette
    // can never advertise a key the app does not honour, and test_palette pins the
    // one-to-one enumeration so a future binding cannot silently drop from here.
    for (const dt_binding &b : dt_nav_bindings())
        out.push_back({b.what, b.keys, PaletteCategory::KeyHint, {}});

    return out;
}

namespace {

// Case-insensitive substring match over label + detail. Empty query matches all.
bool palette_match(const PaletteEntry &e, const std::string &q) {
    if (q.empty())
        return true;
    std::string hay = e.label + " " + e.detail;
    std::string needle = q;
    auto low = [](std::string &x) {
        for (char &c : x)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    low(hay);
    low(needle);
    return hay.find(needle) != std::string::npos;
}

} // namespace

void draw_palette(ShellState &s) {
    if (s.show_palette)
        ImGui::OpenPopup("Command palette");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 440), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Command palette", nullptr, 0)) {
        // Closed (Escape / click-away): keep the flag in step so it does not
        // re-open next frame.
        s.show_palette = false;
        return;
    }

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputTextWithHint(
        "##palettequery",
        "type a view, step/offset, recent, routine, or action…", s.palette_buf,
        sizeof s.palette_buf, ImGuiInputTextFlags_EnterReturnsTrue);

    std::vector<PaletteEntry> entries = build_palette(s);
    std::string q = s.palette_buf;

    // The dispatch discipline shared by every path: run the action (routing only
    // through the spine), clear, close.
    bool close = false;
    auto run = [&](const PaletteEntry &e) {
        if (e.action)
            e.action(s);
        close = true;
    };

    std::vector<const PaletteEntry *> shown;
    for (const PaletteEntry &e : entries)
        if (palette_match(e, q))
            shown.push_back(&e);
    ImGui::TextDisabled("showing %zu of %zu", shown.size(), entries.size());
    ImGui::Separator();

    // Enter runs the top match's action (the first dispatchable shown row).
    if (enter)
        for (const PaletteEntry *e : shown)
            if (e->action) {
                run(*e);
                break;
            }

    if (!close) {
#ifdef ASMDESK_HAVE_IMSEARCH
        // App-only relevance ranking (doc 16 T2 form), guarded on the ImSearch
        // context so the null test backend takes the plain unranked list below.
        if (ImSearch::GetCurrentContext()) {
            if (ImSearch::BeginSearch()) {
                ImSearch::SearchBar("filter");
                for (size_t i = 0; i < entries.size(); i++) {
                    ImSearch::SearchableItem(
                        entries[i].label.c_str(),
                        [&entries, &run, i](const char *) {
                            const PaletteEntry &e = entries[i];
                            if (ImGui::Selectable(e.label.c_str()) && e.action)
                                run(e);
                            if (!e.detail.empty()) {
                                ImGui::SameLine();
                                ImGui::TextDisabled("%s", e.detail.c_str());
                            }
                        });
                }
                ImSearch::Submit();
                ImSearch::EndSearch();
            }
        } else
#endif
        {
            for (const PaletteEntry *e : shown) {
                if (ImGui::Selectable(e->label.c_str()) && e->action) {
                    run(*e);
                    break;
                }
                if (!e->detail.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", e->detail.c_str());
                }
            }
        }
    }

    if (close) {
        s.show_palette = false;
        s.palette_buf[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace asmdesk
