// reweave_apply.h — the reweave engine leg (docs/internal/gui/42-loom-reweave-
// consumption.md T3). FULL BUILD ONLY, like forks.cpp (D4/D9): this pair
// compiles into the app tree alone (DESKTOP_LOOM_APP in mk/desktop.mk), never
// the render-only viewer or the headless null tests — there is no #ifdef here,
// the object is simply absent from those link lines, exactly like forks.o.
//
// The signature is pure-typed (no engine header) on purpose: fabric_imgui.cpp
// compiles into ALL FOUR desktop variants, so it declares the call to
// loom_run_reweave under its own #ifdef ASMTEST_DESKTOP_CAN_AUTHOR guard (the
// same pattern author_door.cpp already uses for asmtest_dataflow_emu_run_arch) —
// only the app-variant TU (which alone defines that macro on fabric_imgui.o)
// ever emits the call, so render/test builds never reference this symbol and
// link fine without reweave_apply.o in their tree.
#ifndef ASMDESK_LOOM_REWEAVE_APPLY_H
#define ASMDESK_LOOM_REWEAVE_APPLY_H

#include <cstdint>
#include <string>

#include "loom/fabric.h"        // loom_fabric_t
#include "loom/reweave_input.h" // ReweaveSource
#include "loom/take_view.h"     // loom_take_view_t

namespace asmdesk {

// Re-execute `src` to checkpoint `req_step`, apply the ONE register edit
// (`req_reg` := `req_value`), weave the resulting fabric beside `base`, and
// build its take view. Always returns a usable view: on any failure (an
// unknown register, K past the run's length, a producer setup failure, or a
// non-exact provenance), the returned view's `node.err` carries the loud
// refusal verbatim (D7) rather than an empty or invented fabric.
loom_take_view_t loom_run_reweave(const loom_fabric_t &base,
                                  const ReweaveSource &src, uint64_t req_step,
                                  const std::string &req_reg,
                                  uint64_t req_value);

} // namespace asmdesk
#endif // ASMDESK_LOOM_REWEAVE_APPLY_H
