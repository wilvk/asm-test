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

    // --- 37 T1: the region a row belongs to, when there is more than one ----
    // A `df_step`'s offset is RELATIVE to its region base (`rbase`), so two
    // regions' rows collide on the offset axis: an `auto` candidate walk
    // records span 0x100000 and span 0x110000 into one stream, and offset 0x6
    // means a different instruction in each. The row must carry its own base
    // and the view must say which region it is, or the timeline shows two
    // regions as one.
    {
        Streams s = load("scene-df-two-span.asmtrace");
        dt_timeline t = dt_timeline_build(s);
        vt::eq("two-span rows", t.rows.size(), size_t{13});
        // The distinct regions the rows span, ascending — the fact the view
        // needs in order to know a region column is warranted at all.
        vt::eq("two-span region count", t.regions.size(), size_t{2});
        if (t.regions.size() == 2) {
            vt::eq("first region base", t.regions[0], uint64_t{0x100000});
            vt::eq("second region base", t.regions[1], uint64_t{0x110000});
        }
        // Each row's OWN base, not the stream's first or last.
        vt::eq("step 2 region", t.rows[2].rbase, uint64_t{0x100000});
        vt::eq("step 5 region", t.rows[5].rbase, uint64_t{0x110000});
        // Steps 2 and 5 share offset 0x6 and disassembly, and differ ONLY by
        // region. Two identical dump lines here is the bug this test exists
        // for: it is the view claiming one region ran both.
        vt::eq("steps 2 and 5 share an offset", t.rows[2].off, t.rows[5].off);
        {
            std::string d = dt_timeline_dump(t);
            vt::check("a multi-region dump names each row's region",
                      d.find("region=0x100000") != std::string::npos &&
                          d.find("region=0x110000") != std::string::npos,
                      "neither region is named:\n" + d);
        }
        vt::golden("timeline-two-span.txt", dt_timeline_dump(t));
    }

    // --- one region: no region chrome, and the dump is unchanged ------------
    // Every ordinary recording is a single region, where the region is a
    // property of the whole timeline and not of any row. Naming it per row
    // would be noise, so `regions` holds it and the rows say nothing.
    {
        Streams s = load("add_signed.asmtrace");
        dt_timeline t = dt_timeline_build(s);
        vt::eq("single-region region count", t.regions.size(), size_t{1});
        vt::eq("single-region base", t.regions[0], uint64_t{0x100000});
        vt::eq("the row still carries it", t.rows[0].rbase, uint64_t{0x100000});
        vt::check("a single-region dump names no per-row region",
                  dt_timeline_dump(t).find("region=") == std::string::npos,
                  "one region needs no per-row column");
    }

    // --- 28 R1 T3: a wide operand's bytes render as hex, not "[wide]" --------
    // No golden carries a wide-with-bytes operand (the emulator L0 corpus has no
    // >8-byte values), so this is a synthetic Streams: the reconstruction of the
    // side buffer in to_recs is what lets the shared annotator show the bytes.
    {
        Streams s;
        s.df.insn_off = {0};
        s.df.nsteps = 1;
        s.df.step_present = {1};
        s.df.disasm = {""};
        ValRec w;
        w.step = 0;
        w.space = "reg";
        w.reg = 35; // rax
        w.size = 16;
        w.write = true;
        w.value_valid = true;
        w.wide = true;
        for (int i = 0; i < 16; i++)
            w.bytes.push_back(static_cast<uint8_t>(i + 1));
        s.df.recs.push_back(w);

        dt_timeline t = dt_timeline_build(s);
        vt::check("a wide operand renders its bytes as hex",
                  t.rows.size() == 1 &&
                      t.rows[0].ann.find(
                          "0x0102030405060708090a0b0c0d0e0f10") !=
                          std::string::npos,
                  t.rows.empty() ? "no rows" : t.rows[0].ann);
        vt::check("no [wide] placeholder when the bytes are present",
                  !t.rows.empty() &&
                      t.rows[0].ann.find("[wide]") == std::string::npos,
                  t.rows.empty() ? "no rows" : t.rows[0].ann);

        // Absent bytes still degrade gracefully to "[wide]".
        s.df.recs[0].bytes.clear();
        dt_timeline t2 = dt_timeline_build(s);
        vt::check("a bytes-less wide operand degrades to [wide]",
                  !t2.rows.empty() &&
                      t2.rows[0].ann.find("[wide]") != std::string::npos,
                  t2.rows.empty() ? "no rows" : t2.rows[0].ann);
    }

    // --- doc 68 T1: the SCOPE line -----------------------------------------
    // The banner speaks only when the recording is damaged, so a clean capture
    // of one function inside a browser rendered N rows under nothing at all —
    // indistinguishable from a complete trace of a small program. The scope line
    // is unconditional and says what population the rows are.
    {
        // Nothing to scope: a trace-only recording has no dataflow at all, and a
        // scope line over an empty set would be chrome about nothing.
        Streams none;
        vt::check("scope: no dataflow -> no line",
                  dt_dataflow_scope(none).empty(),
                  "got: " + dt_dataflow_scope(none));

        Streams s = load("add_signed.asmtrace");
        const std::string sc = dt_dataflow_scope(s);
        vt::check("scope: a clean recording still gets a scope line",
                  !sc.empty(), "a clean capture must not be silent about scope");
        vt::check("scope: says it is a region, not the process",
                  sc.find("region") != std::string::npos &&
                      sc.find("whole process") != std::string::npos,
                  sc);
        vt::check("scope: counts the recorded steps",
                  sc.find("3 ") != std::string::npos, sc);
        // A clean recording's scope line is NOT the truncation banner: the two
        // are different facts and must not be conflated.
        vt::check("scope: is not the banner", dt_timeline_build(s).banner.empty(),
                  "the scope line must not turn a clean recording's banner on");

        // The denominator is never invented. With no footer total and no trace
        // stream the line states what it recorded and stops there.
        vt::check("scope: no invented denominator",
                  sc.find(" of ") == std::string::npos, sc);
        // With a stated total LARGER than what arrived, say both — that is the
        // producer's own measurement, not a guess.
        Streams big = load("add_signed.asmtrace");
        big.has_steps_total = true;
        big.steps_total = 2013;
        const std::string bsc = dt_dataflow_scope(big);
        vt::check("scope: a stated total is reported as N of M",
                  bsc.find("of 2013") != std::string::npos, bsc);
        // A total that is NOT larger adds nothing and must not be printed as a
        // tautological "3 of 3".
        Streams same = load("add_signed.asmtrace");
        same.has_steps_total = true;
        same.steps_total = 3;
        vt::check("scope: an equal total is not printed",
                  dt_dataflow_scope(same).find(" of ") == std::string::npos,
                  dt_dataflow_scope(same));

        // The region the rows are relative to, when the wire stated one.
        Streams reg = load("add_signed.asmtrace");
        reg.df.rbase_present = true;
        for (auto &b : reg.df.insn_rbase)
            b = 0x100000;
        vt::check("scope: names the region base the offsets are relative to",
                  dt_dataflow_scope(reg).find("0x100000") != std::string::npos,
                  dt_dataflow_scope(reg));

        // Two regions in one pass: counted, never printed as one base.
        Streams two = load("add_signed.asmtrace");
        two.df.rbase_present = true;
        for (size_t k = 0; k < two.df.insn_rbase.size(); k++)
            two.df.insn_rbase[k] = k == 0 ? 0x100000 : 0x200000;
        vt::check("scope: multiple regions are counted, not collapsed",
                  dt_dataflow_scope(two).find("2 single-stepped regions") !=
                      std::string::npos,
                  dt_dataflow_scope(two));
    }

    return vt::report("test_timeline");
}
