// observer_draw.cpp — the ImGui half of the Observer views (observer_draw.h).
// Drawing only: every rule this renders was decided in a model TU.
#include "views/observer_draw.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS // node-editor headers use ImVec2 operators
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_memory_editor.h" // interactive codeimage byte view (14 T4)
#include "imgui_node_editor.h"   // topo/tree/hotedges pan-zoom canvas (15 T3)
#include "implot.h"              // src×dst hot-edge heatmap chassis (15 T1)
#include "textselect.hpp"        // select + copy for line panes (14 T6)

#include "ui/theme.h"
#include "views/canvas.h"
#include "views/graph_nav.h"
#include "views/views_draw.h"

namespace ed = ax::NodeEditor;

namespace asmdesk {

// App-only node-editor gate (see observer_draw.h). Default off, so the headless
// null-backend tests — which never call obs_graph_enable — take the fallback
// list/table and never touch node-editor. main.cpp turns it on for the app +
// viewer, exactly as it does for the ImPlot / ImSearch contexts.
namespace {
bool g_graph_enabled = false;
} // namespace
void obs_graph_enable(bool on) { g_graph_enabled = on; }
bool obs_graph_enabled() { return g_graph_enabled; }

namespace {

// Draw one deterministic GraphLayout on a node-editor canvas: positions fed
// EVERY frame (ed::SetNodePosition — the library never owns a position), the
// settings file disabled (kGraphSettingsFile == nullptr — it never persists or
// invents one), off-viewport nodes culled before emission, a double-clicked node
// routed through 04's dt_nav_go, and a "Fit graph" button = NavigateToContent.
// `ctx` is the caller's persistent per-view editor context (lazily created).
void draw_graph_canvas(const char *id, const GraphLayout &g,
                       const std::function<void(const dt_link &)> &go,
                       ed::EditorContext *&ctx) {
    if (ctx == nullptr) {
        ed::Config cfg;
        // The honesty guardrail: no settings file, so node-editor persists no
        // dragged position and loads none — the layout is the app's, always.
        cfg.SettingsFile = kGraphSettingsFile;
        ctx = ed::CreateEditor(&cfg);
    }
    ed::SetCurrentEditor(ctx);

    const bool fit = ImGui::SmallButton("Fit graph");
    ImGui::SameLine();
    ImGui::TextDisabled(
        "drag to pan · wheel to zoom · double-click a node to open it");

    ed::Begin(id, ImVec2(0.0f, 0.0f));

    // The visible canvas rectangle in node coordinates, for culling. Derived
    // from the editor's own screen->canvas transform so pan/zoom is honoured;
    // degenerate on a first frame (graph_visible then culls nothing).
    ImVec2 tl = ed::ScreenToCanvas(ImGui::GetWindowPos());
    ImVec2 br =
        ed::ScreenToCanvas(ImGui::GetWindowPos() + ImGui::GetWindowSize());
    GraphViewport vp;
    vp.x = tl.x;
    vp.y = tl.y;
    vp.w = br.x - tl.x;
    vp.h = br.y - tl.y;
    // A small margin so a node straddling the edge is not popped mid-drag.
    vp.x -= kGraphColW;
    vp.y -= kGraphRowH;
    vp.w += 2.0f * kGraphColW;
    vp.h += 2.0f * kGraphRowH;

    std::vector<std::size_t> vis = graph_visible(g, vp);
    std::vector<bool> shown(g.nodes.size(), false);
    for (std::size_t idx : vis) {
        const GraphNode &n = g.nodes[idx];
        shown[idx] = true;
        ed::SetNodePosition(ed::NodeId(n.id), ImVec2(n.x, n.y));
        ed::BeginNode(ed::NodeId(n.id));
        ImGui::PushID(static_cast<int>(n.id));
        // An input + output pin per node so edges have somewhere to land; the
        // pins are structural, not interactive (no link editing — D4).
        ed::BeginPin(ed::PinId(n.id * 2), ed::PinKind::Input);
        ImGui::TextUnformatted(" ");
        ed::EndPin();
        ImGui::SameLine();
        ImGui::TextUnformatted(n.label.c_str());
        ImGui::SameLine();
        ed::BeginPin(ed::PinId(n.id * 2 + 1), ed::PinKind::Output);
        ImGui::TextUnformatted(" ");
        ed::EndPin();
        ImGui::PopID();
        ed::EndNode();
    }
    // Edges only between two nodes both emitted this frame — a link to a culled
    // node references a pin node-editor never saw.
    std::unordered_map<uint64_t, std::size_t> pos;
    for (std::size_t i = 0; i < g.nodes.size(); i++)
        pos[g.nodes[i].id] = i;
    uint64_t link_id = 1;
    for (const GraphEdge &e : g.edges) {
        auto fi = pos.find(e.from_id), ti = pos.find(e.to_id);
        if (fi == pos.end() || ti == pos.end())
            continue;
        if (!shown[fi->second] || !shown[ti->second]) {
            link_id++;
            continue;
        }
        ed::Link(ed::LinkId(link_id++), ed::PinId(e.from_id * 2 + 1),
                 ed::PinId(e.to_id * 2));
    }

    // A double-clicked node navigates — the graph equivalent of the list's
    // "open this" button, routed through 04's spine (never a private reach).
    if (go) {
        ed::NodeId dbl = ed::GetDoubleClickedNode();
        if (dbl) {
            auto it = pos.find(dbl.Get());
            if (it != pos.end() && g.nodes[it->second].has_link)
                go(g.nodes[it->second].link);
        }
    }

    if (fit)
        ed::NavigateToContent();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace

namespace {

// Warn / refusal from the shared honesty-chrome palette (ui/theme.h). The
// Observer deck had drifted its own amber and red; a redaction or skip banner
// must read the same here as in every replay banner.
const ImVec4 kWarn = dt_warn_col();
const ImVec4 kBad = dt_refuse_col();
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
void draw_obs_syscalls(SyscallView &v, ObserverState &s) {
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

    // Client-side name filter (16 T2). It narrows the DISPLAY only: the model is
    // untouched, the "#" column keeps each row's true index, and a "showing N of
    // M" states exactly how many rows are hidden — so the filter can never read
    // as "the trace only did these syscalls". Order is preserved (the indices
    // come back ascending), which is why this is a plain substring filter and
    // not ImSearch (whose relevance re-ranking would scramble execution order).
    ImGui::SetNextItemWidth(240);
    ImGui::InputTextWithHint("##syscall-filter", "filter by name (e.g. openat)",
                             s.syscall_filter, sizeof s.syscall_filter);
    const std::vector<size_t> shown =
        obs_syscall_filter_indices(v, s.syscall_filter);
    if (s.syscall_filter[0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("showing %zu of %zu", shown.size(), v.rows.size());
    }

    if (!ImGui::BeginTable("syscalls", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("syscall");
    ImGui::TableSetupColumn("payload");
    ImGui::TableHeadersRow();
    for (size_t i : shown) {
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

    // A selectable, copy-able view of the payload-FREE lines (14 T6). TextSelect
    // needs a uniform sequential block from a child's cursor origin, which the
    // 3-column table above cannot provide (its selection math would drift over
    // rows), so the copy view is its own child. Payload-free by schema — the
    // reveal-gated payload is never copyable here. The lines are a pure, tested
    // source (obs_syscall_copy_lines); the static TextSelect holds the selection.
    static std::vector<std::string> copy_lines;
    copy_lines = obs_syscall_copy_lines(v);
    if (!copy_lines.empty()) {
        static TextSelect ts(
            [](size_t i) -> std::string_view {
                return i < copy_lines.size() ? std::string_view(copy_lines[i])
                                             : std::string_view();
            },
            []() -> size_t { return copy_lines.size(); });
        ImGui::SeparatorText("Copy (payload-free lines)");
        ImGui::BeginChild("syscall-copy", ImVec2(0, 120),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_NoMove);
        for (const std::string &ln : copy_lines)
            ImGui::TextUnformatted(ln.c_str());
        ts.update();
        if (ImGui::BeginPopupContextWindow()) {
            ImGui::BeginDisabled(!ts.hasSelection());
            if (ImGui::MenuItem("Copy"))
                ts.copy();
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Select all"))
                ts.selectAll();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }
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

    // Value-over-hit as PlotStairs (15-T1 follow-on) — the watched value between
    // hits is a step function (it holds until the next observed write). Only
    // value_ok hits are plotted; an un-read-back value is a GAP, never a
    // fabricated 0 (obs_watch_plot enforces it). Guarded on the ImPlot context so
    // the null test backends skip straight to the table.
    if (ImPlot::GetCurrentContext()) {
        WatchPlot wp = obs_watch_plot(v);
        if (wp.steps.size() >= 2) {
            if (ImPlot::BeginPlot("##watch-value", ImVec2(-1, 180),
                                  ImPlotFlags_NoLegend |
                                      ImPlotFlags_NoMouseText)) {
                ImPlot::SetupAxes("hit", "value", ImPlotAxisFlags_AutoFit,
                                  ImPlotAxisFlags_AutoFit);
                ImPlot::PlotStairs("value", wp.steps.data(), wp.values.data(),
                                   static_cast<int>(wp.steps.size()));
                ImPlot::EndPlot();
            }
        }
    }

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

    // The graph canvas (15 T3): processes as nodes on the app's deterministic
    // layers (root -> child), pan/zoom + fit-graph + double-click-to-open. Only
    // the real app enables it (node-editor is app-only, like ImPlot); the
    // headless deck smoke falls back to the card list below.
    if (obs_graph_enabled()) {
        static ed::EditorContext *ctx = nullptr;
        draw_graph_canvas("topo-graph", graph_from_topo(v, rec_id), go, ctx);
        return;
    }

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
void draw_obs_hotedges(const HotEdgeView &v, const std::string &rec_id,
                       const std::function<void(const dt_link &)> &go) {
    chrome_line(v.chrome);
    // Statistical, said before the numbers rather than under them.
    ImGui::TextColored(kWarn, "STATISTICAL");
    ImGui::TextWrapped("%s", obs_hotedges_evidence_label(v).c_str());
    if (!v.provenance_conflict.empty())
        ImGui::TextColored(kBad, "%s", v.provenance_conflict.c_str());
    ImGui::TextDisabled("%s", obs_hotedges_chrome_line(v).c_str());
    ImGui::TextDisabled("%s", obs_hotedges_no_flame_note());
    skip_line(v.skip);

    // A src×dst edge-count HEATMAP (15 T1) — a direct display of the edge
    // weights, never a stack (honesty R4). Guarded on the ImPlot context so the
    // headless view tests (which create none) skip straight to the table; only
    // the real app plots. Rows/cols are in rank order; the names live in the
    // table below, so long labels don't fight the grid.
    if (ImPlot::GetCurrentContext() && !v.edges.empty()) {
        HotEdgeMatrix hm = obs_hotedges_matrix(v, 24);
        if (!hm.cells.empty()) {
            double maxc = 0.0;
            for (double c : hm.cells)
                maxc = std::max(maxc, c);
            ImGui::TextDisabled(
                "src×dst edge heatmap (%zu×%zu%s) — rank order; names below",
                hm.rows.size(), hm.cols.size(), hm.truncated ? ", capped" : "");
            if (ImPlot::BeginPlot("##hotedge-heat", ImVec2(-1, 240),
                                  ImPlotFlags_NoMouseText |
                                      ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes(nullptr, nullptr,
                                  ImPlotAxisFlags_NoDecorations,
                                  ImPlotAxisFlags_NoDecorations);
                ImPlot::PlotHeatmap(
                    "edges", hm.cells.data(), static_cast<int>(hm.rows.size()),
                    static_cast<int>(hm.cols.size()), 0.0, maxc, nullptr);
                ImPlot::EndPlot();
            }
            ImGui::SameLine();
            ImPlot::ColormapScale("samples", 0.0, maxc, ImVec2(70, 240));
        }
    }

    // The FROZEN snapshot as a navigable graph (15 T3): branch endpoints as
    // nodes on the app's deterministic grid, edges from -> to, pan/zoom +
    // fit-graph. Still a survey of edges — no parent/child/total is synthesized
    // (doc 08 T4). App-only; the headless smoke and the ranked table below are
    // unaffected. Drawn between the heatmap and the exact table it complements.
    if (obs_graph_enabled() && !v.edges.empty()) {
        ImGui::TextDisabled("frozen snapshot — a graph of the ranked edges "
                            "(the exact counts are in the table below)");
        static ed::EditorContext *ctx = nullptr;
        draw_graph_canvas("hotedges-graph", graph_from_hotedges(v, rec_id), go,
                          ctx);
    }

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
std::string draw_obs_tree(const TreeView &v, ObserverState &s, long pid,
                          const std::string &rec_id,
                          const std::function<void(const dt_link &)> &go) {
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
    // The call tree as a graph (15 T3): x = the engine's EFFECTIVE depth (never
    // recomputed — it is re-based under a focus filter), y = emission order,
    // parent edges the nearest shallower row. Pan/zoom + fit-graph + click-to-
    // open. App-only; the headless deck smoke draws the indented list below.
    if (obs_graph_enabled() && !v.rows.empty()) {
        static ed::EditorContext *ctx = nullptr;
        draw_graph_canvas("tree-graph", graph_from_tree(v, rec_id), go, ctx);
        return cmd;
    }
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
namespace {
// user_data for the codeimage byte editor's read/color callbacks (14 T4). The
// editor iterates a 0-based buffer offset; the absolute address is base + off.
struct CodeMemCtx {
    const DisasmView *v;
    uint64_t when;
    uint64_t base;
};
ImU8 codeimage_read(const ImU8 *, size_t off, void *ud) {
    const CodeMemCtx *c = static_cast<const CodeMemCtx *>(ud);
    bool known = false;
    return obs_disasm_byte_at(*c->v, c->base + off, c->when, &known);
}
ImU32 codeimage_bg(const ImU8 *, size_t off, void *ud) {
    const CodeMemCtx *c = static_cast<const CodeMemCtx *>(ud);
    uint64_t a = c->base + off;
    bool known = false, klatest = false;
    uint8_t asof = obs_disasm_byte_at(*c->v, a, c->when, &known);
    if (!known)
        return IM_COL32(120, 40, 40,
                        110); // UNKNOWN as of this time — never faked
    uint8_t latest = obs_disasm_byte_at(*c->v, a, 0, &klatest);
    if (klatest && latest != asof)
        return IM_COL32(200, 150, 40, 90); // differs from latest (JIT churn)
    return 0;
}
} // namespace

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

    // Interactive byte view over the codeimage, as of the same logical time
    // (14-quick-wins.md T4). ReadFn resolves each byte through obs_disasm_byte_at
    // (greatest `when` <= this); ReadOnly honours never-re-read-live-memory in
    // replay; BgColorFn tints UNKNOWN bytes (dim red — never a fabricated value)
    // and bytes that differ from the latest version (amber — a JIT reused the
    // address). The pane below shows only pre-formatted hex; this is the addon.
    if (!v.versions.empty()) {
        uint64_t lo = UINT64_MAX, hi = 0;
        for (const CodeVersion &c : v.versions) {
            lo = std::min(lo, c.base);
            hi = std::max(hi, c.base + c.len);
        }
        size_t span = lo <= hi ? static_cast<size_t>(hi - lo) : 0;
        const size_t kCap = 64 * 1024; // bound a pathologically sparse span
        bool capped = span > kCap;
        if (capped)
            span = kCap;
        if (span > 0) {
            ImGui::SeparatorText("Bytes (as of trace time)");
            if (capped)
                ImGui::TextColored(
                    kWarn, "showing the first %zu bytes of the codeimage span",
                    span);
            static MemoryEditor ed;
            static CodeMemCtx ctx;
            ed.ReadOnly = true; // replay never re-reads live memory
            ctx.v = &v;
            ctx.when = s.disasm_when;
            ctx.base = lo;
            ed.ReadFn = codeimage_read;
            ed.BgColorFn = codeimage_bg;
            ed.UserData = &ctx;
            ed.DrawContents(&ctx, span, static_cast<size_t>(lo));
        }
    }
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
        draw_obs_syscalls(s.syscalls, s);
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
        draw_obs_hotedges(s.hotedges, rec_id, go);
        ImGui::EndTabItem();
    }
    if (!s.tree.rows.empty() && ImGui::BeginTabItem("Tree")) {
        draw_obs_tree(s.tree, s, 0, rec_id, go);
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
