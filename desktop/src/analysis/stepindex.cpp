// stepindex.cpp — the pure `regstate` index builder (stepindex.h). No ImGui,
// no engine, no I/O: only the document model and nlohmann/json.
#include "analysis/stepindex.h"

#include <algorithm>

namespace asmdesk {

const std::vector<std::string> &stepindex_reg_order() {
    static const std::vector<std::string> order = {
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8",
        "r9",  "r10", "r11", "r12", "r13", "r14", "r15", "rip", "rflags"};
    return order;
}

const RegField *RegFile::find(const std::string &name) const {
    for (const RegField &f : fields)
        if (f.name == name)
            return &f;
    return nullptr;
}

const RegFile *StepIndex::at_step(uint64_t step) const {
    if (step < first_step)
        return nullptr;
    uint64_t i = step - first_step;
    if (i >= entries.size())
        return nullptr;
    return &entries[i];
}

const RegFile *StepIndex::at_index(size_t i) const {
    return i < entries.size() ? &entries[i] : nullptr;
}

namespace {

// Read the `values` object of one `regstate` event into fields, in descriptor
// order, then any extra keys (a forward-compat producer's) sorted after. A
// value that is not an integer is skipped — the schema serialises every field
// as a decimal u64, so a non-integer is a malformed event, not a datum.
std::vector<RegField> read_values(const nlohmann::json &values) {
    std::vector<RegField> out;
    if (!values.is_object())
        return out;
    for (const std::string &name : stepindex_reg_order()) {
        auto it = values.find(name);
        if (it == values.end() || !it->is_number_integer())
            continue;
        RegField f;
        f.name = name;
        f.value = it->get<uint64_t>();
        out.push_back(f);
    }
    // Any key the descriptor order does not name (e.g. a future wide register),
    // appended in a deterministic order so a golden stays byte-stable.
    std::vector<std::string> extras;
    const std::vector<std::string> &order = stepindex_reg_order();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!it.value().is_number_integer())
            continue;
        if (std::find(order.begin(), order.end(), it.key()) == order.end())
            extras.push_back(it.key());
    }
    std::sort(extras.begin(), extras.end());
    for (const std::string &name : extras) {
        RegField f;
        f.name = name;
        f.value = values.at(name).get<uint64_t>();
        out.push_back(f);
    }
    return out;
}

} // namespace

// Build one StepIndex from a list of `regstate` event pointers (already in stream
// order) with the given drop/total/truncation facts. The pure core shared by the
// whole-recording build and the per-pass segmentation (35 T3).
static StepIndex build_index_core(const std::vector<const Event *> &regs,
                                  uint64_t dropped, bool truncated,
                                  uint64_t footer_total) {
    StepIndex idx;
    // Held events are steps [dropped, dropped + count); the footer's drop count
    // is the evicted prefix width (asmtrace-schema.md, `regstate` D7 rule).
    idx.dropped = dropped;
    idx.first_step = dropped;
    // An absent ring (no held regstate) stays fully absent: no held steps, no
    // total, no truncation — total_steps() is 0, exactly as the pre-35 build. A
    // footer/marker total only describes a ring that actually held steps.
    idx.truncated = regs.empty() ? false : truncated;
    // The footer's / marker's total (28 R1 T2): for a tail-drop live ring
    // (dropped == 0 but steps ran past the cap) it is the only source of the true
    // total.
    idx.footer_total = regs.empty() ? 0 : footer_total;

    for (size_t i = 0; i < regs.size(); i++) {
        const nlohmann::json &body = regs[i]->body;
        if (idx.desc.empty()) {
            auto d = body.find("desc");
            if (d != body.end() && d->is_string())
                idx.desc = d->get<std::string>();
        }
        RegFile file;
        file.step = idx.first_step + i;
        file.has_prev = i > 0;
        auto values = body.find("values");
        if (values != body.end())
            file.fields = read_values(*values);
        // Per-field change highlight: diff against the previous HELD step. The
        // first held entry has no baseline (its predecessor is step 0's absence
        // or an evicted step), so nothing is marked changed there.
        if (i > 0) {
            const RegFile &prev = idx.entries.back();
            for (RegField &f : file.fields) {
                const RegField *pf = prev.find(f.name);
                f.changed = pf != nullptr && pf->value != f.value;
            }
        }
        idx.entries.push_back(std::move(file));
    }
    return idx;
}

SegmentedStepIndex build_segmented_step_index(const Recording &r) {
    SegmentedStepIndex seg;

    // The regstate ring events, in stream order (by_kind preserves append order,
    // so they are seq-ascending); pointers so a pass can own a slice by seq.
    std::vector<const Event *> regs;
    auto rit = r.by_kind.find("regstate");
    if (rit != r.by_kind.end())
        for (const Event &e : rit->second)
            regs.push_back(&e);

    // The continuous-capture pass delimiters (35): one emitted before each pass's
    // df_step block. A one-shot recording has none.
    std::vector<const Event *> marks;
    auto mit = r.by_kind.find("df_invocation");
    if (mit != r.by_kind.end())
        for (const Event &e : mit->second)
            marks.push_back(&e);

    if (marks.empty()) {
        // One-shot (or any non-continuous recording): a single pass over the whole
        // regstate list with the footer's drop/total facts — the pre-35 behaviour.
        seg.passes.push_back(
            build_index_core(regs, r.drops_lost, r.drops_lost > 0,
                             r.has_steps_total ? r.steps_total : 0));
        return seg;
    }

    // Segment: each pass owns the regstate events whose stream position (seq) falls
    // at or after its marker and before the next. Each pass restarts at step 0
    // (dropped = 0); its marker carries that pass's own step total + truncation.
    std::sort(marks.begin(), marks.end(),
              [](const Event *a, const Event *b) { return a->seq < b->seq; });
    for (size_t p = 0; p < marks.size(); p++) {
        uint64_t lo = marks[p]->seq;
        uint64_t hi = (p + 1 < marks.size()) ? marks[p + 1]->seq
                                             : ~static_cast<uint64_t>(0);
        std::vector<const Event *> bucket;
        for (const Event *e : regs)
            if (e->seq >= lo && e->seq < hi)
                bucket.push_back(e);
        const nlohmann::json &mb = marks[p]->body;
        uint64_t steps = 0;
        auto st = mb.find("steps");
        if (st != mb.end() && st->is_number_integer())
            steps = st->get<uint64_t>();
        bool trunc = false;
        auto tr = mb.find("truncated");
        if (tr != mb.end() && tr->is_boolean())
            trunc = tr->get<bool>();
        seg.passes.push_back(build_index_core(bucket, 0, trunc, steps));
    }
    return seg;
}

StepIndex build_step_index(const Recording &r) {
    // Segment-aware: a continuous recording resolves to its LATEST invocation (the
    // live default); a one-shot recording resolves to its single pass — the whole
    // regstate list, byte-identical to the pre-35 flat build. An absent ring yields
    // an absent index either way.
    SegmentedStepIndex seg = build_segmented_step_index(r);
    if (seg.passes.empty())
        return StepIndex{};
    return std::move(seg.passes[seg.latest()]);
}

} // namespace asmdesk
