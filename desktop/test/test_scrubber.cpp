// test_scrubber.cpp — the register time-travel scrubber's pure builder
// (09-teaching-producers.md T3). No ImGui, no engine: it links scrubber.o +
// stepindex.o + the doc model and NOTHING else, which is the proof that a
// render-only viewer scrubs a register file with zero engine deps (D4).
//
// It covers the four things the brief asks for: seek (O(1) to the file at a
// step), diff-highlight (each register vs the previous held step), the tear
// (a ring that dropped a prefix renders UNKNOWN, never zero), and the
// producer-absent message (a recording with no `regstate` events).
#include "view_test.h"

#include "analysis/stepindex.h"
#include "views/scrubber.h"

using namespace asmdesk;

namespace {

// Load a golden recording (not decoded streams — the scrubber reads the raw
// `regstate`/`end` events through the step index).
Recording load_rec(const std::string &name) {
    std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
    std::string err;
    auto r = load_recording_file(path, err);
    if (!r) {
        vt::fail("load " + name, err);
        return Recording{};
    }
    return *r;
}

// The whole scrub, as text: every step of the full space [0, total), so one
// golden pins seek, diff-highlight AND the torn region in file order.
std::string scrub_dump(const StepIndex &idx) {
    std::string o;
    uint64_t total = idx.total_steps();
    for (uint64_t ph = 0; ph < total; ph++)
        o += dt_scrubber_dump(dt_scrubber_build(idx, ph));
    return o;
}

} // namespace

int main() {
    // --- a real corpus recording: seek + diff-highlight ---------------------
    {
        Recording r = load_rec("add_signed.asmtrace");
        StepIndex idx = build_step_index(r);
        vt::check("add_signed has a regstate producer", idx.present(),
                  "the corpus recording carries three regstate events");
        vt::eq("add_signed held count", idx.count(), size_t{3});
        vt::eq("add_signed dropped", idx.dropped, uint64_t{0});
        vt::eq("add_signed first step", idx.first_step, uint64_t{0});
        vt::eq("add_signed total steps", idx.total_steps(), uint64_t{3});

        // Seek: O(1) to the file BEFORE a given step.
        const RegFile *s0 = idx.at_step(0);
        const RegFile *s1 = idx.at_step(1);
        const RegFile *s2 = idx.at_step(2);
        vt::check("step 0 seekable", s0 != nullptr, "held step 0 must resolve");
        vt::check("step 1 seekable", s1 != nullptr, "held step 1 must resolve");
        vt::check("step 2 seekable", s2 != nullptr, "held step 2 must resolve");
        vt::check("a step past the end is UNKNOWN, not the last file",
                  idx.at_step(3) == nullptr, "seek must not clamp");

        // The full register file is present (18 fields: 16 GP + rip + rflags).
        vt::eq("step 0 field count", s0->fields.size(), size_t{18});
        const RegField *rax0 = s0->find("rax");
        const RegField *rip0 = s0->find("rip");
        vt::check("rax at step 0 is 0", rax0 && rax0->value == 0, "before mov");
        vt::check("rip at step 0 is 0x100000", rip0 && rip0->value == 1048576,
                  "the routine's entry");

        // Diff-highlight: rax is written at step 0 (`mov rax, rdi`), so step 1's
        // rax differs from step 0's and is marked changed; an untouched register
        // (rbx) is not.
        const RegField *rax1 = s1->find("rax");
        const RegField *rbx1 = s1->find("rbx");
        vt::check("rax changed at step 1",
                  rax1 && rax1->changed && rax1->value == 40,
                  "mov rax, rdi wrote 40");
        vt::check("rbx unchanged at step 1", rbx1 && !rbx1->changed,
                  "nothing wrote rbx");
        vt::check("step 0 has no diff baseline", !s0->has_prev,
                  "the first held step cannot diff against a predecessor");

        // The scrubber model reflects the highlight at its playhead.
        dt_scrubber sc = dt_scrubber_build(idx, 1);
        vt::check("scrubber lands on a held step", sc.at_held,
                  "step 1 is held");
        bool rax_lit = false;
        for (const dt_scrubber_reg &rg : sc.regs)
            if (rg.name == "rax")
                rax_lit = rg.changed;
        vt::check("scrubber highlights rax at step 1", rax_lit, "the write");

        vt::golden("scrubber-add_signed.txt", scrub_dump(idx));
    }

    // --- the dishonesty fixture: a torn edge --------------------------------
    {
        Recording r = load_rec("regstate-truncated.asmtrace");
        StepIndex idx = build_step_index(r);
        vt::check("truncated fixture has a producer", idx.present(), "2 held");
        vt::eq("truncated held count", idx.count(), size_t{2});
        vt::eq("truncated dropped prefix", idx.dropped, uint64_t{4});
        vt::eq("truncated first step", idx.first_step, uint64_t{4});
        vt::check("the index is marked truncated", idx.truncated,
                  "the ring evicted a prefix");
        vt::eq("truncated total steps", idx.total_steps(), uint64_t{6});

        // Seeking into the torn region is UNKNOWN, never step 0's file.
        vt::check("step 0 is torn (dropped)", idx.at_step(0) == nullptr,
                  "the ring evicted it — it is unknown, not zero");
        vt::check("step 3 is torn (dropped)", idx.at_step(3) == nullptr,
                  "still inside the evicted prefix");
        const RegFile *s4 = idx.at_step(4);
        vt::check("step 4 is the first held", s4 != nullptr, "held [4,5]");
        vt::check("the first held step has no baseline (its predecessor was "
                  "dropped)",
                  s4 && !s4->has_prev,
                  "diffing against an evicted step would be a lie");

        // rbx goes 5 -> 0 across the two held steps (`pop rbx`), so it is the
        // register the diff must light at step 5.
        const RegFile *s5 = idx.at_step(5);
        const RegField *rbx5 = s5 ? s5->find("rbx") : nullptr;
        vt::check("rbx changed at step 5",
                  rbx5 && rbx5->changed && rbx5->value == 0,
                  "pop rbx restored it");

        // The scrubber renders the torn region and names the drop count.
        dt_scrubber torn = dt_scrubber_build(idx, 0);
        vt::check("playhead 0 is torn_here", torn.torn_here, "in the tear");
        vt::check("the banner names the dropped count",
                  torn.banner.find("4 step(s) dropped") != std::string::npos,
                  "banner: " + torn.banner);

        vt::golden("scrubber-regstate-truncated.txt", scrub_dump(idx));
    }

    // --- a recording with no producer: the two branches (30 R3 T4) ----------
    // BRANCH A — NOT emulator-replayable: the honest refusal stands, naming the
    // reason, deep-linking the docs, and NEVER offering synthesis. sum_via_rbx is
    // an emu-l0 recording with a `code` header but NO codeimage bytes, so there is
    // nothing to re-run.
    {
        Recording r = load_rec("sum_via_rbx.asmtrace");
        StepIndex idx = build_step_index(r);
        vt::check("sum_via_rbx has no regstate producer", !idx.present(),
                  "this recording predates the ring");
        vt::eq("an absent producer indexes to zero steps", idx.total_steps(),
               uint64_t{0});

        dt_scrubber_replay replay = dt_scrubber_replayable(r);
        vt::check("sum_via_rbx is NOT emulator-replayable (no codeimage bytes)",
                  !replay.replayable, "reason: " + replay.why);

        dt_scrubber sc = dt_scrubber_build(idx, 0, replay);
        vt::check("the view states the producer is absent", !sc.present,
                  "present must be false");
        vt::check("the honest refusal does NOT offer synthesis here",
                  !sc.synthesizable, "no codeimage -> no re-run");
        vt::check("the message names the --steps producer",
                  sc.absent_message.find("--steps") != std::string::npos,
                  "message: " + sc.absent_message);
        vt::check("the refusal names WHY it is not replayable here",
                  sc.absent_message.find("codeimage") != std::string::npos,
                  "message: " + sc.absent_message);
        // The stale "not a day-one feature" disclaimer is GONE — synthesis IS a
        // feature now, just not available for this (non-replayable) recording.
        vt::check("the message no longer claims 'not a day-one feature'",
                  sc.absent_message.find("not a day-one feature") ==
                      std::string::npos,
                  "message: " + sc.absent_message);
        vt::check("the view deep-links the docs", !sc.docs.empty(),
                  "docs link must be present");

        vt::golden("scrubber-no-producer.txt", dt_scrubber_dump(sc));
    }

    // BRANCH B — emulator-replayable (Author, x86-64, codeimage): the deck OFFERS
    // to synthesise a register history, labelled a re-derivation (D6). Built
    // in-memory so the pure decision is proved with no golden dependency.
    {
        Recording emu;
        emu.arch = "x86_64";
        emu.producer.name = "asmtest-author";
        emu.provenance.backend = "author-emulator";
        emu.provenance.exact = true;
        nlohmann::json ci;
        ci["k"] = "codeimage";
        ci["base"] = 0x100000;
        ci["len"] = 1;
        ci["bytes"] = "c3"; // a bare `ret` — enough to be replayable
        emu.by_kind["codeimage"].push_back(Event{"codeimage", ci, emu.next_seq++});

        dt_scrubber_replay er = dt_scrubber_replayable(emu);
        vt::check("an Author/emulator x86-64 recording IS replayable",
                  er.replayable, "reason: " + er.why);

        StepIndex empty; // no regstate: the absent index the offer is built over
        dt_scrubber so = dt_scrubber_build(empty, 0, er);
        vt::check("the replayable deck is still producer-absent", !so.present,
                  "no regstate events");
        vt::check("the replayable deck OFFERS to synthesise", so.synthesizable,
                  "synthesizable must be set");
        vt::check("the offer names the synthesise action",
                  !so.synth_action.empty() &&
                      so.synth_action.find("synthesize") != std::string::npos,
                  "action: " + so.synth_action);
        vt::check("the offer is LABELLED a re-derivation (D6)",
                  so.synth_label.find("re-derived") != std::string::npos &&
                      so.synth_label.find("not") != std::string::npos,
                  "label: " + so.synth_label);
        vt::check("the replayable offer still deep-links the docs",
                  !so.docs.empty(), "docs link must be present");

        vt::golden("scrubber-synthesizable.txt", dt_scrubber_dump(so));
    }

    // A SYNTHESISED deck says so (D6): a present index flagged `synthesized`
    // carries the re-derivation banner — never presenting a re-run as capture.
    {
        StepIndex syn;
        syn.entries.resize(1);
        syn.entries[0].step = 0;
        syn.entries[0].fields.push_back(RegField{"rax", 7, false});
        syn.synthesized = true;
        dt_scrubber ss = dt_scrubber_build(syn, 0);
        vt::check("a synthesised deck is present", ss.present, "one held step");
        vt::check("a synthesised deck is flagged", ss.synthesized,
                  "synthesized must propagate");
        vt::check("the banner announces the re-derivation",
                  ss.banner.find("SYNTHESIZED") != std::string::npos,
                  "banner: " + ss.banner);
    }

    // --- the footer's steps_total (28 R1 T2): total_steps() takes the larger of
    // the reconstructed (dropped+count) and the footer total, so a tail-drop live
    // ring (dropped == 0 but steps ran past the cap) is no longer undercounted,
    // and a drop-oldest ring's reconstruction is never shrunk by a stale footer.
    {
        StepIndex idx;
        idx.entries.resize(2); // 2 held

        // Tail-drop: dropped == 0, but the footer says 5 steps ran. Reconstruction
        // (0+2) would undercount; the footer supplies the true total.
        idx.dropped = 0;
        idx.footer_total = 5;
        vt::eq("tail-drop total uses the footer", idx.total_steps(),
               uint64_t{5});

        // Drop-oldest: the footer equals dropped+count, so it is a no-op.
        idx.dropped = 4;
        idx.footer_total = 6;
        vt::eq("drop-oldest total is unchanged by a matching footer",
               idx.total_steps(), uint64_t{6});

        // A stale/smaller footer never shrinks the reconstructed total.
        idx.footer_total = 3;
        vt::eq("a smaller footer never shrinks the reconstruction",
               idx.total_steps(), uint64_t{6});

        // Absent footer (0) falls back to the reconstruction exactly as before.
        idx.footer_total = 0;
        vt::eq("no footer falls back to dropped+count", idx.total_steps(),
               uint64_t{6});
    }

    return vt::report("test_scrubber");
}
