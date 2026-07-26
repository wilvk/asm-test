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

StepIndex build_step_index(const Recording &r) {
    StepIndex idx;

    auto it = r.by_kind.find("regstate");
    if (it == r.by_kind.end() || it->second.empty())
        return idx; // absent: the ring was disarmed (the normal case)

    // Held events are steps [dropped, dropped + count); the footer's drop count
    // is the evicted prefix width (asmtrace-schema.md, `regstate` D7 rule).
    idx.dropped = r.drops_lost;
    idx.first_step = r.drops_lost;
    idx.truncated = idx.dropped > 0;

    const std::vector<Event> &events = it->second;
    for (size_t i = 0; i < events.size(); i++) {
        const nlohmann::json &body = events[i].body;
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

} // namespace asmdesk
