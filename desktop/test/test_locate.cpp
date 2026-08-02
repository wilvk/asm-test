// test_locate.cpp — space/locate.h (50-two-way-brushing.md T1): scene_locate_off
// / scene_locate_step. Null harness, no display: this binary links locate.o +
// projection.o + the doc model, and NOTHING else — the same engine-free closure
// argument test_goto.cpp/test_drillin.cpp make.
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/locate.h"
#include "space/projection.h"
#include "space/terrain.h"

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

static uint32_t cell_at(const Projection &p, uint64_t addr, bool *ok) {
    float u = 0, v = 0;
    if (!p.project(addr, &u, &v)) {
        *ok = false;
        return 0;
    }
    uint32_t n = uint32_t{1} << p.order;
    uint32_t x = static_cast<uint32_t>(u * n);
    uint32_t y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    *ok = true;
    return y * n + x;
}

static const char *kHdrExact =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\",\"exact\":"
    "true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";

int main() {
    // === scene_locate_off: abs basis ==========================================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));

        bool ok = false;
        uint32_t expect = cell_at(p, 4194304, &ok);
        check("locate_off/abs: cell_at succeeds", ok, "setup failure");

        Located loc = scene_locate_off(p, rec, 4194304);
        check("locate_off/abs: resolves", loc.ok, loc.reason.c_str());
        check("locate_off/abs: matches Projection::project's own cell",
              loc.cell == expect, "cell mismatch");
        check("locate_off/abs: addr is the offset itself", loc.addr == 4194304,
              "an abs offset must be used as the address verbatim");
        check("locate_off/abs: how says offset", loc.how == "offset",
              loc.how.c_str());
        check("locate_off/abs: not ambiguous (unique offset)", !loc.ambiguous,
              "a single-occurrence offset must not be flagged ambiguous");

        // A second trace event at the SAME offset makes it ambiguous — the
        // cell is still correct, the "when" is not unique.
        Recording loopy = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Located loop_loc = scene_locate_off(p, loopy, 4194304);
        check("locate_off/abs: loop offset resolves", loop_loc.ok,
              loop_loc.reason.c_str());
        check("locate_off/abs: loop offset sets ambiguous", loop_loc.ambiguous,
              "an offset revisited by two trace events must be ambiguous");
    }

    // === scene_locate_off: rel basis, single-span anchor ======================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":16}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));

        Located loc = scene_locate_off(p, rec, 16);
        check("locate_off/rel: resolves through the anchor", loc.ok,
              loc.reason.c_str());
        check("locate_off/rel: addr is base+off", loc.addr == 4194304 + 16,
              "the anchor must place base+off, not the raw offset");
        bool ok = false;
        uint32_t expect = cell_at(p, 4194304 + 16, &ok);
        check("locate_off/rel: matches the anchored cell", loc.cell == expect,
              "cell mismatch");
    }

    // === scene_locate_off: unanchorable rel (two codeimage spans) refuses =====
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"codeimage\",\"base\":8388608,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":16}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        const Anchor anchor = resolve_anchor(regions_from_codeimage(rec));
        check("locate_off/rel-unanchorable: setup: anchor itself refuses",
              !anchor.ok, "two code spans must make resolve_anchor refuse");

        Located loc = scene_locate_off(p, rec, 16);
        check("locate_off/rel-unanchorable: refuses", !loc.ok,
              "an offset with two codeimage spans must refuse, never guess");
        check("locate_off/rel-unanchorable: reason is non-empty",
              !loc.reason.empty(), "a refusal must always carry a reason");
        check("locate_off/rel-unanchorable: reason matches resolve_anchor's "
              "verbatim",
              loc.reason == anchor.reason,
              "the refusal reason must be the anchor's own reason, not a "
              "re-derived one");
    }

    // === scene_locate_off: no trace/df_step stream at all refuses =============
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"end\",\"events\":1,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Located loc = scene_locate_off(p, rec, 16);
        check("locate_off/no-stream: refuses", !loc.ok,
              "a recording with neither trace nor df_step has no basis to "
              "resolve an offset against");
        check("locate_off/no-stream: reason is non-empty", !loc.reason.empty(),
              "a refusal must always carry a reason");
    }

    // === scene_locate_step: wire rbase (37), out of range, and no df_step =====
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":32,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":2,\"off\":16,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("locate_step/setup: df decoded", s.df.insn_off.size() == 3,
              "fixture must decode three df_step entries");

        Located loc = scene_locate_step(p, s.df, 0);
        check("locate_step/wire-rbase: resolves", loc.ok, loc.reason.c_str());
        check("locate_step/wire-rbase: addr is rbase+off",
              loc.addr == 4194304 + 16, "wire rbase must win over any anchor");
        check("locate_step/wire-rbase: how names the wire route",
              loc.how == "step->offset (wire rbase)", loc.how.c_str());
        // step 0 and step 2 share the same resolved address (16) -> ambiguous;
        // step 1 (32) does not.
        check("locate_step/wire-rbase: revisited step is ambiguous",
              loc.ambiguous, "steps 0 and 2 resolve to the same address");
        Located loc1 = scene_locate_step(p, s.df, 1);
        check("locate_step/wire-rbase: unique step is not ambiguous",
              loc1.ok && !loc1.ambiguous, "step 1's address is visited once");

        Located oor = scene_locate_step(p, s.df, 3);
        check("locate_step/out-of-range: refuses", !oor.ok,
              "step 3 is past this pass's 3 steps");
        check("locate_step/out-of-range: reason is non-empty",
              !oor.reason.empty(), "a refusal must always carry a reason");

        DataflowStream empty_df;
        Located none = scene_locate_step(p, empty_df, 0);
        check("locate_step/no-df_step: refuses", !none.ok,
              "a pass with no steps at all must refuse, not crash");
        check("locate_step/no-df_step: reason is non-empty",
              !none.reason.empty(), "a refusal must always carry a reason");
    }

    // === scene_locate_step: derived anchor (no wire rbase) =====================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("locate_step/derived-anchor: setup: no wire rbase",
              !s.df.rbase_present, "fixture must omit rbase");

        Located loc = scene_locate_step(p, s.df, 0);
        check("locate_step/derived-anchor: resolves", loc.ok,
              loc.reason.c_str());
        check("locate_step/derived-anchor: addr is base+off",
              loc.addr == 4194304 + 16, "the single-span anchor must place it");
        check("locate_step/derived-anchor: how names the derived route",
              loc.how == "step->offset (derived anchor)", loc.how.c_str());
    }

    if (failures) {
        std::fprintf(stderr, "test_locate: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_locate: all checks passed\n");
    return 0;
}
