// test_crossing.cpp — the kernel-crossing spur layer (57-causal-layers.md T2).
// Null harness, no display: views/crossing.o + syscalls.o + observer.o + the
// space/ resolvers + the doc model, and nothing else.
//
// The load-bearing checks here are the two fabrications this layer could most
// easily commit: anchoring a syscall that precedes every recorded instruction
// to instruction 0, and implying a kernel DWELL the recording never measured.
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "space/projection.h"
#include "space/terrain.h" // regions_from_codeimage
#include "views/crossing.h"

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static Recording mk_rec(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

static const char *kHdr =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-syscalls\",\"exact\":"
    "true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
static const char *kHdrRedacted =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-syscalls\",\"exact\":"
    "true,\"trust\":\"exact\",\"redacted\":true},\"arch\":\"x86_64\"}\n";
static const char *kCodeimage =
    "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
    "\"when\":1,\"bytes\":\"90\"}\n";
static const char *kEnd =
    "{\"k\":\"end\",\"events\":9,\"truncated\":false,\"drops\":{\"lost\":0,"
    "\"throttled\":false}}\n";

int main() {
    // === anchoring: a syscall between two instructions takes the EARLIER =====
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage +                       // seq 0
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // seq 1, t=0
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n" // seq 2, t=1
            "{\"k\":\"syscall\",\"line\":\"openat(AT_FDCWD, <path>, O_RDONLY) "
            "= 3\"}\n"                                             // seq 3
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194336}\n" // seq 4, t=2
            + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        check("anchor: setup: seq_present", sv.seq_present,
              "the fixture's syscall must carry a real stream position");

        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("anchor: layer enabled", layer.enabled, layer.disabled_reason);
        check("anchor: one spur", layer.spurs.size() == 1,
              "got " + std::to_string(layer.spurs.size()));
        if (layer.spurs.size() == 1) {
            const CrossingSpur &sp = layer.spurs[0];
            check("anchor: takes the EARLIER instruction (seq 2, not seq 4)",
                  sp.anchor_addr == 4194320,
                  "anchored to 0x" + std::to_string(sp.anchor_addr));
            check("anchor: carries that instruction's per-tid vertex ordinal",
                  sp.anchor_t == 1,
                  "TrajPoint::t for the second vertex of tid -1 is 1, got " +
                      std::to_string(sp.anchor_t));
            check("anchor: resumes at the NEXT instruction",
                  sp.has_resume && sp.resume_addr == 4194336,
                  "the first instruction with a greater seq is the resume "
                  "vertex");
            check("anchor: class parsed from the payload-free line",
                  sp.cls == SyscallClass::File,
                  std::string("got ") + syscall_class_name(sp.cls));
            check("anchor: outcome parsed as ok (= 3)",
                  sp.outcome == SyscallOutcome::Ok,
                  std::string("got ") + syscall_outcome_name(sp.outcome));
            check("anchor: drill-in target is the syscalls row", sp.row == 0,
                  "a spur must name the row it came from");
            check("anchor: an exact, complete recording draws a solid anchor",
                  !sp.hollow,
                  "nothing about this fixture is truncated or dropped");
            // The rail point is ONE point shared by both spurs, so the pair
            // has no along-rail extent to read as a duration.
            check("anchor: rail is the midpoint of the two vertices",
                  sp.rail_u == 0.5f * (sp.anchor_u + sp.resume_u) &&
                      sp.rail_v == 0.5f * (sp.anchor_v + sp.resume_v),
                  "the out and return spurs must terminate at the same point");
        }
        check("anchor: nothing counted as unanchorable",
              layer.before_first_insn == 0 && layer.off_plane == 0, "");
        check("no duration is implied: rail_span is 0 by construction",
              CrossingLayer::rail_span() == 0.0f,
              "an along-rail span would read as kernel dwell, which nothing "
              "in the recording measures");
        check("the dwell note states the absence",
              std::string(CrossingLayer::dwell_note())
                      .find("not measured") != std::string::npos,
              CrossingLayer::dwell_note());
        check("the anchor label states it is approximate",
              std::string(CrossingLayer::anchor_label()).find("approx") !=
                  std::string::npos,
              CrossingLayer::anchor_label());
    }

    // === a syscall BEFORE any instruction is counted, never anchored to 0 ====
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage +                       // seq 0
            "{\"k\":\"syscall\",\"line\":\"brk(NULL) = 0x1000\"}\n" // seq 1
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // seq 2
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n" // seq 3
            + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("pre-trace: layer enabled", layer.enabled, layer.disabled_reason);
        check("pre-trace: NO spur", layer.spurs.empty(),
              "a syscall before every recorded instruction has no earlier "
              "vertex; anchoring it to instruction 0 would be a fabrication");
        check("pre-trace: counted, not dropped", layer.before_first_insn == 1,
              "got " + std::to_string(layer.before_first_insn));
    }

    // === an unparsed name and an unparsed return both land in "other" ========
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage +
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"syscall\",\"line\":\"frobnicate(1, 2) = ?\"}\n"
            "{\"k\":\"syscall\",\"line\":\"open(<path>, O_RDONLY) = -1 ENOENT "
            "(No such file or directory)\"}\n"
            "{\"k\":\"syscall\",\"line\":\"\"}\n" +
            kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("other-bucket: three spurs", layer.spurs.size() == 3,
              "got " + std::to_string(layer.spurs.size()));
        if (layer.spurs.size() == 3) {
            check("other-bucket: an unknown name is Other, never folded in",
                  layer.spurs[0].cls == SyscallClass::Other,
                  std::string("got ") +
                      syscall_class_name(layer.spurs[0].cls));
            check("other-bucket: an unparsed return is Unknown, never ok",
                  layer.spurs[0].outcome == SyscallOutcome::Unknown,
                  std::string("got ") +
                      syscall_outcome_name(layer.spurs[0].outcome));
            check("other-bucket: a negative return is an error",
                  layer.spurs[1].outcome == SyscallOutcome::Error,
                  std::string("got ") +
                      syscall_outcome_name(layer.spurs[1].outcome));
            check("other-bucket: a known name still classifies",
                  layer.spurs[1].cls == SyscallClass::File, "");
            check("other-bucket: an empty line reads no name and no return",
                  layer.spurs[2].cls == SyscallClass::Other &&
                      layer.spurs[2].outcome == SyscallOutcome::Unknown,
                  "an unreadable line must not be given a family or a verdict");
        }
        check("other-bucket: the Other bucket is a VISIBLE, named class",
              std::string(syscall_class_name(SyscallClass::Other))
                      .find("other") != std::string::npos,
              syscall_class_name(SyscallClass::Other));
    }

    // === record_redacted: hatched spurs, payload text nowhere in the dump ====
    {
        const char *kSecret = "/etc/shadow";
        Recording rec = mk_rec(
            std::string(kHdrRedacted) + kCodeimage +
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"syscall\",\"line\":\"openat(AT_FDCWD, <path>, O_RDONLY) "
            "= 3\",\"payload\":\"/etc/shadow\"}\n" +
            kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        check("redacted: setup: the view says record_redacted",
              sv.record_redacted, "provenance.redacted must be lifted");
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("redacted: one spur", layer.spurs.size() == 1, "");
        if (layer.spurs.size() == 1) {
            check("redacted: the spur is marked", layer.spurs[0].redacted,
                  "a record_redacted recording hatches every spur");
            check("redacted: the byte COUNT still rides (thickness channel)",
                  layer.spurs[0].payload_bytes == 11,
                  "got " + std::to_string(layer.spurs[0].payload_bytes));
        }
        const std::string dump = crossing_layer_dump(layer);
        check("redacted: the payload text appears NOWHERE in the dump",
              dump.find(kSecret) == std::string::npos,
              "the layer leaked withheld content");
        check("redacted: the dump names the withholding",
              dump.find(CrossingLayer::redacted_label()) != std::string::npos,
              dump);
    }

    // === self-gate: seq_present == false ====================================
    {
        Recording rec = mk_rec(
            std::string(kHdr) +
            "{\"k\":\"syscall\",\"line\":\"read(3, <8 bytes>) = 8\"}\n" // seq 0
            + kCodeimage +
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        check("no-seq: setup: seq_present is false", !sv.seq_present,
              "a lone syscall at stream position 0 must read as seq-less");
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("no-seq: layer self-disables", !layer.enabled,
              "with no stream order there is nothing to anchor by");
        check("no-seq: the reason is stated", !layer.disabled_reason.empty(),
              "a disabled layer must always say why");
        check("no-seq: the reason names seq",
              layer.disabled_reason.find("seq") != std::string::npos,
              layer.disabled_reason);
        check("no-seq: no geometry at all", layer.spurs.empty(), "");
    }

    // === self-gate: no `trace` worldline ====================================
    {
        Recording rec =
            mk_rec(std::string(kHdr) + kCodeimage +
                   "{\"k\":\"syscall\",\"line\":\"read(3, <8 bytes>) = 8\"}\n" +
                   kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("no-worldline: layer self-disables", !layer.enabled,
              "there is no path to hang a spur on");
        check("no-worldline: the reason is stated",
              !layer.disabled_reason.empty(), "");
        check("no-worldline: the reason refuses to synthesise",
              layer.disabled_reason.find("synthesise") != std::string::npos,
              layer.disabled_reason);
        const std::string dump = crossing_layer_dump(layer);
        check("no-worldline: the dump carries the reason",
              dump.find(layer.disabled_reason) != std::string::npos, dump);
    }

    // === a truncated recording draws its anchors HOLLOW ======================
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage +
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"syscall\",\"line\":\"read(3, <8 bytes>) = 8\"}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":true,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        SyscallView sv = obs_syscalls_build(rec);
        CrossingLayer layer = build_crossing_layer(sv, rec, p);
        check("truncated: one spur", layer.spurs.size() == 1, "");
        if (layer.spurs.size() == 1)
            check("truncated: the anchor draws hollow", layer.spurs[0].hollow,
                  "over a truncated trace even the nearest recorded "
                  "instruction is a weaker claim");
        check("truncated: no resume vertex was fabricated",
              layer.spurs.size() == 1 && !layer.spurs[0].has_resume,
              "the recording states no instruction after this syscall");
    }

    if (failures) {
        std::fprintf(stderr, "test_crossing: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_crossing: all checks passed\n");
    return 0;
}
