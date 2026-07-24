// test_canvas.cpp — the trace canvas's rules (04-replay-views.md T3).
// Headless: no display, no GL, no engines. The builder is pure, so this test
// links canvas.o and the document model and nothing else.
#include "view_test.h"
#include "views/canvas.h"

using namespace asmdesk;
using vt::load;

int main() {
    // --- straight-line: every row heat 1, disasm from the recording ---------
    {
        Streams s = load("add_signed.asmtrace");
        dt_canvas c = dt_canvas_build(s);
        vt::eq("add_signed basis", c.basis, std::string("rel"));
        vt::check("add_signed has rows", !c.rows.empty(), "no rows built");
        vt::check("add_signed banner is empty", c.banner.empty(),
                  "a complete recording must carry no banner, got: " +
                      c.banner);
        for (const dt_canvas_row &r : c.rows)
            vt::eq("add_signed heat at " + std::to_string(r.off), r.heat, 1u);
        vt::golden("canvas-add_signed.txt", dt_canvas_dump(c));
    }

    // --- a loop: heat > 1, and the gutter is BLOCK-granular -----------------
    {
        Streams s = load("views/loop-coverage.asmtrace");
        dt_canvas c = dt_canvas_build(s);
        size_t hot = 0, covered = 0;
        for (const dt_canvas_row &r : c.rows) {
            if (r.heat > 1)
                hot++;
            if (r.covered)
                covered++;
            // The rule that keeps the gutter honest: coverage is what a
            // `coverage` event recorded (block starts), never "this executed".
            vt::eq("loop covered==block_start at " + std::to_string(r.off),
                   r.covered, r.block_start);
        }
        vt::eq("loop rows with heat > 1", hot, size_t{3});
        vt::eq("loop covered rows", covered, size_t{2});
        vt::golden("canvas-loop.txt", dt_canvas_dump(c));
    }

    // --- mixed bases: a REFUSAL, not a best effort --------------------------
    {
        Streams s = load("views/mixed-basis.asmtrace");
        dt_canvas c = dt_canvas_build(s);
        vt::check("mixed-basis sets basis_error", !c.basis_error.empty(),
                  "a stream mixing rel and abs must set basis_error");
        vt::check("mixed-basis draws NO rows", c.rows.empty(),
                  "got " + std::to_string(c.rows.size()) +
                      " rows; any row here is mis-attributed");
        vt::check("mixed-basis names both bases",
                  c.basis_error.find("rel") != std::string::npos &&
                      c.basis_error.find("abs") != std::string::npos,
                  "the reason must name the two bases: " + c.basis_error);
        vt::golden("canvas-mixed-basis.txt", dt_canvas_dump(c));
    }

    // --- truncation: a loud banner naming BOTH numbers ----------------------
    {
        Streams s = load("views/trunc-trace.asmtrace");
        dt_canvas c = dt_canvas_build(s);
        vt::check("trunc-trace is truncated", c.truncated,
                  "the footer and the coverage event both declare it");
        vt::check("trunc-trace banner says TRUNCATED",
                  c.banner.find("TRUNCATED") != std::string::npos,
                  "banner: " + c.banner);
        vt::check("trunc-trace banner names the recorded count",
                  c.banner.find(" 4 of 93 ") != std::string::npos,
                  "banner must say 4 of 93 instructions, got: " + c.banner);
        vt::check("trunc-trace banner names the block counts",
                  c.banner.find("2 of 5 blocks") != std::string::npos,
                  "banner must say 2 of 5 blocks, got: " + c.banner);
        vt::golden("canvas-trunc-trace.txt", dt_canvas_dump(c));
    }

    // --- D10 absence: offsets, byte-stable, never an error ------------------
    {
        Streams s = load("views/no-disasm.asmtrace");
        dt_canvas c = dt_canvas_build(s);
        vt::check("no-disasm still builds rows", !c.rows.empty(),
                  "absent disasm degrades, it does not fail");
        for (const dt_canvas_row &r : c.rows)
            vt::check("no-disasm row " + std::to_string(r.off) + " has no text",
                      r.disasm.empty(), "got: " + r.disasm);
        vt::golden("canvas-no-disasm.txt", dt_canvas_dump(c));
    }

    // --- two recordings: the delta column and patient zero (T7) -------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/pair-b.asmtrace");
        dt_canvas c = dt_canvas_build2(a, b);
        vt::check("pair canvas is two-up", c.two_up, "two_up must be set");
        vt::check("pair canvas found the divergence", c.div_step.has_value(),
                  "pair-a/pair-b diverge at step 3");
        if (c.div_step)
            vt::eq("pair divergence step", *c.div_step, 3u);
        bool saw_b_only = false, saw_a_only = false;
        for (const dt_canvas_row &r : c.rows) {
            if (r.in_b && !r.in_a)
                saw_b_only = true;
            if (r.in_a && !r.in_b)
                saw_a_only = true;
        }
        vt::check("pair canvas shows an A-only block", saw_a_only,
                  "none found");
        vt::check("pair canvas shows a B-only block", saw_b_only, "none found");
        vt::golden("canvas-pair.txt", dt_canvas_dump(c));
    }

    // --- two recordings in different bases: also a refusal ------------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/mixed-basis.asmtrace");
        dt_canvas c = dt_canvas_build2(a, b);
        vt::check("A vs an unplaceable B refuses", !c.basis_error.empty(),
                  "a B that cannot be placed must refuse the whole canvas");
        vt::check("that refusal draws no rows", c.rows.empty(),
                  "got " + std::to_string(c.rows.size()) + " rows");
    }

    return vt::report("test_canvas");
}
