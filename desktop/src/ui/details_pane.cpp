// details_pane.cpp — the Process details pane.
//
// It probes AUTOMATICALLY as the Processes selection moves, which is only
// defensible because `asmspy --info` never attaches: browsing a process list
// costs its targets nothing. The header says so out loud, because an operator
// who believes a details pane is attaching will avoid using it.
//
// Two sections are open by default — "Can I trace this?" and "What is it doing
// now" — because they are the two that change what the operator does next.
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

// procinfo_names_verdict reads off syms_total/jit_methods, and a budget-
// truncated gather can UNDERCOUNT both — so its confident "a trace of this
// process will show names" conclusion can be wrong in exactly the direction
// that matters (Task 5 review). State the uncertainty instead of the
// conclusion when budget_exceeded is set. This wraps the shared model
// function rather than editing it: procinfo.cpp is a shared file another
// task in this plan is mid-edit on for an unrelated runner fix.
static std::string pi_names_verdict_caveated(const ProcInfo &p) {
    if (p.budget_exceeded)
        return "names verdict withheld: the gather budget ran out before "
               "symbols/JIT methods were fully counted, so syms_total (" +
               std::to_string(p.syms_total) + ") and jit_methods (" +
               std::to_string(p.jit_methods) +
               ") may be undercounts — re-run once the target is idle for a "
               "trustworthy answer";
    return procinfo_names_verdict(p);
}

void draw_details_pane(InspectState &s) {
    s.details.visible = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_DockHierarchy);
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
        ImGui::TextColored(dt_warn_col(),
                           "partial: the 250ms gather budget ran out — module "
                           "sizes, exec bits and per-module symbol counts may "
                           "be absent rather than zero");

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
                ImGui::TextColored(m.ok ? dt_good_col() : dt_bad_col(), "%s",
                                   m.ok ? "yes" : "no");
                ImGui::TableNextColumn();
                // A refusal without its reason is the failure this pane exists
                // to prevent — the producer guarantees why is non-empty when
                // ok is false, so an empty one here is a wire bug worth seeing.
                ImGui::TextUnformatted(m.ok ? "" : m.why.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::TextWrapped("%s", pi_names_verdict_caveated(p).c_str());
    }

    // --- What is it doing now? (open) ------------------------------------
    if (ImGui::CollapsingHeader("What is it doing now",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("%d threads", p.n_threads);
        if (ImGui::BeginTable("threads", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY)) {
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
        if (p.io_readable && r.have)
            ImGui::Text("io   %.0f B/s read · %.0f B/s write", r.read_bps,
                        r.write_bps);
        else if (!p.io_readable)
            ImGui::TextDisabled("io   — (/proc/<pid>/io needs matching creds)");
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
        ImGui::Text("cwd   %s", p.cwd.c_str());
        ImGui::Text("up    %.0fs", p.elapsed_s);
    }
    if (ImGui::CollapsingHeader("Code surface")) {
        ImGui::Text("%llu symbols · %zu modules",
                    (unsigned long long)p.syms_total, p.modules.size());
        if (p.jit_methods)
            ImGui::Text("%llu JIT methods via %s",
                        (unsigned long long)p.jit_methods,
                        p.jit_source.c_str());
        ImGui::Text("%llu KB anonymous executable",
                    (unsigned long long)(p.anon_exec_bytes / 1024));
        if (ImGui::BeginTable("mods", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("module");
            ImGui::TableSetupColumn("symbols",
                                    ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed);
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
        if (p.modules_truncated)
            ImGui::TextDisabled("… (showing the %zu highest-symbol modules)",
                                p.modules.size());
    }
    if (ImGui::CollapsingHeader("Containment")) {
        ImGui::TextUnformatted(p.ns_differs
                                   ? "in a DIFFERENT namespace than this app — "
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
        if (p.children.empty())
            ImGui::TextDisabled("none");
        if (p.children_truncated)
            ImGui::TextDisabled("… (capped at %zu)", p.children.size());
    }
}

} // namespace asmdesk
