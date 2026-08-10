// standalone_gl.cpp — the GL implementation of standalone_gl.h. Reaches GL the
// way scene.cpp does; includes no glfw3.h; links no engine (D4).
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <cmath>

#include "scene3d/standalone_gl.h"

namespace asmdesk::scene3d {

namespace {

// A per-vertex-colour line/point program. Deliberately its OWN pair rather
// than a reuse of kTrajVert/kTrajFrag: those take a UNIFORM colour (one hue
// per trajectory), and every scene here colours per vertex (a rib's register
// class, a byte's value hue, a lane's module band).
const char *kColVert = R"GLSL(#version 130
in vec3 pos;
in vec4 col;
uniform mat4 uMVP;
uniform float uPointSize;
out vec4 vCol;
void main(){
  vCol = col;
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
}
)GLSL";

const char *kColFrag = R"GLSL(#version 130
in vec4 vCol;
out vec4 frag;
void main(){ frag = vCol; }
)GLSL";

// The pick pass: "pos" + an integer "vid", writing the id straight out. The
// same shape as the plane's kPickPointVert/kPickPointFrag; kept local so this
// TU does not reach into shaders/embedded.h (which 55 is reworking).
const char *kIdVert = R"GLSL(#version 130
in vec3 pos;
in uint vid;
uniform mat4 uMVP;
uniform float uPointSize;
flat out uint fId;
void main(){
  fId = vid;
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
}
)GLSL";

const char *kIdFrag = R"GLSL(#version 130
flat in uint fId;
out uint fragid;
void main(){ fragid = fId; }
)GLSL";

// --- palettes ---------------------------------------------------------------
// Every literal below is a RENDERING choice over a fact the pure model already
// decided (a rib's RegClass, a cell's InvCellState, a byte's hue). Nothing here
// classifies anything.

void reg_class_rgba(RegClass c, float out[4]) {
    static const float pal[6][3] = {
        {0.95f, 0.85f, 0.25f}, // Gpr
        {0.45f, 0.80f, 1.00f}, // Vector
        {1.00f, 0.55f, 0.55f}, // Flags
        {0.65f, 1.00f, 0.55f}, // Pc
        {0.80f, 0.60f, 1.00f}, // Segment
        {0.65f, 0.65f, 0.70f}, // Other — deliberately grey: unclassed is not
                               // a colour-coded class, it is an abstention
    };
    const float *p = pal[static_cast<int>(c)];
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = 1.0f;
}

// hue in degrees -> rgb, full saturation/value (the prism's byte identity).
void hue_rgba(uint32_t hue, float alpha, float out[4]) {
    const float h = static_cast<float>(hue % 360u) / 60.0f;
    const int i = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);
    const float q = 1.0f - f, t = f;
    float r = 0, g = 0, b = 0;
    switch (i) {
    case 0: r = 1; g = t; b = 0; break;
    case 1: r = q; g = 1; b = 0; break;
    case 2: r = 0; g = 1; b = t; break;
    case 3: r = 0; g = q; b = 1; break;
    case 4: r = t; g = 0; b = 1; break;
    default: r = 1; g = 0; b = q; break;
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = alpha;
}

// A stable hue per module name, so a lane's band colour is the same in every
// lane (which is what makes a cross-thread residency pattern legible).
void module_rgba(const std::string &m, bool unknown, float out[4]) {
    if (unknown) {
        // Unknown gets its OWN hue and never borrows a resolved module's:
        // a desaturated slate that no hash below can produce.
        out[0] = 0.45f;
        out[1] = 0.45f;
        out[2] = 0.50f;
        out[3] = 1.0f;
        return;
    }
    uint32_t h = 2166136261u;
    for (char c : m) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    hue_rgba(h % 360u, 1.0f, out);
    // Pull it off full saturation so the unknown slate above never collides.
    for (int i = 0; i < 3; i++)
        out[i] = 0.35f + 0.65f * out[i];
}

constexpr float kWhite[4] = {0.92f, 0.92f, 0.95f, 1.0f};
constexpr float kDim[4] = {0.45f, 0.45f, 0.50f, 1.0f};
constexpr float kTorn[4] = {1.00f, 0.45f, 0.35f, 1.0f};
constexpr float kPillar[4] = {1.00f, 0.95f, 0.55f, 1.0f};

} // namespace

StandaloneRenderer::~StandaloneRenderer() { shutdown(); }

bool StandaloneRenderer::init_gl(std::string *err) {
    if (ready_)
        return true;
    std::string e;
    prog_col_ = gl_link_program(kColVert, kColFrag, /*has_vid=*/false,
                                /*pick=*/false, e, /*has_col=*/true);
    if (!prog_col_) {
        err_ = e;
        if (err)
            *err = e;
        return false;
    }
    prog_pick_ = gl_link_program(kIdVert, kIdFrag, /*has_vid=*/true,
                                 /*pick=*/true, e);
    if (!prog_pick_) {
        err_ = e;
        if (err)
            *err = e;
        return false;
    }
    glGenVertexArrays(1, &vao_lines_);
    glGenBuffers(1, &vbo_lines_);
    glGenVertexArrays(1, &vao_hits_);
    glGenBuffers(1, &vbo_hits_);
    glGenVertexArrays(1, &vao_tris_);
    glGenBuffers(1, &vbo_tris_);
    ready_ = true;
    return true;
}

void StandaloneRenderer::shutdown() {
    free_geometry();
    if (vbo_lines_)
        glDeleteBuffers(1, &vbo_lines_);
    if (vao_lines_)
        glDeleteVertexArrays(1, &vao_lines_);
    if (vbo_hits_)
        glDeleteBuffers(1, &vbo_hits_);
    if (vao_hits_)
        glDeleteVertexArrays(1, &vao_hits_);
    if (vbo_tris_)
        glDeleteBuffers(1, &vbo_tris_);
    if (vao_tris_)
        glDeleteVertexArrays(1, &vao_tris_);
    vbo_lines_ = vao_lines_ = vbo_hits_ = vao_hits_ = 0;
    vbo_tris_ = vao_tris_ = 0;
    for (unsigned p : {prog_col_, prog_pick_})
        if (p)
            glDeleteProgram(p);
    prog_col_ = prog_pick_ = 0;
    pick_target_.free_gl();
    ready_ = false;
}

void StandaloneRenderer::free_geometry() {
    lines_.clear();
    hits_.clear();
    tris_.clear();
    line_count_ = hit_count_ = tri_count_ = 0;
    bands_ = PickBands{};
}

void StandaloneRenderer::tri(const float p0[3], const float p1[3],
                             const float p2[3], const float c[4]) {
    for (const float *p : {p0, p1, p2})
        tris_.push_back(Vtx{p[0], p[1], p[2], c[0], c[1], c[2], c[3]});
}

// The session flow's geometry: one smooth ridge surface per row (a top strip
// over the bucket heights — a SURFACE, deliberately not per-event marks) plus
// translucent seam walls. Everything sits in the unit cube like the other
// standalone kinds; row hues mirror the strip's palettes so a thread keeps
// one colour across every surface.
void StandaloneRenderer::build_flow(const space::SessionFlowScene &f) {
    if (!f.enabled || f.rows.empty())
        return;
    static const float kTid[6][4] = {
        {0.95f, 0.85f, 0.25f, 0.9f}, {0.40f, 0.80f, 1.00f, 0.9f},
        {0.55f, 0.95f, 0.45f, 0.9f}, {1.00f, 0.55f, 0.55f, 0.9f},
        {0.80f, 0.60f, 1.00f, 0.9f}, {0.35f, 0.95f, 0.85f, 0.9f}};
    // SyscallClass hue table, index = class value (File..Other), the strip
    // painter's own values; [6] doubles as the grey none/tie bucket.
    static const float kCls[7][4] = {
        {0.34f, 0.61f, 0.84f, 0.9f}, {0.31f, 0.79f, 0.69f, 0.9f},
        {0.85f, 0.63f, 0.87f, 0.9f}, {0.86f, 0.80f, 0.49f, 0.9f},
        {0.88f, 0.42f, 0.46f, 0.9f}, {0.60f, 0.76f, 0.47f, 0.9f},
        {0.50f, 0.50f, 0.50f, 0.9f}};
    static const float kAgg[4] = {0.55f, 0.55f, 0.58f, 0.85f};
    static const float kMem[4] = {0.42f, 0.62f, 0.86f, 0.85f};
    const size_t nr = f.rows.size();
    const float y0 = 0.05f, yspan = 0.85f;
    const float zlo = 0.08f, zhi = 0.92f;
    const float slot = (zhi - zlo) / static_cast<float>(nr);
    const float half = slot * 0.38f;
    const uint32_t nb = f.buckets;
    for (size_t i = 0; i < nr; i++) {
        const space::FlowRow &r = f.rows[i];
        const float z = zlo + slot * (static_cast<float>(i) + 0.5f);
        for (uint32_t b = 0; b + 1 < nb; b++) {
            const float x0 = (static_cast<float>(b) + 0.5f) /
                             static_cast<float>(nb);
            const float x1 = (static_cast<float>(b) + 1.5f) /
                             static_cast<float>(nb);
            const float h0 = y0 + r.heights[b] * yspan;
            const float h1 = y0 + r.heights[b + 1] * yspan;
            if (r.counts[b] == 0 && r.counts[b + 1] == 0)
                continue; // a silent stretch stays flat AND unpainted —
                          // absence reads as absence, not as a floor
            const float *c = kAgg;
            float cls_c[4];
            switch (r.kind) {
            case space::FlowRowKind::Lane:
                c = kTid[r.lane_ord % 6];
                break;
            case space::FlowRowKind::AggregateLanes:
                c = kAgg;
                break;
            case space::FlowRowKind::Kernel: {
                const uint8_t cl = r.bucket_class[b];
                const float *src = cl ? kCls[(cl - 1) % 7] : kCls[6];
                for (int k = 0; k < 4; k++)
                    cls_c[k] = src[k];
                c = cls_c;
                break;
            }
            case space::FlowRowKind::Memory:
                c = kMem;
                break;
            }
            const float a0[3] = {x0, h0, z - half};
            const float a1[3] = {x0, h0, z + half};
            const float b0[3] = {x1, h1, z - half};
            const float b1[3] = {x1, h1, z + half};
            tri(a0, a1, b0, c);
            tri(a1, b1, b0, c);
        }
        // the row's hit proxy, at its crest
        float crest = 0.0f;
        for (uint32_t b = 0; b < nb; b++)
            crest = std::max(crest, r.heights[b]);
        hit(0.5f, y0 + crest * yspan + 0.02f, z, i);
    }
    static const float kSeam[4] = {0.90f, 0.88f, 0.55f, 0.28f};
    for (size_t j = 0; j < f.seams.size(); j++) {
        const float x = (static_cast<float>(f.seams[j].bucket) + 0.5f) /
                        static_cast<float>(nb);
        const float w0[3] = {x, y0, zlo};
        const float w1[3] = {x, y0, zhi};
        const float w2[3] = {x, y0 + yspan, zlo};
        const float w3[3] = {x, y0 + yspan, zhi};
        tri(w0, w1, w2, kSeam);
        tri(w1, w3, w2, kSeam);
        hit(x, y0 + yspan + 0.02f, 0.5f, nr + j);
    }
}

void StandaloneRenderer::line(float x0, float y0, float z0, float x1, float y1,
                              float z1, const float c[4]) {
    lines_.push_back(Vtx{x0, y0, z0, c[0], c[1], c[2], c[3]});
    lines_.push_back(Vtx{x1, y1, z1, c[0], c[1], c[2], c[3]});
}

// An open box outline (four verticals + two rings) — the "solid column" idiom
// every scene here uses. A filled quad would need translucency ordering this
// brief does not own (55 T4 defers OIT), and an outline reads honestly at
// line width 1.0, which is the only width a core context guarantees.
void StandaloneRenderer::box(float cx, float cy0, float cy1, float cz,
                             float half, const float c[4]) {
    const float x0 = cx - half, x1 = cx + half;
    const float z0 = cz - half, z1 = cz + half;
    for (float y : {cy0, cy1}) {
        line(x0, y, z0, x1, y, z0, c);
        line(x1, y, z0, x1, y, z1, c);
        line(x1, y, z1, x0, y, z1, c);
        line(x0, y, z1, x0, y, z0, c);
    }
    line(x0, cy0, z0, x0, cy1, z0, c);
    line(x1, cy0, z0, x1, cy1, z0, c);
    line(x1, cy0, z1, x1, cy1, z1, c);
    line(x0, cy0, z1, x0, cy1, z1, c);
}

void StandaloneRenderer::hit(float x, float y, float z, uint64_t index) {
    const uint32_t id = pick_id_element(kind_, index);
    if (id == 0)
        return; // past the band's capacity: emit nothing, never a wrapped id
    hits_.push_back(PickVtx{x, y, z, id});
}

// --- T2: divergence worldline ----------------------------------------------
// Y = execution step (normalised to [0,1] over the scene's own extent, so the
// camera framing below is kind-independent). X: the fused tube at 0.5, the A
// ribbon at 0.25, the B ribbon at 0.75. Z: a rib's register-class lane.
void StandaloneRenderer::build_divergence(const DivergenceScene &s) {
    if (s.refused)
        return; // the refusal card is ImGui chrome; there is NO geometry
    const float top = static_cast<float>(std::max(s.top_step(), 1u));
    auto ny = [top](uint32_t step) { return static_cast<float>(step) / top; };

    const float y_fork = ny(s.shared_steps);
    // The fused shared prefix.
    line(0.5f, 0.0f, 0.5f, 0.5f, y_fork, 0.5f, kWhite);
    if (s.bounded) {
        // TORN cap: a ragged fan, never a clean terminus. "We stopped looking"
        // and "they agreed to the end" are different claims and get different
        // marks. (A wider stroke would read better here; width 1.0 only.)
        for (int i = 0; i < 6; i++) {
            const float a = 0.9f * static_cast<float>(i) / 6.0f * 6.2831853f;
            line(0.5f, y_fork, 0.5f, 0.5f + 0.05f * std::cos(a),
                 y_fork + 0.03f, 0.5f + 0.05f * std::sin(a), kTorn);
        }
    }

    if (s.diverged) {
        // Patient zero: a bright pillar at the fork.
        box(0.5f, y_fork - 0.02f, y_fork + 0.06f, 0.5f, 0.03f, kPillar);
        hit(0.5f, y_fork + 0.06f, 0.5f, 0);
        // The two ribbons, from the fork to each side's own recorded extent.
        line(0.5f, y_fork, 0.5f, 0.25f, y_fork, 0.5f, kWhite);
        line(0.5f, y_fork, 0.5f, 0.75f, y_fork, 0.5f, kWhite);
        line(0.25f, y_fork, 0.5f, 0.25f, ny(s.steps_a), 0.5f, kWhite);
        line(0.75f, y_fork, 0.5f, 0.75f, ny(s.steps_b), 0.5f, kWhite);
    }

    const uint64_t base = s.diverged ? 1u : 0u; // the fork took index 0
    for (uint64_t i = 0; i < s.ribs.size(); i++) {
        const DivRib &r = s.ribs[i];
        float col[4];
        reg_class_rgba(r.cls, col);
        const float y = ny(r.step);
        // Z lane by register class, so the class is positional as well as
        // hued (the scene's declared Z axis).
        const float z = 0.5f + 0.06f * (static_cast<float>(r.cls) - 2.5f);
        if (r.hollow) {
            // HOLLOW: an outline at a MINIMUM width, never a zero-width rib.
            // A zero-width rib would render "we did not look" as "they
            // agreed", which is the exact misreading StateDelta forbids.
            const float h = 0.012f;
            line(0.25f, y - h, z, 0.75f, y - h, z, kDim);
            line(0.25f, y + h, z, 0.75f, y + h, z, kDim);
            line(0.25f, y - h, z, 0.25f, y + h, z, kDim);
            line(0.75f, y - h, z, 0.75f, y + h, z, kDim);
        } else {
            // Thickness = the number of disagreeing registers, drawn as
            // stacked strands (a core context cannot widen a line).
            const uint32_t strands = std::min<uint32_t>(
                std::max<uint32_t>(r.changed, 1u), 8u);
            for (uint32_t k = 0; k < strands; k++) {
                const float dy = 0.002f * static_cast<float>(k);
                line(0.25f, y + dy, z, 0.75f, y + dy, z, col);
            }
        }
        hit(0.5f, y, z, base + i);
    }
}

// --- T3: invocation stack ---------------------------------------------------
// X/Z stay the Hilbert plane (a block's projected u/v); Y is the INVOCATION
// INDEX — a discrete ordinal, one slab per call, never scrubbed as time.
void StandaloneRenderer::build_invocation(const InvocationScene &s) {
    const float nslab = static_cast<float>(std::max<size_t>(s.slabs.size(), 1));
    uint32_t max_count = 1;
    for (const InvSlab &sl : s.slabs)
        for (const InvCell &c : sl.cells)
            if (c.state == InvCellState::Counted)
                max_count = std::max(max_count, c.count);

    uint64_t index = 0;
    for (size_t i = 0; i < s.slabs.size(); i++) {
        const InvSlab &sl = s.slabs[i];
        const float y = static_cast<float>(i) / nslab;
        // The slab's own frame — a thin ring at its level, so a discrete
        // ordinal reads as a discrete SHELF rather than as a continuum.
        const float *frame = sl.closed ? kWhite : kTorn;
        line(0.0f, y, 0.0f, 1.0f, y, 0.0f, frame);
        line(1.0f, y, 0.0f, 1.0f, y, 1.0f, frame);
        line(1.0f, y, 1.0f, 0.0f, y, 1.0f, frame);
        line(0.0f, y, 1.0f, 0.0f, y, 0.0f, frame);
        if (!sl.closed) {
            // FRAYED: an open invocation is a PREFIX. Ragged spurs off the
            // slab's edge say so geometrically, not only in the legend.
            for (int k = 0; k < 5; k++) {
                const float x = 0.1f + 0.2f * static_cast<float>(k);
                line(x, y, 1.0f, x + 0.03f, y + 0.02f, 1.06f, kTorn);
            }
        }
        for (size_t j = 0; j < sl.cells.size(); j++) {
            const InvCell &c = sl.cells[j];
            if (c.state == InvCellState::Absent)
                continue; // a HOLE: nothing is drawn, and nothing is pickable
            // Fall back to an ascending strip when no projection placed the
            // block — an honest layout, never a fabricated address.
            const float u = c.placed ? c.u
                                     : (s.blocks.empty()
                                            ? 0.5f
                                            : static_cast<float>(j) /
                                                  static_cast<float>(
                                                      s.blocks.size()));
            const float v = c.placed ? c.v : 0.5f;
            float col[4];
            if (c.state == InvCellState::Unknown) {
                // Unknown count: a dim outline at a FIXED token height, never
                // a zero-height column (which would read as "never ran").
                col[0] = kDim[0];
                col[1] = kDim[1];
                col[2] = kDim[2];
                col[3] = kDim[3];
                box(u, y, y + 0.02f, v, 0.006f, col);
            } else {
                // Height + opacity = this block's entry count in THIS
                // invocation; hue = the codeimage version, so a slab executed
                // after a JIT rewrite is visibly a different code body.
                const float frac = static_cast<float>(c.count) /
                                   static_cast<float>(max_count);
                if (sl.codeimage_known)
                    hue_rgba((30u + 70u * static_cast<uint32_t>(
                                              sl.codeimage_version)) % 360u,
                             0.4f + 0.6f * frac, col);
                else {
                    col[0] = kWhite[0];
                    col[1] = kWhite[1];
                    col[2] = kWhite[2];
                    col[3] = 0.4f + 0.6f * frac;
                }
                box(u, y, y + 0.02f + 0.06f * frac, v, 0.006f, col);
            }
            hit(u, y + 0.03f, v, index);
            index++;
        }
    }
}

// --- T4: module excursion ribbon --------------------------------------------
// X = call order, Y = call depth, Z = one sheet per tid. A seam (the module
// changing) is brightened; the lane floor is marked where the capture capped.
void StandaloneRenderer::build_ribbon(const ModuleRibbonScene &s) {
    if (s.lanes.empty())
        return;
    uint32_t max_order = 1;
    int max_depth = 1;
    for (const RibbonLane &l : s.lanes) {
        max_order = std::max<uint32_t>(
            max_order, static_cast<uint32_t>(l.segs.size()));
        max_depth = std::max(max_depth, l.max_depth + 1);
    }
    const float nz = static_cast<float>(std::max<size_t>(s.lanes.size(), 1));

    uint64_t index = 0;
    for (size_t li = 0; li < s.lanes.size(); li++) {
        const RibbonLane &lane = s.lanes[li];
        const float z = nz <= 1.0f ? 0.5f : static_cast<float>(li) / (nz - 1.0f);
        for (size_t si = 0; si < lane.segs.size(); si++) {
            const RibbonSeg &g = lane.segs[si];
            const float x0 =
                static_cast<float>(g.order) / static_cast<float>(max_order);
            const float x1 = static_cast<float>(g.order + 1) /
                             static_cast<float>(max_order);
            const float y =
                1.0f - static_cast<float>(g.depth) /
                           static_cast<float>(max_depth); // depth grows DOWN
            float col[4];
            module_rgba(g.module, g.module_unknown, col);
            line(x0, y, z, x1, y, z, col);
            if (si + 1 < lane.segs.size()) {
                // The connector to the next call — the only place a descent
                // or a return is drawn, and it is drawn from the RECORDED
                // depth change, never inferred from anything else.
                const RibbonSeg &nx = lane.segs[si + 1];
                const float ny_ = 1.0f - static_cast<float>(nx.depth) /
                                             static_cast<float>(max_depth);
                line(x1, y, z, x1, ny_, z, kDim);
            }
            if (g.boundary) {
                // The seam is the finding: a bright vertical tick at the
                // colour change. (A wider stroke would read better; 1.0 only.)
                line(x0, y - 0.04f, z, x0, y + 0.04f, z, kWhite);
            }
            if (g.at_cap) {
                // CAPPED: a clipped edge, not a real bottom. Drawn as a
                // dashed floor under the segment.
                for (int k = 0; k < 3; k++) {
                    const float a = x0 + (x1 - x0) * (0.1f + 0.3f * k);
                    line(a, y - 0.02f, z, a + (x1 - x0) * 0.15f, y - 0.02f, z,
                         kTorn);
                }
            }
            hit(0.5f * (x0 + x1), y, z, index);
            index++;
        }
    }
}

// --- T5: SIMD lane prism ----------------------------------------------------
// X = byte/element index, Y = byte magnitude, Z = stacked writes. Colour is the
// pure model's stable value->hue hash, which is what makes a permutation
// legible: a recorded byte keeps its hue as it moves lanes.
void StandaloneRenderer::build_prism(const LanePrismScene &s) {
    if (s.writes.empty())
        return;
    const float nz = static_cast<float>(std::max<size_t>(s.writes.size(), 1));
    uint64_t index = 0;
    for (size_t wi = 0; wi < s.writes.size(); wi++) {
        const PrismWrite &w = s.writes[wi];
        const float z = static_cast<float>(w.z) / nz;
        if (w.wireframe) {
            // [wide] WIREFRAME: the extent of an uncaptured buffer, never
            // zero bars (which would claim the register held zeros).
            const uint32_t lanes = w.size > 0 ? w.size : 16u;
            for (uint32_t i = 0; i <= lanes; i++) {
                const float x = static_cast<float>(i) /
                                static_cast<float>(lanes);
                line(x, 0.0f, z, x, 0.12f, z, kDim);
            }
            line(0.0f, 0.0f, z, 1.0f, 0.0f, z, kDim);
            line(0.0f, 0.12f, z, 1.0f, 0.12f, z, kDim);
            continue; // nothing recorded here, so nothing is pickable
        }
        const float nb = static_cast<float>(std::max<size_t>(w.bytes.size(), 1));
        for (size_t bi = 0; bi < w.bytes.size(); bi++) {
            const float x = static_cast<float>(bi) / nb;
            const float xw = 1.0f / nb;
            const float h = static_cast<float>(w.bytes[bi].value) / 255.0f;
            float col[4];
            hue_rgba(w.bytes[bi].hue, w.value_valid ? 1.0f : 0.35f, col);
            if (w.value_valid) {
                box(x + 0.5f * xw, 0.0f, h, z, 0.4f * xw, col);
            } else {
                // HOLLOW: value_valid == false. The bytes are the container's
                // shape, not a captured value.
                line(x + 0.5f * xw, 0.0f, z, x + 0.5f * xw, h, z, col);
            }
            // A lane boundary tick, only where the width is KNOWN (one of the
            // four measured LanesN verdicts, 2026-08-08 revised plan). For
            // NoLaneSemantics/Unknown, no boundary is drawn at all — the
            // model's no_lane_note/width_note say why, and a drawn boundary
            // would look measured either way.
            if (prism_width_known(w.width) && w.lane_bytes > 1 &&
                bi % w.lane_bytes == 0)
                line(x, 0.0f, z - 0.01f, x, 1.05f, z - 0.01f, kWhite);
            hit(x + 0.5f * xw, h, z, index);
            index++;
        }
    }
}

void StandaloneRenderer::upload(const StandaloneFrame &f) {
    if (!ready_)
        return;
    free_geometry();
    kind_ = f.kind;
    switch (f.kind) {
    case SceneKind::Plane:
        break; // not this renderer's substrate
    case SceneKind::Divergence:
        if (f.divergence)
            build_divergence(*f.divergence);
        break;
    case SceneKind::Invocation:
        if (f.invocation)
            build_invocation(*f.invocation);
        break;
    case SceneKind::ModuleRibbon:
        if (f.ribbon)
            build_ribbon(*f.ribbon);
        break;
    case SceneKind::LanePrism:
        if (f.prism)
            build_prism(*f.prism);
        break;
    case SceneKind::SessionFlow:
        if (f.flow)
            build_flow(*f.flow);
        break;
    }
    bands_.kind = kind_;
    bands_.nelem = static_cast<uint64_t>(hits_.size());
    flush_geometry();
}

void StandaloneRenderer::flush_geometry() {
    line_count_ = static_cast<int>(lines_.size());
    hit_count_ = static_cast<int>(hits_.size());
    glBindVertexArray(vao_lines_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_lines_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(lines_.size() * sizeof(Vtx)),
                 lines_.empty() ? nullptr : lines_.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          (void *)0);
    glEnableVertexAttribArray(kAttrCol);
    glVertexAttribPointer(kAttrCol, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          (void *)(sizeof(float) * 3));
    tri_count_ = static_cast<int>(tris_.size());
    glBindVertexArray(vao_tris_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_tris_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(tris_.size() * sizeof(Vtx)),
                 tris_.empty() ? nullptr : tris_.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          (void *)0);
    glEnableVertexAttribArray(kAttrCol);
    glVertexAttribPointer(kAttrCol, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx),
                          (void *)(sizeof(float) * 3));
    glBindVertexArray(vao_hits_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_hits_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(hits_.size() * sizeof(PickVtx)),
                 hits_.empty() ? nullptr : hits_.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, sizeof(PickVtx),
                          (void *)0);
    glEnableVertexAttribArray(kAttrVid);
    glVertexAttribIPointer(kAttrVid, 1, GL_UNSIGNED_INT, sizeof(PickVtx),
                           (void *)(sizeof(float) * 3));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void StandaloneRenderer::render(const Camera &cam, int fbw, int fbh) {
    if (!ready_ || fbw <= 0 || fbh <= 0 ||
        (line_count_ == 0 && tri_count_ == 0))
        return;
    float mvp[16];
    cam.mvp(mvp, static_cast<float>(fbw) / fbh);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog_col_);
    glUniformMatrix4fv(glGetUniformLocation(prog_col_, "uMVP"), 1, GL_FALSE,
                       mvp);
    glUniform1f(glGetUniformLocation(prog_col_, "uPointSize"), 1.0f);
    // Line width stays at the spec's guaranteed floor: glLineWidth(>1) is
    // invalid on a core forward-compatible context (55 T6).
    glLineWidth(1.0f);
    if (tri_count_ > 0) {
        // the flow's surfaces draw first so its seam walls blend over them
        glBindVertexArray(vao_tris_);
        glDrawArrays(GL_TRIANGLES, 0, tri_count_);
    }
    glBindVertexArray(vao_lines_);
    glDrawArrays(GL_LINES, 0, line_count_);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

uint32_t StandaloneRenderer::pick(const Camera &cam, int fbw, int fbh, int x,
                                  int y) {
    if (!ready_ || hit_count_ == 0 || fbw <= 0 || fbh <= 0)
        return 0;
    const int prev = pick_target_.bind_and_clear(fbw, fbh);
    float mvp[16];
    cam.mvp(mvp, static_cast<float>(fbw) / fbh);
    glUseProgram(prog_pick_);
    glUniformMatrix4fv(glGetUniformLocation(prog_pick_, "uMVP"), 1, GL_FALSE,
                       mvp);
    // Drawn larger than they render, so a near-element click resolves — the
    // same reason the plane's pick points are enlarged.
    glUniform1f(glGetUniformLocation(prog_pick_, "uPointSize"), 7.0f);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(vao_hits_);
    glDrawArrays(GL_POINTS, 0, hit_count_);
    glBindVertexArray(0);
    glDisable(GL_PROGRAM_POINT_SIZE);
    const uint32_t id = pick_target_.read_pixel(x, y, fbh);
    GlPickTarget::restore(prev);
    return id;
}

void StandaloneRenderer::render_pick_buffer(const Camera &cam, int fbw, int fbh,
                                            std::vector<uint32_t> &out) {
    out.assign(static_cast<size_t>(fbw) * fbh, 0u);
    if (!ready_ || hit_count_ == 0 || fbw <= 0 || fbh <= 0)
        return;
    const int prev = pick_target_.bind_and_clear(fbw, fbh);
    float mvp[16];
    cam.mvp(mvp, static_cast<float>(fbw) / fbh);
    glUseProgram(prog_pick_);
    glUniformMatrix4fv(glGetUniformLocation(prog_pick_, "uMVP"), 1, GL_FALSE,
                       mvp);
    glUniform1f(glGetUniformLocation(prog_pick_, "uPointSize"), 7.0f);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(vao_hits_);
    glDrawArrays(GL_POINTS, 0, hit_count_);
    glBindVertexArray(0);
    glDisable(GL_PROGRAM_POINT_SIZE);
    pick_target_.read_all(fbw, fbh, out);
    GlPickTarget::restore(prev);
}

} // namespace asmdesk::scene3d
