// test_ptslice_run.cpp — the PT-replay slice, actually replayed
// (08-observer-views.md T8). FULL-BUILD ONLY: it links the PT replay producer
// and the emulator behind it.
//
// This is the test the doc's gate discussion makes possible. Capturing a PT
// path needs PT silicon; REPLAYING one does not — the path is already decoded
// and recorded (`stitch`), and the bytes with it (`codeimage`). So a recording
// made on a PT box is replayable anywhere the full app builds, and the claim
// "a live slice renders with zero single-steps of the target" stops being a
// promise only a lab can check: everything after the capture is exercised here,
// on a host with no Intel PT at all.
#include "live/ptslice.h"

#include "analysis/slice.h"
#include "view_test.h"

using namespace asmdesk;

int main() {
    PtSliceFacts f = ptslice_facts();
    vt::check("the producer is linked in this build", f.producer_linked,
              "this binary is built with ASMTEST_DESKTOP_HAVE_PT_REPLAY");
    // The F6 hazard: the producer ships no header, so this build re-declares
    // its info struct. A skew here would silently mis-read every field.
    vt::check("the re-declared info layout matches the producer's", f.layout_ok,
              f.layout_detail);

    Recording rec = vt::load_fixture("obs-ptslice.asmtrace");
    PtSliceInput in = ptslice_input_from(rec);
    vt::eq("input assembled", in.error, std::string());
    // endbr64 / mov eax, edi / add eax, esi / ret — with rdi=40, rsi=2.
    in.args = {40, 2};

    PtSliceResult r = ptslice_run(in);
    if (!r.ok && r.reason.find("Capstone") != std::string::npos) {
        // The producer compiles to an ENOSYS stub without Capstone/Unicorn.
        // That is a BUILD fact this binary should not have been built under —
        // it is only linked when the engines are present — so it is reported,
        // not skipped past.
        vt::fail("ptslice_run", "the producer is a stub here: " + r.reason);
        return vt::report("test_ptslice_run");
    }
    vt::check("the replay produced a stream", r.ok, r.reason);
    vt::eq("one step per decoded offset", r.steps, uint64_t{4});
    vt::eq("...and the stream agrees", r.df.nsteps, uint32_t{4});
    vt::eq("path length carried", r.path_len, uint64_t{4});
    vt::check("no divergence on a faithful path", !r.diverged,
              "the replay diverged from the recorded path at step " +
                  std::to_string(r.diverged_at));
    vt::eq("offsets are the recorded ones", r.df.insn_off[2], uint64_t{6});

    // The values the hardware never recorded, reconstructed: rdi=40 read at
    // step 1, and the sum written at step 2.
    bool saw_written_42 = false, saw_read_40 = false;
    for (const ValRec &v : r.df.recs) {
        if (v.value_valid && !v.write && v.value == 40)
            saw_read_40 = true;
        if (v.value_valid && v.write && v.value == 42)
            saw_written_42 = true;
    }
    vt::check("an entry argument was reconstructed", saw_read_40,
              "rdi=40 should be read by `mov eax, edi`");
    vt::check("a computed value was reconstructed", saw_written_42,
              "`add eax, esi` should write 40+2");

    // And the whole point: this feeds 04's slice explorer unchanged.
    vt::check("def-use edges were built", !r.df.edges.empty(),
              "no L1 edges: the slice explorer would have nothing to light");
    dt_slice back = dt_slice_backward(r.df.edges, r.df.nsteps, 2);
    vt::check("the backward cone from the add reaches the mov",
              back.contains(1),
              "step 2 consumes what step 1 produced; the cone must say so");

    // A path that does not match the code must DIVERGE rather than invent a
    // plausible trace — the producer's negative control, re-asserted here
    // because this is the layer a user would trust.
    PtSliceInput bogus = in;
    bogus.path[2] = 0x99;
    PtSliceResult br = ptslice_run(bogus);
    vt::check("a bogus path diverges", br.diverged || br.truncated || !br.ok,
              "a path that does not match the bytes must not replay cleanly");
    if (br.diverged)
        vt::check("...and says where", br.diverged_at != 0,
                  "the divergence step must be reported");

    return vt::report("test_ptslice_run");
}
