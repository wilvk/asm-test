// crossing.h — the builder for the kernel-crossing spur layer
// (57-causal-layers.md T2). The GEOMETRY type lives in space/crossing.h (a
// POD, so scene3d/ can consume it without ever depending on views/, D4); this
// is the half that needs a SyscallView and a Recording, and it is its OWN TU
// rather than an addition to syscalls.cpp deliberately: syscalls.o is in the
// DESKTOP_OBS_PURE bundle every Observer-view binary links, and making that
// bundle drag space/locate.o + space/stepplace.o into a dozen link lines to
// serve one 3D layer would be a worse trade than one more small TU.
//
// Pure and engine-free (D4): no ImGui, no GL, no engine.
#ifndef ASMDESK_VIEWS_CROSSING_H
#define ASMDESK_VIEWS_CROSSING_H

#include <string>

#include "doc/recording.h"
#include "space/crossing.h"
#include "space/projection.h"
#include "views/syscalls.h"

namespace asmdesk {

// Build the kernel-crossing spur layer's plane-space geometry.
//
// Each syscall row is anchored to the LAST `trace` instruction with a smaller
// `Event::seq` (54 T3), and its RESUME vertex is the first with a greater one.
// Both are located onto the plane by their ADDRESS, through space/locate.h's
// scene_locate_off — never by an ordinal (46 §4). A syscall whose seq precedes
// every trace instruction is COUNTED (CrossingLayer::before_first_insn) and
// produces no spur: anchoring it to instruction 0 would be a fabrication, and
// it is the single anchoring mistake this layer is most likely to make.
//
// SELF-GATES, each with a stated reason and NO geometry (T2 step 5): a
// recording with no `trace` worldline has nothing to hang a spur on, and one
// whose syscall rows carry no stream position (`seq_present == false`) has no
// order to anchor by. Never synthesise a path to decorate.
space::CrossingLayer build_crossing_layer(const SyscallView &v,
                                          const Recording &r,
                                          const space::Projection &proj);

// A payload-FREE textual dump of the layer, for the tests and for a copyable
// diagnostic. It CANNOT leak a payload, because space::CrossingSpur never
// holds one — only its byte count — but the test asserts the absence anyway,
// exactly as test_obs_syscalls.cpp's blunt negative check does for the view.
std::string crossing_layer_dump(const space::CrossingLayer &layer);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_CROSSING_H
