// test_obs_disasm.cpp — the codeimage-versioned disassembly pane
// (08-observer-views.md T7).
//
// The fixture holds ONE region with TWO versions of its bytes (the region was
// patched between invocations, which is what a JIT does), and the assertions
// are all about time: a step captured between the two refreshes must resolve to
// the OLDER bytes. A pane that reached for the newest version would pass every
// obvious check and be wrong exactly where the mechanism was needed.
#include "views/disasm.h"

#include <sstream>
#include <string>

#include "view_test.h"

#include "doc/streams.h"

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

    // --- obs_disasm_byte_at: the memory editor's ReadFn (14 T4) --------------
    // The single-byte form the interactive byte view resolves each cell with. It
    // agrees with the run form, contrasts across versions (JIT churn), and — the
    // fidelity point — reports UNKNOWN rather than fabricating a byte.
    bool known = false;
    vt::eq("byte_at resolves the historical version",
           obs_disasm_byte_at(v, kBase + 4, 2, &known), uint8_t{0x55});
    vt::check("...and is known", known, "a covered byte reported unknown");
    vt::eq("byte_at contrasts across the refresh (JIT churn)",
           obs_disasm_byte_at(v, kBase + 4, 3, &known), uint8_t{0x90});
    vt::check("...still known", known, "");
    obs_disasm_byte_at(late, kBase, 2, &known);
    vt::check("an UNKNOWN byte is reported, never faked", !known,
              "the byte view would have shown a fabricated value");
    obs_disasm_byte_at(v, 0xdeadbeef, 0, &known);
    vt::check("an untracked byte is reported unknown", !known,
              "an address in no region reported a byte");

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
    vt::eq("neither bytes nor text is truthful about it", grows[1].source,
           std::string("unknown"));

    vt::golden("obs-codeimage.txt", obs_disasm_dump(v));
    vt::golden("obs-codeimage-gate.txt", obs_disasm_dump(g));

    // --- 37 T4: stamped_when — the manual slider's default, from df_step -----
    // `obs-codeimage.asmtrace` carries no df_step at all (it is a `trace`-mode
    // fixture), so the default stays unset — the pre-T4 recordings this test
    // already covers must not spontaneously grow a default from nowhere.
    vt::eq("no df_step: stamped_when stays 0", v.stamped_when, uint64_t{0});
    {
        // A continuous capture's LATER pass restates `when` (a re-arm can see a
        // different codeimage timeline) — the default tracks the FRESHEST one,
        // last write in event order.
        std::string nd = std::string(
            "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":"
            "\"test\",\"version\":\"0\"},\"provenance\":{\"backend\":\"ptrace-"
            "dataflow\",\"exact\":true,\"trust\":\"exact\"},\"arch\":"
            "\"x86_64\"}\n");
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4096,"
              "\"when\":5,\"ops\":[]}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"rbase\":4096,"
              "\"when\":5,\"ops\":[]}\n";
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":8192,"
              "\"when\":9,\"ops\":[]}\n";
        std::istringstream in(nd);
        std::string err;
        auto recOpt = load_recording(in, err);
        vt::check("stamped_when fixture loads", recOpt.has_value(), err);
        if (recOpt) {
            DisasmView sv = obs_disasm_build(*recOpt);
            vt::eq("stamped_when: the freshest (last) pass's when", sv.stamped_when,
                   uint64_t{9});
        }
    }

    // --- 37 T4: (rbase, when) keying — `when` alone must never conflate two --
    // spans. A candidate walk re-arms the codeimage timeline per span, so two
    // DIFFERENT spans can each stamp `when:1` on their own first step; only the
    // address (rbase + off) disambiguates which version answers which step.
    {
        std::string nd = std::string(
            "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":"
            "\"test\",\"version\":\"0\"},\"provenance\":{\"backend\":\"ptrace-"
            "dataflow\",\"exact\":true,\"trust\":\"exact\"},\"arch\":"
            "\"x86_64\"}\n");
        // Span A: base 0x1000, one byte 0xaa, at version/when 1.
        nd += "{\"k\":\"codeimage\",\"base\":4096,\"len\":1,\"version\":0,"
              "\"when\":1,\"bytes\":\"aa\"}\n";
        // Span B: an unrelated candidate region, base 0x9000, one byte 0xbb,
        // ALSO at when 1 — its own timeline restarted at the same seq.
        nd += "{\"k\":\"codeimage\",\"base\":36864,\"len\":1,\"version\":0,"
              "\"when\":1,\"bytes\":\"bb\"}\n";
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4096,"
              "\"when\":1,\"ops\":[]}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":0,\"rbase\":36864,"
              "\"when\":1,\"ops\":[]}\n";
        std::istringstream in(nd);
        std::string err;
        auto recOpt = load_recording(in, err);
        vt::check("keying fixture loads", recOpt.has_value(), err);
        if (recOpt) {
            DisasmView kv = obs_disasm_build(*recOpt);
            SegmentedDataflow seg = build_segmented_dataflow(*recOpt);
            vt::eq("keying: one pass", seg.passes.size(), size_t{1});
            if (!seg.passes.empty()) {
                const DataflowStream &p = seg.passes[0];
                vt::eq("keying: two steps", p.nsteps, uint32_t{2});
                if (p.insn_rbase.size() == 2 && p.insn_when.size() == 2 &&
                    p.insn_off.size() == 2) {
                    uint64_t addrA = p.insn_rbase[0] + p.insn_off[0];
                    uint64_t addrB = p.insn_rbase[1] + p.insn_off[1];
                    CodeBytes a = obs_disasm_bytes_at(kv, addrA, p.insn_when[0]);
                    CodeBytes b = obs_disasm_bytes_at(kv, addrB, p.insn_when[1]);
                    vt::check("keying: span A resolves", a.found, a.why);
                    vt::check("keying: span B resolves", b.found, b.why);
                    if (a.found && a.len > 0)
                        vt::eq("keying: span A gets its OWN bytes", a.data[0],
                               uint8_t{0xaa});
                    if (b.found && b.len > 0)
                        vt::eq("keying: span B gets its OWN bytes, not A's",
                               b.data[0], uint8_t{0xbb});
                }
            }
        }
    }

    return vt::report("test_obs_disasm");
}
