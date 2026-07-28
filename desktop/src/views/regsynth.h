// regsynth.h — re-derive a per-step register history for an emulator-replayable
// recording (30 R3 T4, the Scrubber's "synthesize register history" action).
//
// Where a recording carries no `regstate` events but IS emulator-replayable
// (Author/emulator run, x86-64 guest, code bytes in a `codeimage` event —
// dt_scrubber_replayable), this re-runs the recorded code under the emulator's
// per-step register ring and builds the SAME StepIndex the `--steps` producer
// would, marked `synthesized` so the deck labels it a RE-DERIVATION, never the
// bytes the original run observed (D6).
//
// FULL BUILD ONLY: the real synthesiser links the emulator tier (emu.o), so this
// TU is compiled ONLY into asmtest-desktop (and its engine test), exactly like
// forks.cpp — asmtest-viewer never sees it and stays engine-free (D4). The header
// is a plain declaration (no engine header), so scrubber_draw.cpp can #include it
// in every tree and gate the CALL on ASMTEST_DESKTOP_CAN_AUTHOR.
#ifndef ASMDESK_VIEWS_REGSYNTH_H
#define ASMDESK_VIEWS_REGSYNTH_H

#include <string>

#include "analysis/stepindex.h"
#include "doc/recording.h"

namespace asmdesk {

// Re-derive `r`'s register history into *out (a StepIndex with `synthesized` set).
// Returns true on success; false with *err set on a non-replayable recording (no
// codeimage, wrong arch), a malformed codeimage, or a re-run that produced no
// steps. x86-64 guest only (the resume/ring tier's scope).
bool regsynth_synthesize(const Recording &r, StepIndex *out, std::string *err);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_REGSYNTH_H
