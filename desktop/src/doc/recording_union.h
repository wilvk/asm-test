// recording_union.h — the SESSION-UNION Recording (live union weave).
//
// A live serve session keeps each capture as its own Recording (live/session.h:
// `done_` plus a growing `cur_`), and the 3D pane's per-capture weave is what
// made every fresh Start REPLACE the scene instead of adding to it. The union
// this builds is not a new artifact shape: `asmspy --serve --record=<f>` already
// tees every capture of a session into ONE loadable file (one header, however
// many engines — cli/asmspy.c's session sink), and File ▸ Open on that file is
// the long-standing way to see all substrates at once. merge_session_recordings
// produces the same document IN MEMORY, so the live tab can weave it directly.
//
// Honesty rules (D7) — the union may never claim more than its weakest part:
//   - provenance.exact ANDs; trust takes the WEAKEST rank present
//     (exact > strong > statistical > weak); backends join with `+`;
//   - a torn part keeps the union torn (a missing footer must not vanish);
//   - `has_end` is the LAST part's — a union with a growing tail is open;
//   - drops/steps/declared counts SUM; `skipped` only when EVERY part skipped;
//   - `code` identity survives only when every part hashes the same bytes.
//
// Pure document model: no ImGui, no GL, no engine, no session objects — links
// beside doc/recording.o in both binaries and the null test harness (D4).
#ifndef ASMDESK_DOC_RECORDING_UNION_H
#define ASMDESK_DOC_RECORDING_UNION_H

#include <vector>

#include "doc/recording.h"

namespace asmdesk {

// The union of a live session's captures: every `done` Recording in order,
// then the growing one (nullptr between sessions). 0 parts -> Recording{};
// 1 part -> a verbatim copy. Event `seq` is reassigned as a running offset so
// stream order — and the per-part [trace…][coverage] interleaving facts that
// live in it — survive concatenation.
Recording merge_session_recordings(const std::vector<Recording> &done,
                                   const Recording *growing);

} // namespace asmdesk
#endif // ASMDESK_DOC_RECORDING_UNION_H
