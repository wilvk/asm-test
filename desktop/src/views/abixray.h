// abixray.h — the ABI x-ray (09-teaching-producers.md T4).
//
// The classroom flagship: one call's argument marshalling under System V and
// Microsoft x64, side by side. The two panes ARE two register scrubbers (T3):
// this composes dt_scrubber_build over the SysV and Win64 halves of a PAIRED
// recording (tools/asmtrace_record.c's abixray-<routine>-{sysv,win64}) at ONE
// locked playhead, and an authored walkthrough (06's `note` stops, carried in
// the SysV/reference leg) drives that playhead — advancing a stop seeks BOTH
// panes together. The per-step register deltas do the "animation": no bespoke
// graphics beyond the scrubber deck.
//
// The value the x-ray adds over two loose scrubbers is the CROSS-PANE contrast —
// at each step, which registers hold DIFFERENT values between the conventions.
// That set is the marshalling rule made mechanical (a0 in rdi on the left, rcx
// on the right), and it is what the narration points at.
//
// The builder is pure — no ImGui, no I/O, no engine (D4) — so every rule below
// is a headless assertion; abixray_draw.cpp renders the built model.
//
// Fidelity (D7), refused rather than papered over:
//   - A pane has NO regstate producer -> present==false, the placard names the
//     --steps producer and deep-links the docs (the scrubber's absent case).
//   - The two panes are NOT step-aligned (different lengths) -> aligned==false
//     and a banner says so: the lock is a claim about a shared step space, and
//     if that space does not exist the view will not pretend it does.
//   - A pane DROPPED this step (torn ring) -> that pane renders UNKNOWN, and the
//     banner names it (the scrubber's torn case, surfaced per pane).
//   - A stop anchored past the recorded window is REFUSED, never clamped: the
//     playhead stays put and stop_beyond_window is set (walkthrough.h's rule).
#ifndef ASMDESK_VIEWS_ABIXRAY_H
#define ASMDESK_VIEWS_ABIXRAY_H

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/stepindex.h"
#include "views/scrubber.h"
#include "walkthrough.h"

namespace asmdesk {

// One register row across BOTH panes at the locked playhead. `differs` is the
// CROSS-PANE contrast (SysV value != Win64 value) — the marshalling difference
// made mechanical. `sysv_changed`/`win64_changed` are each pane's own
// vs-previous-held-step highlight (the scrubber's diff). `sysv_known`/
// `win64_known` are false when that pane is torn here: the value is UNKNOWN,
// never rendered as zero.
struct dt_abixray_row {
    std::string name;
    uint64_t sysv = 0;
    uint64_t win64 = 0;
    bool sysv_known = false;
    bool win64_known = false;
    bool differs = false;
    bool sysv_changed = false;
    bool win64_changed = false;
};

struct dt_abixray {
    // present==false: at least one pane has no regstate producer. `message` is
    // the placard and `docs` the deep-link; the deck is empty.
    bool present = false;
    std::string message;
    std::string docs;

    // Fixed pane identities (the two calling conventions).
    std::string sysv_label = "System V (SysV)";
    std::string win64_label = "Microsoft x64 (Win64)";
    std::string desc;   // the state-descriptor id both panes render from
    std::string banner; // alignment / torn fidelity placard (empty when clean)

    bool aligned = false; // the two panes share one step space [0, total)
    uint64_t total = 0;   // that shared step count

    // The walkthrough rail (decoded from the SysV/reference recording).
    std::string title; // the rail's intro (the SysV leg's leading note)
    int stop_cur = -1; // 0-based current stop, -1 when the recording has none
    int stop_count = 0;
    bool has_stop = false;
    int stop_ordinal = 0;  // 1-based, file order
    long stop_anchor = -1; // the stop's step, or -1 when unanchored
    std::string stop_title;
    std::string stop_body;
    bool stop_beyond_window = false; // the stop points past the recorded window

    // The single locked playhead, and the two REUSED scrubber decks built at it
    // (this is what "the two panes ARE two scrubbers" means mechanically).
    uint64_t playhead = 0;
    dt_scrubber sysv;  // == dt_scrubber_build(sysv_index, playhead)
    dt_scrubber win64; // == dt_scrubber_build(win64_index, playhead)

    // The cross-pane register deck (descriptor order) and the differing names.
    std::vector<dt_abixray_row> rows;
    std::vector<std::string> differ; // names whose SysV value != Win64 value
    size_t n_differ = 0;
};

// The playhead the rail puts the panes at for the walkthrough's CURRENT stop
// (walk.cur): its anchor if that stop is anchored and in-window, else the
// nearest such stop at-or-before it, else 0. An unanchored or beyond-window stop
// does NOT move the panes — it is a remark, and the reader keeps their place.
uint64_t dt_abixray_playhead(const wt_model &walk);

// Build the x-ray at the walkthrough's current stop — the rail-driven path the
// flagship animates through. `sysv`/`win64` are the paired recordings' step
// indices; `walk` is decoded from the SysV (reference) recording.
dt_abixray dt_abixray_build(const StepIndex &sysv, const StepIndex &win64,
                            const wt_model &walk);

// Build at an EXPLICIT playhead (the shared slider / `[` `]` step keys), with
// the stop fields still taken from walk.cur. dt_abixray_build is exactly this at
// the rail-derived playhead.
dt_abixray dt_abixray_seek(const StepIndex &sysv, const StepIndex &win64,
                           const wt_model &walk, uint64_t playhead);

std::string dt_abixray_dump(const dt_abixray &x);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_ABIXRAY_H
