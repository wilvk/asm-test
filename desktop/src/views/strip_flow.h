// strip_flow.h — the Session flow builder (2026-08-10 spec): StripModel →
// space::SessionFlowScene. Lives in views/ because StripModel is a views/
// type (the crossing.cpp precedent); the OUTPUT is a space/ POD so scene3d
// consumes it without a views/ dependency (D4).
//
// The lane set is strip_selected_lanes(m, /*detail=*/false) — the SAME top-N
// + counted-aggregate rule the strip draws by, so the flow and the strip can
// never disagree about which threads lead a session.
#ifndef ASMDESK_VIEWS_STRIP_FLOW_H
#define ASMDESK_VIEWS_STRIP_FLOW_H

#include <string>
#include <vector>

#include "nav.h"
#include "space/sessionflow.h"
#include "views/strip.h"

namespace asmdesk {

space::SessionFlowScene build_session_flow(const StripModel &m);

// Deterministic dump — the golden surface for the pure tests.
std::string session_flow_dump(const space::SessionFlowScene &f);

// Pick surface (scene_kind pick-band sizing + the shell's one decode):
// order = rows first (index i), then seams (rows.size() + j).
size_t flow_pick_order(const space::SessionFlowScene &f);
// Lane/AggregateLanes/Kernel rows → the syscalls view (pid when the lane's
// tgid is known); Memory → the timeline; seams → nullopt (hover-only).
std::optional<dt_link> flow_pick_link(const space::SessionFlowScene &f,
                                      size_t ord, const std::string &rec_id);
// The hover line for pick ordinal `ord`: "<label> — N events" for a row,
// the seam's own label for a seam; "" out of range.
std::string flow_pick_hint(const space::SessionFlowScene &f, size_t ord);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_STRIP_FLOW_H
