// test_timeline.cpp — the operand-value timeline (04-replay-views.md T4).
//
// The load-bearing assertions here are the annotation LITERALS. They are copied
// from the grammar cli/asmspy_dataview.h documents, so if the GUI ever grew its
// own dialect of "->0x2a" this test fails — which is the whole point of reusing
// the TUI's helper instead of writing a second annotator.
#include "view_test.h"
#include "views/timeline.h"

using namespace asmdesk;
using vt::load;

int main() {
    // --- a real corpus recording: exact annotation strings ------------------
    {
        Streams s = load("add_signed.asmtrace");
        dt_timeline t = dt_timeline_build(s);
        vt::eq("add_signed rows", t.rows.size(), size_t{3});
        vt::check("add_signed has no banner", t.banner.empty(),
                  "a clean recording carries none, got: " + t.banner);

        // step 0: `mov rax, rdi` — the register READ is skipped (the disasm
        // already names it) and the WRITE shows its captured value.
        vt::eq("step 0 annotation", t.rows[0].ann, std::string("->0x28"));
        // step 1: `add rax, rsi` — two writes (the flags register and rax).
        vt::eq("step 1 annotation", t.rows[1].ann, std::string("->0x2 ->0x2a"));
        // step 2: `ret` — a register write plus a memory LOAD, which shows its
        // effective address and value.
        vt::eq("step 2 annotation", t.rows[2].ann,
               std::string("->0x210000 [0x20fff8]->0xf00000"));

        // def-use counts: the one recorded edge is 0 -> 1.
        vt::eq("step 0 out-edges", t.rows[0].n_out, size_t{1});
        vt::eq("step 0 in-edges", t.rows[0].n_in, size_t{0});
        vt::eq("step 1 in-edges", t.rows[1].n_in, size_t{1});
        vt::golden("timeline-add_signed.txt", dt_timeline_dump(t));
    }

    // --- uncaptured values, wide values, and dropped steps ------------------
    {
        Streams s = load("views/trunc-dataflow.asmtrace");
        dt_timeline t = dt_timeline_build(s);
        vt::eq("trunc-dataflow rows", t.rows.size(), size_t{5});
        vt::check("trunc-dataflow banner says TRUNCATED",
                  t.banner.find("TRUNCATED") != std::string::npos,
                  "banner: " + t.banner);
        vt::check("the banner names the dropped steps",
                  t.banner.find("3 of 5 steps") != std::string::npos,
                  "banner must say 3 of 5 steps, got: " + t.banner);
        vt::check("the banner says a blank row is not offset 0",
                  t.banner.find("not as offset 0") != std::string::npos,
                  "banner: " + t.banner);
        // A step with no df_step event is UNKNOWN, not "offset 0". The row is
        // flagged so neither the draw nor the dump can claim the routine's
        // entry instruction ran there.
        vt::check("step 2 is flagged missing", t.rows[2].missing,
                  "a dropped step must not look like a recorded one");
        vt::check("step 3 is flagged missing", t.rows[3].missing,
                  "not flagged");
        vt::check("recorded steps are NOT flagged missing",
                  !t.rows[0].missing && !t.rows[1].missing &&
                      !t.rows[4].missing,
                  "a recorded step was flagged as dropped");
        // `?` for an uncaptured value and `[wide]` for a >8-byte one: both are
        // measurement outcomes, and neither is rendered as a number.
        vt::eq("step 1 annotation (uncaptured + wide)", t.rows[1].ann,
               std::string("[0x601048]<-[wide]"));
        // Retained steps keep everything they carried — truncation degrades
        // the view, it does not blank it.
        vt::eq("step 4 keeps its disasm", t.rows[4].disasm, std::string("ret"));
        vt::eq("step 4 in-edges", t.rows[4].n_in, size_t{1});
        vt::golden("timeline-trunc-dataflow.txt", dt_timeline_dump(t));
    }

    // --- cone emphasis: IN-SLICE vs DIMMED ----------------------------------
    {
        Streams s = load("sum_via_rbx.asmtrace");
        dt_timeline plain = dt_timeline_build(s);
        for (const dt_timeline_row &r : plain.rows)
            vt::check("no cone => NORMAL at step " + std::to_string(r.step),
                      r.style == dt_rowstyle::normal, "got a slice style");

        dt_slice cone =
            dt_slice_backward(s.df.edges, s.df.nsteps, s.df.nsteps - 1);
        vt::check("the backward cone from the last step is non-empty",
                  !cone.steps.empty(), "no edges reached it");
        dt_timeline lit = dt_timeline_build(s, &cone);
        size_t in = 0, dim = 0;
        for (const dt_timeline_row &r : lit.rows) {
            if (r.style == dt_rowstyle::in_slice)
                in++;
            if (r.style == dt_rowstyle::dimmed)
                dim++;
            vt::check(
                "style agrees with membership at step " +
                    std::to_string(r.step),
                (r.style == dt_rowstyle::in_slice) == cone.contains(r.step),
                "the row emphasis must be the slice, not an approximation");
        }
        vt::check("some rows are lit", in > 0, "none");
        vt::check("some rows are dimmed", dim > 0, "none");
        vt::golden("timeline-sum_via_rbx-cone.txt", dt_timeline_dump(lit));
    }

    // --- two recordings: rows past patient zero are UNALIGNED ---------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/pair-b.asmtrace");
        dt_timeline t = dt_timeline_build2(a, b);
        vt::check("pair timeline is two-up", t.two_up, "two_up must be set");
        // pair-a/pair-b carry no dataflow stream, so there are no rows to mark
        // — but the divergence itself must still be found and reported.
        vt::check("pair timeline found the divergence", t.div_step.has_value(),
                  "the divergence comes from the shared alignment seam");
        if (t.div_step)
            vt::eq("pair timeline divergence step", *t.div_step, 3u);
    }

    // --- an uncomparable pair says so, and does not pretend to align --------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/mixed-basis.asmtrace");
        dt_timeline t = dt_timeline_build2(a, b);
        vt::check("an uncomparable B is named in the banner",
                  t.banner.find("NOT COMPARABLE") != std::string::npos,
                  "banner: " + t.banner);
        vt::check("no divergence is claimed for an uncomparable pair",
                  !t.div_step.has_value(),
                  "a refused pair has no meaningful divergence");
    }

    return vt::report("test_timeline");
}
