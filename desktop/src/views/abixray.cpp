// abixray.cpp — the pure builder + dump of abixray.h. No ImGui, no I/O.
#include "views/abixray.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The register at `name` in one already-built scrubber deck, or nullptr when the
// deck does not carry it (a torn pane has an empty deck).
const dt_scrubber_reg *find(const dt_scrubber &s, const std::string &name) {
    for (const dt_scrubber_reg &r : s.regs)
        if (r.name == name)
            return &r;
    return nullptr;
}

} // namespace

uint64_t dt_abixray_playhead(const wt_model &walk) {
    // Scan from the current stop back to the first: the nearest anchored,
    // in-window stop names the step the panes sit at. An unanchored stop (a
    // remark about the whole call) or one anchored past the recorded window does
    // not move the panes — the reader keeps their place (walkthrough.h's rule).
    int n = static_cast<int>(walk.stops.size());
    for (int i = walk.cur; i >= 0 && i < n; i--) {
        long a = walk.stops[static_cast<size_t>(i)].step_anchor;
        if (a >= 0 && walk.anchor_in_window(a))
            return static_cast<uint64_t>(a);
    }
    return 0;
}

dt_abixray dt_abixray_build(const StepIndex &sysv, const StepIndex &win64,
                            const wt_model &walk) {
    return dt_abixray_seek(sysv, win64, walk, dt_abixray_playhead(walk));
}

dt_abixray dt_abixray_seek(const StepIndex &sysv_idx,
                           const StepIndex &win64_idx, const wt_model &walk,
                           uint64_t playhead) {
    dt_abixray x;

    // The walkthrough rail — read straight off the wt_model, never re-derived,
    // so a stop's step and the pane it seeks cannot disagree.
    x.title = walk.title;
    x.stop_count = static_cast<int>(walk.stops.size());
    x.stop_cur = walk.stops.empty() ? -1 : walk.cur;
    if (const wt_stop *cs = walk.current()) {
        x.has_stop = true;
        x.stop_ordinal = cs->ordinal;
        x.stop_anchor = cs->step_anchor;
        x.stop_title = cs->title;
        x.stop_body = cs->body;
        // Refused, never clamped: a stop past the window keeps the panes put.
        x.stop_beyond_window = !walk.anchor_in_window(cs->step_anchor);
    }

    // Both panes need a per-step register producer, or there is nothing to lock.
    if (!sysv_idx.present() || !win64_idx.present()) {
        x.present = false;
        x.message =
            "the ABI x-ray needs BOTH the SysV and Win64 recordings to carry "
            "per-step register capture (record each with asmtrace_record "
            "--steps=<cap>); one of the pair has no regstate producer.";
        x.docs = "docs/internal/archive/gui/09-teaching-producers.md";
        return x;
    }
    x.present = true;
    x.desc = sysv_idx.desc;

    // Alignment: the locked playhead is a claim about ONE shared step space. If
    // the panes disagree on how many steps there are they are different runs, so
    // say so rather than overlay two timelines that do not correspond.
    x.aligned = sysv_idx.total_steps() == win64_idx.total_steps();
    x.total = sysv_idx.total_steps();
    if (!x.aligned)
        x.banner = "NOT ALIGNED: the SysV pane has " +
                   std::to_string(sysv_idx.total_steps()) +
                   " step(s), the Win64 pane " +
                   std::to_string(win64_idx.total_steps()) +
                   " step(s) — the panes are different runs and cannot share a "
                   "playhead";

    // Clamp into the shared space, then build BOTH scrubber decks at the SAME
    // playhead — one value feeds both, which is exactly what "locked" means.
    if (x.total > 0 && playhead >= x.total)
        playhead = x.total - 1;
    x.playhead = playhead;
    x.sysv = dt_scrubber_build(sysv_idx, playhead);
    x.win64 = dt_scrubber_build(win64_idx, playhead);

    // Torn fidelity, per pane: no data is not zero data.
    if (x.sysv.torn_here || x.win64.torn_here) {
        std::string which;
        if (x.sysv.torn_here)
            which += x.sysv_label + " ";
        if (x.win64.torn_here)
            which += x.win64_label + " ";
        x.banner = "TORN: step " + std::to_string(playhead) +
                   " was dropped by the ring in the " + which +
                   "pane — its register file is UNKNOWN, not zero";
    }

    // The cross-pane register deck. Descriptor order first (a stable deck), then
    // any extra register a forward-compat producer emitted, appended once in
    // sorted order. `differs` — the SysV and Win64 values disagree at this step —
    // is the marshalling contrast the narration points at.
    std::vector<std::string> names = stepindex_reg_order();
    std::vector<std::string> extras;
    for (const dt_scrubber *deck : {&x.sysv, &x.win64})
        for (const dt_scrubber_reg &r : deck->regs)
            if (std::find(names.begin(), names.end(), r.name) == names.end() &&
                std::find(extras.begin(), extras.end(), r.name) == extras.end())
                extras.push_back(r.name);
    std::sort(extras.begin(), extras.end());
    names.insert(names.end(), extras.begin(), extras.end());

    for (const std::string &name : names) {
        const dt_scrubber_reg *a = find(x.sysv, name);
        const dt_scrubber_reg *b = find(x.win64, name);
        if (a == nullptr && b == nullptr)
            continue; // neither pane carries it (both torn, or a subset producer)
        dt_abixray_row row;
        row.name = name;
        row.sysv_known = a != nullptr;
        row.win64_known = b != nullptr;
        if (a) {
            row.sysv = a->value;
            row.sysv_changed = a->changed;
        }
        if (b) {
            row.win64 = b->value;
            row.win64_changed = b->changed;
        }
        row.differs =
            row.sysv_known && row.win64_known && row.sysv != row.win64;
        if (row.differs)
            x.differ.push_back(name);
        x.rows.push_back(std::move(row));
    }
    x.n_differ = x.differ.size();
    return x;
}

std::string dt_abixray_dump(const dt_abixray &x) {
    std::string o;
    if (!x.present) {
        o += "no-producer\n";
        o += "message=" + x.message + "\n";
        o += "docs=" + x.docs + "\n";
        return o;
    }
    o += "abixray " + x.sysv_label + " | " + x.win64_label + "\n";
    o += "desc=" + x.desc + "\n";
    if (!x.title.empty())
        o += "title=" + x.title + "\n";
    o += "steps: total=" + std::to_string(x.total) +
         " aligned=" + (x.aligned ? "yes" : "no") + "\n";
    if (!x.banner.empty())
        o += "banner=" + x.banner + "\n";

    if (x.has_stop) {
        o += "stop " + std::to_string(x.stop_cur + 1) + "/" +
             std::to_string(x.stop_count) +
             " ordinal=" + std::to_string(x.stop_ordinal);
        o += x.stop_anchor < 0 ? " unanchored"
                               : " step=" + std::to_string(x.stop_anchor);
        if (x.stop_beyond_window)
            o += " BEYOND_WINDOW";
        if (!x.stop_title.empty())
            o += " title=\"" + x.stop_title + "\"";
        o += "\n";
    } else {
        o += "stop none\n";
    }

    o += "playhead=" + std::to_string(x.playhead) + "\n";
    for (const dt_abixray_row &r : x.rows) {
        o += "  " + r.name +
             " sysv=" + (r.sysv_known ? hex(r.sysv) : std::string("?")) +
             " win64=" + (r.win64_known ? hex(r.win64) : std::string("?"));
        if (r.differs)
            o += " DIFF";
        if (r.sysv_changed)
            o += " S*";
        if (r.win64_changed)
            o += " W*";
        o += "\n";
    }
    o += "differ(" + std::to_string(x.n_differ) + "):";
    for (const std::string &n : x.differ)
        o += " " + n;
    o += "\n";
    return o;
}

} // namespace asmdesk
