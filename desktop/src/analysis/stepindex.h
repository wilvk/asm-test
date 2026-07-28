// stepindex.h — index a recording's `regstate` events by step
// (docs/internal/gui/09-teaching-producers.md T3).
//
// The per-step register ring (09-T1) emits one `regstate` event per HELD
// pre-state, oldest first (asmtrace-schema.md, `regstate` descriptor). This
// builds an O(1) seek from a step index to the register file BEFORE that step's
// instruction — the single lookup the register time-travel scrubber (09-T3) and
// the Loom's now-column (05) both read. It is deliberately NOT a view: it has no
// ImGui and no engine dependency (D4), only the document model, so both a
// render-only viewer and a headless test can seek a register file.
//
// Truncation is data (D7). When the ring evicts the earliest steps, the held
// events are steps [dropped, dropped + count) — the `end` footer carries the
// evicted count in `drops.lost`. This index offsets the first held step by that
// count and exposes `dropped`, so a reader renders the missing prefix as a TORN
// edge rather than as step 0. The full step space is therefore [0, total_steps):
// [0, dropped) is torn (UNKNOWN, never zero) and [dropped, total_steps) is held.
#ifndef ASMDESK_ANALYSIS_STEPINDEX_H
#define ASMDESK_ANALYSIS_STEPINDEX_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"

namespace asmdesk {

// One register field of a held step's file: its name (from `values`, e.g.
// "rax"), its value, and whether it DIFFERS from the same-named field of the
// previous held step. `changed` is the per-step highlight the scrubber and the
// now-column both draw; it is false for the first held entry, which has no
// baseline to diff against (either it is genuinely step 0, or its true
// predecessor was evicted — the torn-edge case).
struct RegField {
    std::string name;
    uint64_t value = 0;
    bool changed = false;
};

// The register file BEFORE one held step's instruction.
struct RegFile {
    uint64_t step = 0; // ABSOLUTE step index (offset past any dropped prefix)
    bool has_prev = false; // a previous held step exists to diff against
    std::vector<RegField>
        fields; // fields the producer emitted, descriptor order
    const RegField *find(const std::string &name) const;
};

// The full register-file index of one recording's `regstate` events.
struct StepIndex {
    std::vector<RegFile> entries; // held pre-states, oldest first
    uint64_t first_step = 0; // = drops.lost; the absolute step of entries[0]
    uint64_t dropped = 0;    // steps the ring evicted before the first held one
    bool truncated =
        false;        // the ring evicted a prefix -> a torn edge precedes it
    std::string desc; // the state-descriptor id referenced (chrome)

    // The footer's `steps_total` (28 R1 T2), 0 when absent. A drop-OLDEST ring
    // reconstructs the total as dropped+count; a drop-NEWEST (tail-drop) live ring
    // passes dropped=0 yet ran past the cap, so ONLY this field supplies the true
    // total. total_steps() takes the larger, so a footer total never shrinks the
    // reconstructed one and the tail-drop case is no longer undercounted.
    uint64_t footer_total = 0;

    bool present() const { return !entries.empty(); }
    size_t count() const { return entries.size(); }

    // The full step space is [0, total_steps()).
    uint64_t total_steps() const {
        uint64_t reconstructed = dropped + entries.size();
        return footer_total > reconstructed ? footer_total : reconstructed;
    }

    // O(1) seek: the held file whose ABSOLUTE step == `step`, or nullptr when
    // that step was dropped (torn) or never recorded. Never returns a
    // neighbouring step's file — an UNKNOWN step is unknown, not the next one.
    const RegFile *at_step(uint64_t step) const;
    // O(1) seek by held ordinal (0 = oldest held).
    const RegFile *at_index(size_t i) const;
};

// Build the index from a loaded recording. Reads only its `regstate` events and
// its `end`-footer drop count; a recording with none yields an empty (absent)
// index, which is the normal case (the ring is opt-in, `--steps` defaults off).
StepIndex build_step_index(const Recording &r);

// The descriptor's field order (asmtrace-schema.md `regstate` descriptor):
// rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15 rip rflags.
// Exposed so a deck renders a stable order; a producer MAY emit a subset, and
// any extra `values` key outside this list is appended in sorted order.
const std::vector<std::string> &stepindex_reg_order();

} // namespace asmdesk
#endif // ASMDESK_ANALYSIS_STEPINDEX_H
