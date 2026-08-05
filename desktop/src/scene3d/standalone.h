// standalone.h — the four scenes whose load-bearing axis is NOT an address
// (docs/internal/gui/59-standalone-scenes.md T2–T5), as PURE MODELS.
//
// This is the model half, in the same split every view in this tree uses: no
// GL, no ImGui, no engine (D4) — a builder plus a deterministic dump, so every
// fidelity rule below is asserted headlessly by test_standalone.cpp and the GL
// half (standalone_gl.cpp) only turns these structs into vertices.
//
//   T2 DivergenceScene    two recordings, one shared prefix, then a fork.
//                         Y = execution step.
//   T3 InvocationScene    one routine's control flow ACROSS its calls.
//                         Y = invocation #, a discrete ordinal.
//   T4 ModuleRibbonScene  which thread is in which library, when.
//                         X = call order, Y = call depth, Z = tid lane.
//   T5 LanePrismScene     inside one wide vector register, over its writes.
//                         X = byte index, Y = magnitude, Z = stacked writes.
//
// Each names its axes through scene3d::scene_axes(SceneKind) — the axis
// contract lives with the KIND, not with the model, so a scene cannot ship an
// unlabelled axis (T1 step 4).
//
// **These do not compose.** Nothing here shares a coordinate with the address
// plane or with each other; drawing two together would put two meanings on one
// screen position.
#ifndef ASMDESK_SCENE3D_STANDALONE_H
#define ASMDESK_SCENE3D_STANDALONE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "analysis/diff.h" // dt_divergence / dt_statediff — T2's whole source
#include "doc/streams.h"
#include "nav.h"
#include "scene3d/camera.h" // standalone_default_camera's return type
#include "scene3d/scene_kind.h"
#include "space/projection.h" // T3: the slab's X/Z stays the Hilbert plane
#include "views/region.h"     // T3: RegionInvocation
#include "views/tree.h"       // T4: TreeRow / TreeView

namespace asmdesk::scene3d {

// ===========================================================================
// T2 — divergence worldline
// ===========================================================================

// A rib's register class, derived from the register NAME the statediff carries
// (the recording states names, not classes). `Other` is the ABSTAIN value, not
// a bucket of last resort: a class this file cannot name is drawn as unclassed,
// never merged into a class it might not belong to.
enum class RegClass : uint8_t { Gpr, Vector, Flags, Pc, Segment, Other };
const char *reg_class_name(RegClass);
// Pure, name-derived, guest-agnostic (x86 + arm64 spellings). Case-insensitive.
RegClass reg_class_of(const std::string &reg_name);

// One horizontal rib between the A and B ribbons, at one execution step.
struct DivRib {
    uint32_t step = 0;
    // Thickness: how many registers disagree at this step. 0 is legal ONLY on
    // a hollow rib (see below) — a 0-thickness solid rib would assert "nothing
    // changed", which is exactly the misreading StateDelta's own comment
    // forbids.
    uint32_t changed = 0;
    // HOLLOW: at least one side had no COMPUTED delta here (the first held
    // step, or across an evicted boundary), so agreement was never observed.
    // The renderer draws a hollow outline at a MINIMUM width — never a
    // zero-width rib, which would render "we did not look" as "they agreed".
    bool hollow = false;
    RegClass cls = RegClass::Other; // the Z lane + the hue
    std::vector<std::string> regs;  // the diverging names, sorted (may be empty
                                    // on a hollow rib — that is the point)
};

struct DivergenceScene {
    // The admission gate is a REFUSAL CARD, not a silent empty scene: a failed
    // code_sha / basis / arch check sets these and emits NO geometry, the same
    // gate (and the same words) the diff view already applies.
    bool refused = false;
    std::string refusal;

    bool diverged = false;
    // TORN: at least one recording was truncated, so the shared tube ends
    // because we STOPPED LOOKING, not because the two agreed to the end. The
    // renderer caps it torn; a clean proven-identical terminus is a different
    // claim and gets a different cap.
    bool bounded = false;
    uint32_t fork_step = 0;      // patient zero; meaningless when !diverged
    uint64_t off_a = 0, off_b = 0; // the two sides' offsets at the fork

    uint32_t shared_steps = 0; // the fused tube's height
    uint32_t steps_a = 0, steps_b = 0; // each ribbon's own recorded extent

    std::vector<DivRib> ribs; // ascending by step; empty is a stated fact
    // Why there are no ribs, when there are none. NEVER empty-and-silent: an
    // absent rib field must say whether statediff was missing or refused.
    std::string rib_note;
    std::string identity_note; // dt_diff_build's caveat, carried verbatim
    std::string rec_a, rec_b;  // the two recording ids (the drill-in's keys)

    // The scene's vertical extent, in execution steps.
    uint32_t top_step() const;
    // How many ribs are hollow — the fidelity count the HUD states.
    uint32_t hollow_ribs() const;
};

DivergenceScene build_divergence_scene(const Streams &a, const Streams &b);
std::string divergence_scene_dump(const DivergenceScene &s);

// The pickable elements of a divergence scene, in the SAME order the GL half
// uploads ids for (pick_id_element's index space): the fork pillar first (when
// there is one), then every rib in `ribs` order.
enum class DivElemKind : uint8_t { Fork, Rib };
struct DivElem {
    DivElemKind kind = DivElemKind::Rib;
    uint64_t rib = 0; // index into DivergenceScene::ribs, for Rib
};
std::vector<DivElem> divergence_pick_order(const DivergenceScene &s);
// Where a click on element `index` goes: the fork pillar opens the DIFF view's
// patient-zero row (v=diff, rec, rec_b, step); a rib opens the SLICE explorer
// at its step. nullopt for an index past the live geometry.
std::optional<dt_link> divergence_pick_link(const DivergenceScene &s,
                                            uint64_t index);

// ===========================================================================
// T3 — invocation stack
// ===========================================================================

// A cell of one slab. THREE states, because two would force a lie:
//   Absent  — this block is not in this invocation's block set: a HOLE in the
//             slab, never a zero-height column ("it did not run here").
//   Counted — present, with an exact entry count.
//   Unknown — present in the coverage footer, but the recorded instruction
//             stream never described it (a capped trace buffer). The count is
//             UNKNOWN, never 0. "Exact only" (the brief's own fidelity rule).
enum class InvCellState : uint8_t { Absent, Counted, Unknown };

struct InvCell {
    uint64_t block = 0; // the block's offset, in the recording's basis
    InvCellState state = InvCellState::Absent;
    uint32_t count = 0; // entries of this block in THIS invocation (Counted)
    bool placed = false; // the Hilbert plane resolved this block's X/Z
    float u = 0.0f, v = 0.0f;
};

struct InvSlab {
    uint32_t number = 1; // 1-based invocation number — dt_link::invocation
    // FRAYED: no `coverage` footer closed this invocation, so what is shown is
    // a PREFIX of a call that was still running. Labelled, never rendered as a
    // short invocation that simply did less.
    bool closed = false;
    bool truncated = false; // this invocation's own coverage said so
    // The codeimage version in force when this invocation started, so a slab
    // executed after a JIT rewrite is visibly a different code body.
    // `codeimage_known` false means the recording carries no codeimage
    // timeline at all — the slab is untinted rather than tinted "version 0".
    bool codeimage_known = false;
    uint64_t codeimage_version = 0;
    std::vector<InvCell> cells; // parallel to InvocationScene::blocks
};

struct InvocationScene {
    // The UNION block set across every invocation, ascending — the slab's
    // shared X/Z, which is what makes a missing block read as a hole rather
    // than as a differently-shaped slab.
    std::vector<uint64_t> blocks;
    std::vector<InvSlab> slabs; // in call order, oldest first

    std::string basis;
    std::string basis_error; // refuse rather than mix "rel" and absolute
    std::string note;        // why the scene is empty, when it is
    // The legend line that keeps this scene distinct from a LOOP HELIX — the
    // two produce similar-looking stacks and answer different questions.
    static const char *legend();
    // Counts, for the HUD: holes and unknown-count cells are both fidelity
    // facts, and they are DIFFERENT facts.
    uint32_t holes = 0;
    uint32_t unknown_cells = 0;
    uint32_t open_slabs = 0; // frayed prefixes

    // True when this scene has geometry a user can actually SEE. A slab with no
    // cells is a wireframe with nothing in it: `blocks` is the slab's shared
    // X/Z, and it is empty when no `coverage` event ever described a block set,
    // so every cell loop runs zero times. The availability gate must ask this
    // rather than `slabs.empty()`, which only answers "did the builder produce
    // a container" — a question whose answer is yes for a scene that draws a
    // frame ring around nothing.
    bool drawable() const { return !slabs.empty() && !blocks.empty(); }
};

// `proj` (optional) keeps the slab's X/Z on the Hilbert address plane; without
// it a cell is `placed == false` and the renderer lays the blocks out in
// ascending order along X instead — an honest fallback, never a fabricated
// address. `codeimage_versions` is the recording's own codeimage timeline
// (RegionInvocation::codeimage_version); it rides the RegionView.
InvocationScene build_invocation_scene(const RegionView &rv,
                                       const space::Projection *proj = nullptr);
std::string invocation_scene_dump(const InvocationScene &s);

// Pickable elements, in upload order: every (slab, cell) pair whose state is
// not Absent — a hole is not pickable, because there is nothing there.
struct InvElem {
    uint64_t slab = 0;
    uint64_t cell = 0;
};
std::vector<InvElem> invocation_pick_order(const InvocationScene &s);
// A slab cell opens the REGION view at that invocation number and offset,
// through dt_link::invocation (54 T6). It never touches dt_link::step, which
// means a DATAFLOW STEP INDEX — that conflation is the trap 46 §4 forbids.
std::optional<dt_link> invocation_pick_link(const InvocationScene &s,
                                            const std::string &rec,
                                            uint64_t index);

// ===========================================================================
// T4 — module excursion ribbon
// ===========================================================================

// The label an unresolved module gets. Never blank, and never merged into a
// resolved module's band — "we could not resolve this" is a different fact
// from "this is libc".
const char *ribbon_unknown_module();

struct RibbonSeg {
    uint64_t seq = 0;   // Event::seq — the recording's own stream position
    uint32_t order = 0; // this lane's own 0-based call index (the drawn X)
    int depth = 0;      // the engine's EFFECTIVE (focus-rebased) depth
    uint64_t addr = 0;
    std::string name;
    std::string module; // ribbon_unknown_module() where unresolved
    bool module_unknown = false;
    // A boundary crossing: the module changed from the previous segment of
    // THIS lane. That seam is the layer's finding, so it is a field, not a
    // rendering accident.
    bool boundary = false;
    // This segment sits ON the capture's depth cap: a CLIPPED edge, not a real
    // bottom. Survives the drill-in (RibbonDrill::at_cap).
    bool at_cap = false;
};

struct RibbonLane {
    long tid = 0;
    std::vector<RibbonSeg> segs; // in call order
    int max_depth = 0;
};

struct ModuleRibbonScene {
    std::vector<RibbonLane> lanes; // ascending tid
    // ONE lane is a worse 2D chart tilted: the caller renders the flat icicle
    // timeline instead, and says so. Not an empty 3D scene.
    bool single_thread = false;
    bool depth_capped = false;
    int depth_cap = 0;
    std::string cap_note; // "" unless depth_capped
    uint32_t seams = 0;
    uint32_t unknown_modules = 0;
    std::string note; // why the scene is empty, when it is
    // Returns are INFERRED from recorded depth decreases and from nothing
    // else; the legend says so, because a drawn descent looks like a measured
    // return. No survey data touches this scene: it is the exact call tree.
    static const char *legend();
};

ModuleRibbonScene build_module_ribbon(const TreeView &tv);
std::string module_ribbon_dump(const ModuleRibbonScene &s);

// Pickable elements, in upload order: lane-then-segment.
struct RibbonElem {
    uint64_t lane = 0;
    uint64_t seg = 0;
};
std::vector<RibbonElem> ribbon_pick_order(const ModuleRibbonScene &s);
// A segment opens the call TREE at that call. dt_link carries no tid and no
// module field (and this brief does not add one — 54 T6 owns dt_link's
// growth), so the tid + module filter and the target symbol travel ALONGSIDE
// the link, in this struct, and the caller applies them to the tree's filter
// panel. Stating that here rather than silently dropping them is the point.
struct RibbonDrill {
    bool ok = false;
    dt_link link;
    long tid = 0;
    std::string module;
    std::string symbol;
    bool at_cap = false; // the cap survives the drill-in
};
RibbonDrill ribbon_pick_link(const ModuleRibbonScene &s, const std::string &rec,
                             uint64_t index);

// ===========================================================================
// T5 — SIMD lane prism
// ===========================================================================

// The default lane width when the recording does not state one — 16 byte
// lanes, LABELLED as a default (prism_default_note()). ValRec::size is the
// TOTAL operand width, so `pshufb` (byte lanes) and `paddd` (dword lanes) are
// indistinguishable from the data; defaulting silently would be an inference
// dressed as a measurement.
inline constexpr uint32_t kPrismDefaultLaneBytes = 1;
const char *prism_default_note();

struct LaneByte {
    uint8_t value = 0;
    // A STABLE value->hue hash: the same byte value always gets the same hue,
    // which is what makes a permutation reveal itself — a recorded byte keeps
    // its hue as it moves lanes under a shuffle. This is a faithful rendering
    // of recorded bytes; it asserts nothing about what the instruction MEANT.
    uint32_t hue = 0; // 0..359 degrees
};

struct PrismWrite {
    uint32_t step = 0;
    uint32_t z = 0;         // stacked-write index, 0 = oldest
    uint64_t insn_off = 0;  // for the disasm drill-in
    std::string disasm;     // D10 text, "" when the producer had none
    uint32_t size = 0;      // ValRec::size — the TOTAL operand width
    bool value_valid = true; // false => HOLLOW, never zeros
    // `bytes` absent, or of a length that is not a whole number of lanes =>
    // a [wide] WIREFRAME, never zero bars.
    bool wireframe = false;
    uint32_t lane_bytes = kPrismDefaultLaneBytes;
    // false => lane_bytes is the DEFAULT above, and the scene must say so.
    bool lane_width_recorded = false;
    std::vector<LaneByte> bytes; // in memory order, as decoded
};

struct LanePrismScene {
    uint32_t reg_id = 0; // the Capstone register id this prism is of
    std::vector<PrismWrite> writes; // ascending by step
    // Present whenever ANY write fell back to the default lane width.
    std::string width_note;
    std::string note; // why the prism is empty, when it is
    uint32_t wireframes = 0;
    uint32_t hollow = 0;
};

// Which wide registers this pass wrote, ascending by Capstone id — the
// selector's options. A prism is of ONE register; a scene that mixed two
// would put two meanings on one X axis.
// Is one ValRec a prism WRITE? The single rule, shared by the option list and
// the scene builder so the two can never disagree about which records the Z
// axis counts. `write` is load-bearing: the x86-64 producer captures wide
// values for READ operands too (df_on_code calls df_capture_reg_value on every
// register read), so without it one `paddd xmm0, xmm1` stacks the pre-state
// read under the post-state write as two Z levels for a single write, and
// offers xmm1 — never written — as a selectable subject. scene_kind.h states
// the axis as "stacked writes, oldest nearest ... Z counts recorded writes".
inline bool is_prism_write(const ValRec &v) {
    return v.wide && v.write && v.space == "reg";
}

std::vector<uint32_t> lane_prism_registers(const DataflowStream &df);
// `guest` is the recording's arch ("x86" / "arm64"), passed to
// space::mnemonic_class for the ambiguity gate. An unknown guest simply never
// subdivides, which is the correct abstention.
LanePrismScene build_lane_prism(const DataflowStream &df, uint32_t reg_id,
                                const std::string &guest);
std::string lane_prism_dump(const LanePrismScene &s);

// Pickable elements, in upload order: write-then-byte.
struct PrismElem {
    uint64_t write = 0;
    uint64_t byte = 0;
};
std::vector<PrismElem> prism_pick_order(const LanePrismScene &s);
// A byte opens the operand TIMELINE at that step (the exact hex is there, and
// the disasm at `insn_off` rides the same row).
std::optional<dt_link> prism_pick_link(const LanePrismScene &s,
                                       const std::string &rec, uint64_t index);

// The stable byte->hue hash itself, exposed so the GL half and the tests use
// the ONE function rather than two that could drift.
uint32_t prism_byte_hue(uint8_t value);

// ===========================================================================
// T1 — per-scene camera framing
// ===========================================================================
// The plane orbit's fixed target (0.5, 0, 0.5) was chosen for the unit plane
// (camera.h:32) and is wrong for a scene whose interesting mass sits at mid
// height. Each kind therefore states where to look — through 48 T1's MOVABLE
// target (Camera::frame), never a second camera type, exactly as the brief's
// risk note instructs. PURE (camera.h is pure math), so the shell can pick a
// framing without linking GL and test_scene_kind can assert one headlessly.
Camera standalone_default_camera(SceneKind k);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_STANDALONE_H
