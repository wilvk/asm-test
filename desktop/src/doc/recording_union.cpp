// recording_union.cpp — see recording_union.h for the contract and why the
// merge mirrors the `--serve --record` tee's single-header shape.
#include "doc/recording_union.h"

#include <string>
#include <vector>

namespace asmdesk {

namespace {

// The trust ladder, weakest first. An unknown value ranks weakest: a trust the
// registry does not know cannot outrank one it does.
int trust_rank(const std::string &t) {
    if (t == "exact")
        return 3;
    if (t == "strong")
        return 2;
    if (t == "statistical")
        return 1;
    return 0; // "weak" and anything unknown
}

} // namespace

Recording merge_session_recordings(const std::vector<Recording> &done,
                                   const Recording *growing) {
    std::vector<const Recording *> parts;
    parts.reserve(done.size() + 1);
    for (const Recording &r : done)
        parts.push_back(&r);
    if (growing != nullptr)
        parts.push_back(growing);

    if (parts.empty())
        return Recording{};
    if (parts.size() == 1)
        return *parts.front();

    Recording u;
    // Identity from the FIRST part — the tee writes one header and keeps it.
    u.version = parts.front()->version;
    u.producer = parts.front()->producer;
    u.arch = parts.front()->arch;
    u.provenance = parts.front()->provenance;

    // provenance: weakest-part rules.
    for (size_t i = 1; i < parts.size(); i++) {
        const Provenance &p = parts[i]->provenance;
        if (p.backend != u.provenance.backend &&
            u.provenance.backend.find(p.backend) == std::string::npos)
            u.provenance.backend += "+" + p.backend;
        u.provenance.exact = u.provenance.exact && p.exact;
        if (trust_rank(p.trust) < trust_rank(u.provenance.trust))
            u.provenance.trust = p.trust;
        u.provenance.redacted = u.provenance.redacted || p.redacted;
    }

    // code identity survives only when every part hashed the SAME bytes.
    u.code = parts.front()->code;
    for (size_t i = 1; i < parts.size() && u.code.present; i++)
        if (!parts[i]->code.present ||
            parts[i]->code.sha256 != u.code.sha256)
            u.code = Code{};

    // Events: concatenate per part in order, reassigning seq by a running
    // offset so stream order is preserved across the session boundary.
    uint64_t offset = 0;
    for (const Recording *p : parts) {
        for (const auto &kv : p->by_kind) {
            std::vector<Event> &dst = u.by_kind[kv.first];
            for (const Event &e : kv.second) {
                Event copy = e;
                copy.seq = e.seq + offset;
                dst.push_back(std::move(copy));
            }
        }
        offset += p->next_seq;
        u.unknown_kinds += p->unknown_kinds;
    }
    u.next_seq = offset;

    // Fidelity: has_end is the LAST part's (a growing tail keeps the union
    // open); everything countable sums; everything shameful ORs.
    u.has_end = parts.back()->has_end;
    for (const Recording *p : parts) {
        u.torn = u.torn || p->torn;
        u.end_truncated = u.end_truncated || p->end_truncated;
        u.drops_lost += p->drops_lost;
        u.drops_throttled = u.drops_throttled || p->drops_throttled;
        u.declared_events += p->declared_events;
        if (p->has_steps_total) {
            u.has_steps_total = true;
            u.steps_total += p->steps_total;
        }
    }

    // skipped only when EVERY part skipped — a union where one capture landed
    // is not a refusal. The first skipped part names the reason.
    bool all_skipped = true;
    for (const Recording *p : parts)
        all_skipped = all_skipped && p->skipped;
    if (all_skipped) {
        u.skipped = true;
        u.skip_code = parts.front()->skip_code;
        u.skip_reason = parts.front()->skip_reason;
    }

    // An in-memory union has no file and is never dirty: it is derived state,
    // re-derivable from the session at any time.
    u.path.clear();
    u.dirty = false;
    return u;
}

} // namespace asmdesk
