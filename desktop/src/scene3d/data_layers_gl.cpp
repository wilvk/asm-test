// data_layers_gl.cpp — the DRAW half of the memory data-cell family
// (58-memory-data-cell-family.md T2-T6). These are ordinary scene3d::Scene
// member functions; they live in their own TU rather than in scene.cpp for one
// concrete reason: five layers' worth of upload + draw code would be a large
// footprint in the tree's hottest merge file, and the split costs nothing.
// scene.cpp keeps exactly one call site (draw_data_layers, inside render) and
// one teardown call.
//
// Same GL spelling as scene.cpp (GL_GLEXT_PROTOTYPES + libGL on Linux, the
// OpenGL framework's core 3.2 exports on Darwin), and the same "no
// glad/glew/gl3w" property reached the same way.
//
// EVERY layer here is a BATCHED GL_LINES buffer, reusing prog_traj_
// (kTrajVert/kTrajFrag) with uStipple/uHasHead/uHasTimeCut all zeroed —
// exactly as draw_mispred does. No new shader, no new program, no new pick
// band: these layers are drawn, not picked (the terrain cell under them stays
// the pick target, so a click still opens the flat reader for the cell rather
// than for a bar that stands on it).
//
// The fidelity property this file is responsible for: an ABSENT measurement
// emits NO VERTEX. It is never a zero-length segment and never a flat surface
// at height 0 — because a viewer cannot tell those apart from a measured zero,
// and this family's whole subject is memory behaviour that was NOT observed.
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <vector>

#include "scene3d/scene.h"

namespace asmdesk::scene3d {

namespace {
// Mirrors scene.cpp's own kAttrPos (the 3.0 spelling that stands in for
// `layout(location=)`; see shaders/embedded.h). A second constant rather than a
// shared one because scene.cpp keeps its GL binding constants file-local — the
// value is bound from C++ in link_program and is pinned there.
constexpr GLuint kAttrPos = 0;

// Push one GL_LINES segment (two vertices, 3 floats each).
inline void seg(std::vector<float> &v, float x0, float y0, float z0, float x1,
                float y1, float z1) {
    v.push_back(x0);
    v.push_back(y0);
    v.push_back(z0);
    v.push_back(x1);
    v.push_back(y1);
    v.push_back(z1);
}
} // namespace

void Scene::upload_data_batch(DataLineBatch &b, const std::vector<float> &verts) {
    b.count = static_cast<int>(verts.size() / 3);
    if (b.count == 0) {
        // Keep the GL objects (cheap) but draw nothing. An empty batch is the
        // representation of "not measured", so it must be reachable without
        // freeing/reallocating on every playhead move.
        return;
    }
    if (!b.vao)
        glGenVertexArrays(1, &b.vao);
    if (!b.vbo)
        glGenBuffers(1, &b.vbo);
    glBindVertexArray(b.vao);
    glBindBuffer(GL_ARRAY_BUFFER, b.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

void Scene::free_data_layers() {
    DataLineBatch *all[] = {&relief_read_, &relief_write_, &tide_live_,
                            &tide_watermark_};
    for (DataLineBatch *b : all) {
        if (b->vbo)
            glDeleteBuffers(1, &b->vbo);
        if (b->vao)
            glDeleteVertexArrays(1, &b->vao);
        *b = DataLineBatch{};
    }
}

// T2 (58): the read/write twin relief. Two mirrored bas-relief surfaces over
// the shared data cells — +Y for observed reads in a COOL hue, -Y for observed
// writes in a WARM one, both already log1p'd by the model and both cut at the
// terrain playhead by the slice the model was built at.
//
// A spike per cell rather than a tessellated surface: the read is whether a
// cell is a peak, a pit, or both, and a per-cell bar answers it at a fraction
// of the geometry — which matters because this layer composites over the exact
// terrain in the SAME footprint (the brief's own frame-budget note).
//
// THE RULE: a cell whose read direction was never observed contributes no
// vertex to relief_read_, and likewise for writes. The absence is geometric.
void Scene::set_data_relief(const space::DataReliefLayer &relief) {
    std::vector<float> reads, writes;
    reads.reserve(relief.cells.size() * 6);
    writes.reserve(relief.cells.size() * 6);
    for (const space::ReliefCell &c : relief.cells) {
        if (c.has_read)
            seg(reads, c.u, 0.0f, c.v, c.u,
                static_cast<float>(c.read_height) * y_scale, c.v);
        if (c.has_write)
            seg(writes, c.u, 0.0f, c.v, c.u,
                -static_cast<float>(c.write_height) * y_scale, c.v);
    }
    upload_data_batch(relief_read_, reads);
    upload_data_batch(relief_write_, writes);
    // Cool = reads, warm = writes; the SAME cool/amber axis the misprediction
    // ramp uses, so two layers never claim the same hue for two meanings.
    // A torn capture floors BOTH surfaces, so both go red-shifted together
    // (the terrain shader's own torn-gash mix, applied here on the CPU because
    // these batches share the plain kTrajFrag colour uniform).
    const float torn = relief.torn ? 0.55f : 0.0f;
    const float mix = 1.0f - torn;
    relief_read_.color[0] = 0.30f * mix + 1.00f * torn;
    relief_read_.color[1] = 0.72f * mix + 0.15f * torn;
    relief_read_.color[2] = 0.95f * mix + 0.15f * torn;
    relief_read_.color[3] = 0.85f;
    relief_write_.color[0] = 1.00f * mix + 1.00f * torn;
    relief_write_.color[1] = 0.60f * mix + 0.15f * torn;
    relief_write_.color[2] = 0.20f * mix + 0.15f * torn;
    relief_write_.color[3] = 0.85f;
    relief_read_.line_width = relief_write_.line_width = 2.0f;
}

// T3 (58): the working-set tide. The LIVE crest and the cold WATERMARK are
// separate batches, for a fidelity reason and not a rendering one: a cold cell
// is excluded from the live mass by construction, so no draw-order accident
// can composite a decayed cell into "what is hot right now". The watermark
// draws at ~10% alpha — a faithful decay, never a zero: a cell that has gone
// cold has not gone to zero, and a zero-height nub would say it was never
// touched at all. A cell never touched by this slice produces no TideCell at
// all, so it contributes no vertex to either batch.
void Scene::set_working_set_tide(const space::WorkingSetTide &tide) {
    std::vector<float> live, mark;
    live.reserve(tide.cells.size() * 6);
    mark.reserve(tide.cells.size() * 6);
    for (const space::TideCell &c : tide.cells) {
        if (c.live)
            seg(live, c.u, 0.0f, c.v, c.u,
                static_cast<float>(c.live_height) * y_scale, c.v);
        else if (c.cold)
            seg(mark, c.u, 0.0f, c.v, c.u,
                static_cast<float>(c.watermark_height) * y_scale, c.v);
    }
    upload_data_batch(tide_live_, live);
    upload_data_batch(tide_watermark_, mark);
    // A torn capture floors the window, so the crest is red-shifted with the
    // same mix the relief uses.
    const float torn = tide.torn ? 0.55f : 0.0f, mix = 1.0f - torn;
    tide_live_.color[0] = 0.35f * mix + 1.00f * torn;
    tide_live_.color[1] = 0.95f * mix + 0.15f * torn;
    tide_live_.color[2] = 0.70f * mix + 0.15f * torn;
    tide_live_.color[3] = 0.90f;
    tide_live_.line_width = 2.5f;
    // The watermark keeps the live hue (it IS the same quantity, at an earlier
    // time) and spends its difference entirely on alpha — 10%, the "faded
    // watermark" the layer's own rule names.
    tide_watermark_.color[0] = tide_live_.color[0];
    tide_watermark_.color[1] = tide_live_.color[1];
    tide_watermark_.color[2] = tide_live_.color[2];
    tide_watermark_.color[3] = 0.10f;
    tide_watermark_.line_width = 1.5f;
}

void Scene::draw_data_layers(const float mvp[16], const SceneLayers &layers) {
    if (!prog_traj_)
        return;
    std::vector<DataLineBatch *> batches;
    if (layers.data_relief) {
        batches.push_back(&relief_read_);
        batches.push_back(&relief_write_);
    }
    if (layers.working_set) {
        // Watermark first, live second: the faded decay sits behind the mass
        // it decayed from wherever the two overlap.
        batches.push_back(&tide_watermark_);
        batches.push_back(&tide_live_);
    }
    bool any = false;
    for (const DataLineBatch *b : batches)
        if (b->count > 0)
            any = true;
    if (!any)
        return;

    glUseProgram(prog_traj_);
    glUniformMatrix4fv(glGetUniformLocation(prog_traj_, "uMVP"), 1, GL_FALSE,
                       mvp);
    const GLint uColor = glGetUniformLocation(prog_traj_, "uColor");
    // uStipple=0: a stipple is THIS tree's statistical mark on trajectories,
    // and every exact layer here is exact. T6's statistical half carries its
    // own separate batch + its own STATISTICAL label instead (never a second
    // stipple technique competing with the trajectories' one).
    glUniform1i(glGetUniformLocation(prog_traj_, "uStipple"), 0);
    glUniform1i(glGetUniformLocation(prog_traj_, "uHasHead"), 0);
    // uHasTimeCut=0: these layers are ALREADY cut at the playhead by the pure
    // model they were built from (a slice t), so a second, shader-side cut
    // would double-apply it.
    glUniform1i(glGetUniformLocation(prog_traj_, "uHasTimeCut"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (DataLineBatch *b : batches) {
        if (b->count == 0 || !b->vao)
            continue;
        glUniform4fv(uColor, 1, b->color);
        glLineWidth(b->line_width);
        glBindVertexArray(b->vao);
        glDrawArrays(GL_LINES, 0, b->count);
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

} // namespace asmdesk::scene3d
