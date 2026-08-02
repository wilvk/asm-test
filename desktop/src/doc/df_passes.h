// df_passes.h — what an invocation pass IS, in the words a pager can show.
//
// A continuous / `auto` capture is many `df_invocation` passes in one recording
// (35 T1), and the producer stamps exactly ONE region base on each pass
// (cli/asmspy.c's dataflow_record writes the scoped `base` as every df_step's
// `rbase`). So a multi-region capture reaches a reader as several passes whose
// REGIONS DIFFER — never as several regions inside one pass.
//
// That makes paging a bigger act than it looks: stepping from pass 1 to pass 2
// can silently change which code is on screen, and every offset in the view is
// relative to a base that just moved. A bare ordinal ("pass 2 of 3") does not
// say so. These helpers give the pager the words for it.
//
// Pure over the decoded streams — no ImGui, no ShellState — so the null-backend
// test drives them directly.
#ifndef ASMDESK_DOC_DF_PASSES_H
#define ASMDESK_DOC_DF_PASSES_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"

namespace asmdesk {

// The distinct region bases across ALL passes, ascending. Empty when the
// recording states none (a pre-37 producer, or one that omitted `rbase`) —
// which is "not known", never "one region at 0".
std::vector<uint64_t> df_pass_regions(const SegmentedDataflow &seg);

// The pager's description of pass `p`: "pass 2 of 3", plus the region that pass
// covers WHEN the recording spans more than one. With a single region the
// region is a property of the whole recording and naming it per pass would be a
// label that never changes; with none stated, nothing is claimed.
//
// The region comes from the pass's OWN steps, never from `p` — an `auto`
// candidate walk may return to a span it already visited (A, B, A), so a
// position-derived region would be wrong for exactly the recordings this exists
// to describe.
std::string df_pass_desc(const SegmentedDataflow &seg, size_t p);

} // namespace asmdesk
#endif // ASMDESK_DOC_DF_PASSES_H
