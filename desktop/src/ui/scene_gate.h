// scene_gate.h — the pure re-upload decision for the 3D overview's GL host.
//
// The GL host (ui/gl_scene_host.cpp) caches its uploaded geometry and re-uploads
// only when it must. That decision is the ONE bit of the host that is testable
// without a GL context, so it lives here as a pure predicate: the host calls it,
// and test_trajectory pins it (D4). It intentionally pulls in NO GL and no heavy
// headers — a growing-capture regression is a data decision, not a render one.
//
// The bug this closes: keying the trajectory/arc re-upload on the recording
// IDENTITY alone froze the worldlines of a LIVE, growing capture after the first
// batch — the identity (a hash of the basename) never changes as the recording
// grows, while the terrain kept re-uploading on the advancing playhead. Folding a
// growth generation (the recording's event_count) into the gate re-uploads the
// worldlines each time the capture actually grows.
#ifndef ASMDESK_UI_SCENE_GATE_H
#define ASMDESK_UI_SCENE_GATE_H

#include <cstdint>

namespace asmdesk {

// Should the host re-upload the trajectory + convergence buffers? True when the
// recording identity changed, OR it grew (same identity, advanced generation —
// the live-capture case), OR nothing has been uploaded yet. `key` is the pure
// recording identity; `gen` is a monotonic growth counter (event_count).
inline bool scene_needs_traj_upload(uint64_t key, uint64_t gen, uint64_t up_key,
                                    uint64_t up_gen, bool have_upload) {
    return !have_upload || key != up_key || gen != up_gen;
}

} // namespace asmdesk
#endif // ASMDESK_UI_SCENE_GATE_H
