/* vm_compat.cpp — compiles the asmspy view-model headers as C++17 so desktop
 * views reuse the TUI's tested inline logic (plan D5). Verified 2026-07-23,
 * re-verified 2026-07-24 against the libasmspy extraction (07-serve-live-host
 * T0), which changed fact 2:
 *  1. cli/libasmspy.h includes <stdatomic.h>; GCC provides no atomic_bool for
 *     C++ < 23, so the alias below must precede it. Name-level fix only, not an
 *     ABI claim: the desktop never links the engines (D9) and never defines an
 *     atomic_bool object. STILL LOAD-BEARING.
 *  2. cli/asmspy_autoregion.h used asmspy_sample_edge_t in its signatures
 *     WITHOUT including the header that declares it, so this file had to pin an
 *     include ORDER by hand. T0 made it self-contained (it now includes
 *     libasmspy.h), so the order no longer matters — RESOLVED, not merely
 *     worked around. The headers below are in whatever order clang-format
 *     wants, which is the point: a public header set that only compiles in one
 *     order is not one a consumer can use.
 *
 * These are the ENGINE'S public headers (libasmspy.h's preamble lists them),
 * included for their pure inline view models only. Nothing here links the
 * library — the desktop reaches the engines solely through the `asmspy --serve`
 * subprocess (D9). */
#include <atomic>
using std::atomic_bool;

#include "asmspy_autoregion.h"
#include "asmspy_dataview.h"
#include "asmspy_graphsort.h"
#include "asmspy_logview.h"
#include "libasmspy.h"
/* asmspy_treefilter.h arrives via libasmspy.h; its guard makes that fine. */

namespace asmdesk {
/* Touch one symbol per header: an empty include (broken guard, wrong -I)
 * fails HERE, not in a view; also keeps -Wunused-function quiet for the qsort
 * adapter, which the GUI must never call (single-thread latch). */
int vm_compat_anchor() {
    long top = 0;
    (void)asmspy_log_window(1, 1, 0, 1, &top);
    (void)gnode_cmp;
    asmspy_autocand_t c[1];
    return (int)asmspy_autoregion_rank(nullptr, 0, nullptr, nullptr, nullptr, c,
                                       1);
}
} // namespace asmdesk
