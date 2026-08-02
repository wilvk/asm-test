// mnemonic.h — "what kind of instruction is this", engine-free and ImGui-free
// (docs/internal/gui/54-3d-catalog-phase0-plumbing.md T4).
//
// space/ is already the engine-free, ImGui-free island the terrain builder
// lives in (D4), which is where a mnemonic classifier belongs — the existing
// mnemonic vocabulary (ui/asm_language.h, the Author editor's syntax
// highlighter) is double-coupled: it pulls in TextEditor.h (the addon) and
// asmtest_assemble.h (an engine header) and cannot be depended on from here.
//
// So this file COPIES the word-lists rather than including that header — the
// tree already has the precedent (kindHue in scene3d/shaders/embedded.h
// deliberately duplicates region_style()) — and test_mnemonic.cpp PINS the
// copy against the real one: every keyword `ui/asm_language.cpp` generates for
// x86/arm64 must classify to something other than Unknown here, so a mnemonic
// added there and not copied here fails a named test rather than silently
// under-classifying.
#ifndef ASMDESK_SPACE_MNEMONIC_H
#define ASMDESK_SPACE_MNEMONIC_H

#include <cstdint>
#include <string_view>

namespace asmdesk::space {

// A coarse instruction-kind bucket — coarse by design, because the consumer
// (a 3D terrain layer, L3 of 56-fidelity-and-module-layers.md) tints a cell,
// not a disassembly pane. Unknown is the ABSTAIN value: returned for a guest
// this file has no vocabulary for, an empty token, or a real token this
// file's copied lists do not contain. A classifier that guesses is worse than
// one that abstains, because the tint would read as a measurement.
enum class OpClass : uint8_t {
    Unknown = 0,
    Move,          // data movement: mov/ldr/str/push/pop/lea/adr, conditional
                   // moves and compare-and-swap (the comparison GATES a move;
                   // it does not become the instruction's identity)
    IntArith,      // integer arithmetic: add/sub/mul/div/inc/neg and friends
    Logic,         // bitwise/shift/bit-manipulation: and/or/xor/not/shl/bt*
    CompareBranch, // anything that sets flags from a comparison, or that
                   // transfers control (conditional or not): cmp/test/jmp/
                   // call/ret/b/bl/fcmp/ucomiss — the family a control-flow
                   // or misprediction layer (L11/L14) reads as one group
    ScalarFloat,   // scalar (one-lane) float arithmetic: addss/fadd/fmul
    VectorSIMD,    // packed/vector operations: addps/pxor/ld1, AVX v*
    System,        // privileged, barrier, or processor-state instructions:
                   // syscall/cpuid/mfence/svc/dmb/nop
};

const char *op_class_name(OpClass);

// Whether an instruction touches MEMORY is never derived here — that comes
// from the recording's own ValRec.space ("abs"/"off"), never from the
// mnemonic text. Saying so here because it is the single most tempting wrong
// inference to add to this file later.
struct MnemonicClass {
    OpClass cls = OpClass::Unknown;
    // Some real mnemonics legitimately sit between two classes — a GPR<->
    // vector-register-file move (`movd`/`movq`/ARM's `umov`/`ins`/`dup`) or a
    // scalar float<->integer conversion (`cvtsi2sd`, ARM's `fcvt`/`scvtf`).
    // `cls` carries this file's best single answer; `ambiguous` says a reader
    // that cares should not treat it as a clean measurement — matching L3's
    // own requirement to MARK the cell rather than silently bucket it.
    bool ambiguous = false;

    friend bool operator==(const MnemonicClass &a, const MnemonicClass &b) {
        return a.cls == b.cls && a.ambiguous == b.ambiguous;
    }
};

// `guest` selects the vocabulary — "x86" or "arm64" (the only two guests this
// file has copied lists for; ARM32/RISC-V are not covered). An unrecognised
// guest, an empty token, or a token in neither list all return
// `{Unknown, false}`. Matching is case-insensitive (the source highlighter's
// own `caseSensitive = false`), and `first_token` should be exactly the
// mnemonic — the first whitespace-delimited word of a disassembled line, no
// operands.
MnemonicClass mnemonic_class(std::string_view first_token,
                             std::string_view guest);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_MNEMONIC_H
