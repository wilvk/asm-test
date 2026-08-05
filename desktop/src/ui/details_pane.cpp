// details_pane.cpp — the Process details pane.
//
// It probes AUTOMATICALLY as the Processes selection moves, which is only
// defensible because `asmspy --info` never attaches: browsing a process list
// costs its targets nothing. The header says so out loud, because an operator
// who believes a details pane is attaching will avoid using it.
//
// Two sections are open by default — "Can I trace this?" and "What is it doing
// now" — because they are the two that change what the operator does next.
#include <algorithm>
#include <string>

#include "imgui.h"
#include "live/procinfo.h"
#include "ui/doors.h"
#include "ui/theme.h"

namespace asmdesk {

// 1 = yes, 0 = no, -1 = unknown. Same three colours inspect_door.cpp's
// verdict_colour uses, deliberately: two panes showing the same verdict in
// different colours is a UI that argues with itself.
static ImVec4 pi_verdict_col(int attachable) {
    return attachable == 1   ? dt_good_col()
           : attachable == 0 ? dt_bad_col()
                             : dt_maybe_col();
}

// The budget_exceeded caveat (Task 5 review) now lives IN
// procinfo_names_verdict itself (Task 7 review round 2, IMPORTANT 4):
// procinfo.cpp is no longer mid-edit by another task, and leaving the
// caveat pane-local meant the shared function still manufactured a
// confident "no symbols and no JIT map" verdict out of a budget-truncated
// zero for every OTHER caller (a row tooltip, a --shot card, ...). This
// stays a thin pass-through rather than disappearing at the call site, so a
// future pane-local wrinkle has somewhere to go without touching every call.
static std::string pi_names_verdict_caveated(const ProcInfo &p) {
    return procinfo_names_verdict(p);
}

void draw_details_pane(InspectState &s) {
    s.details.visible = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_DockHierarchy);
    // The Connect pane's "asmspy path" field (InspectState::asmspy_path, set
    // from Settings in main.cpp and editable in shell.cpp/inspect_door.cpp)
    // is the ONLY writer of this runner's exe. Nothing outside the tests
    // assigned it before, so the runner always fell through to
    // resolve_asmspy_path() and its own failure remedy — "or set the path in
    // Connect" — named a control that did not feed it. procinfo_tick reads
    // the change and clears the failure backoff, which is what makes fixing
    // the path in Connect take effect on the next frame instead of after a
    // backoff for the OLD path. Blank still means resolve, so a session that
    // never opens Connect behaves exactly as before.
    s.details.asmspy_path = s.asmspy_path;
    procinfo_tick(s.details, s.selected_pid, ImGui::GetTime());

    if (s.selected_pid <= 0) {
        ImGui::TextDisabled("pick a process in the Processes pane first.");
        return;
    }
    const ProcInfo &p = procinfo_current(s.details);
    // The status line is NEVER empty — "nothing yet" is a state worth naming.
    ImGui::TextDisabled("%s", procinfo_status(s.details).c_str());
    if (s.ssh_host[0])
        ImGui::TextDisabled(
            "note: this pid is LOCAL. The capture host is %s — the Processes "
            "list is this machine's /proc.",
            s.ssh_host);
    if (!p.valid) {
        if (!p.parse_error.empty())
            ImGui::TextWrapped("%s", p.parse_error.c_str());
        return;
    }
    // --- header ---------------------------------------------------------
    ImGui::Text("pid %ld  %s", p.pid, p.comm.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]  %c",
                        p.runtime.empty() ? "?" : p.runtime.c_str(), p.state);
    if (!p.argv.empty()) {
        std::string cmd;
        for (size_t i = 0; i < p.argv.size(); ++i)
            cmd += (i ? " " : "") + p.argv[i];
        if (p.argv_truncated)
            cmd += " …";
        ImGui::TextWrapped("%s", cmd.c_str());
    }

    // A gather that ran out of budget carries only PART of what it names, and
    // the sections it cut still hold their zero defaults. Rendering those as
    // measured is the one thing this whole snapshot refuses to do, so the
    // partial state is stated once, up front, before any number below it.
    if (p.budget_exceeded)
        ImGui::TextColored(
            dt_warn_col(),
            "partial: the 250ms gather budget ran out — module sizes, "
            "per-module symbol counts, the symbol/module totals, "
            "anonymous-executable bytes, the fd count and the children "
            "list may be absent or short rather than zero");

    // --- Can I trace this? (open) ---------------------------------------
    if (ImGui::CollapsingHeader("Can I trace this?",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        const char *word = p.attachable == 1   ? "YES"
                           : p.attachable == 0 ? "NO"
                                               : "MAYBE";
        ImGui::TextColored(pi_verdict_col(p.attachable), "attach   %s", word);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.attach_why.c_str());
        if (!p.attach_remedy.empty())
            ImGui::TextDisabled("-> %s", p.attach_remedy.c_str());

        if (ImGui::BeginTable("modes", 3,
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("mode");
            ImGui::TableSetupColumn("runs?");
            ImGui::TableSetupColumn("why not");
            ImGui::TableHeadersRow();
            for (const PiMode &m : p.modes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(m.mode.c_str());
                ImGui::TableNextColumn();
                // `ok` is two-valued; the attach verdict is three-valued.
                // A mode that clears its own gates under an UNDETERMINED
                // attach (attachable == -1 — Yama ptrace_scope=1, the stock
                // Debian/Ubuntu default, so the common case) is "maybe",
                // not "yes": nine green cells directly under a verdict line
                // reading MAYBE is a pane arguing with itself, and it is
                // the state most operators see first. Same three colours as
                // the verdict word above, for the same reason.
                const bool undetermined = m.ok && p.attachable == -1;
                ImGui::TextColored(undetermined ? dt_maybe_col()
                                   : m.ok       ? dt_good_col()
                                                : dt_bad_col(),
                                   "%s",
                                   undetermined ? "maybe"
                                   : m.ok       ? "yes"
                                                : "no");
                ImGui::TableNextColumn();
                // A refusal without its reason is the failure this pane exists
                // to prevent — the producer guarantees why is non-empty when
                // ok is false, so an empty one here is a wire bug worth seeing.
                // An undetermined cell carries the attach_why for the same
                // reason: a hedge with no reason is a hedge nobody can act on.
                ImGui::TextUnformatted(m.ok && !undetermined ? ""
                                                             : m.why.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", pi_names_verdict_caveated(p).c_str());
    }

    // --- What is it doing now? (open) ------------------------------------
    if (ImGui::CollapsingHeader("What is it doing now",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%d threads", p.n_threads);
        // ScrollY with no outer_size claims ALL remaining vertical space in
        // the window — leaving nothing for anything drawn after it (Task 7
        // review IMPORTANT 1: the mods table below measured -175px available
        // and BeginTable("mods", ...) returned false EVERY FRAME as a
        // result). Cap this table's height to what its own rows need
        // (+1 for the header), so the rest of the pane still gets space.
        const float row_h = ImGui::GetTextLineHeightWithSpacing();
        const float threads_h =
            row_h * (std::min<size_t>(p.threads.size(), 8) + 1);
        if (ImGui::BeginTable("threads", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY,
                              ImVec2(0, threads_h))) {
            ImGui::TableSetupColumn("tid", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("comm");
            ImGui::TableSetupColumn("state",
                                    ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("in");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (const PiThread &t : p.threads) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%ld", t.tid);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.comm.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%c", t.state);
                ImGui::TableNextColumn();
                // Preference order matters: a resolved syscall name is the
                // most specific true thing we have, then the kernel wchan,
                // then the REASON we could not read the syscall. The last is
                // dimmed because it describes our permission, not the target.
                if (t.have_syscall && !t.name.empty()) {
                    ImGui::TextUnformatted(t.name.c_str());
                    if (!t.pc_sym.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", t.pc_sym.c_str());
                    }
                } else if (!t.wchan.empty()) {
                    ImGui::TextUnformatted(t.wchan.c_str());
                } else {
                    ImGui::TextDisabled("%s", t.why.c_str());
                }
            }
            ImGui::EndTable();
        }
        if (p.threads_truncated)
            ImGui::TextDisabled("… (showing %zu of %d)", p.threads.size(),
                                p.n_threads);
    }

    // --- Resources (open) -------------------------------------------------
    if (ImGui::CollapsingHeader("Resources", ImGuiTreeNodeFlags_DefaultOpen)) {
        const ProcRates &r = procinfo_current_rates(s.details);
        // have == false means NOT YET MEASURABLE (one snapshot), which is a
        // different claim from a measured zero. An em dash says so; "0%" would
        // assert a measurement that was never taken.
        if (r.have) {
            dt_cell_magnitude_bar(dt_magnitude_frac(r.cpu_pct, 100.0),
                                  dt_dim_u32());
            ImGui::Text("cpu  %.1f%%", r.cpu_pct);
        } else {
            ImGui::TextDisabled("cpu  — (needs a second sample)");
        }
        ImGui::Text("rss  %llu KB", (unsigned long long)p.rss_kb);
        // Gated on io_readable / r.io_have, NOT the CPU-derived r.have: io's
        // own readiness is unrelated to whether the CPU tick rate or the
        // utime/stime counters cooperated this round (Task 7 review "cheap
        // fix") — collapsing them onto one flag would render "needs a
        // second sample" for io even when io itself was genuinely measured.
        if (!p.io_readable)
            ImGui::TextDisabled("io   — (/proc/<pid>/io needs matching creds)");
        else if (r.io_have)
            ImGui::Text("io   %.0f B/s read · %.0f B/s write", r.read_bps,
                        r.write_bps);
        else
            ImGui::TextDisabled("io   — (needs a second sample)");
        if (p.fds_readable)
            ImGui::Text("fds  %d", p.fds);
        else
            ImGui::TextDisabled("fds  — (unreadable)");
    }

    // --- collapsed detail sections ---------------------------------------
    if (ImGui::CollapsingHeader("Identity")) {
        ImGui::Text("uid   %s (%ld)", p.user.c_str(), p.uid);
        ImGui::Text("ppid  %ld", p.ppid);
        ImGui::Text("exe   %s%s", p.exe.empty() ? "(unreadable)" : p.exe.c_str(),
                    p.exe_deleted ? "  (deleted)" : "");
        // Same readlink(/proc/<pid>/cwd) failure as exe above (reachable on
        // a same-uid non-dumpable target, e.g. /usr/bin/ping) rendered a
        // blank line here instead of the same "(unreadable)" exe already
        // states for the identical cause (Task 7 review "cheap fix").
        ImGui::Text("cwd   %s", p.cwd.empty() ? "(unreadable)" : p.cwd.c_str());
        ImGui::Text("up    %.0fs", p.elapsed_s);
    }
    if (ImGui::CollapsingHeader("Code surface")) {
        // A budget-truncated gather can abort THIS ENTIRE SECTION before it
        // starts (Task 7 review IMPORTANT 2): syms_total/jit_methods/
        // anon_exec_bytes keep their zero defaults and modules stays empty.
        // "0 symbols · 0 modules" then asserts a measurement that was never
        // taken — pi_names_verdict_caveated above already withholds ITS
        // conclusion for exactly this reason; this guard is the same rule
        // applied to the raw counts.
        //
        // A permission denial on /proc/<pid>/maps is the SECOND way to get
        // the identical all-zero struct, and the more common one: the
        // symbol loader and the module scanner both read that file first,
        // so a target this user does not own yields "0 symbols · 0 modules"
        // and a confident "will show raw addresses" verdict — measured
        // against 684 live pids, 4 of them landed in exactly that shape.
        // Checked FIRST, because "we never got to look" dominates "we ran
        // out of time looking" when both are true.
        const char *code_unmeasured =
            !p.maps_readable
                ? "symbols / modules / anonymous executable — (/proc/<pid>/"
                  "maps could not be read, so the code surface was never "
                  "measured)"
            : p.budget_exceeded && p.modules.empty()
                ? "symbols / modules / anonymous executable — (the gather "
                  "budget ran out before this section was read)"
                : nullptr;
        if (code_unmeasured) {
            ImGui::TextDisabled("%s", code_unmeasured);
        } else {
            ImGui::Text("%llu symbols · %zu modules",
                        (unsigned long long)p.syms_total, p.modules.size());
            if (p.jit_methods)
                ImGui::Text("%llu JIT methods via %s",
                            (unsigned long long)p.jit_methods,
                            p.jit_source.c_str());
            ImGui::Text("%llu KB anonymous executable",
                        (unsigned long long)(p.anon_exec_bytes / 1024));
        }
        // An empty table (headers, zero rows) reads as "confirmed zero
        // modules" — the same failure the guard above refuses for the
        // counts. Skip the table entirely when there is nothing to show;
        // `code_unmeasured` above already said why.
        if (!p.modules.empty()) {
            const float row_h = ImGui::GetTextLineHeightWithSpacing();
            const float mods_h =
                row_h * (std::min<size_t>(p.modules.size(), 8) + 1);
            if (ImGui::BeginTable("mods", 3,
                                  ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY,
                                  ImVec2(0, mods_h))) {
                ImGui::TableSetupColumn("module");
                ImGui::TableSetupColumn("symbols",
                                        ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("size",
                                        ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const PiModule &m : p.modules) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(m.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)m.syms);
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu KB", (unsigned long long)(m.size / 1024));
                }
                ImGui::EndTable();
            }
            // "the N highest-symbol modules" is only true when the cut was
            // the 64-row CAP, which is applied AFTER a full symbol-count
            // ranking. A budget abort cuts the module-merge loop MID-SCAN
            // instead: the survivors never reach the ranking, every one of
            // them still reads 0 symbols, and what is left is whatever
            // /proc/<pid>/maps happened to list first — address order, which
            // the qsort then leaves in NAME order among the equal-zero
            // rows. Claiming those are the highest-symbol modules describes
            // a ranking that never ran.
            if (p.modules_truncated && p.budget_exceeded)
                ImGui::TextDisabled(
                    "… (%zu modules, cut mid-scan when the gather budget ran "
                    "out — not ranked, and their symbol counts are unread)",
                    p.modules.size());
            else if (p.modules_truncated)
                ImGui::TextDisabled(
                    "… (showing the %zu highest-symbol modules)",
                    p.modules.size());
        }
    }
    if (ImGui::CollapsingHeader("Containment")) {
        // ns_differs is computed from four readlink(/proc/<pid>/ns/*) ids,
        // each 0 when unreadable — and "0 == 0" is not a match, it is two
        // absences comparing equal. All four zero therefore reaches the
        // else arm and asserts "same namespaces as this app" about a
        // process whose namespaces were never read: verified against a live
        // `docker run alpine`, which rendered that claim two lines above a
        // docker-…scope cgroup flatly contradicting it.
        if (!p.ns_pid && !p.ns_net && !p.ns_mnt && !p.ns_user)
            ImGui::TextUnformatted(
                "— (namespace ids unreadable — whether this process shares "
                "our namespaces is unknown; the cgroup below may say)");
        else
            ImGui::TextUnformatted(
                p.ns_differs ? "in a DIFFERENT namespace than this app — "
                               "the pid does not mean what the local "
                               "table implies"
                             : "same namespaces as this app");
        ImGui::Text("cgroup   %s", p.cgroup.c_str());
        ImGui::Text("seccomp  %s", p.seccomp < 0 ? "unknown"
                                   : p.seccomp == 0 ? "off"
                                   : p.seccomp == 1 ? "strict"
                                                    : "filtered");
    }
    if (ImGui::CollapsingHeader("Children")) {
        for (const PiChild &c : p.children)
            ImGui::Text("%ld  %s", c.pid, c.comm.c_str());
        // "none" is a MEASUREMENT — the whole /proc walk completed and
        // found no process whose ppid is this one. A truncated walk did not
        // establish that; it stopped looking. And the cut is not always the
        // 32-row cap: a budget abort can end the scan before it has found a
        // single child (this pid's children sit wherever /proc's inode
        // order puts them), which rendered "none" followed by the frankly
        // absurd "capped at 0".
        if (p.children.empty() && !p.children_truncated)
            ImGui::TextDisabled("none");
        else if (p.children.empty())
            ImGui::TextDisabled(
                "— (the /proc scan stopped early — this is not 'no "
                "children')");
        else if (p.children_truncated)
            ImGui::TextDisabled("… (%zu shown; the scan stopped early, so "
                                "there may be more)",
                                p.children.size());
    }
}

} // namespace asmdesk
