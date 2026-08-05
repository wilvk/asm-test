// glcommon.h — the GL machinery every 3D SUBSTRATE shares
// (docs/internal/gui/59-standalone-scenes.md T1 step 2).
//
// T1's factoring rule, stated at the brief and kept here: three things are
// genuinely shared between the address plane and a standalone scene, and three
// are not.
//
//   shared      — program compile/link, the R32UI pick target and its readback,
//                 the camera and the MVP (scene3d/camera.h, already pure and
//                 already shared).
//   NOT shared  — the grid mesh, the height/flags/kind textures, the trajectory
//                 buffers.
//
// So this header holds the first group and NOTHING else. The second group is
// deliberately left in scene.cpp: a scene whose geometry is ribs between two
// ribbons has nothing to gain from a height texture, and generalising one would
// produce an abstraction that fits neither.
//
// This TU includes GL (the same GL_GLEXT_PROTOTYPES / OpenGL-framework split
// scene.cpp uses) but no ImGui and no engine, so it compiles into
// asmtest-viewer exactly as into the full app (D4). It is NOT included by the
// headless test tree — like scene.cpp, only the GL FBO smoke links it.
#ifndef ASMDESK_SCENE3D_GLCOMMON_H
#define ASMDESK_SCENE3D_GLCOMMON_H

#include <cstdint>
#include <string>
#include <vector>

namespace asmdesk::scene3d {

// The attribute + fragment-output locations bound from C++ (the GLSL 1.30
// spelling that stands in for `layout(location=)`; see shaders/embedded.h).
// Shared so the plane's programs and a standalone scene's programs agree — two
// files picking their own numbers is how a VAO ends up bound to the wrong
// attribute on one driver and the right one on another.
inline constexpr unsigned kAttrPos = 0; // "cell" (terrain) / "pos" (lines)
inline constexpr unsigned kAttrVid = 1; // "vid"  (pick ids)
inline constexpr unsigned kAttrCol = 2; // "col"  (per-vertex colour, 59)
// 55 T6 step 1: the quad-expanded line layout's two extra attributes
// (shaders/embedded.h's kLineVert) — the segment's OTHER endpoint, and which
// side of the centreline this corner sits on. Slots 3 and 4 because 59's
// kAttrCol already holds 2; the numbers are arbitrary but must be agreed in
// exactly one place, which is the whole point of this header.
inline constexpr unsigned kAttrOther = 3; // "other" (the far endpoint)
inline constexpr unsigned kAttrSide = 4;  // "side"  (-1 / +1 across the line)
// 61 T5: the vertex's TRACE STEP. Once trace time leaves the Y axis, pos.y is
// a constant lift and the comet tail and the terrain-playhead cut can no
// longer be read off it — both are step comparisons, so the step must travel
// with the vertex. A draw that does not enable this attribute gets GL's
// default 0, which is why the point glyphs (which set uHasHead/uHasTimeCut to
// 0) need no buffer of their own.
inline constexpr unsigned kAttrStep = 5;  // "step"  (trace step, as a float)
inline constexpr unsigned kFragId = 0;    // "fragid" (integer pick output)

// Compile + link a program from two GLSL sources, binding the attribute names
// above. `has_vid` binds "vid"; `has_col` binds "col"; `pick` binds the integer
// output "fragid" (harmless for a colour program that declares no such output).
// Returns 0 with `err` set on a driver that will not build it — the caller then
// shows the message rather than a blank scene.
unsigned gl_link_program(const char *vs, const char *fs, bool has_vid, bool pick,
                         std::string &err, bool has_col = false);

// The colour-ID pick target: an R32UI colour texture + a depth renderbuffer,
// sized lazily, with the readback both a single-pixel click and a whole-buffer
// headless probe need.
//
// SACRED (D7, and 55 T1/T5's own bar): this target is never multisampled and
// never post-processed. An id is an exact integer identity; a resolve or a
// blur would average two ids into a third that names nothing.
class GlPickTarget {
  public:
    ~GlPickTarget() { free_gl(); }
    GlPickTarget() = default;
    GlPickTarget(const GlPickTarget &) = delete;
    GlPickTarget &operator=(const GlPickTarget &) = delete;

    // (Re)build for this size; a no-op when it already matches.
    void ensure(int w, int h);
    // ensure() + bind + clear to 0 (the background id) with depth testing on
    // and blending off. Returns the PREVIOUSLY bound draw framebuffer so the
    // caller can restore it after reading.
    int bind_and_clear(int w, int h);
    // Read the id under window pixel (x, y), y from the TOP (glReadPixels is
    // bottom-left origin; this does the flip). 0 == background / out of range.
    uint32_t read_pixel(int x, int y, int fbh) const;
    // The whole id buffer, row-major, bottom-left origin as GL reads it.
    void read_all(int w, int h, std::vector<uint32_t> &out) const;
    // Restore a framebuffer binding saved by bind_and_clear.
    static void restore(int prev_fbo);

    void free_gl();

  private:
    unsigned fbo_ = 0, tex_ = 0, rbo_depth_ = 0;
    int w_ = 0, h_ = 0;
};

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_GLCOMMON_H
