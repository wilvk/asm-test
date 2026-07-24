/* vm_compat.cpp — compiles the asmspy view-model headers as C++17 so desktop
 * views reuse the TUI's tested inline logic (plan D5). Verified 2026-07-23:
 *  1. cli/asmspy.h includes <stdatomic.h> (:22); GCC provides no atomic_bool
 *     for C++ < 23, so the alias below must precede it. Name-level fix only,
 *     not an ABI claim: the desktop never links the engines (D9) and never
 *     defines an atomic_bool object.
 *  2. cli/asmspy_autoregion.h uses asmspy_sample_edge_t (cli/asmspy.h:509) in
 *     its signatures WITHOUT including asmspy.h — asmspy.h must come first
 *     (the plan-D5 include-order dependency, reproduced by compilation). */
#include <atomic>
using std::atomic_bool;

/* This include order is load-bearing (fact 2 above), so it is pinned against
 * clang-format's include sorting, which would otherwise reorder it. */
// clang-format off
#include "asmspy.h"            /* FIRST: supplies autoregion/graphsort types */
#include "asmspy_logview.h"
#include "asmspy_dataview.h"
#include "asmspy_autoregion.h" /* AFTER asmspy.h (fact 2)                    */
#include "asmspy_graphsort.h"  /* AFTER T4 (context lift + C++ casts)        */
/* asmspy_treefilter.h arrives via asmspy.h:27; its guard makes that fine.    */
// clang-format on

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
