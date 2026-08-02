// opcode_terrain.h — the opcode-class code terrain
// (56-fidelity-and-module-layers.md T4): a spatial map of *what kind of
// work* each code region does (move, int-arith, logic, compare-branch,
// scalar-float, vector-SIMD, system), rather than a linear disassembly
// scroll.
//
// Pure and engine-free (D4): space/terrain.h + space/mnemonic.h + the
// document model — no GL, no ImGui.
#ifndef ASMDESK_SPACE_OPCODE_TERRAIN_H
#define ASMDESK_SPACE_OPCODE_TERRAIN_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/mnemonic.h"
#include "space/terrain.h"

namespace asmdesk::space {

// One code cell's opcode-class read. `dominant` is Unknown when the cell has
// no classifiable offset at all (an empty disasm string, or a token neither
// vocabulary lists) — abstain, never a coerced guess. `purity` is the
// dominant class's share of this cell's DISTINCT offsets (never weighted by
// execution count — this is "what kinds of instructions live here", not
// "which ran the most", T3's canopy already answers that question).
// `ambiguous` is true when ANY offset in the cell classified with
// MnemonicClass::ambiguous set (a GPR<->vector or float<->int boundary
// mnemonic) — ambiguity WIDENS this cell's uncertainty even if the dominant
// class itself is confident.
struct CellOpcode {
    uint32_t cell = 0;
    OpClass dominant = OpClass::Unknown;
    float purity = 0.0f;
    bool ambiguous = false;
    // The most-executed offset in this cell (by step count), for a drill-in
    // that wants to land somewhere meaningful rather than the cell's
    // geometric center — see the brief's own step 5.
    uint64_t hottest_off = 0;
    uint32_t distinct_offsets = 0; // the purity denominator, for tests
};

// Classifies every code cell in `model` using the recording's own recorded
// disasm text (D10) — NEVER a re-disassembly, since this island has no
// Capstone (D4). `guest` selects mnemonic_class's vocabulary ("x86" |
// "arm64"); an unrecognised or empty guest abstains every cell to Unknown,
// which is correct (not a bug to work around) for a guest this file has no
// words for. A cell with no recorded disasm text for any of its offsets is
// Unknown with distinct_offsets reflecting only the offsets that HAD text.
std::vector<CellOpcode> build_opcode_terrain(const TerrainModel &model,
                                             const Recording &rec,
                                             const std::string &guest);

// Recording::arch ("x86_64" | "aarch64" | ...) -> mnemonic_class's own
// guest vocabulary key ("x86" | "arm64"). An unmapped arch string passes
// through unchanged, so mnemonic_class's OWN abstain-on-unknown-guest rule
// is what handles it — this function never invents a guest.
std::string opcode_guest_from_arch(const std::string &arch);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_OPCODE_TERRAIN_H
