// capview.h — the capability panel's pure view-model
// (docs/internal/gui/06-doors-and-learning.md T6).
//
// One panel answering "what can this host do, and why not?" — built ENTIRELY
// from the library's own status APIs. The GUI never re-probes: it calls
// asmtest_trace_resolve / asmtest_hwtrace_status / asmtest_ibs_* once and
// renders exactly what they said.
//
// TWO UI LAWS, both testable here rather than in the painter:
//
//  1. **A greyed row always shows its machine reason, verbatim.** Not a
//     paraphrase, not "unavailable" — the measured `reason[160]` the library
//     computed, because that string is what tells a user whether to change a
//     sysctl, install a package, or buy a different CPU.
//  2. **Never auto-fall back across the native→virtual line.** When the
//     native-only cascade is empty the panel says the library returns EUNAVAIL
//     rather than silently downgrading, and offers the crossing as an explicit
//     choice.
//
// Pure: the C headers it includes are declarations and plain structs, and this
// TU calls none of their functions — the probing lives in the panel, which is
// full-build only. That is what lets the render-only viewer link the view-model
// (and show a LOADED RECORDING's provenance instead, since a viewer that
// probed would be reporting on the wrong machine).
#ifndef ASMDESK_CAPVIEW_H
#define ASMDESK_CAPVIEW_H

#include <string>
#include <vector>

extern "C" {
#include "asmtest_hwtrace.h"
#include "asmtest_trace_auto.h"
}

namespace asmdesk {

// One rendered row. `kind` groups the deck; `reason` is the machine string and
// is never rewritten.
enum class cap_kind { cascade, backend, ibs, emulator, refusal };

struct cap_row {
    cap_kind kind = cap_kind::backend;
    std::string label;
    std::string reason; // VERBATIM from the library; "" only when available
    std::string chip;   // "sampled, never exact" / "exact" / stage name
    int stage = 0;      // ASMTEST_HW_STAGE_*
    int code = 0;       // ASMTEST_HW_OK / _EUNAVAIL / _EPERM
    int fidelity = 0;   // asmtest_trace_fidelity_t
    int paranoid = 0;   // /proc/sys/kernel/perf_event_paranoid
    bool available = false;
    bool statistical = false;
    // The panel draws a rule under the last row above the native→virtual line.
    bool below_fidelity_line = false;
};

// Build the deck. Every argument is DATA the caller already probed — which is
// what makes this testable on a host with no PT, no LBR and no AMD silicon.
// `st` is indexed by asmtest_trace_backend_t (the four hardware backends).
std::vector<cap_row> capview_build(const asmtest_trace_choice_t *cascade,
                                   size_t n,
                                   const asmtest_hwtrace_status_t st[4],
                                   int ibs_avail, const char *ibs_substrate,
                                   const char *ibs_capture, bool native_only);

// Deterministic dump — the test surface.
std::string capview_dump(const std::vector<cap_row> &rows);

const char *cap_stage_name(int stage);
const char *cap_backend_name(int backend);
const char *cap_tier_name(int tier);
const char *cap_fidelity_name(int fidelity);

// Copy pinned verbatim by test_capview.cpp.
extern const char *const kCapNativeOnlyEmpty;
extern const char *const kCapFidelityLine;
extern const char *const kCapStatisticalChip;
extern const char *const kCapViewerNoProbe;

} // namespace asmdesk
#endif // ASMDESK_CAPVIEW_H
