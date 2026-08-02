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
                            &tide_watermark_, &pillars_,     &pillars_open_,
                            &ribbon_read_,    &ribbon_write_, &ribbon_leap_,
                            &sediment_exact_, &sediment_stat_};
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

// T4 (58): the observed-lifetime pillars. Y is TRACE TIME (traj_scale_, the
// same world-Y-per-step the worldlines use), so the terrain playhead reads as
// a horizontal plane through them: below = born, intersected = live, above =
// untouched. Whole-recording geometry — it does not move with the playhead,
// which is the point of a Gantt.
//
// OPEN-TOPPED pillars live in their own batch. A torn capture cut the
// OBSERVATION, not the object, so the top of such a pillar is a lower bound on
// the interval and never its end; an interval and a lower bound on an interval
// are different claims and must not share a buffer or a colour.
//
// A zero-length pillar (one single touch) still emits a segment — but a
// deliberately MINIMAL one, one step tall, so it reads as the stub it is
// rather than vanishing. That is not a fabricated extent: LifetimePillar
// states zero_length, the HUD says so, and a vanished pillar would hide a real
// observed touch.
void Scene::set_lifetime_pillars(const space::LifetimePillars &pillars) {
    std::vector<float> solid, open;
    solid.reserve(pillars.pillars.size() * 6);
    for (const space::LifetimePillar &p : pillars.pillars) {
        const float y0 = static_cast<float>(p.first_step) * traj_scale_;
        float y1 = static_cast<float>(p.last_step) * traj_scale_;
        if (p.zero_length)
            y1 = y0 + traj_scale_; // a one-step stub, never a vanished touch
        seg(p.open_top ? open : solid, p.u, y0, p.v, p.u, y1, p.v);
    }
    upload_data_batch(pillars_, solid);
    upload_data_batch(pillars_open_, open);
    // Translucent, per T4's own wording — the pillars stand over the terrain
    // and the worldlines pass through them, so they must not occlude.
    pillars_.color[0] = 0.65f;
    pillars_.color[1] = 0.80f;
    pillars_.color[2] = 0.90f;
    pillars_.color[3] = 0.40f;
    pillars_.line_width = 2.0f;
    // Open-topped: the torn red-shift, and thinner, so a lower bound never
    // reads as heavier evidence than a closed interval.
    pillars_open_.color[0] = 1.00f;
    pillars_open_.color[1] = 0.35f;
    pillars_open_.color[2] = 0.30f;
    pillars_open_.color[3] = 0.55f;
    pillars_open_.line_width = 1.5f;
}

// T5 (58): the data-access worldline ribbon. One vertex per recorded access at
// its effective address's plane position, Y = trace time, joined in step order
// — the SAME per-vertex line construction the PC worldlines use, with `ea`
// substituted for the PC.
//
// THE RULE: a segment exists only where the MODEL emitted one. A gap in the
// step coverage, or an access no region maps, produces NO segment — never an
// interpolated join, which would draw an access that was never recorded. This
// file does not decide where the gaps are; it structurally cannot invent a
// segment, because it only ever walks DataRibbon::segs.
//
// Three batches: reads, writes (T5's colour channel) and cross-region LEAPS. A
// leap between distinct observed-data spans is genuine non-locality, so it is
// drawn distinctly and never blamed on the address compaction.
void Scene::set_data_ribbon(const space::DataRibbon &ribbon) {
    std::vector<float> reads, writes, leaps;
    for (const space::RibbonSegment &sg : ribbon.segs) {
        if (sg.a >= ribbon.verts.size() || sg.b >= ribbon.verts.size())
            continue;
        const space::RibbonVertex &a = ribbon.verts[sg.a];
        const space::RibbonVertex &b = ribbon.verts[sg.b];
        const float ya = static_cast<float>(a.step) * traj_scale_;
        const float yb = static_cast<float>(b.step) * traj_scale_;
        std::vector<float> &into =
            sg.cross_region                       ? leaps
            : (b.dir == space::RibbonVertex::Write) ? writes
                                                    : reads;
        seg(into, a.u, ya, a.v, b.u, yb, b.v);
    }
    upload_data_batch(ribbon_read_, reads);
    upload_data_batch(ribbon_write_, writes);
    upload_data_batch(ribbon_leap_, leaps);
    // The SAME cool=read / warm=write axis the twin relief uses, so two layers
    // never claim one hue for two meanings.
    ribbon_read_.color[0] = 0.30f;
    ribbon_read_.color[1] = 0.72f;
    ribbon_read_.color[2] = 0.95f;
    ribbon_read_.color[3] = 0.75f;
    ribbon_write_.color[0] = 1.00f;
    ribbon_write_.color[1] = 0.60f;
    ribbon_write_.color[2] = 0.20f;
    ribbon_write_.color[3] = 0.75f;
    // A leap gets its own colour AND its own line width: it is the layer's
    // real finding (pointer-chasing across objects), not a rendering artefact.
    ribbon_leap_.color[0] = 0.85f;
    ribbon_leap_.color[1] = 0.30f;
    ribbon_leap_.color[2] = 0.85f;
    ribbon_leap_.color[3] = 0.80f;
    ribbon_read_.line_width = ribbon_write_.line_width = 1.5f;
    ribbon_leap_.line_width = 2.0f;
}

// T6 (58): the residency sediment columns. Each band is one segment on the
// TRACE-TIME axis (traj_scale_), so a column reads as stacked strata with gaps
// where the cell was not touched — the temporal phase the moving slice only
// reveals by scrubbing, stood up with the playhead paused.
//
// EXACT and STATISTICAL are separate batches: the isolation invariant made
// geometry, so no draw path can merge a survey column into the exact buffer,
// and the survey half is drawn thinner and dimmer (it carries magnitude but
// NO phase — one unattributed band over the whole axis).
//
// This is the densest layer in the family, which is why the MODEL already
// coarsened its band count against the caller's scene budget before we get
// here. This file draws what it was given and adds no throttle of its own.
void Scene::set_sediment_columns(const space::SedimentColumns &cols) {
    std::vector<float> exact, stat;
    for (const space::SedimentColumn &c : cols.exact)
        for (const space::SedimentBand &b : c.bands)
            seg(exact, c.u, static_cast<float>(b.lo_step) * traj_scale_, c.v,
                c.u, static_cast<float>(b.hi_step) * traj_scale_, c.v);
    for (const space::SedimentColumn &c : cols.stat)
        for (const space::SedimentBand &b : c.bands)
            seg(stat, c.u, static_cast<float>(b.lo_step) * traj_scale_, c.v,
                c.u, static_cast<float>(b.hi_step) * traj_scale_, c.v);
    upload_data_batch(sediment_exact_, exact);
    upload_data_batch(sediment_stat_, stat);
    sediment_exact_.color[0] = 0.80f;
    sediment_exact_.color[1] = 0.75f;
    sediment_exact_.color[2] = 0.55f;
    sediment_exact_.color[3] = 0.65f;
    sediment_exact_.line_width = 3.0f;
    // The survey half: the desaturated grey the ghost-fog terrain already uses
    // for sampled residency, thinner and fainter — never the exact palette.
    sediment_stat_.color[0] = 0.55f;
    sediment_stat_.color[1] = 0.55f;
    sediment_stat_.color[2] = 0.60f;
    sediment_stat_.color[3] = 0.25f;
    sediment_stat_.line_width = 1.0f;
}

void Scene::draw_data_layers(const float mvp[16], const SceneLayers &layers) {
    if (!prog_traj_)
        return;
    std::vector<DataLineBatch *> batches;
    if (layers.data_relief) {
        batches.push_back(&relief_read_);
        batches.push_back(&relief_write_);
    }
    if (layers.lifetime) {
        batches.push_back(&pillars_);
        batches.push_back(&pillars_open_);
    }
    if (layers.data_ribbon) {
        batches.push_back(&ribbon_read_);
        batches.push_back(&ribbon_write_);
        batches.push_back(&ribbon_leap_);
    }
    if (layers.sediment) {
        batches.push_back(&sediment_exact_);
        batches.push_back(&sediment_stat_);
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
