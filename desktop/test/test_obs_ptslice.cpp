// test_obs_ptslice.cpp — the PT-replay slice's GATE and input assembly
// (08-observer-views.md T8).
//
// This binary is built WITHOUT the producer (no ASMTEST_DESKTOP_HAVE_PT_REPLAY),
// which is deliberately the render-only viewer's situation: it must explain
// itself rather than offering a button it cannot honour. The replay itself —
// which needs Unicorn, not PT silicon, because the path is already decoded — is
// exercised by desktop_test_ptslice_run in the engine lane.
#include "live/ptslice.h"

#include "view_test.h"

using namespace asmdesk;

int main() {
    // --- the gate, every combination, on a host with none of it -----------
    PtSliceFacts none;
    PtSliceGate g = ptslice_gate(none);
    vt::check("no producer: no replay", !g.can_replay,
              "a build with no producer cannot replay");
    vt::check("no producer: no capture", !g.can_capture, "nor capture");
    vt::check("...and it blames the BUILD, not the host",
              g.reason.find("render-only viewer") != std::string::npos,
              g.reason);

    PtSliceFacts linked;
    linked.producer_linked = true;
    linked.capture_available = false;
    linked.capture_reason =
        "Intel PT is not present on this CPU (no intel_pt PMU in "
        "/sys/bus/event_source/devices)";
    PtSliceGate lg = ptslice_gate(linked);
    // The load-bearing distinction: replay needs no PT hardware at all.
    vt::check("a recorded path replays without PT silicon", lg.can_replay,
              "the path is already decoded; replay needs only the emulator");
    vt::check("live capture is still gated", !lg.can_capture,
              "capture needs PT silicon");
    vt::eq("...and the reason is the library's, VERBATIM", lg.reason,
           linked.capture_reason);

    PtSliceFacts pt = linked;
    pt.capture_available = true;
    pt.capture_reason.clear();
    PtSliceGate pg = ptslice_gate(pt);
    vt::check("on a PT host, both", pg.can_replay && pg.can_capture, pg.reason);
    vt::eq("...with nothing to explain", pg.reason, std::string());

    // A skewed re-declaration must REFUSE, not report numbers it cannot trust.
    PtSliceFacts skewed = pt;
    skewed.layout_ok = false;
    skewed.layout_detail = "producer says size=48 last_off=40, this build has "
                           "size=40 last_off=32";
    PtSliceGate sg = ptslice_gate(skewed);
    vt::check("a layout skew refuses to run", !sg.can_replay,
              "a silently skewed struct mis-reads every telemetry field");
    vt::check("...and shows the measured detail",
              sg.reason.find("size=48") != std::string::npos, sg.reason);

    // --- input assembly, pure ---------------------------------------------
    Recording rec = vt::load_fixture("obs-ptslice.asmtrace");
    PtSliceInput in = ptslice_input_from(rec);
    vt::eq("no assembly error", in.error, std::string());
    vt::eq("tid from the stitch", in.tid, 4242L);
    vt::eq("path length", in.path.size(), size_t{4});
    vt::eq("path is the decoded offsets", in.path[2], uint64_t{6});
    vt::eq("code bytes came from the image", in.code.size(), size_t{9});
    vt::eq("region base", in.base, uint64_t{4198400});
    vt::eq("first byte", in.code[0], uint8_t{0xf3}); // endbr64

    // The pairing rule: a path is only meaningful against the version it was
    // decoded against, so a missing version is a refusal, never "use the
    // newest one" — on a JIT that is a different routine entirely.
    Recording noimg = vt::load_fixture("obs-ptslice-noimage.asmtrace");
    PtSliceInput bad = ptslice_input_from(noimg);
    vt::check("no image: refused", !bad.error.empty(),
              "a path with no code image must not be replayed against guesses");
    vt::check("...naming the version",
              bad.error.find("version 0") != std::string::npos, bad.error);
    vt::check("...and carrying the producer's measured reason",
              bad.error.find("PAGEMAP_SCAN") != std::string::npos, bad.error);

    Recording plain = vt::load_fixture("obs-codeimage.asmtrace");
    PtSliceInput nostitch = ptslice_input_from(plain);
    vt::check("no stitch: refused", !nostitch.error.empty(),
              "a recording with no decoded path cannot feed a PT slice");
    vt::check("...saying which event is missing",
              nostitch.error.find("stitch") != std::string::npos,
              nostitch.error);

    PtSliceInput wrongtid = ptslice_input_from(rec, 9999);
    vt::check("an unknown tid is refused", !wrongtid.error.empty(),
              "a tid with no stitch must not silently fall back to another");

    // --- the run, in a build with no producer -----------------------------
    PtSliceResult r = ptslice_run(in);
    vt::check("no producer: the run refuses", !r.ok,
              "a build with no producer must not claim a result");
    vt::check("...with the gate's reason",
              r.reason.find("render-only viewer") != std::string::npos,
              r.reason);
    vt::eq("...and an EMPTY stream, not a partial one", r.df.nsteps,
           uint32_t{0});

    // The two grades of evidence in this view are both named.
    std::string disc = ptslice_disclosure();
    vt::check("disclosure names the hardware path",
              disc.find("hardware-recorded") != std::string::npos, disc);
    vt::check("disclosure names the reconstruction",
              disc.find("RECONSTRUCTED") != std::string::npos, disc);
    vt::check("disclosure states the zero-single-step property",
              disc.find("zero single-steps") != std::string::npos, disc);

    return vt::report("test_obs_ptslice");
}
