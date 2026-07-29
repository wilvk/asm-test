// streams.cpp — decode a loaded Recording into typed view streams (streams.h).
#include "doc/streams.h"

#include <algorithm>
#include <set>

namespace asmdesk {

namespace {

// Read one JSON field, leaving `out` untouched when it is absent or the wrong
// type. Absence is normal in this schema (optional fields are OMITTED, never
// null), so it is never an error here — a view degrades instead.
template <typename T>
void get(const nlohmann::json &j, const char *key, T &out) {
    auto it = j.find(key);
    if (it == j.end())
        return;
    if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string())
            out = it->get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean())
            out = it->get<bool>();
    } else {
        if (it->is_number_integer() || it->is_number_unsigned())
            out = it->get<T>();
    }
}

// Decode a lowercase-hex string into bytes in order; a malformed/odd-length one
// yields no bytes rather than a partial guess (the view then degrades to [wide]).
std::vector<uint8_t> decode_hex(const std::string &h) {
    std::vector<uint8_t> out;
    if (h.size() % 2 != 0)
        return out;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        int hi = nib(h[i]), lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

ValRec decode_op(const nlohmann::json &j) {
    ValRec v;
    get(j, "space", v.space);
    get(j, "reg", v.reg);
    get(j, "addr", v.addr);
    get(j, "size", v.size);
    get(j, "write", v.write);
    get(j, "value_valid", v.value_valid);
    get(j, "wide", v.wide);
    get(j, "value", v.value);
    std::string hex;
    get(j, "bytes", hex);
    if (!hex.empty())
        v.bytes = decode_hex(hex);
    return v;
}

// Record one event's basis tag. The schema forbids defaulting it, so an event
// without one is not silently adopted into whatever basis came first.
void note_basis(TraceStream &t, const nlohmann::json &body, const char *kind) {
    std::string basis;
    get(body, "basis", basis);
    if (basis.empty()) {
        if (t.basis_error.empty())
            t.basis_error = std::string("a ") + kind +
                            " event carries no \"basis\" — the schema forbids "
                            "defaulting it, so this stream cannot be placed";
        return;
    }
    if (t.basis.empty()) {
        t.basis = basis;
        return;
    }
    if (t.basis != basis && t.basis_error.empty())
        t.basis_error = "mixed address bases: \"" + t.basis + "\" and \"" +
                        basis +
                        "\" events in one stream — region-relative "
                        "offsets and absolute addresses cannot share "
                        "one axis; re-record, or open the streams "
                        "separately";
}

const std::vector<Event> *kind(const Recording &r, const char *k) {
    auto it = r.by_kind.find(k);
    return it == r.by_kind.end() ? nullptr : &it->second;
}

// Decode one invocation pass's df_step + df_edge events (already the slice for this
// pass, in stream order) into a DataflowStream. The `step` axis is 0-based within
// THIS pass; a continuous capture's passes are sliced apart by
// build_segmented_dataflow before they reach here, so no pass's step overwrites
// another's (40). Pure — it reads only the supplied events, the exact logic
// decode_streams held inline pre-40.
DataflowStream decode_dataflow(const std::vector<const Event *> &step_evs,
                               const std::vector<const Event *> &edge_evs) {
    DataflowStream df;
    if (!step_evs.empty()) {
        // Steps arrive in file order, which the deterministic producer emits
        // ascending — but a stitched or resumed stream need not, and
        // asmspy_df_annotate's cursor walk is only correct over ascending, grouped
        // records. Sort by step, and say so if the file was not already.
        struct Step {
            uint32_t step;
            uint64_t off;
            uint64_t
                rbase; // 37: region base off is relative to; 0 = not stated
            std::string disasm;
            std::vector<ValRec> ops;
        };
        std::vector<Step> steps;
        steps.reserve(step_evs.size());
        uint32_t prev = 0;
        bool first = true;
        for (const Event *ep : step_evs) {
            const Event &e = *ep;
            Step st{};
            auto it = e.body.find("step");
            if (it == e.body.end() || !it->is_number())
                continue;
            st.step = it->get<uint32_t>();
            get(e.body, "off", st.off);
            get(e.body, "rbase", st.rbase); // 37: 0 when the wire omitted it
            if (st.rbase)
                df.rbase_present = true;
            get(e.body, "disasm", st.disasm);
            auto ops = e.body.find("ops");
            if (ops != e.body.end() && ops->is_array())
                for (const auto &o : *ops) {
                    ValRec v = decode_op(o);
                    v.step = st.step;
                    st.ops.push_back(v);
                }
            if (!first && st.step < prev)
                df.recs_grouped = false;
            prev = st.step;
            first = false;
            steps.push_back(std::move(st));
        }
        std::stable_sort(
            steps.begin(), steps.end(),
            [](const Step &a, const Step &b) { return a.step < b.step; });
        for (const Step &st : steps)
            df.nsteps = std::max(df.nsteps, st.step + 1);
        df.insn_off.assign(df.nsteps, 0);
        df.insn_rbase.assign(df.nsteps, 0); // 37, parallel to insn_off
        df.disasm.assign(df.nsteps, std::string());
        std::vector<char> seen(df.nsteps, 0);
        for (const Step &st : steps) {
            df.insn_off[st.step] = st.off;
            df.insn_rbase[st.step] = st.rbase;
            if (!st.disasm.empty())
                df.disasm[st.step] = st.disasm;
            seen[st.step] = 1;
            for (const ValRec &v : st.ops)
                df.recs.push_back(v);
        }
        // A gap in the step indices is a dropped step, not a step at offset 0:
        // count it so the banner can say the stream is incomplete.
        for (char c : seen)
            if (!c)
                df.steps_missing++;
        df.step_present = std::move(seen);
    }
    for (const Event *ep : edge_evs) {
        const Event &e = *ep;
        dt_edge edge{};
        auto f = e.body.find("from");
        auto t = e.body.find("to");
        if (f == e.body.end() || t == e.body.end() || !f->is_number() ||
            !t->is_number())
            continue;
        edge.from_step = f->get<uint32_t>();
        edge.to_step = t->get<uint32_t>();
        df.edges.push_back(edge);
        auto loc = e.body.find("loc");
        df.edge_loc.push_back(loc != e.body.end() && loc->is_object()
                                  ? decode_op(*loc)
                                  : ValRec{});
    }
    return df;
}

} // namespace

std::string recording_id(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

Streams decode_streams(const Recording &r) {
    Streams s;
    s.arch = r.arch;
    s.backend = r.provenance.backend;
    s.statistical = r.statistical();
    s.redacted = r.provenance.redacted;
    s.torn = r.torn;
    s.truncated = r.truncated();
    s.lost = r.drops_lost;
    s.throttled = r.drops_throttled;
    s.id = recording_id(r.path);
    s.code_present = r.code.present;
    s.code_sha = r.code.sha256;
    s.code_name = r.code.name;
    s.code_len = r.code.len;
    s.has_steps_total = r.has_steps_total;
    s.steps_total = r.steps_total;

    // --- trace ------------------------------------------------------------
    if (const auto *ev = kind(r, "trace")) {
        for (const Event &e : *ev) {
            note_basis(s.trace, e.body, "trace");
            uint64_t off = 0;
            auto it = e.body.find("off");
            if (it == e.body.end() || !it->is_number())
                continue; // an offset-less trace event places nothing
            off = it->get<uint64_t>();
            s.trace.insns.push_back(off);
            std::string dis;
            get(e.body, "disasm", dis);
            if (!dis.empty())
                s.trace.disasm.emplace(off, dis);
        }
    }
    if (const auto *ev = kind(r, "coverage")) {
        std::set<uint64_t> blocks(s.trace.blocks.begin(), s.trace.blocks.end());
        for (const Event &e : *ev) {
            note_basis(s.trace, e.body, "coverage");
            auto it = e.body.find("blocks");
            if (it != e.body.end() && it->is_array())
                for (const auto &b : *it)
                    if (b.is_number())
                        blocks.insert(b.get<uint64_t>());
            uint64_t bt = 0, it_ = 0;
            get(e.body, "blocks_total", bt);
            get(e.body, "insns_total", it_);
            s.trace.blocks_total = std::max(s.trace.blocks_total, bt);
            s.trace.insns_total = std::max(s.trace.insns_total, it_);
            bool trunc = false;
            get(e.body, "truncated", trunc);
            if (trunc) {
                s.trace.truncated = true;
                s.truncated = true;
            }
        }
        s.trace.blocks.assign(blocks.begin(), blocks.end());
    }
    if (r.truncated())
        s.trace.truncated = s.trace.truncated || s.trace.present();

    // --- dataflow ---------------------------------------------------------
    // Segment-aware (40): Streams::df is the LATEST invocation pass, so a
    // continuous capture's passes never conflate into one fake-monotonic run
    // (35 T1). A one-shot recording resolves to its single pass — byte-identical
    // to the pre-40 flat decode. Use build_segmented_dataflow to reach an earlier
    // pass; this mirrors build_step_index over the register ring.
    {
        SegmentedDataflow seg = build_segmented_dataflow(r);
        if (!seg.passes.empty())
            s.df = std::move(seg.passes[seg.latest()]);
    }

    // --- survey (statistical) --------------------------------------------
    if (const auto *ev = kind(r, "survey")) {
        for (const Event &e : *ev) {
            get(e.body, "samples", s.survey_samples);
            get(e.body, "lost", s.survey_lost);
            auto edges = e.body.find("edges");
            if (edges == e.body.end() || !edges->is_array())
                continue;
            for (const auto &x : *edges) {
                SurveyEdge se;
                get(x, "from_addr", se.from);
                get(x, "to_addr", se.to);
                get(x, "count", se.count);
                get(x, "mispred", se.mispred);
                s.survey.push_back(se);
            }
        }
    }

    // --- blame (backward attribution, 33 R6 T1) --------------------------
    if (const auto *ev = kind(r, "blame")) {
        for (const Event &e : *ev) {
            BlameAttr b;
            get(e.body, "step", b.step);
            get(e.body, "off", b.off);
            get(e.body, "born_untraced", b.born_untraced);
            auto loc = e.body.find("loc");
            if (loc != e.body.end() && loc->is_object()) {
                b.loc = decode_op(*loc);
                b.has_loc = true;
            }
            auto cone = e.body.find("cone");
            if (cone != e.body.end() && cone->is_array()) {
                for (const auto &c : *cone) {
                    uint32_t st = 0;
                    get(c, "step", st);
                    b.cone.push_back(st);
                }
            }
            s.blame.push_back(std::move(b));
        }
    }

    // --- statediff (step-to-step register delta, 33 R6 T2) ---------------
    if (const auto *ev = kind(r, "statediff")) {
        for (const Event &e : *ev) {
            StateDelta d;
            get(e.body, "step", d.step);
            get(e.body, "computed", d.computed);
            auto ch = e.body.find("changed");
            if (ch != e.body.end() && ch->is_object()) {
                for (auto it = ch->begin(); it != ch->end(); ++it) {
                    if (it->is_number_integer() || it->is_number_unsigned())
                        d.changed[it.key()] = it->get<uint64_t>();
                }
            }
            s.statediff.push_back(std::move(d));
        }
    }
    return s;
}

SegmentedDataflow build_segmented_dataflow(const Recording &r) {
    SegmentedDataflow seg;

    // df_step + df_edge event pointers in stream order (by_kind preserves append
    // order, so they are seq-ascending), and the continuous-capture pass
    // delimiters (35): one df_invocation marker emitted before each pass's df_step
    // block. A one-shot recording has none.
    std::vector<const Event *> steps, edges;
    if (auto it = r.by_kind.find("df_step"); it != r.by_kind.end())
        for (const Event &e : it->second)
            steps.push_back(&e);
    if (auto it = r.by_kind.find("df_edge"); it != r.by_kind.end())
        for (const Event &e : it->second)
            edges.push_back(&e);
    std::vector<const Event *> marks;
    if (auto it = r.by_kind.find("df_invocation"); it != r.by_kind.end())
        for (const Event &e : it->second)
            marks.push_back(&e);

    if (marks.empty()) {
        // One-shot (or any non-continuous recording): a single pass over the whole
        // df list — the pre-40 flat decode. An empty df list yields no pass, so
        // decode_streams leaves Streams::df absent exactly as before.
        if (!steps.empty() || !edges.empty())
            seg.passes.push_back(decode_dataflow(steps, edges));
        return seg;
    }

    // Segment: each pass owns the df events whose stream position (seq) falls at or
    // after its marker and before the next. Each pass restarts at step 0. This is
    // the same seq-window bucketing build_segmented_step_index applies to the
    // register ring (stepindex.cpp).
    std::sort(marks.begin(), marks.end(),
              [](const Event *a, const Event *b) { return a->seq < b->seq; });
    for (size_t p = 0; p < marks.size(); p++) {
        uint64_t lo = marks[p]->seq;
        uint64_t hi = (p + 1 < marks.size()) ? marks[p + 1]->seq
                                             : ~static_cast<uint64_t>(0);
        std::vector<const Event *> sb, eb;
        for (const Event *e : steps)
            if (e->seq >= lo && e->seq < hi)
                sb.push_back(e);
        for (const Event *e : edges)
            if (e->seq >= lo && e->seq < hi)
                eb.push_back(e);
        seg.passes.push_back(decode_dataflow(sb, eb));
    }
    return seg;
}

} // namespace asmdesk
