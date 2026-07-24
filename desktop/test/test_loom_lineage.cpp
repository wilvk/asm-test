// test_loom_lineage.cpp — selection, the generation walk, the biography and the
// zeroization audit (05-loom-day-one.md T3). Render-only: fabric.o + lineage.o.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "loom/lineage.h"
#include "loom_fixture.h"

using namespace asmdesk;
using namespace loomfx;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}
static std::string show(const std::vector<uint32_t> &v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? "," : "") + std::to_string(v[i]);
    return s + "}";
}
static void eq(const std::string &what, const std::vector<uint32_t> &got,
               const std::vector<uint32_t> &want) {
    if (got != want)
        fail(what, "got " + show(got) + ", want " + show(want));
}

int main() {
    Fixture fx;
    loom_fabric_t f = build(fx);
    const asmtest_defuse_edge_t *E = fx.edges.data();
    const size_t N = fx.edges.size();

    const int rax = lane_named(f, "rax");
    const int rcx = lane_named(f, "rcx");
    const int rdx = lane_named(f, "rdx");
    const int rdi = lane_named(f, "rdi");
    const int band = band_lane(f);

    // --- selecting (rax, step 4) ------------------------------------------
    // The hand-derived slice at examples/test_dataflow_emu.c:111 — {0,1,2,3,4},
    // and NOT 5: the `ret` reads rax on real hardware, but this trace's edges
    // record no such dependence, and the fabric shows edges, not folklore.
    loom_selection_t sel;
    check("select (rax, 4)",
          loom_select(f, E, N, static_cast<uint32_t>(rax), 4, &sel), "refused");
    eq("closure of (rax, 4)", sel.steps, {0, 1, 2, 3, 4});
    check("origin step is the DEFINING step, not the click",
          sel.origin_step == 4, std::to_string(sel.origin_step));

    // --- the generation walk ------------------------------------------------
    eq("generation 0 is the origin", loom_generation(sel, 0), {4});
    eq("[ from step 4 highlights {3}", loom_generation(sel, -1), {3});
    eq("[[ highlights {2}", loom_generation(sel, -2), {2});
    eq("the load-after-store ring", loom_generation(sel, -3), {1});
    eq("the entry ring", loom_generation(sel, -4), {0});
    check("gen bounds", sel.gen_lo == -4 && sel.gen_hi == 0,
          std::to_string(sel.gen_lo) + ".." + std::to_string(sel.gen_hi));
    {
        loom_selection_t s = sel;
        loom_gen_step(s, -1);
        check("[ moves one ring out", s.gen_view == -1, "");
        for (int i = 0; i < 10; i++)
            loom_gen_step(s, -1);
        check("[ clamps at gen_lo", s.gen_view == -4,
              std::to_string(s.gen_view));
        for (int i = 0; i < 10; i++)
            loom_gen_step(s, +1);
        check("] clamps at gen_hi", s.gen_view == 0,
              std::to_string(s.gen_view));
    }

    // --- forward selection --------------------------------------------------
    {
        loom_selection_t s;
        loom_select(f, E, N, static_cast<uint32_t>(rdi), 0, &s);
        check("rdi at step 0 is born of untraced state", s.born_untraced, "");
        check("its closure is empty, and it SAYS why", s.steps.empty(),
              show(s.steps));
        check("the note names the rule",
              s.note.find("no producer inside the recorded window") !=
                  std::string::npos,
              s.note);
    }
    {
        loom_selection_t s;
        loom_select(f, E, N, static_cast<uint32_t>(rcx), 3, &s);
        eq("selecting rcx at 3 reaches both directions", s.steps,
           {0, 1, 2, 3, 4});
        eq("its descendants are generation +1", loom_generation(s, 1), {3});
        eq("its ancestors are generation -1", loom_generation(s, -1), {1});
    }
    // A lane the trace has not reached yet holds NOTHING; that is not the same
    // as holding zero, and select must refuse rather than invent a residency.
    {
        loom_selection_t s;
        check("selecting an unreached lane is refused",
              !loom_select(f, E, N, static_cast<uint32_t>(rdx), 0, &s), "");
        check("selecting an out-of-range lane is refused",
              !loom_select(f, E, N, 9999, 0, &s), "");
    }

    // --- biography ----------------------------------------------------------
    {
        // The step-1 store: a STORE of value 7, whose consumer is the
        // load-after-store at step 2.
        uint32_t store = kLoomNoSpan;
        for (uint32_t i = 0; i < f.spans.size(); i++)
            if (static_cast<int>(f.spans[i].lane) == band &&
                f.spans[i].t_write == 1)
                store = i;
        check("the step-1 store span exists", store != kLoomNoSpan, "");
        auto bio = loom_biography(f, E, N, store);
        std::string all;
        for (const loom_bio_row_t &r : bio)
            all += r.kind + ": " + r.text + "\n";
        check("the biography names the birth kind 'store'",
              bio[0].kind == "birth" &&
                  bio[0].text.find("store to mem[") != std::string::npos,
              all);
        check("the biography names the value 7",
              bio[0].text.find("0x7") != std::string::npos, all);
        check("the biography carries the load-after-store hop",
              all.find("hop: read at mov rcx, qword ptr [rsp - 8]") !=
                  std::string::npos,
              all);
        check("the biography names the producer tier",
              all.find("producer: dataflow-emu — exact, replay") !=
                  std::string::npos,
              all);
        check("the biography states the isolated guest",
              all.find("isolated guest") != std::string::npos, all);
        check("the biography states the window bound",
              all.find("bounded to the 6 recorded steps") != std::string::npos,
              all);
    }
    {
        // A register value that escapes to memory says so.
        uint32_t rax0 = kLoomNoSpan;
        for (uint32_t i = 0; i < f.spans.size(); i++)
            if (static_cast<int>(f.spans[i].lane) == rax &&
                f.spans[i].t_write == 0)
                rax0 = i;
        auto bio = loom_biography(f, E, N, rax0);
        std::string all;
        for (const loom_bio_row_t &r : bio)
            all += r.kind + ": " + r.text + "\n";
        check("a value written to a band reports the escape",
              all.find("escapes to memory at step 1") != std::string::npos,
              all);
    }
    {
        // A truncated fabric's biography must say the rows are a lower bound.
        loom_provenance_t p = prov();
        p.truncated = true;
        loom_fabric_t t = build(fx, p);
        auto bio = loom_biography(t, E, N, 0);
        std::string all;
        for (const loom_bio_row_t &r : bio)
            all += r.text + "\n";
        check("a truncated window is stated as a lower bound",
              all.find("lower bound") != std::string::npos, all);
    }

    // --- the zeroization audit ---------------------------------------------
    {
        std::vector<uint32_t> lit;
        loom_audit_lanes(f, E, N, 0, 5, &lit);
        std::vector<uint32_t> want = {
            static_cast<uint32_t>(rdx), static_cast<uint32_t>(rcx),
            static_cast<uint32_t>(rax), static_cast<uint32_t>(band)};
        std::sort(want.begin(), want.end());
        eq("birth 0 at the last step lights {rcx, rdx, rax, mem band}", lit,
           want);

        loom_audit_lanes(f, E, N, 0, 1, &lit);
        std::vector<uint32_t> want1 = {static_cast<uint32_t>(rax),
                                       static_cast<uint32_t>(band)};
        std::sort(want1.begin(), want1.end());
        eq("scrubbing back to step 1 lights {rax, mem band}", lit, want1);

        // rdi/rsi/rsp hold values whose provenance starts at instrumentation.
        // They are never lit: the fabric cannot claim they descend from step 0.
        for (const char *n : {"rdi", "rsi", "rsp"}) {
            loom_audit_lanes(f, E, N, 0, 5, &lit);
            uint32_t lane = static_cast<uint32_t>(lane_named(f, n));
            check(std::string("a born-untraced resident never lights (") + n +
                      ")",
                  std::find(lit.begin(), lit.end(), lane) == lit.end(), "");
        }
    }
    {
        // Hollow residents light with the flag: the route is known, the value
        // is not, and the audit must not present the two the same way.
        Fixture h;
        // Make the step-4 rax write a deferred (never-filled) value.
        for (at_val_rec_t &r : h.recs)
            if (r.step == 4 && r.reg == RAX && r.is_write)
                r.value_valid = false;
        h.bind();
        loom_fabric_t hf = build(h);
        std::vector<loom_lit_t> lit;
        loom_audit(hf, h.edges.data(), h.edges.size(), 0, 5, &lit);
        bool found = false;
        for (const loom_lit_t &r : lit)
            if (static_cast<int>(r.lane) == lane_named(hf, "rax"))
                found = r.hollow;
        check("a hollow resident lights WITH the hollow flag", found,
              "the audit lit an uncaptured value as if it had one");
    }

    // --- the audit's copy ---------------------------------------------------
    check("the audit title is bounded, verbatim",
          std::string(kLoomAuditTitle) ==
              "zeroization audit — bounded to the traced window",
          kLoomAuditTitle);
    check("the audit hover states what 'clear' means, verbatim",
          std::string(kLoomAuditHover) ==
              "'clear' means *not overwritten within the traced window* — "
              "untraced state and post-trace writes are invisible",
          kLoomAuditHover);

    if (failures) {
        std::fprintf(stderr, "%d loom lineage check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_lineage: all checks passed\n");
    return 0;
}
