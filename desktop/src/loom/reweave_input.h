// reweave_input.h — the engine-free reweave data source + identity latch
// (docs/internal/gui/42-loom-reweave-consumption.md T1/T2).
//
// Reweave is intrinsically author-time: `loom_take_run_from_step` re-executes
// the emulator from entry, so it needs the routine's raw bytes AND its entry
// arguments — and a recording carries neither (Streams exposes only the code
// IDENTITY, and no schema event records entry args at all). The Author door's
// last run is the one place both live, so `ReweaveSource` is a plain, engine-
// free view of ShellState::author, built at the two draw_loom call sites
// (shell.cpp) and threaded through with no engine header crossing the seam.
#ifndef ASMDESK_LOOM_REWEAVE_INPUT_H
#define ASMDESK_LOOM_REWEAVE_INPUT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace asmdesk {

// `code` is null (and `code_sha` empty) whenever there is nothing eligible to
// reweave with: no Author run yet, an arch the resume seam does not support
// (x86-64 only — the caller withholds `code_sha` for any other arch), or an
// unsuccessful assemble. The struct never owns its buffers; it borrows from
// AuthorState for exactly one frame.
struct ReweaveSource {
    const uint8_t *code = nullptr;
    size_t code_len = 0;
    const long *args = nullptr;
    int nargs = 0;
    std::string code_sha; // sha256 of `code`, precomputed once per Author run
};

// The identity latch (D7): may `streams_code_sha` (the ACTIVE recording's code
// identity) be reweaved using `author_code_sha` (the Author state's last
// eligible run)? False whenever either is absent or they name different
// routines — running the wrong bytes would paint a fabricated counterfactual
// as this recording's, the exact failure the app is built to refuse. `nargs`
// is bounds-checked against the fixed 6-slot argument array (AuthorState::args)
// rather than required positive: a legitimate routine may take zero arguments.
inline bool loom_reweave_available(const std::string &streams_code_sha,
                                   const std::string &author_code_sha,
                                   int nargs) {
    return nargs >= 0 && nargs <= 6 && !streams_code_sha.empty() &&
          streams_code_sha == author_code_sha;
}

} // namespace asmdesk
#endif // ASMDESK_LOOM_REWEAVE_INPUT_H
