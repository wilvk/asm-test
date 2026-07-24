// fabric.h — the Loom's spacetime fabric model (05-loom-day-one.md T1).
//
// A recorded run as a fabric: horizontal = trace step, vertical = location
// lanes, and every value a WORLDLINE (a `loom_span_t`) you can select, walk and
// audit. This file turns the two things a value producer already yields — an
// `asmtest_valtrace_t` (L0: per-step operand read/write records) and an
// `asmtest_defuse_t` (L1: last-writer edges) — into lanes, spans, hops and
// knots. Nothing here draws: `fabric_plan.h` turns a fabric into primitives and
// `fabric_imgui.cpp` paints them, so every rule below is testable headlessly.
//
// ENGINE-FREE (D4). asmtest_valtrace.h is a pure header (stdbool/stddef/stdint
// + asmtest_trace.h); this TU calls none of the functions it declares, so
// asmtest-viewer links no engine object to render a fabric. The sub-register
// fold below is deliberately a LOCAL copy of src/dataflow_emu.c's
// `cap_x86_to_uc` switch keyed on Capstone ids — linking the producer to fold a
// register id would drag Unicorn into the render-only binary.
//
// THE EXACT-ONLY LAW. `loom_fabric_build` REFUSES a non-exact provenance and
// returns no partial fabric. A sampled feed cannot say what a value was at a
// step; drawing one as a worldline would render a guess in the same ink as a
// measurement. Statistical producers reach the lane annex (annex.h) and stop
// there. The refusal is the enforcement point, and it is tested.
#ifndef ASMDESK_LOOM_FABRIC_H
#define ASMDESK_LOOM_FABRIC_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "asmtest_valtrace.h" // pure: at_val_rec_t / asmtest_valtrace_t / edges

namespace asmdesk {

// A span that is still live when the trace ends. NEVER a thread-death cap: the
// fabric knows only that nothing overwrote it inside the recorded window.
inline constexpr uint32_t kLoomAlive = UINT32_MAX;
// A span with no defining record: the synthetic born-of-untraced-state entry.
inline constexpr size_t kLoomNoRec = SIZE_MAX;
// "no span here" — a hop whose consumer step defines nothing.
inline constexpr uint32_t kLoomNoSpan = UINT32_MAX;

// How the fabric came to exist, and what it is allowed to claim.
struct loom_provenance_t {
    std::string producer;            // "dataflow-emu" today
    bool exact = false;              // statistical input NEVER weaves
    bool truncated = false;          // vt->truncated || recording drop counts
    bool isolated_guest = false;     // emulator producer -> badge
    std::vector<std::string> disasm; // per step; empty -> offsets (D10)

    // The truncation banner's two numbers. `steps_total == 0` means the feed did
    // not record a total — the v1 schema carries no dataflow step total (only
    // the `end` footer's truncated flag), so a replayed recording usually cannot
    // supply M. The chrome says which of the two texts it is rendering rather
    // than inventing an M; see fabric_plan.cpp.
    uint64_t steps_recorded = 0;
    uint64_t steps_total = 0;
};

enum class loom_lane_kind { reg, mem_band };

// One location the fabric tracks over time. Register lanes are the folded
// 64-bit containers; memory lanes are coalesced BANDS of bytes.
struct loom_lane_t {
    loom_lane_kind kind = loom_lane_kind::reg;
    uint32_t reg = 0; // folded 64-bit container Capstone id (kind == reg)
    uint64_t lo = 0, hi = 0; // [lo,hi) bytes (kind == mem_band)
    std::string name;        // "rdi", "eflags", "mem[0x20fff8..0x210000)"
    std::string space;       // mem bands: the capture's mem_space, "abs"|"off"
    uint32_t first_touch_step = 0; // orders mem bands
};

// One worldline interval: the value resident on `lane` from `t_write` until the
// next write that overlaps it.
struct loom_span_t {
    uint32_t lane = 0;
    uint32_t t_write = 0;
    uint32_t t_end = kLoomAlive; // kLoomAlive = alive at trace end
    size_t rec = kLoomNoRec;     // defining record in vt->recs
    bool value_valid = false;    // false => a HOLLOW thread (route, not value)
    bool born_untraced = false;  // synthetic entry span
    uint64_t value = 0;
    uint64_t lo = 0, hi = 0; // the bytes this write covered (mem lanes)
};

// A def-use edge, resolved onto the fabric. `to_span` is kLoomNoSpan when the
// consuming step defines nothing (a pure read, e.g. `ret`): the hop still
// happened, it just terminates at the step column.
struct loom_hop_t {
    uint32_t from_span = 0;
    uint32_t to_span = kLoomNoSpan;
    uint32_t edge = 0; // index into the def-use edge array
};

// A step that both reads and writes: where worldlines cross.
struct loom_knot_t {
    uint32_t step = 0;
};

// One read, indexed by place. Spans are the WRITE half of the model; this is
// the other half, and it is what lets a consumer ask "which steps read this
// lane without any in-trace producer?" — the seed rule for a fork's cone of
// consequence (T6) and the born-of-untraced-state evidence.
struct loom_read_t {
    uint32_t step = 0;
    uint32_t lane = 0;
    bool has_producer = false; // a def-use edge lands on this (step, lane)
};

struct loom_fabric_t {
    std::vector<loom_lane_t> lanes;
    std::vector<loom_span_t> spans;
    std::vector<loom_hop_t> hops;
    std::vector<loom_knot_t> knots;
    std::vector<loom_read_t> reads;
    loom_provenance_t prov;
    uint32_t steps = 0;
    // Per-step instruction offset, copied from the value trace. It is what the
    // code-space annex join and T6's shared-prefix alignment key on; a step with
    // no recorded offset keeps UINT64_MAX, which is NOT offset 0 (that would
    // claim the routine's entry instruction ran there).
    std::vector<uint64_t> insn_off;

    bool is_knot(uint32_t step) const;
    // The [lo, hi] envelope of the recorded offsets. False when nothing was
    // recorded — a caller must not fall back to [0, 0].
    bool code_range(uint64_t *lo, uint64_t *hi) const;
    // The span resident on `lane` at `t` (last t_write <= t < t_end), or
    // kLoomNoSpan. Residency is what the zeroization audit scrubs over.
    uint32_t resident(uint32_t lane, uint32_t t) const;
};

// Build the fabric. Returns false with `*err` set — and *out untouched — when
// the provenance is not exact (the statistical-never-woven law) or the inputs
// are missing. NEVER returns a partial fabric.
bool loom_fabric_build(const asmtest_valtrace_t *vt, const asmtest_defuse_t *g,
                       const loom_provenance_t &prov, loom_fabric_t *out,
                       std::string *err);

// --- register identity, without Capstone ------------------------------------
// Fold a Capstone x86 register id to its 64-bit container (the local copy of
// src/dataflow_emu.c:64's switch). An id this build does not model folds to
// itself, so an unmodelled register still gets its own lane rather than being
// merged into somebody else's.
uint32_t loom_fold_reg(uint32_t cap_reg);
// "rdi" / "eflags" / "reg#123" for a container id. Never empty.
std::string loom_reg_name(uint32_t container);

// The fixed register deck order, as container ids: the SysV argument registers
// (src/dataflow_emu.c:273 `arg_regs`), then the remaining GPs in `df_zero_gp`
// order (:229), then rsp, rip, eflags. Only TOUCHED registers materialize as
// lanes, but the order among those that do never changes.
const std::vector<uint32_t> &loom_reg_deck();

// Deterministic one-line-per-fact dump — the golden-test surface.
std::string loom_fabric_dump(const loom_fabric_t &f);

} // namespace asmdesk
#endif // ASMDESK_LOOM_FABRIC_H
