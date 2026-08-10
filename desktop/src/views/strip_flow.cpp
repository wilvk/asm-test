// strip_flow.cpp — StripModel → space::SessionFlowScene. Pure: no ImGui, no
// GL, no I/O, no engine (D4).
#include "views/strip_flow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace asmdesk {

namespace {

uint32_t bucket_of(uint64_t seq, uint64_t seq_end) {
    if (seq_end == 0)
        return 0;
    uint64_t b = seq * space::kFlowBuckets / seq_end;
    if (b >= space::kFlowBuckets)
        b = space::kFlowBuckets - 1;
    return static_cast<uint32_t>(b);
}

// One smoothing pass for DISPLAY only (h' = ¼·prev + ½·self + ¼·next, ends
// clamped) over log1p'd counts. `counts` stays raw — the recoverable fact.
std::vector<float> smooth_heights(const std::vector<uint64_t> &counts) {
    const size_t n = counts.size();
    std::vector<float> raw(n, 0.0f), out(n, 0.0f);
    for (size_t i = 0; i < n; i++)
        raw[i] = std::log1p(static_cast<float>(counts[i]));
    for (size_t i = 0; i < n; i++) {
        const float p = raw[i > 0 ? i - 1 : i];
        const float s = raw[i];
        const float nx = raw[i + 1 < n ? i + 1 : i];
        out[i] = 0.25f * p + 0.5f * s + 0.25f * nx;
    }
    return out;
}

} // namespace

space::SessionFlowScene build_session_flow(const StripModel &m) {
    space::SessionFlowScene f;
    f.seq_end = m.seq_end;
    if (m.seq_end == 0) {
        f.disabled_reason = "no stream to bucket — the recording holds no "
                            "events at all";
        return f;
    }

    auto bucket_counts = [&](const std::vector<uint64_t> &seqs) {
        std::vector<uint64_t> c(space::kFlowBuckets, 0);
        for (uint64_t s : seqs)
            c[bucket_of(s, m.seq_end)]++;
        return c;
    };
    auto total = [](const std::vector<uint64_t> &c) {
        uint64_t t = 0;
        for (uint64_t x : c)
            t += x;
        return t;
    };

    // --- lanes: the strip's own posture rule, so the two surfaces agree ----
    const StripSelection sel = strip_selected_lanes(m, /*detail=*/false);
    for (size_t k : sel.keep) {
        space::FlowRow r;
        r.kind = space::FlowRowKind::Lane;
        r.label = m.lanes[k].label;
        r.tid = m.lanes[k].tid;
        r.tgid = m.lanes[k].tgid;
        r.lane_ord = static_cast<uint32_t>(k); // the strip pc-mark hue
        r.counts = bucket_counts(m.lane_activity[k]);
        r.events = total(r.counts);
        f.rows.push_back(std::move(r)); // a silent kept lane stays: flat,
                                        // never invented away
    }
    if (sel.hidden) {
        std::vector<char> kept(m.lanes.size(), 0);
        for (size_t k : sel.keep)
            kept[k] = 1;
        std::vector<uint64_t> merged;
        for (size_t i = 0; i < m.lane_activity.size(); i++)
            if (!kept[i])
                merged.insert(merged.end(), m.lane_activity[i].begin(),
                              m.lane_activity[i].end());
        space::FlowRow r;
        r.kind = space::FlowRowKind::AggregateLanes;
        r.counts = bucket_counts(merged);
        r.events = total(r.counts);
        r.label = "(+" + std::to_string(sel.hidden) + " lanes, " +
                  std::to_string(r.events) + " events)";
        f.rows.push_back(std::move(r));
    }

    // --- the kernel row: dominant class per bucket, grey on tie/none -------
    if (!m.sys.empty()) {
        space::FlowRow r;
        r.kind = space::FlowRowKind::Kernel;
        r.label = "kernel crossings";
        r.counts.assign(space::kFlowBuckets, 0);
        r.bucket_class.assign(space::kFlowBuckets, 0);
        // per bucket, per class tallies (7 classes incl. Other)
        std::vector<std::array<uint32_t, 7>> tally(
            space::kFlowBuckets, std::array<uint32_t, 7>{});
        for (const StripSys &s : m.sys) {
            const uint32_t b = bucket_of(s.seq, m.seq_end);
            r.counts[b]++;
            tally[b][static_cast<size_t>(s.cls)]++;
        }
        for (uint32_t b = 0; b < space::kFlowBuckets; b++) {
            uint32_t best = 0, best_n = 0, second = 0;
            for (uint32_t c = 0; c < 7; c++)
                if (tally[b][c] > best_n) {
                    second = best_n;
                    best_n = tally[b][c];
                    best = c;
                } else if (tally[b][c] > second)
                    second = tally[b][c];
            // dominant = a strict winner; a tie stays the visible grey 0
            r.bucket_class[b] =
                (best_n > 0 && best_n > second)
                    ? static_cast<uint8_t>(best + 1)
                    : 0;
        }
        r.events = total(r.counts);
        f.rows.push_back(std::move(r));
    }

    // --- the memory row (mem carries no tid: r/w-shaded, never thread-hued)
    if (!m.mem.empty()) {
        space::FlowRow r;
        r.kind = space::FlowRowKind::Memory;
        r.label = "memory accesses";
        std::vector<uint64_t> seqs;
        seqs.reserve(m.mem.size());
        for (const StripMemMark &k : m.mem)
            seqs.push_back(k.seq);
        r.counts = bucket_counts(seqs);
        r.events = total(r.counts);
        f.rows.push_back(std::move(r));
    }

    // --- seams ---------------------------------------------------------------
    for (size_t i = 0; i < m.seams.size(); i++) {
        space::FlowSeam s;
        s.bucket = bucket_of(m.seams[i].seq, m.seq_end);
        s.label = m.seams[i].label;
        s.kind = static_cast<uint8_t>(m.seams[i].kind);
        f.seams.push_back(std::move(s));
    }

    if (f.rows.empty()) {
        f.disabled_reason =
            "recording carries no mem/syscall/trace/call/watch/df_step "
            "events";
        return f;
    }

    // --- display heights: log1p + one smoothing pass, normalized to the
    //     SCENE max so rows compare honestly --------------------------------
    float scene_max = 0.0f;
    for (auto &r : f.rows) {
        r.heights = smooth_heights(r.counts);
        for (float h : r.heights)
            scene_max = std::max(scene_max, h);
    }
    if (scene_max > 0.0f)
        for (auto &r : f.rows)
            for (float &h : r.heights)
                h /= scene_max;

    f.enabled = true;
    return f;
}

std::string session_flow_dump(const space::SessionFlowScene &f) {
    std::string s;
    char buf[160];
    std::snprintf(buf, sizeof buf, "flow enabled=%d rows=%zu seams=%zu seq_end=%llu\n",
                  f.enabled ? 1 : 0, f.rows.size(), f.seams.size(),
                  static_cast<unsigned long long>(f.seq_end));
    s += buf;
    if (!f.enabled)
        return s + "reason: " + f.disabled_reason + "\n";
    for (const auto &r : f.rows) {
        std::snprintf(buf, sizeof buf,
                      "row kind=%u tid=%lld ord=%u events=%llu %s\n",
                      static_cast<unsigned>(r.kind),
                      static_cast<long long>(r.tid), r.lane_ord,
                      static_cast<unsigned long long>(r.events),
                      r.label.c_str());
        s += buf;
    }
    for (const auto &sm : f.seams) {
        std::snprintf(buf, sizeof buf, "seam bucket=%u kind=%u %s\n",
                      sm.bucket, sm.kind, sm.label.c_str());
        s += buf;
    }
    return s;
}

size_t flow_pick_order(const space::SessionFlowScene &f) {
    return f.rows.size() + f.seams.size();
}

std::optional<dt_link> flow_pick_link(const space::SessionFlowScene &f,
                                      size_t ord, const std::string &rec_id) {
    if (ord >= f.rows.size())
        return std::nullopt; // seams are hover-only
    const space::FlowRow &r = f.rows[ord];
    dt_link l;
    l.rec = rec_id;
    switch (r.kind) {
    case space::FlowRowKind::Lane:
        l.view = dt_view::syscalls;
        if (r.tgid != -1)
            l.pid = r.tgid;
        return l;
    case space::FlowRowKind::AggregateLanes:
    case space::FlowRowKind::Kernel:
        l.view = dt_view::syscalls;
        return l;
    case space::FlowRowKind::Memory:
        l.view = dt_view::timeline;
        return l;
    }
    return std::nullopt;
}

std::string flow_pick_hint(const space::SessionFlowScene &f, size_t ord) {
    if (ord < f.rows.size()) {
        const space::FlowRow &r = f.rows[ord];
        return r.label + " — " + std::to_string(r.events) + " events";
    }
    const size_t j = ord - f.rows.size();
    if (j < f.seams.size())
        return f.seams[j].label;
    return std::string();
}

} // namespace asmdesk
