// observer_draw.cpp — the ImGui half of the Observer views (observer_draw.h).
// Drawing only: every rule this renders was decided in a model TU.
#include "views/observer_draw.h"

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "views/canvas.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {

const ImVec4 kWarn(0.90f, 0.78f, 0.35f, 1.0f);
const ImVec4 kBad(0.90f, 0.45f, 0.40f, 1.0f);
const ImVec4 kDim(0.65f, 0.65f, 0.65f, 1.0f);

void chrome_line(const ObsChrome &c) {
    ImGui::TextDisabled("%s", obs_chrome_chip(c).c_str());
    if (!c.banner.empty())
        // Not collapsible, by design: a banner you can dismiss is a banner you
        // forget while the numbers under it are still wrong.
        ImGui::TextColored(kWarn, "%s", c.banner.c_str());
}

void skip_line(const ObsSkip &s) {
    if (!s.present)
        return;
    // A skip is a SUCCESSFUL session with nothing to report, so it is never
    // drawn in the error colour — and its reason is the measured one, verbatim.
    ImGui::TextColored(kWarn, "skipped (%d)", s.code);
    ImGui::TextWrapped("%s", s.reason.c_str());
}

std::string hexs(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

void observer_build(ObserverState &s, const Recording &r,
                    const ObsLifecycle *lc) {
    s.syscalls = obs_syscalls_build(r, lc);
    s.watch = obs_watch_build(r, lc);
    s.topo = obs_topo_build(r, lc);
    s.hotedges = obs_hotedges_build(r, lc);
    s.tree = obs_tree_build(r, lc);
    s.region = obs_region_build(r, lc);
    s.disasm = obs_disasm_build(r, lc);
    s.built = true;
}

ObsLifecycle observer_lifecycle_from(const std::vector<nlohmann::json> &notes) {
    ObsLifecycle lc;
    for (const nlohmann::json &n : notes)
        if (n.is_object() && n.value("k", std::string()) == "session")
            lc.sessions.push_back(n);
    return lc;
}

bool observer_has_any(const ObserverState &s) {
    return !s.syscalls.rows.empty() || !s.watch.hits.empty() ||
           s.watch.skip.present || !s.topo.cards.empty() ||
           !s.hotedges.edges.empty() || !s.tree.rows.empty() ||
           !s.region.invocations.empty() || !s.disasm.versions.empty();
}

// --- T1 ---------------------------------------------------------------------
void draw_obs_syscalls(SyscallView &v) {
    chrome_line(v.chrome);
    ImGui::TextDisabled("%s", obs_syscall_tid_note());
    if (v.record_redacted)
        ImGui::TextColored(kWarn, "payload REDACTED at record time — the "
                                  "content is not in this recording at all");
    ImGui::Text("follow: %s", v.follow ? "child processes included" : "off");

    // The first press only ARMS the confirmation (the model's rule, not this
    // file's); the prompt below performs it.
    if (ImGui::Button("Reveal every payload..."))
        obs_syscall_reveal_all(v);
    if (v.reveal_all_armed) {
        ImGui::TextColored(kWarn, "%s",
                           obs_syscall_reveal_all_prompt(v).c_str());
        if (ImGui::Button("Yes, reveal them"))
            obs_syscall_reveal_all(v);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            v.reveal_all_armed = false;
    }

    if (!ImGui::BeginTable("syscalls", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("syscall");
    ImGui::TableSetupColumn("payload");
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < v.rows.size(); i++) {
        const SyscallRow &row = v.rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%zu", i);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.line.c_str());
        ImGui::TableNextColumn();
        const bool shown =
            v.reveal_all || (i < v.revealed.size() && v.revealed[i] != 0);
        ImGui::TextUnformatted(obs_syscall_payload_cell(v, i).c_str());
        if (row.has_payload && !v.reveal_all) {
            ImGui::SameLine();
            char id[32];
            std::snprintf(id, sizeof id, "%s##p%zu", shown ? "hide" : "reveal",
                          i);
            if (ImGui::SmallButton(id))
                obs_syscall_reveal(v, i, !shown);
        }
    }
    ImGui::EndTable();
}

// --- T2 ---------------------------------------------------------------------
void draw_obs_watch(const WatchView &v) {
    chrome_line(v.chrome);
    if (v.have_effective)
        ImGui::Text("armed: %s, %d byte(s), %s", hexs(v.effective.addr).c_str(),
                    v.effective.len,
                    v.effective.rw ? "reads and writes" : "writes only");
    skip_line(v.skip);
    if (v.hits.empty() && !v.skip.present)
        ImGui::TextDisabled("no hits yet — the target runs at native speed "
                            "between them");

    if (!ImGui::BeginTable("watch", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("hit");
    ImGui::TableSetupColumn("tid");
    ImGui::TableSetupColumn("direction");
    ImGui::TableSetupColumn("from");
    ImGui::TableSetupColumn("value");
    ImGui::TableHeadersRow();
    for (const WatchHit &h : v.hits) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long)h.hit_no);
        ImGui::TableNextColumn();
        ImGui::Text("%ld", h.tid);
        ImGui::TableNextColumn();
        if (h.is_write < 0)
            // The third outcome keeps its own colour as well as its own word:
            // it is a real measurement, not a missing one.
            ImGui::TextColored(kDim, "%s", obs_watch_dir_word(h.is_write));
        else
            ImGui::TextUnformatted(obs_watch_dir_word(h.is_write));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(obs_watch_loc(h).c_str());
        ImGui::TableNextColumn();
        if (!h.value_ok)
            ImGui::TextColored(kDim, "%s", obs_watch_value_cell(h).c_str());
        else
            ImGui::TextUnformatted(obs_watch_value_cell(h).c_str());
    }
    ImGui::EndTable();
}

// --- T3 ---------------------------------------------------------------------
void draw_obs_topo(const TopoView &v, const std::string &rec_id,
                   const std::function<void(const dt_link &)> &go) {
    chrome_line(v.chrome);
    ImGui::TextColored(kWarn, "%s", obs_topo_jack_note());
    skip_line(v.skip);
    ImGui::Text("%zu process(es) from %zu snapshot(s); `inv` counts %s",
                v.cards.size(), v.snapshots,
                v.count_mode.empty() ? "(unstated)" : v.count_mode.c_str());

    for (const TopoCard &c : v.cards) {
        ImGui::PushID(static_cast<int>(c.tgid));
        ImGui::Separator();
        ImGui::TextUnformatted(obs_topo_fingerprint(v, c).c_str());
        if (go && ImGui::SmallButton("open this process")) {
            go(obs_topo_drill_link(rec_id, c));
        }
        for (const TopoTask &t : c.threads)
            ImGui::BulletText("tid %ld  %s%s  inv=%llu", t.tid, t.comm.c_str(),
                              t.leader ? " (leader)" : "",
                              (unsigned long long)t.inv);
        ImGui::PopID();
    }
}

// --- T4 ---------------------------------------------------------------------
void draw_obs_hotedges(const HotEdgeView &v) {
    chrome_line(v.chrome);
    // Statistical, said before the numbers rather than under them.
    ImGui::TextColored(kWarn, "STATISTICAL");
    ImGui::TextWrapped("%s", obs_hotedges_evidence_label(v).c_str());
    if (!v.provenance_conflict.empty())
        ImGui::TextColored(kBad, "%s", v.provenance_conflict.c_str());
    ImGui::TextDisabled("%s", obs_hotedges_chrome_line(v).c_str());
    ImGui::TextDisabled("%s", obs_hotedges_no_flame_note());
    skip_line(v.skip);

    if (!ImGui::BeginTable("hotedges", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("from");
    ImGui::TableSetupColumn("to");
    ImGui::TableSetupColumn("samples");
    ImGui::TableSetupColumn("mispred / ret");
    ImGui::TableHeadersRow();
    for (const HotEdge &e : v.edges) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%d", e.rank);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.from.empty() ? hexs(e.from_addr).c_str()
                                              : e.from.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.to.empty() ? hexs(e.to_addr).c_str()
                                            : e.to.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long)e.count);
        ImGui::TableNextColumn();
        ImGui::Text("%llu / %llu", (unsigned long long)e.mispred,
                    (unsigned long long)e.is_return);
    }
    ImGui::EndTable();
}

// --- T5 ---------------------------------------------------------------------
std::string draw_obs_tree(const TreeView &v, ObserverState &s, long pid) {
    std::string cmd;
    chrome_line(v.chrome);
    skip_line(v.skip);
    if (v.have_effective)
        ImGui::TextDisabled(
            "running with: depth=%d focus=%s module=%s tid=%ld follow=%s",
            v.effective.depth,
            v.effective.focus.empty() ? "(none)" : v.effective.focus.c_str(),
            v.effective.module.empty() ? "(none)" : v.effective.module.c_str(),
            v.effective.tid, v.effective.follow ? "yes" : "no");

    ImGui::SeparatorText("filter (engine-side — it bounds what is EMITTED, so "
                         "the depths stay true)");
    ImGui::SetNextItemWidth(120);
    ImGui::InputInt("depth (0 = unlimited)", &s.filter.depth);
    ImGui::InputText("focus", s.focus_buf, sizeof s.focus_buf);
    ImGui::InputText("module", s.module_buf, sizeof s.module_buf);
    int tid = static_cast<int>(s.filter.tid);
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("tid (0 = every thread)", &tid))
        s.filter.tid = tid;
    ImGui::Checkbox("follow child processes", &s.filter.follow);
    s.filter.focus = s.focus_buf;
    s.filter.module = s.module_buf;

    std::string err = obs_tree_filter_error(s.filter);
    if (!err.empty()) {
        // The client refuses first, in the same words the server would use —
        // so nobody learns the rule by having a command bounce.
        ImGui::TextColored(kBad, "%s", err.c_str());
    } else {
        ImGui::BeginDisabled(pid <= 0);
        if (ImGui::Button("Start a tree session with this filter"))
            cmd = obs_tree_start_command(s.filter, pid);
        ImGui::EndDisabled();
        if (pid <= 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("select a process first");
        }
    }

    ImGui::SeparatorText("calls");
    for (const TreeRow &r : v.rows) {
        ImGui::Indent(static_cast<float>(r.depth) * 12.0f);
        ImGui::Text("-> %s [%s]  tid %ld",
                    r.name.empty() ? "(unresolved)" : r.name.c_str(),
                    r.module.empty() ? "?" : r.module.c_str(), r.tid);
        ImGui::Unindent(static_cast<float>(r.depth) * 12.0f);
    }
    return cmd;
}

// --- T6 ---------------------------------------------------------------------
void draw_obs_region(RegionView &v) {
    chrome_line(v.chrome);
    if (!v.crawl_warning.empty())
        ImGui::TextColored(kWarn, "%s", v.crawl_warning.c_str());
    if (!v.jit_hint.empty())
        ImGui::TextColored(kWarn, "%s", v.jit_hint.c_str());
    if (!v.basis_error.empty())
        ImGui::TextColored(kBad, "%s", v.basis_error.c_str());
    skip_line(v.skip);
    if (v.invocations.empty()) {
        ImGui::TextDisabled("no invocation captured yet — the engine waits at "
                            "the region's entry for a thread to arrive");
        return;
    }

    // Discrete paging, never a scrub: between two invocations the target ran
    // unobserved for an unknown time, and a slider would draw that gap as
    // elapsed captured time.
    if (ImGui::Button("< previous"))
        obs_region_page(v, -1);
    ImGui::SameLine();
    ImGui::Text("invocation #%zu of %zu", v.invocations[v.selected].number,
                v.invocations.size());
    ImGui::SameLine();
    if (ImGui::Button("next >"))
        obs_region_page(v, 1);

    const RegionInvocation &inv = v.invocations[v.selected];
    if (!inv.closed)
        ImGui::TextColored(kWarn,
                           "this invocation has no coverage footer — the "
                           "capture stopped inside it, so it is a prefix");
    ImGui::TextDisabled("%zu instruction(s) of %llu seen, %zu block(s) of %llu",
                        inv.insns.size(), (unsigned long long)inv.insns_total,
                        inv.blocks.size(),
                        (unsigned long long)inv.blocks_total);
    // 04's canvas renders the snapshot: one canvas for replay and for live.
    draw_canvas(dt_canvas_build(obs_region_snapshot_streams(v, v.selected)));
}

// --- T7 ---------------------------------------------------------------------
void draw_obs_disasm(const DisasmView &v, ObserverState &s) {
    chrome_line(v.chrome);
    if (!v.unavailable_reason.empty())
        ImGui::TextColored(kWarn, "no code image: %s",
                           v.unavailable_reason.c_str());
    if (v.versions.empty()) {
        ImGui::TextDisabled("falling back to the recorded disassembly (D10); "
                            "bytes-as-of-trace-time need codeimage events");
    }
    int when = static_cast<int>(s.disasm_when);
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputInt("as of logical time (0 = latest)", &when))
        s.disasm_when = when < 0 ? 0 : static_cast<uint64_t>(when);
    ImGui::TextDisabled("bytes are resolved to the version with the greatest "
                        "`when` <= this — never the newest, because a JIT "
                        "reuses addresses");

    std::vector<uint64_t> addrs;
    for (const auto &kv : v.recorded_disasm)
        addrs.push_back(kv.first);
    if (addrs.empty())
        for (const CodeVersion &c : v.versions)
            addrs.push_back(c.base);

    if (!ImGui::BeginTable("disasm", 4,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("address");
    ImGui::TableSetupColumn("bytes");
    ImGui::TableSetupColumn("disassembly");
    ImGui::TableSetupColumn("source");
    ImGui::TableHeadersRow();
    for (const DisasmRow &r : obs_disasm_rows(v, addrs, s.disasm_when, 8)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(hexs(r.addr).c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.bytes.empty() ? "-" : r.bytes.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.text.empty() ? "(no recorded text)"
                                              : r.text.c_str());
        ImGui::TableNextColumn();
        // The source is never implicit: a D10 fallback must not read like a
        // byte-exact reconstruction.
        ImGui::TextDisabled("%s", r.source.c_str());
    }
    ImGui::EndTable();
}

void draw_observer(ObserverState &s, const Recording &r,
                   const std::string &rec_id,
                   const std::function<void(const dt_link &)> &go) {
    if (!s.built)
        observer_build(s, r);
    if (!observer_has_any(s)) {
        ImGui::TextDisabled("this recording carries none of the Observer "
                            "kinds (syscall, watch, topo, survey, call, "
                            "trace, codeimage)");
        return;
    }
    if (!ImGui::BeginTabBar("observer"))
        return;
    if (!s.syscalls.rows.empty() && ImGui::BeginTabItem("Syscalls")) {
        draw_obs_syscalls(s.syscalls);
        ImGui::EndTabItem();
    }
    if ((!s.watch.hits.empty() || s.watch.skip.present) &&
        ImGui::BeginTabItem("Watch")) {
        draw_obs_watch(s.watch);
        ImGui::EndTabItem();
    }
    if (!s.topo.cards.empty() && ImGui::BeginTabItem("Topology")) {
        draw_obs_topo(s.topo, rec_id, go);
        ImGui::EndTabItem();
    }
    if (!s.hotedges.edges.empty() && ImGui::BeginTabItem("Hot edges")) {
        draw_obs_hotedges(s.hotedges);
        ImGui::EndTabItem();
    }
    if (!s.tree.rows.empty() && ImGui::BeginTabItem("Tree")) {
        draw_obs_tree(s.tree, s, 0);
        ImGui::EndTabItem();
    }
    if (!s.region.invocations.empty() && ImGui::BeginTabItem("Invocations")) {
        draw_obs_region(s.region);
        ImGui::EndTabItem();
    }
    if ((!s.disasm.versions.empty() || !s.disasm.unavailable_reason.empty()) &&
        ImGui::BeginTabItem("Disassembly")) {
        draw_obs_disasm(s.disasm, s);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

} // namespace asmdesk
