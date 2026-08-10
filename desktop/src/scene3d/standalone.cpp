// standalone.cpp — the pure builders + dumps of standalone.h. No ImGui, no GL,
// no engine, no I/O (D4): every fidelity rule in the four scenes below is a
// property of these functions, which is what lets test_standalone.cpp assert
// them headlessly.
#include "scene3d/standalone.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

#include "space/mnemonic.h" // T5: the ambiguity gate for element width

namespace asmdesk::scene3d {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string lower(const std::string &s) {
    std::string o = s;
    for (char &c : o)
        c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    return o;
}

// The first whitespace-delimited word of a disassembled line — the mnemonic,
// no operands (space::mnemonic_class's own precondition).
std::string first_token(const std::string &disasm) {
    size_t i = disasm.find_first_not_of(" \t");
    if (i == std::string::npos)
        return "";
    size_t j = disasm.find_first_of(" \t", i);
    return disasm.substr(i, j == std::string::npos ? std::string::npos : j - i);
}

} // namespace

// ===========================================================================
// T2 — divergence worldline
// ===========================================================================

const char *reg_class_name(RegClass c) {
    switch (c) {
    case RegClass::Gpr:
        return "GPR";
    case RegClass::Vector:
        return "vector";
    case RegClass::Flags:
        return "flags";
    case RegClass::Pc:
        return "PC";
    case RegClass::Segment:
        return "segment";
    case RegClass::Other:
        return "unclassed";
    }
    return "unclassed";
}

RegClass reg_class_of(const std::string &name) {
    const std::string n = lower(name);
    if (n.empty())
        return RegClass::Other;
    // Flags and the program counter first: they are single named registers on
    // both guests and would otherwise be swallowed by the GPR prefixes below.
    if (n == "eflags" || n == "rflags" || n == "flags" || n == "cpsr" ||
        n == "nzcv" || n == "fpsr" || n == "fpcr" || n == "mxcsr")
        return RegClass::Flags;
    if (n == "rip" || n == "eip" || n == "ip" || n == "pc")
        return RegClass::Pc;
    if (n == "cs" || n == "ds" || n == "es" || n == "fs" || n == "gs" ||
        n == "ss")
        return RegClass::Segment;
    // Vector files: x86 xmm/ymm/zmm/mm/k, arm64 v/q/d/s/z/p.
    const char c0 = n[0];
    if (n.rfind("xmm", 0) == 0 || n.rfind("ymm", 0) == 0 ||
        n.rfind("zmm", 0) == 0 || n.rfind("mm", 0) == 0 ||
        (c0 == 'k' && n.size() == 2 && std::isdigit(
                                           static_cast<unsigned char>(n[1]))))
        return RegClass::Vector;
    if ((c0 == 'v' || c0 == 'q' || c0 == 'd' || c0 == 's' || c0 == 'z' ||
         c0 == 'p') &&
        n.size() >= 2 && std::isdigit(static_cast<unsigned char>(n[1])))
        return RegClass::Vector;
    // GPRs: x86 r*/e*, arm64 x*/w*, plus the named 16-bit/8-bit legacy set.
    if (c0 == 'r' || c0 == 'e' || c0 == 'x' || c0 == 'w')
        return RegClass::Gpr;
    if (n == "ax" || n == "bx" || n == "cx" || n == "dx" || n == "si" ||
        n == "di" || n == "bp" || n == "sp" || n == "al" || n == "bl" ||
        n == "cl" || n == "dl" || n == "lr" || n == "fp")
        return RegClass::Gpr;
    return RegClass::Other;
}

uint32_t DivergenceScene::top_step() const {
    uint32_t t = shared_steps;
    t = std::max(t, steps_a);
    t = std::max(t, steps_b);
    for (const DivRib &r : ribs)
        t = std::max(t, r.step);
    return t;
}

uint32_t DivergenceScene::hollow_ribs() const {
    uint32_t n = 0;
    for (const DivRib &r : ribs)
        if (r.hollow)
            n++;
    return n;
}

DivergenceScene build_divergence_scene(const Streams &a, const Streams &b) {
    DivergenceScene s;
    s.rec_a = a.id;
    s.rec_b = b.id;

    // The admission gate FIRST — a refusal card, never a silent empty scene.
    // Same gate, same words as the diff view: dt_diff_build owns the
    // code_sha / basis / arch check, and a second copy here would be a second
    // opinion about the same pair.
    dt_diff d;
    std::string err;
    if (!dt_diff_build(a, b, d, err)) {
        s.refused = true;
        s.refusal = err;
        return s; // NO geometry
    }
    s.identity_note = d.identity_note;
    s.diverged = d.div.diverged;
    s.bounded = d.div.bounded;
    s.fork_step = d.div.step;
    s.off_a = d.div.off_a;
    s.off_b = d.div.off_b;
    s.steps_a = static_cast<uint32_t>(a.trace.insns.size());
    s.steps_b = static_cast<uint32_t>(b.trace.insns.size());
    // The fused tube climbs the SHARED prefix. When the streams diverge that
    // is the fork step; when they do not it is the whole shorter recording.
    s.shared_steps = s.diverged ? s.fork_step : std::min(s.steps_a, s.steps_b);

    // The ribs: per-step architectural-state disagreement, from the SAME
    // merge the slice explorer's two-recording view uses.
    const dt_statediff sd = dt_statediff_build(a, b);
    if (!sd.err.empty()) {
        // The state merge refused for a reason the coverage gate did not
        // catch. Say it rather than draw a rib-less scene that looks like
        // agreement.
        s.rib_note = "no ribs: the state merge refused — " + sd.err;
        return s;
    }
    if (!sd.merged) {
        s.rib_note = "no ribs: " + (sd.note.empty()
                                        ? std::string("no statediff stream")
                                        : sd.note) +
                     " — this scene shows WHERE the streams part, and says "
                     "nothing about how their register state evolved";
        return s;
    }
    for (const dt_state_step &ss : sd.steps) {
        DivRib rib;
        rib.step = ss.step;
        rib.changed = static_cast<uint32_t>(ss.regs.size());
        rib.hollow = ss.bounded;
        rib.regs = ss.regs;
        rib.cls = ss.regs.empty() ? RegClass::Other : reg_class_of(ss.regs[0]);
        s.ribs.push_back(std::move(rib));
    }
    if (s.ribs.empty())
        s.rib_note = "no ribs: the two recordings evolve register state "
                     "identically over every compared step";
    return s;
}

std::string divergence_scene_dump(const DivergenceScene &s) {
    std::string out;
    if (s.refused)
        return "refused: " + s.refusal + "\n";
    out += "identity: " + s.identity_note + "\n";
    out += "axis: " + std::string(scene_axes(SceneKind::Divergence).y) + "\n";
    out += "tube: shared_steps=" + std::to_string(s.shared_steps) +
           " a_steps=" + std::to_string(s.steps_a) +
           " b_steps=" + std::to_string(s.steps_b) +
           (s.bounded ? " cap=TORN" : " cap=clean") + "\n";
    if (s.diverged)
        out += "fork: step=" + std::to_string(s.fork_step) +
               " a=" + hex(s.off_a) + " b=" + hex(s.off_b) + "\n";
    else
        out += std::string("fork: none") +
               (s.bounded ? " observed within the recorded window (bounded — "
                            "we stopped looking)"
                          : " — the streams are identical")+
               "\n";
    if (!s.rib_note.empty())
        out += "ribs: " + s.rib_note + "\n";
    else
        out += "ribs: " + std::to_string(s.ribs.size()) + " (" +
               std::to_string(s.hollow_ribs()) + " hollow)\n";
    for (const DivRib &r : s.ribs) {
        out += "  rib step=" + std::to_string(r.step) +
               " thickness=" + std::to_string(r.changed) +
               " class=" + reg_class_name(r.cls) +
               (r.hollow ? " HOLLOW" : "");
        for (const std::string &g : r.regs)
            out += " " + g;
        out += "\n";
    }
    return out;
}

std::vector<DivElem> divergence_pick_order(const DivergenceScene &s) {
    std::vector<DivElem> v;
    if (s.refused)
        return v; // a refusal card has no geometry, so it has no picks
    if (s.diverged)
        v.push_back(DivElem{DivElemKind::Fork, 0});
    for (uint64_t i = 0; i < s.ribs.size(); i++)
        v.push_back(DivElem{DivElemKind::Rib, i});
    return v;
}

std::optional<dt_link> divergence_pick_link(const DivergenceScene &s,
                                            uint64_t index) {
    const std::vector<DivElem> order = divergence_pick_order(s);
    if (index >= order.size())
        return std::nullopt;
    const DivElem &e = order[index];
    dt_link l;
    l.rec = s.rec_a;
    if (e.kind == DivElemKind::Fork) {
        l.view = dt_view::diff;
        l.rec_b = s.rec_b;
        l.step = s.fork_step;
        return l;
    }
    l.view = dt_view::slice;
    l.step = s.ribs[e.rib].step;
    return l;
}

// ===========================================================================
// T3 — invocation stack
// ===========================================================================

const char *InvocationScene::legend() {
    return "one slab per INVOCATION of this routine, stacked in call order — "
           "this compares control flow ACROSS calls. It is not a loop helix, "
           "which counts iterations WITHIN one run's loop; the two produce "
           "similar-looking stacks and answer different questions.";
}

InvocationScene build_invocation_scene(const RegionView &rv,
                                       const space::Projection *proj) {
    InvocationScene s;
    s.basis = rv.basis;
    s.basis_error = rv.basis_error;
    if (!s.basis_error.empty()) {
        s.note = "refused: " + s.basis_error;
        return s;
    }
    if (rv.invocations.empty()) {
        s.note = "no invocations recorded — this recording carries no region "
                 "capture (a `trace` run closed by a `coverage` footer)";
        return s;
    }

    // The UNION block set: the slab's shared X/Z. A block absent from one
    // invocation is then a HOLE in that slab rather than a differently shaped
    // slab, which is the whole finding.
    std::set<uint64_t> all;
    for (const RegionInvocation &inv : rv.invocations)
        for (uint64_t b : inv.blocks)
            all.insert(b);
    s.blocks.assign(all.begin(), all.end());

    // Instructions were recorded but no `coverage` event ever described a block
    // set, so the slab has no X/Z to place anything on: every cell loop below
    // runs zero times and the scene draws a frame ring around nothing. The
    // slabs are still built — dropping them would hide the last thing the
    // target did — so `slabs.empty()` cannot detect this and the gate asks
    // drawable() instead. State the gap precisely: this is a capture stopped
    // before its first footer, NOT a recording with no region capture at all.
    if (all.empty())
        s.note = "no block coverage in this recording — the invocation stack "
                 "places cells on the block set a `coverage` event carries, and "
                 "this capture's instructions were never closed by one";

    for (const RegionInvocation &inv : rv.invocations) {
        InvSlab slab;
        slab.number = static_cast<uint32_t>(inv.number);
        slab.closed = inv.closed;
        slab.truncated = inv.truncated;
        slab.codeimage_known = inv.codeimage_known;
        slab.codeimage_version = inv.codeimage_version;
        if (!slab.closed)
            s.open_slabs++;

        // How many times each recorded instruction offset appeared. A block's
        // ENTRY count is the number of times its start offset shows up in the
        // recorded instruction stream — exact, and derived without inferring a
        // single block EXTENT (the greedy block-attribution rule this tree's
        // reconstructors are forbidden from using).
        std::map<uint64_t, uint32_t> hits;
        for (uint64_t off : inv.insns)
            hits[off]++;

        const std::set<uint64_t> mine(inv.blocks.begin(), inv.blocks.end());
        for (uint64_t b : s.blocks) {
            InvCell c;
            c.block = b;
            if (mine.count(b) == 0) {
                c.state = InvCellState::Absent; // a HOLE, never a zero column
                s.holes++;
            } else {
                auto it = hits.find(b);
                if (it == hits.end()) {
                    // In the coverage footer, but no recorded instruction
                    // described it: the count is UNKNOWN, never 0.
                    c.state = InvCellState::Unknown;
                    s.unknown_cells++;
                } else {
                    c.state = InvCellState::Counted;
                    c.count = it->second;
                }
            }
            if (proj != nullptr)
                c.placed = proj->project(b, &c.u, &c.v);
            slab.cells.push_back(c);
        }
        s.slabs.push_back(std::move(slab));
    }
    return s;
}

std::string invocation_scene_dump(const InvocationScene &s) {
    std::string out;
    out += "axis: " + std::string(scene_axes(SceneKind::Invocation).y) + "\n";
    out += "axis-not: " + std::string(scene_axes(SceneKind::Invocation).y_not) +
           "\n";
    if (!s.note.empty())
        out += "note: " + s.note + "\n";
    out += "blocks:";
    for (uint64_t b : s.blocks)
        out += " " + hex(b);
    out += "\n";
    out += "slabs: " + std::to_string(s.slabs.size()) +
           " holes=" + std::to_string(s.holes) +
           " unknown=" + std::to_string(s.unknown_cells) +
           " open=" + std::to_string(s.open_slabs) + "\n";
    for (const InvSlab &sl : s.slabs) {
        out += "  invocation #" + std::to_string(sl.number);
        if (!sl.closed)
            out += " FRAYED(prefix)";
        if (sl.truncated)
            out += " TRUNCATED";
        if (sl.codeimage_known)
            out += " codeimage=v" + std::to_string(sl.codeimage_version);
        else
            out += " codeimage=unstated";
        out += "\n";
        for (const InvCell &c : sl.cells) {
            out += "    " + hex(c.block) + " ";
            switch (c.state) {
            case InvCellState::Absent:
                out += "HOLE";
                break;
            case InvCellState::Unknown:
                out += "unknown";
                break;
            case InvCellState::Counted:
                out += std::to_string(c.count);
                break;
            }
            out += c.placed ? " placed" : " unplaced";
            out += "\n";
        }
    }
    return out;
}

std::vector<InvElem> invocation_pick_order(const InvocationScene &s) {
    std::vector<InvElem> v;
    for (uint64_t i = 0; i < s.slabs.size(); i++)
        for (uint64_t j = 0; j < s.slabs[i].cells.size(); j++)
            if (s.slabs[i].cells[j].state != InvCellState::Absent)
                v.push_back(InvElem{i, j});
    return v;
}

std::optional<dt_link> invocation_pick_link(const InvocationScene &s,
                                            const std::string &rec,
                                            uint64_t index) {
    const std::vector<InvElem> order = invocation_pick_order(s);
    if (index >= order.size())
        return std::nullopt;
    const InvSlab &slab = s.slabs[order[index].slab];
    const InvCell &cell = slab.cells[order[index].cell];
    dt_link l;
    l.rec = rec;
    l.view = dt_view::region;
    l.off = cell.block;
    // 54 T6's dedicated field. `step` is a DATAFLOW STEP INDEX and stays
    // unset: an invocation number is a different axis, and putting it there
    // would be the ordinal conflation 46 §4 forbids.
    l.invocation = slab.number;
    return l;
}

// ===========================================================================
// T4 — module excursion ribbon
// ===========================================================================

const char *ribbon_unknown_module() { return "unknown"; }

const char *ModuleRibbonScene::legend() {
    return "one sheet per thread; X is CALL ORDER (the recording's own event "
           "sequence), never either playhead. A descent is drawn only where "
           "the recording's depth DECREASED — returns are inferred from that "
           "and from nothing else. This is the exact recorded call tree: no "
           "survey data touches it.";
}

ModuleRibbonScene build_module_ribbon(const TreeView &tv) {
    ModuleRibbonScene s;
    if (tv.have_effective && tv.effective.depth != 0) {
        s.depth_capped = true;
        s.depth_cap = tv.effective.depth;
        s.cap_note = "depth-capped capture (depth=" +
                     std::to_string(tv.effective.depth) +
                     "): the lane floor is a CLIPPED EDGE, not a real bottom "
                     "— calls deeper than this were never emitted";
    }
    if (tv.rows.empty()) {
        s.note = "no calls recorded — this recording carries no call tree";
        return s;
    }

    // Rows in STREAM order (Event::seq), so two threads' calls interleave the
    // way they actually did rather than the way by_kind happened to group them.
    std::vector<const TreeRow *> rows;
    rows.reserve(tv.rows.size());
    for (const TreeRow &r : tv.rows)
        rows.push_back(&r);
    std::stable_sort(rows.begin(), rows.end(),
                     [](const TreeRow *a, const TreeRow *b) {
                         return a->seq < b->seq;
                     });

    std::map<long, size_t> lane_of;
    for (const TreeRow *r : rows) {
        auto it = lane_of.find(r->tid);
        if (it == lane_of.end()) {
            lane_of[r->tid] = s.lanes.size();
            RibbonLane lane;
            lane.tid = r->tid;
            s.lanes.push_back(std::move(lane));
            it = lane_of.find(r->tid);
        }
        RibbonLane &lane = s.lanes[it->second];
        RibbonSeg seg;
        seg.seq = r->seq;
        seg.order = static_cast<uint32_t>(lane.segs.size());
        seg.depth = r->depth;
        seg.addr = r->addr;
        seg.name = r->name;
        // An unresolved module is its OWN band, never blank and never merged
        // into a resolved one. "?" is what the wire spells it (tree.h).
        if (r->module.empty() || r->module == "?") {
            seg.module = ribbon_unknown_module();
            seg.module_unknown = true;
            s.unknown_modules++;
        } else {
            seg.module = r->module;
        }
        // The seam: the module changed from this lane's previous segment.
        if (!lane.segs.empty() && lane.segs.back().module != seg.module) {
            seg.boundary = true;
            s.seams++;
        }
        // A cap of N emits depths 0..N-1, so the floor is N-1.
        if (s.depth_capped && seg.depth == s.depth_cap - 1)
            seg.at_cap = true;
        lane.max_depth = std::max(lane.max_depth, seg.depth);
        lane.segs.push_back(std::move(seg));
    }
    std::sort(s.lanes.begin(), s.lanes.end(),
              [](const RibbonLane &a, const RibbonLane &b) {
                  return a.tid < b.tid;
              });
    // ONE lane in a 3D scene is a worse 2D chart. Say what the shape IS — the
    // note must not promise a substitute view, because there is none:
    // `single_thread` has no consumer outside this model's dump and its unit
    // test, so nothing in the tree renders an icicle timeline. The scene stays
    // SELECTABLE either way (59 T4), which is why this is a note and never a
    // kind_unavailable entry — those are rendered under BeginDisabled.
    s.single_thread = s.lanes.size() == 1;
    if (s.single_thread)
        s.note = "single-threaded recording: one lane, so the depth axis "
                 "carries the whole finding and the thread axis carries "
                 "nothing — a 2D icicle would show this better";
    return s;
}

std::string module_ribbon_dump(const ModuleRibbonScene &s) {
    std::string out;
    out += "axis: " + std::string(scene_axes(SceneKind::ModuleRibbon).x) +
           " | " + std::string(scene_axes(SceneKind::ModuleRibbon).y) + " | " +
           std::string(scene_axes(SceneKind::ModuleRibbon).z) + "\n";
    if (!s.note.empty())
        out += "note: " + s.note + "\n";
    if (s.depth_capped)
        out += "cap: " + s.cap_note + "\n";
    out += "lanes: " + std::to_string(s.lanes.size()) +
           " seams=" + std::to_string(s.seams) +
           " unknown_modules=" + std::to_string(s.unknown_modules) +
           (s.single_thread ? " SINGLE-THREAD(2D form)" : "") + "\n";
    for (const RibbonLane &l : s.lanes) {
        out += "  tid " + std::to_string(l.tid) +
               " max_depth=" + std::to_string(l.max_depth) + "\n";
        for (const RibbonSeg &g : l.segs) {
            out += "    x=" + std::to_string(g.order) +
                   " seq=" + std::to_string(g.seq) +
                   " depth=" + std::to_string(g.depth) + " " + g.module + "!" +
                   g.name;
            if (g.boundary)
                out += " SEAM";
            if (g.at_cap)
                out += " CAPPED";
            if (g.module_unknown)
                out += " unresolved";
            out += "\n";
        }
    }
    return out;
}

std::vector<RibbonElem> ribbon_pick_order(const ModuleRibbonScene &s) {
    std::vector<RibbonElem> v;
    for (uint64_t i = 0; i < s.lanes.size(); i++)
        for (uint64_t j = 0; j < s.lanes[i].segs.size(); j++)
            v.push_back(RibbonElem{i, j});
    return v;
}

RibbonDrill ribbon_pick_link(const ModuleRibbonScene &s, const std::string &rec,
                             uint64_t index) {
    RibbonDrill d;
    const std::vector<RibbonElem> order = ribbon_pick_order(s);
    if (index >= order.size())
        return d;
    const RibbonLane &lane = s.lanes[order[index].lane];
    const RibbonSeg &seg = lane.segs[order[index].seg];
    d.ok = true;
    d.link.rec = rec;
    d.link.view = dt_view::tree;
    d.link.off = seg.addr;
    d.tid = lane.tid;
    // An unresolved module is carried as unresolved: filtering the tree by the
    // literal string "unknown" would silently ask for a module by that name.
    d.module = seg.module_unknown ? std::string() : seg.module;
    d.symbol = seg.name;
    d.at_cap = seg.at_cap; // the cap survives the drill-in
    return d;
}

// ===========================================================================
// T5 — SIMD lane prism
// ===========================================================================

const char *prism_default_note() {
    return "element width UNKNOWN: this mnemonic is not in the table, so the "
           "byte-lane geometry shown here is an INFERRED axis, not a "
           "measurement.";
}

const char *prism_no_lanes_note() {
    return "this operand has no element width to know: a bulk move copies "
           "bytes, it does not divide them into lanes. The geometry below is "
           "a rendering default, not an inferred measurement.";
}

uint32_t prism_byte_hue(uint8_t value) {
    // A stable, well-spread value -> hue map. The requirement is only that the
    // SAME byte always gets the SAME hue (so a recorded byte keeps its colour
    // as a shuffle moves it between lanes) and that neighbouring values are
    // far apart in hue (so a permutation is legible). The golden-ratio step is
    // the standard way to get both; it is a rendering choice, and it asserts
    // nothing about what the byte MEANS.
    return static_cast<uint32_t>((static_cast<uint32_t>(value) * 137u) % 360u);
}

namespace {

// The bulk-move family (2026-08-08 revised plan): measured over blend_tile
// (cli/scenes_victim.c:82), movdqa x5 and movd x1 account for 6 of its 11
// wide register writes, and EVERY one of them genuinely has no element width
// -- a straight byte copy, not an array of same-sized lanes. movdqu/movaps/
// movups/movq complete the family by the same reasoning (movaps appears in
// blend_tile too, but only as the trailing STORE, which writes memory, not a
// register, so is_prism_write never counts it -- see the task report for the
// arithmetic this corrects). This is a fact about the operand, so it is
// checked unconditionally, before and independent of the ambiguity gate
// below: whether mnemonic_class also calls movd/movq a GPR<->vector
// ambiguity is a different axis (OpClass) from this one (does this operand
// have lanes at all), and the answer here does not change either way.
const char *const kBulkMoveFamily[] = {
    "movdqa", "movdqu", "movaps", "movups", "movd", "movq",
};

// The AVX-512 masking-granularity family (2026-08-08 final-plan review,
// Finding 3): vmovdqu8/16/32/64 and vmovdqa32/64 -- what Task 4's
// cli/evex_victim.c emits (`vmovdqu64`) and what glibc's
// __memmove_evex_unaligned_erms tail uses (`vmovdqu8` under a real `{k}`
// mask, G8) -- were newly VISIBLE once Task 4 let the producer read
// ymm16-31, and land in Unknown: stripped of their `v`, "movdqu64" matches
// neither kBulkMoveFamily (exact string compare) nor kLaneWidths, so they hit
// the honest-guess fallback and raise the warning-coloured "INFERRED axis"
// note for a case that, like the SSE moves above, usually has nothing to
// infer.
//
// The numeric suffix is NOT an element width by itself -- confirmed
// empirically (a standalone Capstone 5.0.1 harness disassembling `as`-
// assembled bytes, this task's report has the transcript), not assumed:
//   unmasked:        "vmovdqu64 ymm16, ymm17"        -- no `{k` anywhere
//   masked (k1):     "vmovdqu64 ymm16 {k1}, ymm17"   -- masked
//   masked+zero:     "vmovdqu64 ymm16 {k1} {z}, ymm17"
// UNMASKED, every element passes through unconditionally, so the write is a
// straight byte copy with the identical effect as the SSE spelling -- the
// suffix says nothing observable about the result, which is why this table
// is consulted before it, not folded into it (the verdict depends on more
// than the mnemonic). MASKED, which bytes of the destination get overwritten
// genuinely depends on the suffix's granularity, so it IS the lane width
// there. Capstone renders the mask as a trailing `{k<n>}` token (space-
// separated from the register, per the transcript above), which is why the
// check below is a substring search over the FULL disasm text rather than a
// second mnemonic-table lookup.
struct EvexMaskGranularity {
    const char *mnemonic; // post-'v'-strip, as kBulkMoveFamily's entries are
    uint32_t bytes;       // the masking granularity IF a `{k}` mask is present
};
const EvexMaskGranularity kEvexMaskGranularity[] = {
    {"movdqu8", 1}, {"movdqu16", 2}, {"movdqu32", 4}, {"movdqu64", 8},
    {"movdqa32", 4}, {"movdqa64", 8},
};

// The one place a lane width may be widened past the default, and only where
// the mnemonic is unambiguous. Deliberately SHORT: every entry is a mnemonic
// whose element width is part of its name. Anything not here keeps the
// default and is labelled — a second guess would be exactly the D7 failure T5
// is written to avoid.
struct LaneWidthRule {
    const char *mnemonic;
    uint32_t bytes;
};
const LaneWidthRule kLaneWidths[] = {
    // x86 SSE/AVX integer packed ops whose suffix names the element width.
    {"paddb", 1},
    {"psubb", 1},
    {"pshufb", 1},
    {"pcmpeqb", 1},
    {"paddw", 2},
    {"psubw", 2},
    {"pcmpeqw", 2},
    {"pshufhw", 2},
    {"pshuflw", 2},
    {"paddd", 4},
    {"psubd", 4},
    {"pslld", 4},
    {"psrld", 4},
    {"pcmpeqd", 4},
    {"pshufd", 4},
    {"pmulld", 4},
    {"paddq", 8},
    {"psubq", 8},
    {"psllq", 8},
    {"psrlq", 8},
    {"pcmpeqq", 8},
    // The interleave family (2026-08-08 revised plan): the ONE genuine table
    // gap the measurement found. punpckldq unambiguously interleaves 4-byte
    // lanes -- it reads two registers and interleaves their lanes, which is
    // exactly the per-element behaviour this table exists to record, unlike
    // the bulk moves above. Neither Capstone nor the table named it before.
    {"punpcklbw", 1},
    {"punpckhbw", 1},
    {"punpcklwd", 2},
    {"punpckhwd", 2},
    {"punpckldq", 4},
    {"punpckhdq", 4},
    {"punpcklqdq", 8},
    {"punpckhqdq", 8},
};

// kLaneWidths only ever stores 1/2/4/8 (every SSE/AVX integer element width
// here is a power of two up to the qword), so this covers every value the
// table can produce; the unreached fallback keeps a future non-power-of-two
// entry from silently claiming a false "known" width.
PrismWidth prism_width_of_bytes(uint32_t bytes) {
    switch (bytes) {
    case 1:
        return PrismWidth::Lanes1;
    case 2:
        return PrismWidth::Lanes2;
    case 4:
        return PrismWidth::Lanes4;
    case 8:
        return PrismWidth::Lanes8;
    }
    return PrismWidth::Unknown;
}

// The geometry stride for a verdict: prism_width_of_bytes's inverse for the
// four known cases, kPrismDefaultLaneBytes for the other two -- still a
// sensible rendering choice for NoLaneSemantics/Unknown, just never dressed
// up as a measurement (see the two notes in the header).
uint32_t prism_geometry_bytes(PrismWidth w) {
    switch (w) {
    case PrismWidth::Lanes1:
        return 1;
    case PrismWidth::Lanes2:
        return 2;
    case PrismWidth::Lanes4:
        return 4;
    case PrismWidth::Lanes8:
        return 8;
    case PrismWidth::NoLaneSemantics:
    case PrismWidth::Unknown:
        break;
    }
    return kPrismDefaultLaneBytes;
}

} // namespace

bool prism_width_known(PrismWidth w) {
    switch (w) {
    case PrismWidth::Lanes1:
    case PrismWidth::Lanes2:
    case PrismWidth::Lanes4:
    case PrismWidth::Lanes8:
        return true;
    case PrismWidth::NoLaneSemantics:
    case PrismWidth::Unknown:
        return false;
    }
    return false;
}

PrismWidth lane_width_class(const std::string &disasm,
                            const std::string &guest) {
    const std::string tok = lower(first_token(disasm));
    if (tok.empty())
        return PrismWidth::Unknown;
    // The AVX `v` prefix names the same element width/semantics as its SSE
    // original, for both checks below.
    std::string base = tok;
    if (base.size() > 1 && base[0] == 'v')
        base = base.substr(1);

    for (const char *m : kBulkMoveFamily)
        if (base == m)
            return PrismWidth::NoLaneSemantics;

    // The AVX-512 masking-granularity family: a fact about the operand, like
    // kBulkMoveFamily above, so also checked unconditionally and before the
    // ambiguity gate -- but the verdict needs the full disasm text, not just
    // the mnemonic, since UNMASKED (no `{k` anywhere) is the bulk-copy case
    // and MASKED is the one case where the numeric suffix really is the lane
    // width. See kEvexMaskGranularity's comment for the Capstone transcript
    // this is measured from.
    for (const EvexMaskGranularity &r : kEvexMaskGranularity)
        if (base == r.mnemonic)
            return disasm.find("{k") != std::string::npos
                       ? prism_width_of_bytes(r.bytes)
                       : PrismWidth::NoLaneSemantics;

    // 54 T4's ambiguity flag is the GATE for what remains, not a second
    // opinion: a mnemonic that classifier calls ambiguous (a float<->int
    // conversion, e.g.) never subdivides, whatever its spelling suggests.
    const space::MnemonicClass mc = space::mnemonic_class(tok, guest);
    if (mc.ambiguous)
        return PrismWidth::Unknown;

    for (const LaneWidthRule &r : kLaneWidths)
        if (base == r.mnemonic)
            return prism_width_of_bytes(r.bytes);
    return PrismWidth::Unknown;
}

std::vector<uint32_t> lane_prism_registers(const DataflowStream &df) {
    std::set<uint32_t> regs;
    for (const ValRec &v : df.recs)
        if (is_prism_write(v))
            regs.insert(v.reg);
    return std::vector<uint32_t>(regs.begin(), regs.end());
}

LanePrismScene build_lane_prism(const DataflowStream &df, uint32_t reg_id,
                                const std::string &guest) {
    LanePrismScene s;
    s.reg_id = reg_id;
    bool any_no_lanes = false;
    bool any_unknown = false;
    for (const ValRec &v : df.recs) {
        if (!is_prism_write(v) || v.reg != reg_id)
            continue;
        PrismWrite w;
        w.step = v.step;
        w.z = static_cast<uint32_t>(s.writes.size());
        w.size = v.size;
        w.value_valid = v.value_valid;
        w.insn_off = v.step < df.insn_off.size() ? df.insn_off[v.step] : 0;
        w.disasm = v.step < df.disasm.size() ? df.disasm[v.step] : std::string();

        w.width = lane_width_class(w.disasm, guest);
        w.lane_bytes = prism_geometry_bytes(w.width);
        if (w.width == PrismWidth::NoLaneSemantics)
            any_no_lanes = true;
        else if (w.width == PrismWidth::Unknown)
            any_unknown = true;

        if (v.bytes.empty() ||
            (w.lane_bytes > 1 && v.bytes.size() % w.lane_bytes != 0)) {
            // Absent bytes, or a length that is not a whole number of lanes:
            // a [wide] WIREFRAME, never zero bars. Zero bars would say "the
            // register held zeros", which nobody recorded.
            w.wireframe = true;
            s.wireframes++;
        } else {
            for (uint8_t b : v.bytes)
                w.bytes.push_back(LaneByte{b, prism_byte_hue(b)});
        }
        if (!w.value_valid)
            s.hollow++;
        s.writes.push_back(std::move(w));
    }
    if (s.writes.empty()) {
        s.note = "no wide register writes recorded for this register — a "
                 "prism needs `df_step.ops` entries with wide=true, which "
                 "only a producer that serialises the `bytes` field emits";
        return s;
    }
    // Two different notes for two different facts (2026-08-08 revised plan):
    // width_note disclaims an actual guess (Unknown); no_lane_note states a
    // fact that was never a guess (NoLaneSemantics). A scene can carry
    // either, neither, or both -- movdqa and an unrecognised mnemonic can
    // coexist in one register's write history.
    if (any_unknown)
        s.width_note = prism_default_note();
    if (any_no_lanes)
        s.no_lane_note = prism_no_lanes_note();
    return s;
}

std::string lane_prism_dump(const LanePrismScene &s) {
    std::string out;
    out += "axis: " + std::string(scene_axes(SceneKind::LanePrism).x) + " | " +
           std::string(scene_axes(SceneKind::LanePrism).y) + " | " +
           std::string(scene_axes(SceneKind::LanePrism).z) + "\n";
    out += "reg: " + std::to_string(s.reg_id) + "\n";
    if (!s.note.empty())
        out += "note: " + s.note + "\n";
    if (!s.width_note.empty())
        out += "width: " + s.width_note + "\n";
    if (!s.no_lane_note.empty())
        out += "no-lanes: " + s.no_lane_note + "\n";
    out += "writes: " + std::to_string(s.writes.size()) +
           " wireframe=" + std::to_string(s.wireframes) +
           " hollow=" + std::to_string(s.hollow) + "\n";
    for (const PrismWrite &w : s.writes) {
        // A one-word tag per verdict, not just "recorded"/"default" -- the
        // whole point of the three-way split is that a reader (this dump
        // included) can tell a bulk move's default from an unknown's guess.
        const char *tag = "(unknown)";
        switch (w.width) {
        case PrismWidth::Lanes1:
            tag = "(known:1)";
            break;
        case PrismWidth::Lanes2:
            tag = "(known:2)";
            break;
        case PrismWidth::Lanes4:
            tag = "(known:4)";
            break;
        case PrismWidth::Lanes8:
            tag = "(known:8)";
            break;
        case PrismWidth::NoLaneSemantics:
            tag = "(no-lanes)";
            break;
        case PrismWidth::Unknown:
            break;
        }
        out += "  z=" + std::to_string(w.z) +
               " step=" + std::to_string(w.step) +
               " size=" + std::to_string(w.size) +
               " lane_bytes=" + std::to_string(w.lane_bytes) + tag;
        if (w.wireframe)
            out += " WIREFRAME";
        if (!w.value_valid)
            out += " HOLLOW";
        out += "\n";
        if (!w.bytes.empty()) {
            out += "   ";
            for (size_t i = 0; i < w.bytes.size(); i++) {
                char b[32];
                std::snprintf(b, sizeof b, " [%zu]%02x/h%u", i,
                              static_cast<unsigned>(w.bytes[i].value),
                              static_cast<unsigned>(w.bytes[i].hue));
                out += b;
            }
            out += "\n";
        }
    }
    return out;
}

std::vector<PrismElem> prism_pick_order(const LanePrismScene &s) {
    std::vector<PrismElem> v;
    for (uint64_t i = 0; i < s.writes.size(); i++)
        for (uint64_t j = 0; j < s.writes[i].bytes.size(); j++)
            v.push_back(PrismElem{i, j});
    return v;
}

std::optional<dt_link> prism_pick_link(const LanePrismScene &s,
                                       const std::string &rec,
                                       uint64_t index) {
    const std::vector<PrismElem> order = prism_pick_order(s);
    if (index >= order.size())
        return std::nullopt;
    dt_link l;
    l.rec = rec;
    l.view = dt_view::timeline;
    l.step = s.writes[order[index].write].step;
    l.off = s.writes[order[index].write].insn_off;
    return l;
}


// ===========================================================================
// T1 — per-scene camera framing
// ===========================================================================

Camera standalone_default_camera(SceneKind k) {
    // 48 T1 made Camera::target movable and gave it frame(); this uses THAT
    // rather than building a second camera, per the brief's own instruction.
    // Every scene above is authored inside the unit cube, so the framing
    // differs only in where the interesting mass sits.
    Camera c;
    switch (k) {
    case SceneKind::Plane:
        return c; // the plane's own default, untouched
    case SceneKind::Divergence:
        // The fork is mid-height and the ribbons spread in X: pull back and
        // look level-ish, so the two sides read side by side.
        c.frame(0.5f, 0.5f, 2.6f);
        c.pitch = 0.25f;
        c.yaw = 0.0f;
        break;
    case SceneKind::Invocation:
        // A stack of shelves: a three-quarter view from slightly above.
        c.frame(0.5f, 0.5f, 2.8f);
        c.pitch = 0.45f;
        c.yaw = 0.7f;
        break;
    case SceneKind::ModuleRibbon:
        // Lanes run along X and stack in Z: look down the Z axis a little.
        c.frame(0.5f, 0.5f, 2.4f);
        c.pitch = 0.55f;
        c.yaw = 0.35f;
        break;
    case SceneKind::LanePrism:
        // A shallow slab: nearly side-on so byte magnitudes are comparable.
        c.frame(0.5f, 0.5f, 2.2f);
        c.pitch = 0.30f;
        c.yaw = 0.15f;
        break;
    case SceneKind::SessionFlow:
        // Ribbons run along X and stack in Z, like the module ribbon; a
        // three-quarter view from above reads the rate heights side by side.
        c.frame(0.5f, 0.5f, 2.5f);
        c.pitch = 0.5f;
        c.yaw = 0.4f;
        break;
    }
    c.target[1] = 0.4f; // the scenes above are authored around mid-height
    return c;
}


} // namespace asmdesk::scene3d
