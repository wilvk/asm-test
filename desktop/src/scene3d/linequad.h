// linequad.h — the CPU half of T6 step 1's portable line width
// (docs/internal/gui/55-scene-render-quality.md): turning a polyline into the
// per-segment quad geometry `shaders/embedded.h`'s kLineVert widens in screen
// space.
//
// Pure and header-only (no GL, no ImGui, no engine): scene.cpp calls it to
// build the buffers, and test_camera.cpp — which links nothing but itself —
// calls the SAME function to pin the maths, so the tested expansion is the
// shipped one rather than a re-derivation of it.
//
// Why this exists at all: `glLineWidth` above 1.0 is deprecated in a core
// profile, where an implementation may report GL_ALIASED_LINE_WIDTH_RANGE =
// [1,1] and clamp (or raise GL_INVALID_VALUE). This app's own Apple path asks
// for exactly such a context (main.cpp), so every width the scene draws had to
// move from that call into geometry.
#ifndef ASMDESK_SCENE3D_LINEQUAD_H
#define ASMDESK_SCENE3D_LINEQUAD_H

#include <cstddef>
#include <vector>

namespace asmdesk::scene3d {

// Floats per expanded vertex: pos.xyz, other.xyz, side. The vertex shader
// derives the screen-space perpendicular from (other - pos), so a corner needs
// no precomputed normal and the same buffer stays correct under any camera.
inline constexpr int kQuadStrideFloats = 7;
// Vertices per segment: two triangles.
inline constexpr int kQuadVertsPerSegment = 6;

// Expand ONE segment (a -> b) into the six vertices of two triangles.
//
// The subtlety worth stating: the perpendicular at `b` is derived from
// (a - b) and so points the OPPOSITE way to the one at `a`. The two corners
// that are physically on the same side of the centreline therefore carry
// OPPOSITE `side` values — which is what the (b, a, +1) / (b, a, -1) pairing
// below encodes. Get this wrong and the quad self-intersects into a bowtie.
inline void emit_quad_segment(std::vector<float> &out, const float *a,
                              const float *b) {
    auto corner = [&out](const float *p, const float *q, float side) {
        out.insert(out.end(), {p[0], p[1], p[2], q[0], q[1], q[2], side});
    };
    corner(a, b, +1.0f); // A+
    corner(a, b, -1.0f); // A-
    corner(b, a, +1.0f); // B- (physically opposite A+, see above)
    corner(a, b, +1.0f); // A+
    corner(b, a, +1.0f); // B-
    corner(b, a, -1.0f); // B+
}

// Expand a whole vertex array (3 floats per vertex) into quad geometry.
// `strip` true: consecutive vertices form segments (what GL_LINE_STRIP drew).
// false: disjoint pairs (what GL_LINES drew).
//
// `per_segment_id`/`out_ids`, when given, carry the pick pass's parallel array:
// each segment's id repeated once per emitted vertex. Built HERE, beside the
// positions it labels, so the two can never fall out of step — a mislabelled
// pick id is a wrong drill-in, not a cosmetic defect.
inline void
expand_line_quads(const std::vector<float> &pts, bool strip,
                  std::vector<float> &out,
                  const std::vector<unsigned> *per_segment_id = nullptr,
                  std::vector<unsigned> *out_ids = nullptr) {
    const size_t nv = pts.size() / 3;
    const size_t step = strip ? 1u : 2u;
    size_t seg = 0;
    for (size_t i = 0; i + 1 < nv; i += step) {
        emit_quad_segment(out, &pts[i * 3], &pts[(i + 1) * 3]);
        if (out_ids) {
            const unsigned id = (per_segment_id && seg < per_segment_id->size())
                                    ? (*per_segment_id)[seg]
                                    : 0u;
            for (int k = 0; k < kQuadVertsPerSegment; k++)
                out_ids->push_back(id);
        }
        seg++;
    }
}

// How many segments an expansion of `nv` vertices produces — the count a
// caller needs to size a per-segment id array before calling above.
inline size_t line_quad_segments(size_t nv, bool strip) {
    if (nv < 2)
        return 0;
    return strip ? nv - 1 : nv / 2;
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_LINEQUAD_H
