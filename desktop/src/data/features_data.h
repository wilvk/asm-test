// features_data.h — the backend-completeness data readers
// (docs/internal/gui/02-exporters-and-readers.md T5).
//
// Three committed producers already emit everything the completeness view
// needs, so this is a reader library and one table view, not new probes:
//
//   tools/asmfeatures.c        the live capability sweep -> {"features":[...]}
//   scripts/bench-report.sh    merges it into a box record and the full report
//   benchmarks/boxes/<box>/    the committed per-box features.json
//
// Two properties are load-bearing and are why the optionals below are optionals:
//
//   - "NOT MEASURED" IS NOT "MEASURED ZERO". asmfeatures prints JSON null for
//     complete / trace_insns / insns_truth when it did not measure them, and
//     bench-report.sh's substitute probe row OMITS those keys entirely. Both
//     mean the same thing and both map to nullopt; a reader that defaulted them
//     to 0/false would turn "we never ran this" into "it captured nothing",
//     which is the exact inversion the completeness matrix exists to prevent.
//   - `skip_reason` IS THE ANSWER for an unavailable backend, and it is carried
//     verbatim from here to the screen. It is a MEASURED string (the engine's
//     own reason), never a paraphrase, so nothing between the probe and the
//     user is allowed to shorten it.
//
// Engine-free (D4): only nlohmann/json and the standard library, so these link
// into asmtest-viewer.
#ifndef ASMDESK_DATA_FEATURES_DATA_H
#define ASMDESK_DATA_FEATURES_DATA_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace asmdesk::data {

// One asmfeatures row() emission, with its keys verbatim
// (tools/asmfeatures.c: tier, backend, arch, scope, available, skip_reason,
// fidelity, complete, trace_insns, insns_truth, and the optional note).
struct FeatureRow {
    std::string tier, backend, arch, scope;
    bool available = false;
    std::string skip_reason;      // "" when available; VERBATIM otherwise
    std::string fidelity;         // "virtual-exact" | "native" | "n/a" | ""
    std::optional<bool> complete; // null / absent == not measured
    std::optional<std::int64_t> trace_insns, insns_truth;
    std::optional<std::string> note; // a workload label; absent on most rows
};

// The 12 `system` keys scripts/bench-report.sh writes.
struct BoxSystem {
    std::string box_id, os, os_version, arch, cpu, uarch, vendor, cc,
        asmtest_version, commit, timestamp;
    bool virtualized = false;
};

struct FeaturesDoc {
    std::optional<BoxSystem> system; // absent for a bare asmfeatures stdout
    std::vector<FeatureRow> features;
    std::string source; // which envelope shape this came from, for the chrome
};

// Accepts all three committed envelopes, dispatching on the keys present:
//   {"features":[...]}                            bare asmfeatures stdout
//   {"system":{...},"features":[...]}              benchmarks/boxes/<box>/
//   {"schema":"asmtest-bench-report/v1", ...}      the full merged report
// Unknown fields are ignored. Throws std::runtime_error when there is no
// recognisable features array — an unreadable file is an error, not an empty
// table that looks like a box with no backends.
FeaturesDoc load_features(const nlohmann::json &doc);
FeaturesDoc
load_features_file(const std::string &path); // path + reason in what()

} // namespace asmdesk::data
#endif // ASMDESK_DATA_FEATURES_DATA_H
