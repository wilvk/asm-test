// test_loom_takeview.cpp — the fork UX rules (05-loom-day-one.md T6).
// Render-only: fabric.o + fabric_plan.o + take_view.o + diff.o. The engine-side
// half (loom_take_run actually re-running the emulator) is test_loom_forks.cpp.
//
// The fixture is the doc's `fork_demo`, hand-filled rather than emulated, so the
// verdict truth table is exercised without a guest:
//
//   0x00  mov rdx, rdi   ; hot     — the value differs across takes
//   0x03  shr rdx, 63    ; DIMMED  — 0 for both non-negative arguments
//   0x07  mov rax, rdi   ; hot
//   0x0a  cmp rax, 10    ; hot     — the flags differ
//   0x0e  jle 0x13       ; last aligned step
//   0x10  neg rax        ; take only (a > 10)
//   0x13  ret
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "loom/take_view.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

enum : uint32_t { RAX = 35, RDX = 40, RDI = 39, EFLAGS = 25 };

static at_val_rec_t reg(uint32_t step, uint32_t r, bool write, bool valid,
                        uint64_t value) {
    at_val_rec_t v{};
    v.kind = AT_LOC_REG;
    v.reg = r;
    v.size = 8;
    v.is_write = write;
    v.value_valid = valid;
    v.value = value;
    v.step = step;
    return v;
}

struct Side {
    std::vector<uint64_t> off;
    std::vector<at_val_rec_t> recs;
    std::vector<asmtest_defuse_edge_t> edges;
    asmtest_valtrace_t vt{};
    asmtest_defuse_t g{};
    void edge(uint32_t f, uint32_t t, uint32_t r) {
        asmtest_defuse_edge_t e{};
        e.from_step = f;
        e.to_step = t;
        e.loc = reg(t, r, false, true, 0);
        edges.push_back(e);
    }
    void bind() {
        vt = asmtest_valtrace_t{};
        vt.insn_off = off.data();
        vt.steps_cap = vt.steps_len = off.size();
        vt.steps_total = off.size();
        vt.recs = recs.data();
        vt.recs_cap = vt.recs_len = recs.size();
        vt.recs_total = recs.size();
        vt.mem_space = AT_LOC_MEM_ABS;
        g = asmtest_defuse_t{};
        g.edges = edges.data();
        g.n = edges.size();
        g.nsteps = off.size();
    }
    loom_fabric_t weave() {
        bind();
        loom_provenance_t p;
        p.producer = "dataflow-emu";
        p.exact = true;
        p.isolated_guest = true;
        p.steps_recorded = off.size();
        p.steps_total = off.size();
        loom_fabric_t f;
        std::string err;
        if (!loom_fabric_build(&vt, &g, p, &f, &err)) {
            std::fprintf(stderr, "FAIL fixture: %s\n", err.c_str());
            std::exit(1);
        }
        return f;
    }
};

// `a` is the entry argument; a > 10 takes the `neg` arm.
static Side make(long a) {
    Side s;
    const bool neg = a > 10;
    s.off = {0x00, 0x03, 0x07, 0x0a, 0x0e};
    if (neg)
        s.off.push_back(0x10);
    s.off.push_back(0x13);

    uint64_t rdx0 = static_cast<uint64_t>(a);
    uint64_t rdx1 = rdx0 >> 63; // 0 for both non-negative arguments
    uint64_t rax = static_cast<uint64_t>(a);
    uint64_t flags = static_cast<uint64_t>(a - 10);

    s.recs = {
        reg(0, RDI, false, true, rdx0),    reg(0, RDX, true, true, rdx0),
        reg(1, RDX, false, true, rdx0),    reg(1, RDX, true, true, rdx1),
        reg(1, EFLAGS, true, true, 2),     reg(2, RDI, false, true, rax),
        reg(2, RAX, true, true, rax),      reg(3, RAX, false, true, rax),
        reg(3, EFLAGS, true, true, flags), reg(4, EFLAGS, false, true, flags),
    };
    s.edge(0, 1, RDX);
    s.edge(2, 3, RAX);
    s.edge(3, 4, EFLAGS);
    if (neg) {
        s.recs.push_back(reg(5, RAX, false, true, rax));
        s.recs.push_back(reg(5, RAX, true, true, static_cast<uint64_t>(-a)));
        s.recs.push_back(reg(5, EFLAGS, true, true, 1));
        s.edge(2, 5, RAX);
    }
    return s;
}

static loom_edit_desc_t arg_edit() {
    loom_edit_desc_t e;
    e.code_patch = false;
    e.arg_reg = RDI;
    e.label = "arg0 := 11";
    return e;
}

static const loom_take_step_t *at(const loom_take_view_t &v, uint32_t step) {
    for (const loom_take_step_t &s : v.steps)
        if (s.step == step)
            return &s;
    return nullptr;
}

int main() {
    Side b = make(3), t = make(11);
    loom_fabric_t base = b.weave(), take = t.weave();
    loom_take_view_t v = loom_take_view(base, take, t.edges.data(),
                                        t.edges.size(), arg_edit(), "", "");
    std::string dump = loom_take_dump(v);

    // --- prefix / patient-zero math ----------------------------------------
    check("the aligned prefix is 5", v.prefix == 5,
          "got " + std::to_string(v.prefix) + "\n" + dump);
    check("patient zero is flagged", v.div.diverged, dump);
    check("patient zero is at step 5", v.div.step == 5,
          std::to_string(v.div.step));
    check("patient zero names both offsets",
          v.div.off_a == 0x13 && v.div.off_b == 0x10,
          "a=" + std::to_string(v.div.off_a) +
              " b=" + std::to_string(v.div.off_b));
    check("the gutter names patient zero",
          v.node.alignment.find("patient zero at step 5") != std::string::npos,
          v.node.alignment);
    check("the gutter carries the native→virtual disclosure",
          v.node.disclosure.find("never evidence about a live process") !=
              std::string::npos,
          v.node.disclosure);

    // --- the cone of the edited fact ---------------------------------------
    check("the cone seeds at the steps that read the edited argument",
          std::find(v.cone.begin(), v.cone.end(), 0u) != v.cone.end() &&
              std::find(v.cone.begin(), v.cone.end(), 2u) != v.cone.end(),
          dump);
    check("the cone closes forward over the take's edges",
          std::find(v.cone.begin(), v.cone.end(), 1u) != v.cone.end() &&
              std::find(v.cone.begin(), v.cone.end(), 4u) != v.cone.end(),
          dump);

    // --- the three-way verdict table ---------------------------------------
    check("step 0 (mov rdx, rdi) is HOT",
          at(v, 0)->verdict == take_verdict::hot, dump);
    check("step 1 (shr rdx, 63) is DIM — dependence without consequence",
          at(v, 1)->verdict == take_verdict::dim, dump);
    check("step 2 (mov rax, rdi) is HOT",
          at(v, 2)->verdict == take_verdict::hot, dump);
    check("step 3 (cmp) is HOT — the flags differ",
          at(v, 3)->verdict == take_verdict::hot, dump);
    check("step 4 (jle) writes nothing: outline only",
          at(v, 4)->no_writes && at(v, 4)->verdict == take_verdict::neutral,
          dump);
    check("step 5 is past the prefix and marked unaligned", at(v, 5)->unaligned,
          dump);
    check("an unaligned step carries NO verdict",
          at(v, 5)->verdict == take_verdict::neutral, dump);

    // --- either side invalid => NEITHER dim nor hot ------------------------
    {
        Side t2 = make(11);
        for (at_val_rec_t &r : t2.recs)
            if (r.step == 1 && r.reg == RDX && r.is_write)
                r.value_valid = false; // the producer never captured it
        loom_fabric_t take2 = t2.weave();
        loom_take_view_t v2 = loom_take_view(
            base, take2, t2.edges.data(), t2.edges.size(), arg_edit(), "", "");
        check("an uncaptured value on either side yields NEITHER verdict",
              at(v2, 1)->verdict == take_verdict::neutral,
              "equality of unknown values was claimed:\n" + loom_take_dump(v2));
        check("the step is still in the cone", at(v2, 1)->in_cone, "");
    }

    // --- an arg edit that changes no offsets: aligned end-to-end ----------
    {
        Side t3 = make(4); // still <= 10, so the same arm runs
        loom_fabric_t take3 = t3.weave();
        loom_take_view_t v3 = loom_take_view(
            base, take3, t3.edges.data(), t3.edges.size(), arg_edit(), "", "");
        check("offsets that never diverge produce NO patient zero",
              !v3.div.diverged, loom_take_dump(v3));
        check("the gutter says so verbatim",
              v3.node.alignment == std::string(kLoomAlignedEndToEnd),
              v3.node.alignment);
        check("the values still diverge, so step 0 is hot",
              at(v3, 0)->verdict == take_verdict::hot, loom_take_dump(v3));
        check("and step 1 is still dim",
              at(v3, 1)->verdict == take_verdict::dim, loom_take_dump(v3));
    }

    // --- a code patch seeds at the first differing instruction -------------
    {
        loom_edit_desc_t e;
        e.code_patch = true;
        e.label = "code patch: shr rdx, 63 -> shr rdx, 1";
        // 0x00 mov rdx,rdi | 0x03 shr rdx,63 | 0x07 mov rax,rdi | ...
        e.base_code.assign(0x14, 0x90);
        e.take_code = e.base_code;
        e.take_code[0x05] = 0x01; // the shr's immediate, at 0x03..0x07
        loom_take_view_t vp = loom_take_view(base, take, t.edges.data(),
                                             t.edges.size(), e, "", "");
        check("a code patch seeds at the first step whose bytes differ",
              !vp.cone.empty() && vp.cone.front() == 1, loom_take_dump(vp));
    }
    {
        // A patch that changed nothing the run executed seeds NOTHING, and the
        // cone is honestly empty rather than defaulting to step 0.
        loom_edit_desc_t e;
        e.code_patch = true;
        e.label = "code patch: no executed byte changed";
        e.base_code.assign(0x40, 0x90);
        e.take_code = e.base_code;
        e.take_code[0x30] = 0x11; // past everything this run executed
        loom_take_view_t vp = loom_take_view(base, take, t.edges.data(),
                                             t.edges.size(), e, "", "");
        check("an unexecuted patch yields an empty cone", vp.cone.empty(),
              loom_take_dump(vp));
    }

    // --- the plan: dashed tails, patient zero, fault card ------------------
    {
        loom_view_t cam;
        cam.steps_per_px = 0.05;
        cam.px_w = 800;
        cam.px_h = 200;
        cam.lane_h = 18;
        std::vector<loom_prim_t> prims;
        loom_take_plan(v, cam, &prims);
        auto count = [&](loom_prim k) {
            size_t n = 0;
            for (const loom_prim_t &p : prims)
                if (p.kind == k)
                    n++;
            return n;
        };
        check("exactly one patient-zero prim",
              count(loom_prim::patient_zero) == 1, "");
        check("one dashed tail per unaligned step",
              count(loom_prim::take_dashed_tail) == 1,
              "got " + std::to_string(count(loom_prim::take_dashed_tail)));
        check("one dim prim", count(loom_prim::take_dim) == 1, "");
        check("three hot prims", count(loom_prim::take_hot) == 3,
              "got " + std::to_string(count(loom_prim::take_hot)));
        check("no fault card without a fault",
              count(loom_prim::fault_card) == 0, "");
        for (const loom_prim_t &p : prims)
            if (p.kind == loom_prim::take_dashed_tail)
                check("the dashed tail's hover refuses to read as agreement",
                      p.text == std::string(kLoomDashedTailHover), p.text);

        // A faulting take renders a CARD, not a crash.
        loom_take_view_t vf =
            loom_take_view(base, take, t.edges.data(), t.edges.size(),
                           arg_edit(), "read fault at 0x0 — no mapping", "");
        prims.clear();
        loom_take_plan(vf, cam, &prims);
        check("a faulting take renders a fault card",
              count(loom_prim::fault_card) == 1, "");
    }

    // --- determinism --------------------------------------------------------
    {
        loom_take_view_t a = loom_take_view(base, take, t.edges.data(),
                                            t.edges.size(), arg_edit(), "", "");
        check("two identical views are byte-identical",
              loom_take_dump(a) == dump, "");
    }

    if (failures) {
        std::fprintf(stderr, "%d loom take-view check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_takeview: all checks passed\n");
    return 0;
}
