// causal.cpp — the GL half of the four causal layers (57-causal-layers.md
// T2-T5): kernel-crossing spurs, the taint isochrone front, the blame
// convergence forest, and the dominant-path ridge.
//
// A SEPARATE TU from scene.cpp, defining Scene methods declared there. Two
// reasons, in order of weight:
//
//  1. **Merge surface.** Four sibling briefs (51, 55, 57, 58, 59) edit
//     scene3d/scene.{h,cpp} concurrently. Putting four uploads and four draws
//     in scene.cpp would put four large hunks in the file most likely to
//     conflict. Here, the whole family costs scene.cpp exactly two lines (a
//     draw_causal() call in render(), a free_causal() call in shutdown()) and
//     scene.h one contiguous block.
//  2. **Reading.** These four share one substrate (the plane + the trajectory
//     time axis) and one rule (nothing here may state a causal link the
//     recording does not), so they read better together than interleaved with
//     the terrain passes.
//
// NO NEW SHADER. Every draw reuses prog_traj_ (kTrajVert/kTrajFrag) exactly as
// draw_mispred does: uStipple=0 (a stipple is this family's STATISTICAL mark
// and must not be reused for anything else), uHasHead=0, uHasTimeCut=0.
//
// **NO glLineWidth ABOVE 1.0.** 55 T6 records that width > 1 is invalid on a
// core forward-compatible context (the profile this app's own Apple path
// requests) and is being removed tree-wide; nothing added here may reintroduce
// it. Where a layer wants a heavier stroke, the magnitude rides on a channel
// that IS portable — a point-sprite size (gl_PointSize via uPointSize), a
// colour ramp, or geometry — and the wants are listed in this brief's status
// note so the quad-expansion work can pick them up.
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <cmath>
#include <vector>

#include "scene3d/scene.h"

namespace asmdesk::scene3d {

namespace {

// The SAME attribute location scene.cpp binds from C++ (glBindAttribLocation
// for "pos" on prog_traj_). A second `constexpr` rather than a shared header
// constant because scene.cpp's own copy is file-local by design (scene.h
// declares no GL type at all, which is what keeps hud.cpp and the pure tests
// able to include it) — the two are pinned together by this comment, the same
// keep-in-sync convention hud.h already uses for the shader colour literals.
constexpr GLuint kAttrPos = 0;

// A GL_LINES / GL_LINE_STRIP / GL_POINTS buffer with a colour, at line width
// 1.0 always (see the header note).
struct Prim {
    unsigned vao = 0, vbo = 0;
    int count = 0;    // vertex count
    GLenum mode = GL_LINES;
    float point_size = 1.0f; // only read for GL_POINTS
    float color[4] = {1, 1, 1, 1};
};

void free_prim(Prim &p) {
    if (p.vbo)
        glDeleteBuffers(1, &p.vbo);
    if (p.vao)
        glDeleteVertexArrays(1, &p.vao);
    p.vbo = p.vao = 0;
    p.count = 0;
}

// Upload `verts` (3 floats per vertex) as one primitive. An empty vector
// yields a zero-count Prim that draws nothing — never a stray GL object.
Prim upload(const std::vector<float> &verts, GLenum mode, const float rgba[4],
            float point_size = 1.0f) {
    Prim p;
    p.mode = mode;
    p.point_size = point_size;
    for (int i = 0; i < 4; i++)
        p.color[i] = rgba[i];
    if (verts.empty())
        return p;
    p.count = static_cast<int>(verts.size() / 3);
    glGenVertexArrays(1, &p.vao);
    glGenBuffers(1, &p.vbo);
    glBindVertexArray(p.vao);
    glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return p;
}

void push_seg(std::vector<float> &v, const float a[3], const float b[3]) {
    v.insert(v.end(), {a[0], a[1], a[2], b[0], b[1], b[2]});
}

// A DASHED segment: real geometry, not a shader mode. The stipple uniform
// kTrajFrag offers is this family's stated STATISTICAL mark (see draw_mispred's
// own comment), so reusing it to mean "withheld at record time" would make one
// idiom carry two unrelated claims. Nine sub-segments, five drawn.
void push_dashed(std::vector<float> &v, const float a[3], const float b[3]) {
    const int kParts = 9;
    for (int i = 0; i < kParts; i += 2) {
        const float t0 = static_cast<float>(i) / kParts;
        const float t1 = static_cast<float>(i + 1) / kParts;
        const float p0[3] = {a[0] + (b[0] - a[0]) * t0,
                             a[1] + (b[1] - a[1]) * t0,
                             a[2] + (b[2] - a[2]) * t0};
        const float p1[3] = {a[0] + (b[0] - a[0]) * t1,
                             a[1] + (b[1] - a[1]) * t1,
                             a[2] + (b[2] - a[2]) * t1};
        push_seg(v, p0, p1);
    }
}

// A small open diamond in the plane's XZ at world (x, y, z) — the HOLLOW
// anchor glyph. Hollow, not dimmer: "the nearest recorded instruction over a
// sparse trace" is a weaker claim than "the nearest recorded instruction", and
// a filled point at reduced alpha would read as distance, not as doubt.
void push_ring(std::vector<float> &v, const float c[3], float r) {
    const float pts[4][3] = {{c[0] - r, c[1], c[2]},
                             {c[0], c[1], c[2] - r},
                             {c[0] + r, c[1], c[2]},
                             {c[0], c[1], c[2] + r}};
    for (int i = 0; i < 4; i++)
        push_seg(v, pts[i], pts[(i + 1) % 4]);
}

// Hue per derived syscall family. `Other` is a REAL, visible grey — never an
// absence, and deliberately not a hue anyone would read as a category
// (space/crossing.h's own rule: never folded into a known class).
void crossing_hue(space::SyscallClass c, float *r, float *g, float *b) {
    switch (c) {
    case space::SyscallClass::File:
        *r = 0.35f, *g = 0.75f, *b = 0.95f;
        return;
    case space::SyscallClass::Net:
        *r = 0.45f, *g = 0.90f, *b = 0.60f;
        return;
    case space::SyscallClass::Process:
        *r = 0.95f, *g = 0.70f, *b = 0.35f;
        return;
    case space::SyscallClass::Memory:
        *r = 0.80f, *g = 0.55f, *b = 0.95f;
        return;
    case space::SyscallClass::Signal:
        *r = 0.95f, *g = 0.45f, *b = 0.55f;
        return;
    case space::SyscallClass::Time:
        *r = 0.85f, *g = 0.85f, *b = 0.45f;
        return;
    case space::SyscallClass::Other:
        break;
    }
    *r = *g = *b = 0.62f; // visible grey, not a washed-out version of a hue
}

} // namespace

// Every causal layer's GL objects. Defined here, declared opaquely in scene.h.
struct Scene::CausalGL {
    // T2: the out/return spurs (solid), the redacted spurs (dashed geometry),
    // the hollow anchor rings, and the rail-point glyphs. Split by IDIOM
    // rather than by spur so each can carry its own draw state without a
    // per-spur draw call.
    std::vector<Prim> crossing_spurs;  // one per hue bucket, GL_LINES
    std::vector<Prim> crossing_glyphs; // one per payload-magnitude bucket
    Prim crossing_hollow;              // the hollow anchor rings, GL_LINES

    // T3: the taint front. Solid marks are points bucketed by BFS depth (one
    // draw per depth band); hollow routes, escape glyphs and unknown frays are
    // line geometry, because each is a DIFFERENT KIND of statement and a
    // reader must be able to tell them apart without a legend lookup.
    std::vector<Prim> taint_solid;  // GL_POINTS, one per depth band
    Prim taint_hollow;              // GL_LINES: value observed, value unknown
    Prim taint_escape;              // GL_LINES: a recorded region-kind change
    Prim taint_unknown;             // GL_LINES: the front runs into a gap

    // T4: the blame convergence forest. Beacons are POINTS on their OWN
    // channel (size + brightness), never terrain height: height already
    // encodes access density, and overloading it would make two quantities
    // share one axis (46 G8's complaint about the vertical).
    std::vector<Prim> blame_beacons;  // GL_POINTS, one per weight band
    Prim blame_sinks;                 // GL_LINES: sink rings
    Prim blame_untraced;              // GL_LINES: the born-untraced verdict
};

// Each set_* below replaces ONLY its own layer's geometry, so the four
// uploads are order-independent and a caller that re-uploads one does not
// silently wipe the other three.
void Scene::ensure_causal() {
    if (!causal_)
        causal_ = new CausalGL();
}

void Scene::free_causal() {
    if (!causal_)
        return;
    for (Prim &p : causal_->crossing_spurs)
        free_prim(p);
    for (Prim &p : causal_->crossing_glyphs)
        free_prim(p);
    free_prim(causal_->crossing_hollow);
    for (Prim &p : causal_->taint_solid)
        free_prim(p);
    free_prim(causal_->taint_hollow);
    free_prim(causal_->taint_escape);
    free_prim(causal_->taint_unknown);
    for (Prim &p : causal_->blame_beacons)
        free_prim(p);
    free_prim(causal_->blame_sinks);
    free_prim(causal_->blame_untraced);
    delete causal_;
    causal_ = nullptr;
}

// T2 (57-causal-layers): kernel-crossing spurs.
//
// GEOMETRY, and why it is shaped this way. From the anchor worldline vertex
// (u, t*traj_scale, v) a spur shoots to ONE rail point, and the return spur
// comes back from that SAME point to the resume vertex. One shared point means
// zero along-rail extent, which is the whole reason the layer cannot imply a
// kernel dwell (space::CrossingLayer::rail_span() == 0, pinned by
// test_crossing.cpp). The rail sits a CONSTANT lift above the higher of the
// two vertices, so its height encodes nothing either.
//
// The THICKNESS channel the brief asks for (payload size) rides on the rail
// glyph's POINT SIZE, not on glLineWidth: line width > 1 is invalid on a core
// forward-compatible context (55 T6) and this file adds none. The spur line
// itself is one of the draws that WANTS a heavier stroke and is listed as such
// in 57's status note.
void Scene::set_crossing_layer(const space::CrossingLayer &layer) {
    ensure_causal();
    for (Prim &p : causal_->crossing_spurs)
        free_prim(p);
    causal_->crossing_spurs.clear();
    for (Prim &p : causal_->crossing_glyphs)
        free_prim(p);
    causal_->crossing_glyphs.clear();
    free_prim(causal_->crossing_hollow);
    if (!layer.enabled || layer.spurs.empty())
        return;

    // One GL_LINES buffer per (class, redacted) bucket: a handful of buckets,
    // so the draw stays a handful of calls however many crossings there are.
    constexpr int kClasses = 7;
    std::vector<float> solid[kClasses];
    std::vector<float> dashed[kClasses];
    std::vector<float> hollow_rings;
    // Payload magnitude buckets for the rail glyph's point size: 0, <=64,
    // <=4K, larger. Four buckets rather than a per-spur size so the whole
    // layer stays four draw calls.
    constexpr int kMagBuckets = 4;
    std::vector<float> glyphs[kMagBuckets];

    for (const space::CrossingSpur &sp : layer.spurs) {
        const float ay = static_cast<float>(sp.anchor_t) * traj_scale_;
        const float ry =
            sp.has_resume ? static_cast<float>(sp.resume_t) * traj_scale_ : ay;
        const float rail_y = (ay > ry ? ay : ry) + layer.rail_lift;
        const float a[3] = {sp.anchor_u, ay, sp.anchor_v};
        const float rail[3] = {sp.rail_u, rail_y, sp.rail_v};
        const int ci = static_cast<int>(sp.cls);
        std::vector<float> &out = sp.redacted ? dashed[ci] : solid[ci];
        if (sp.redacted)
            push_dashed(out, a, rail);
        else
            push_seg(out, a, rail);
        if (sp.has_resume) {
            const float b[3] = {sp.resume_u, ry, sp.resume_v};
            // The RETURN spur starts at the same rail point the out spur ended
            // at — literally the same coordinates, so the pair has no span.
            if (sp.redacted)
                push_dashed(out, rail, b);
            else
                push_seg(out, rail, b);
        }
        if (sp.hollow)
            push_ring(hollow_rings, a, 0.006f);
        int mag = 0;
        if (sp.has_payload && sp.payload_bytes > 0)
            mag = sp.payload_bytes <= 64 ? 1 : (sp.payload_bytes <= 4096 ? 2 : 3);
        glyphs[mag].insert(glyphs[mag].end(), {rail[0], rail[1], rail[2]});
    }

    for (int c = 0; c < kClasses; c++) {
        float r = 0, g = 0, b = 0;
        crossing_hue(static_cast<space::SyscallClass>(c), &r, &g, &b);
        const float solid_rgba[4] = {r, g, b, 0.9f};
        // A redacted spur is dashed AND dimmer: the dash says "withheld", the
        // dimming keeps it from competing with the crossings whose content is
        // actually in the file.
        const float dashed_rgba[4] = {r, g, b, 0.55f};
        if (!solid[c].empty())
            causal_->crossing_spurs.push_back(
                upload(solid[c], GL_LINES, solid_rgba));
        if (!dashed[c].empty())
            causal_->crossing_spurs.push_back(
                upload(dashed[c], GL_LINES, dashed_rgba));
    }
    for (int m = 0; m < kMagBuckets; m++) {
        if (glyphs[m].empty())
            continue;
        const float rgba[4] = {0.95f, 0.95f, 1.0f, 0.85f};
        // 3, 5, 7, 9 px: the payload-magnitude channel, on a portable
        // primitive. gl_PointSize is written by kTrajVert from uPointSize.
        causal_->crossing_glyphs.push_back(
            upload(glyphs[m], GL_POINTS, rgba, 3.0f + 2.0f * m));
    }
    if (!hollow_rings.empty()) {
        const float rgba[4] = {0.85f, 0.85f, 0.90f, 0.75f};
        causal_->crossing_hollow = upload(hollow_rings, GL_LINES, rgba);
    }
}

// T3 (57-causal-layers): the taint isochrone.
//
// EVERY MARK SITS ON THE PLANE, at world Y just above it — the depth is a HUE,
// never a height. Height on this plane already encodes access density, and
// giving the def-use generation the same axis would make two quantities share
// one channel, which is precisely 46 G8's complaint about the vertical. It
// would also silently fuse the def-use clock with the terrain's, which 34
// deliberately left unfused.
//
// Four distinct idioms, because each is a different KIND of statement:
//   solid point   — the value reached here, and the value is known;
//   hollow ring   — the value reached here and the VALUE was not captured
//                   (the flow was observed; only its content was not);
//   escape cross  — this cell's RECORDED region kind differs from the
//                   origin's: the layer's actual finding;
//   fray ticks    — the front runs from here into a step the recording never
//                   described. A POSITIVE mark, because "did not spread" and
//                   "not recorded" both render as nothing by default, and
//                   keeping them apart is the whole point of this layer.
void Scene::set_taint_front(const space::TaintFront &front) {
    ensure_causal();
    for (Prim &p : causal_->taint_solid)
        free_prim(p);
    causal_->taint_solid.clear();
    free_prim(causal_->taint_hollow);
    free_prim(causal_->taint_escape);
    free_prim(causal_->taint_unknown);
    if (!front.enabled || front.reached.empty())
        return;

    // Depth bands: NEAR is hot, far is cool. Eight bands is enough to read
    // "how far" without turning the ramp into noise, and it keeps the layer at
    // eight draw calls however many cells it covers.
    constexpr int kBands = 8;
    std::vector<float> band[kBands];
    std::vector<float> hollow, escape, unknown;
    const int span = front.depth_max > 0 ? front.depth_max : 1;

    for (const space::TaintReach &tr : front.reached) {
        const float y = 0.004f; // a hair above the terrain, never a height
        const float c[3] = {tr.u, y, tr.v};
        if (tr.hollow)
            push_ring(hollow, c, 0.005f);
        else {
            int b = static_cast<int>((static_cast<float>(tr.depth) / span) *
                                     (kBands - 1) + 0.5f);
            if (b < 0)
                b = 0;
            if (b >= kBands)
                b = kBands - 1;
            band[b].insert(band[b].end(), {c[0], c[1], c[2]});
        }
        if (tr.escape) {
            // A cross, not a ring: the finding must not be mistakable for the
            // hollow "value unknown" idiom at a glance.
            const float r = 0.009f;
            const float a1[3] = {c[0] - r, y, c[2] - r};
            const float b1[3] = {c[0] + r, y, c[2] + r};
            const float a2[3] = {c[0] - r, y, c[2] + r};
            const float b2[3] = {c[0] + r, y, c[2] - r};
            push_seg(escape, a1, b1);
            push_seg(escape, a2, b2);
        }
        // A bounded walk's OWN rim is a lower bound too (dt_walk::bounded), so
        // the deepest band frays exactly as an unknown-gap cell does — same
        // idiom, because it is the same claim: "this is where we stopped
        // looking", not "this is where the value stopped".
        const bool rim = front.bounded && tr.depth == front.depth_max;
        if (tr.unknown_beyond || rim) {
            const float r0 = 0.007f, r1 = 0.013f;
            const float dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto &d : dirs) {
                const float a[3] = {c[0] + d[0] * r0, y, c[2] + d[1] * r0};
                const float b[3] = {c[0] + d[0] * r1, y, c[2] + d[1] * r1};
                push_seg(unknown, a, b);
            }
        }
    }

    for (int b = 0; b < kBands; b++) {
        if (band[b].empty())
            continue;
        const float t = static_cast<float>(b) / (kBands - 1);
        // near-hot: a bright warm origin cooling with each hop.
        const float rgba[4] = {1.0f - 0.55f * t, 0.85f - 0.30f * t,
                               0.35f + 0.45f * t, 0.9f - 0.25f * t};
        causal_->taint_solid.push_back(
            upload(band[b], GL_POINTS, rgba, 6.0f - 3.0f * t));
    }
    if (!hollow.empty()) {
        const float rgba[4] = {0.95f, 0.85f, 0.55f, 0.8f};
        causal_->taint_hollow = upload(hollow, GL_LINES, rgba);
    }
    if (!escape.empty()) {
        const float rgba[4] = {1.0f, 0.35f, 0.75f, 0.95f};
        causal_->taint_escape = upload(escape, GL_LINES, rgba);
    }
    if (!unknown.empty()) {
        // The UNKNOWN grey-violet, deliberately not on the depth ramp: it is
        // not a distance, it is an absence of evidence.
        const float rgba[4] = {0.70f, 0.66f, 0.85f, 0.85f};
        causal_->taint_unknown = upload(unknown, GL_LINES, rgba);
    }
}

// T4 (57-causal-layers): the blame convergence forest.
//
// **CONVERGENCE RIDES ON ITS OWN CHANNEL, NOT ON HEIGHT.** The terrain's
// vertical already encodes access density; putting a set-overlap count there
// too would make two unrelated quantities share one axis, which is exactly
// 46 G8's complaint. So a beacon is a POINT whose SIZE and BRIGHTNESS scale
// with weight, at plane level.
//
// **DEGRADE HONESTLY.** A single-cone recording (BlameForest::single_cone)
// has nothing to converge, so its beacons are drawn at the faint baseline and
// look like one bundle rather than like a finding. A born-untraced cone gets
// its own separate mark and CANNOT be part of a beacon, because its weight
// contribution is zero by construction in the builder.
void Scene::set_blame_forest(const space::BlameForest &forest) {
    ensure_causal();
    for (Prim &p : causal_->blame_beacons)
        free_prim(p);
    causal_->blame_beacons.clear();
    free_prim(causal_->blame_sinks);
    free_prim(causal_->blame_untraced);
    if (!forest.enabled)
        return;

    // Weight bands: 1 (the baseline) plus up to five above it. A convergence
    // of 2 and a convergence of 20 must look different; a recording with no
    // convergence at all must sit flat at the baseline.
    constexpr int kBands = 6;
    std::vector<float> band[kBands];
    const float top = forest.max_weight > 1
                          ? static_cast<float>(forest.max_weight - 1)
                          : 1.0f;
    for (const space::BlameConvergence &c : forest.producers) {
        int b = 0;
        if (c.weight > 1) {
            const float t = static_cast<float>(c.weight - 1) / top;
            b = 1 + static_cast<int>(t * (kBands - 2) + 0.5f);
            if (b >= kBands)
                b = kBands - 1;
        }
        band[b].insert(band[b].end(), {c.u, 0.006f, c.v});
    }
    for (int b = 0; b < kBands; b++) {
        if (band[b].empty())
            continue;
        const float t = static_cast<float>(b) / (kBands - 1);
        // The baseline (b == 0, weight 1) is deliberately faint: a step that
        // exactly one cone blames is not a shared root cause.
        const float rgba[4] = {0.55f + 0.45f * t, 0.60f + 0.35f * t,
                               0.95f - 0.35f * t, 0.35f + 0.60f * t};
        causal_->blame_beacons.push_back(
            upload(band[b], GL_POINTS, rgba, 3.0f + 9.0f * t));
    }

    std::vector<float> sinks, untraced;
    for (const space::BlameSink &sk : forest.sinks) {
        // An unplaceable sink has no cell and was counted by the placer; it
        // must emit nothing at all, not a ring at the plane origin.
        if (sk.u == 0.0f && sk.v == 0.0f && sk.cell == 0)
            continue;
        const float c[3] = {sk.u, 0.006f, sk.v};
        if (sk.born_untraced) {
            // Its OWN idiom — a bracket, not a ring — because "no traced
            // producer" is a verdict about provenance, not a weaker version
            // of a sink.
            const float r = 0.010f;
            const float a1[3] = {c[0] - r, c[1], c[2] - r};
            const float b1[3] = {c[0] - r, c[1], c[2] + r};
            const float a2[3] = {c[0] + r, c[1], c[2] - r};
            const float b2[3] = {c[0] + r, c[1], c[2] + r};
            push_seg(untraced, a1, b1);
            push_seg(untraced, a2, b2);
        } else {
            push_ring(sinks, c, 0.008f);
        }
    }
    if (!sinks.empty()) {
        const float rgba[4] = {0.95f, 0.95f, 0.75f, 0.85f};
        causal_->blame_sinks = upload(sinks, GL_LINES, rgba);
    }
    if (!untraced.empty()) {
        const float rgba[4] = {0.75f, 0.55f, 0.35f, 0.9f};
        causal_->blame_untraced = upload(untraced, GL_LINES, rgba);
    }
}

void Scene::draw_causal(const float mvp[16], const SceneLayers &layers) {
    if (!causal_ || !prog_traj_)
        return;
    const bool any_crossing =
        layers.crossings &&
        (!causal_->crossing_spurs.empty() || !causal_->crossing_glyphs.empty() ||
         causal_->crossing_hollow.count > 0);
    const bool any_taint =
        layers.taint &&
        (!causal_->taint_solid.empty() || causal_->taint_hollow.count > 0 ||
         causal_->taint_escape.count > 0 || causal_->taint_unknown.count > 0);
    const bool any_blame =
        layers.blame &&
        (!causal_->blame_beacons.empty() || causal_->blame_sinks.count > 0 ||
         causal_->blame_untraced.count > 0);
    if (!any_crossing && !any_taint && !any_blame)
        return;

    glUseProgram(prog_traj_);
    glUniformMatrix4fv(glGetUniformLocation(prog_traj_, "uMVP"), 1, GL_FALSE,
                       mvp);
    const GLint uColor = glGetUniformLocation(prog_traj_, "uColor");
    const GLint uPointSize = glGetUniformLocation(prog_traj_, "uPointSize");
    // uStipple=0: the stipple is this family's STATISTICAL mark and none of
    // these four layers is statistical (T3/T4 are exact-only by their own
    // fidelity notes; T5's survey fallback carries its own separate ink).
    glUniform1i(glGetUniformLocation(prog_traj_, "uStipple"), 0);
    glUniform1i(glGetUniformLocation(prog_traj_, "uHasHead"), 0);
    glUniform1i(glGetUniformLocation(prog_traj_, "uHasTimeCut"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // Every line in this TU is width 1.0 — restated at the draw so a future
    // edit that reaches for glLineWidth has to delete this line first.
    glLineWidth(1.0f);

    auto draw = [&](const Prim &p) {
        if (p.count == 0)
            return;
        glUniform4fv(uColor, 1, p.color);
        if (p.mode == GL_POINTS)
            glUniform1f(uPointSize, p.point_size);
        glBindVertexArray(p.vao);
        glDrawArrays(p.mode, 0, p.count);
    };

    if (any_crossing) {
        for (const Prim &p : causal_->crossing_spurs)
            draw(p);
        draw(causal_->crossing_hollow);
        for (const Prim &p : causal_->crossing_glyphs)
            draw(p);
    }
    if (any_taint) {
        for (const Prim &p : causal_->taint_solid)
            draw(p);
        draw(causal_->taint_hollow);
        draw(causal_->taint_escape);
        draw(causal_->taint_unknown);
    }
    if (any_blame) {
        for (const Prim &p : causal_->blame_beacons)
            draw(p);
        draw(causal_->blame_sinks);
        draw(causal_->blame_untraced);
    }

    glBindVertexArray(0);
    glUniform1f(uPointSize, 1.0f); // restore render()'s own default
    glDisable(GL_BLEND);
}

} // namespace asmdesk::scene3d
