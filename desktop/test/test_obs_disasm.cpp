// test_obs_disasm.cpp — the codeimage-versioned disassembly pane
// (08-observer-views.md T7).
//
// The fixture holds ONE region with TWO versions of its bytes (the region was
// patched between invocations, which is what a JIT does), and the assertions
// are all about time: a step captured between the two refreshes must resolve to
// the OLDER bytes. A pane that reached for the newest version would pass every
// obvious check and be wrong exactly where the mechanism was needed.
#include "views/disasm.h"

#include "view_test.h"

using namespace asmdesk;

static const uint64_t kBase = 4198400; // 0x401000

int main() {
    Recording rec = vt::load_fixture("obs-codeimage.asmtrace");
    DisasmView v = obs_disasm_build(rec);

    vt::eq("versions", v.versions.size(), size_t{2});
    vt::eq("one region", v.spans().size(), size_t{1});
    vt::eq("v0 when", v.versions[0].when, uint64_t{1});
    vt::eq("v1 when", v.versions[1].when, uint64_t{3});

    // --- the rule: greatest `when` at or before the query -------------------
    CodeBytes at2 = obs_disasm_bytes_at(v, kBase + 4, 2);
    vt::check("a step between refreshes resolves", at2.found, at2.why);
    vt::eq("...to the HISTORICAL version", at2.version->version, uint64_t{0});
    vt::eq("...and its bytes", at2.data[0], uint8_t{0x55}); // push rbp

    CodeBytes at3 = obs_disasm_bytes_at(v, kBase + 4, 3);
    vt::eq("at the refresh itself, the new version", at3.version->version,
           uint64_t{1});
    vt::eq("...and its bytes", at3.data[0], uint8_t{0x90}); // nop

    CodeBytes latest = obs_disasm_bytes_at(v, kBase + 4, 0);
    vt::eq("when == 0 means the latest (the C API's convention)",
           latest.version->version, uint64_t{1});

    // Before any version exists the bytes are UNKNOWN — never the next one
    // along, which would be the succeeding method's code.
    DisasmView late;
    late.versions.push_back(v.versions[1]); // only the v1 snapshot survives
    CodeBytes gap = obs_disasm_bytes_at(late, kBase, 2);
    vt::check("no version at-or-before is UNKNOWN", !gap.found,
              "a lookup before the earliest version returned bytes");
    vt::check("...and says why", gap.why.find("UNKNOWN") != std::string::npos,
              gap.why);
    vt::check("...naming the JIT reuse hazard",
              gap.why.find("reuses addresses") != std::string::npos, gap.why);

    CodeBytes outside = obs_disasm_bytes_at(v, 0xdeadbeef, 0);
    vt::check("an untracked address is not covered", !outside.found,
              "an address in no tracked region returned bytes");
    vt::check("...and says so",
              outside.why.find("no tracked region") != std::string::npos,
              outside.why);

    // A lookup near the end of a span returns only what is available.
    CodeBytes tail = obs_disasm_bytes_at(v, kBase + 7, 0);
    vt::eq("tail length is clamped to the span", tail.len, size_t{1});

    // --- rows: byte source vs the D10 fallback, never blurred ---------------
    std::vector<uint64_t> addrs = {kBase, kBase + 4};
    std::vector<DisasmRow> rows = obs_disasm_rows(v, addrs, 2, 4);
    vt::eq("rows", rows.size(), size_t{2});
    vt::eq("row bytes are the historical ones", rows[1].bytes,
           std::string("554889e5"));
    vt::eq("row source names the version", rows[1].source,
           std::string("codeimage v0"));
    vt::check("recorded text still shown", !rows[1].text.empty(),
              "D10 text and code-image bytes are complementary, not exclusive");

    // --- the gate: no image, a MEASURED reason, and the D10 fallback --------
    Recording gated = vt::load_fixture("obs-codeimage-gate.asmtrace");
    DisasmView g = obs_disasm_build(gated);
    vt::eq("no versions", g.versions.size(), size_t{0});
    vt::check("the measured reason survived", !g.unavailable_reason.empty(),
              "a pane with no image and no reason cannot be told from one "
              "nobody asked to capture");
    vt::check("the reason names the kernel gate",
              g.unavailable_reason.find("6.7") != std::string::npos,
              g.unavailable_reason);
    vt::check("the reason is in the render",
              obs_disasm_dump(g).find("PAGEMAP_SCAN") != std::string::npos,
              "the gate's reason must be on screen");

    std::vector<DisasmRow> grows = obs_disasm_rows(g, addrs, 0, 4);
    vt::eq("no bytes without an image", grows[0].bytes, std::string());
    vt::eq("falls back to the recorded text", grows[0].text,
           std::string("endbr64"));
    vt::eq("...LABELLED as the weaker source", grows[0].source,
           std::string("recorded disasm"));
    vt::eq("neither bytes nor text is honest about it", grows[1].source,
           std::string("unknown"));

    vt::golden("obs-codeimage.txt", obs_disasm_dump(v));
    vt::golden("obs-codeimage-gate.txt", obs_disasm_dump(g));
    return vt::report("test_obs_disasm");
}
