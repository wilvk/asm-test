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
    if (const auto *ev = kind(r, "df_step")) {
        // Steps arrive in file order, which the deterministic producer emits
        // ascending — but a stitched or resumed stream need not, and
        // asmspy_df_annotate's cursor walk is only correct over ascending,
        // grouped records. Sort by step, and say so if the file was not already.
        struct Step {
            uint32_t step;
            uint64_t off;
            std::string disasm;
            std::vector<ValRec> ops;
        };
        std::vector<Step> steps;
        steps.reserve(ev->size());
        uint32_t prev = 0;
        bool first = true;
        for (const Event &e : *ev) {
            Step st{};
            auto it = e.body.find("step");
            if (it == e.body.end() || !it->is_number())
                continue;
            st.step = it->get<uint32_t>();
            get(e.body, "off", st.off);
            get(e.body, "disasm", st.disasm);
            auto ops = e.body.find("ops");
            if (ops != e.body.end() && ops->is_array())
                for (const auto &o : *ops) {
                    ValRec v = decode_op(o);
                    v.step = st.step;
                    st.ops.push_back(v);
                }
            if (!first && st.step < prev)
                s.df.recs_grouped = false;
            prev = st.step;
            first = false;
            steps.push_back(std::move(st));
        }
        std::stable_sort(
            steps.begin(), steps.end(),
            [](const Step &a, const Step &b) { return a.step < b.step; });
        for (const Step &st : steps)
            s.df.nsteps = std::max(s.df.nsteps, st.step + 1);
        s.df.insn_off.assign(s.df.nsteps, 0);
        s.df.disasm.assign(s.df.nsteps, std::string());
        std::vector<char> seen(s.df.nsteps, 0);
        for (const Step &st : steps) {
            s.df.insn_off[st.step] = st.off;
            if (!st.disasm.empty())
                s.df.disasm[st.step] = st.disasm;
            seen[st.step] = 1;
            for (const ValRec &v : st.ops)
                s.df.recs.push_back(v);
        }
        // A gap in the step indices is a dropped step, not a step at offset 0:
        // count it so the banner can say the stream is incomplete.
        for (char c : seen)
            if (!c)
                s.df.steps_missing++;
        s.df.step_present = std::move(seen);
    }
    if (const auto *ev = kind(r, "df_edge")) {
        for (const Event &e : *ev) {
            dt_edge edge{};
            auto f = e.body.find("from");
            auto t = e.body.find("to");
            if (f == e.body.end() || t == e.body.end() || !f->is_number() ||
                !t->is_number())
                continue;
            edge.from_step = f->get<uint32_t>();
            edge.to_step = t->get<uint32_t>();
            s.df.edges.push_back(edge);
            auto loc = e.body.find("loc");
            s.df.edge_loc.push_back(loc != e.body.end() && loc->is_object()
                                        ? decode_op(*loc)
                                        : ValRec{});
        }
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
    return s;
}

} // namespace asmdesk
