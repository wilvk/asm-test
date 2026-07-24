// test_obs_region.cpp — region invocation snapshots (08-observer-views.md T6).
//
// The fixture is deliberately awkward: two complete invocations followed by a
// third that the capture cut off mid-flight. A viewer that split on "offset 0
// starts a new invocation" would get the same first two and then silently merge
// or drop the tail; splitting on the `coverage` footer — the thing the producer
// actually writes at the end of an invocation — gets all three, with the last
// one marked OPEN.
#include "views/region.h"

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording rec = vt::load_fixture("obs-region.asmtrace");
    RegionView v = obs_region_build(rec);

    vt::eq("invocations", v.invocations.size(), size_t{3});
    vt::eq("numbered from 1", v.invocations[0].number, size_t{1});
    vt::eq("first invocation", v.invocations[0].insns.size(), size_t{5});
    vt::eq("second invocation", v.invocations[1].insns.size(), size_t{6});
    vt::check("both closed", v.invocations[0].closed && v.invocations[1].closed,
              "a coverage footer closes the invocation before it");
    vt::eq("blocks belong to their invocation", v.invocations[1].blocks.size(),
           size_t{3});
    vt::check("coverage truncation kept", v.invocations[1].truncated,
              "blocks_total > len(blocks) is an honest truncation signal");

    // The cut-off tail is kept and marked, never presented as a short run.
    vt::eq("open tail kept", v.invocations[2].insns.size(), size_t{2});
    vt::check("open tail marked", !v.invocations[2].closed,
              "an invocation with no coverage footer is incomplete");
    vt::check("open tail says so in the render",
              obs_region_dump(v).find("OPEN") != std::string::npos,
              "the incomplete invocation must say so on screen");

    // Paging is discrete and clamped — there is no scrub, by design.
    vt::eq("start at the first", v.selected, size_t{0});
    vt::eq("page forward", obs_region_page(v, 1), size_t{1});
    vt::eq("page past the end clamps", obs_region_page(v, 99), size_t{2});
    vt::eq("page before the start clamps", obs_region_page(v, -99), size_t{0});

    // Each snapshot renders through 04's canvas: same code, same rules.
    Streams s = obs_region_snapshot_streams(v, 1);
    vt::eq("snapshot insns", s.trace.insns.size(), size_t{6});
    vt::eq("snapshot blocks", s.trace.blocks.size(), size_t{3});
    vt::eq("basis carried, never defaulted", s.trace.basis, std::string("rel"));
    vt::check("snapshot disasm carried",
              s.trace.disasm.count(10) && s.trace.disasm[10] == "add eax, esi",
              "D10 text belongs to the invocation it was recorded in");
    vt::check("truncated snapshot says so", s.trace.truncated,
              "this invocation's own coverage declared truncation");
    Streams open = obs_region_snapshot_streams(v, 2);
    vt::check("an unclosed invocation is truncated too", open.truncated,
              "no footer means what is shown is a prefix");
    Streams none = obs_region_snapshot_streams(v, 99);
    vt::eq("out-of-range snapshot is empty, not garbage",
           none.trace.insns.size(), size_t{0});

    // The crawl warning is a property of the capture, not an opinion.
    vt::check("crawl warning present", !v.crawl_warning.empty(),
              "a single-step region capture must warn that the target crawls");
    vt::check("crawl warning names the recovery",
              v.crawl_warning.find("native speed") != std::string::npos,
              v.crawl_warning);
    vt::eq("no crawl warning for an out-of-band sampler",
           obs_region_crawl_warning("ibs-op"), std::string());
    vt::eq("no crawl warning for the emulator",
           obs_region_crawl_warning("emu-l0"), std::string());
    vt::check("dataflow warns about its operand reads too",
              obs_region_crawl_warning("ptrace-dataflow").find("operands") !=
                  std::string::npos,
              "the dataflow capture is the slowest and should say so");

    // No codeimage events in this fixture: no JIT hint invented.
    vt::eq("no JIT hint without codeimage", v.jit_hint, std::string());

    vt::golden("obs-region.txt", obs_region_dump(v));
    return vt::report("test_obs_region");
}
