// annex.h — the lane inspector's cross-feed join (05-loom-day-one.md T4).
//
// Click a lane header and the annex answers: what did the OTHER recordings in
// this workspace record about this place? Watch hits, taint hits, source-map
// rows and IBS survey edges are joined by place — not merged into the fabric,
// which stays exactly what one exact producer measured.
//
// CORROBORATION, NEVER CONTRADICTION. The verdict enum has exactly two
// enumerators and there is a static_assert-style test pinning that. The reason
// is not politeness, it is what the evidence supports:
//
//  - A statistical feed's SILENCE proves nothing. A timebase-free histogram
//    that never sampled an address is not evidence the address was never
//    touched, so "the survey disagrees" is not a conclusion available here.
//  - An exact companion that recorded something the fabric did not is still
//    "unconfirmed", not "conflicts": two exact producers with different
//    windows, scopes or address bases will honestly disagree about coverage
//    without either being wrong.
//
// So every entry reads either "corroborates" or "recorded by <producer> —
// unconfirmed here", and no output of this file ever contains the word
// "contradict".
//
// ENGINE-FREE (D4). This includes asmtest_taint.h and asmtest_trace.h (both
// pure). It must NEVER include cli/asmspy.h — that header pulls
// asmtest_ptrace.h, and the render-only viewer links no ptrace engine. Watch
// hits reach the viewer only inside recordings (D9), so their fields are
// MIRRORED here rather than shared.
#ifndef ASMDESK_LOOM_ANNEX_H
#define ASMDESK_LOOM_ANNEX_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "doc/workspace.h"
#include "loom/fabric.h"

namespace asmdesk {

enum class annex_kind { watch_hit, taint_hit, srcmap_row, ibs_edge };
// Deliberately closed. See the header comment.
enum class annex_verdict { corroborates, unconfirmed };

const char *annex_kind_name(annex_kind k);

// The two join spaces. `data` joins on bytes (a memory band's range); `code`
// joins on the step column / the routine's code range. Every entry declares
// which one it used, because "same address" and "same instruction" are
// different claims and conflating them is how a lane annex starts lying.
enum class annex_space { data, code, value_only };

struct annex_entry_t {
    annex_kind kind = annex_kind::watch_hit;
    annex_verdict verdict = annex_verdict::unconfirmed;
    annex_space space = annex_space::data;
    bool exact = false;
    std::string producer;   // the companion recording's backend
    std::string provenance; // chip: "rec-B / ptrace-region / exact"
    uint64_t addr = 0;
    std::string text;
    // Statistical entries carry their drop accounting into the chip: a survey
    // that lost samples must not look like one that did not.
    uint64_t lost = 0;
    bool throttled = false;
};

// "corroborates" | "recorded by <producer> — unconfirmed here"
std::string annex_verdict_text(const annex_entry_t &e);

// Join every recording in `ws` against lane `lane` of `f`. `self` is the index
// of the recording the fabric itself came from (-1 when it came from a live or
// fork feed); it is skipped, since a recording cannot corroborate itself.
size_t loom_annex_join(const Workspace &ws, const loom_fabric_t &f,
                       uint32_t lane, int self,
                       std::vector<annex_entry_t> *out);

// Deterministic dump — the test surface.
std::string loom_annex_dump(const std::vector<annex_entry_t> &rows);

} // namespace asmdesk
#endif // ASMDESK_LOOM_ANNEX_H
