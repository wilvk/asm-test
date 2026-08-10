// test_recording_union.cpp — the session-union Recording (live union weave).
// Pure document model: links doc/recording.o + doc/streams.o and nothing else.
// The union mirrors what `--serve --record=<f>` tees to disk (ONE header,
// every capture's events in order), so these checks pin the same honesty rules
// the loader keeps: exact ANDs, trust weakens, a torn part stays torn.
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/recording_union.h"

static int failures = 0;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        failures++;
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
    }
}

static asmdesk::Recording load(const std::string &text) {
    std::istringstream in(text);
    std::string err;
    auto r = asmdesk::load_recording(in, err);
    check("fixture loads", r.has_value(), err.c_str());
    return r.value_or(asmdesk::Recording{});
}

static const char *kHdrExact =
    R"({"asmtrace":1,"producer":{"name":"asmspy","version":"1.1.0"},)"
    R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
static const char *kHdrStat =
    R"({"asmtrace":1,"producer":{"name":"asmspy","version":"1.1.0"},)"
    R"("provenance":{"backend":"ibs-sample","exact":false,"trust":"statistical"},"arch":"x86_64"})";

int main() {
    using asmdesk::Recording;
    // Two ended captures over DIFFERENT regions of one process.
    Recording a = load(std::string(kHdrExact) + "\n" +
                       R"({"k":"codeimage","base":4096,"len":64,"version":0})" +
                       "\n" + R"({"k":"df_step","step":0,"off":0})" + "\n" +
                       R"({"k":"end","events":2})" + "\n");
    Recording b = load(std::string(kHdrExact) + "\n" +
                       R"({"k":"codeimage","base":8192,"len":32,"version":0})" +
                       "\n" + R"({"k":"df_step","step":0,"off":4})" + "\n" +
                       R"({"k":"df_step","step":1,"off":8})" + "\n" +
                       R"({"k":"end","events":3})" + "\n");

    { // empty + identity
        Recording u0 = asmdesk::merge_session_recordings({}, nullptr);
        check("empty union has no events", u0.event_count() == 0, "0 parts");
        Recording u1 = asmdesk::merge_session_recordings({a}, nullptr);
        check("single-part union is the part",
              u1.event_count() == a.event_count() && u1.arch == a.arch &&
                  u1.has_end,
              "1 part must merge to itself");
    }
    { // two ended parts: events concatenate, seq stays strictly increasing
        Recording u = asmdesk::merge_session_recordings({a, b}, nullptr);
        check("union event_count sums",
              u.event_count() == a.event_count() + b.event_count(),
              "2+3 events expected");
        check("union keeps both code regions",
              u.by_kind.at("codeimage").size() == 2,
              "codeimage from BOTH captures must survive");
        const auto &df = u.by_kind.at("df_step");
        bool inc = true;
        for (size_t i = 1; i < df.size(); i++)
            inc = inc && df[i - 1].seq < df[i].seq;
        check("union seq strictly increases across the boundary", inc,
              "reassigned seq must preserve stream order");
        check("union of ended parts has an end", u.has_end && !u.torn,
              "last part ended cleanly");
    }
    { // a growing tail: the union is open (no end), never falsely complete
        Recording g = load(std::string(kHdrExact) + "\n" +
                           R"({"k":"df_step","step":0,"off":0})" + "\n");
        g.torn = false; // a growing live capture is open, not torn
        g.has_end = false;
        Recording u = asmdesk::merge_session_recordings({a}, &g);
        check("union with growing tail is open", !u.has_end,
              "has_end must come from the LAST part");
        check("growing events included",
              u.event_count() == a.event_count() + g.event_count(),
              "growing part's events must be in the union");
    }
    { // provenance honesty: exact AND, weakest trust, backends joined
        Recording s = load(std::string(kHdrStat) + "\n" +
                           R"({"k":"end","events":0})" + "\n");
        Recording u = asmdesk::merge_session_recordings({a, s}, nullptr);
        check("union of exact+statistical is NOT exact", !u.provenance.exact,
              "exact must AND");
        check("union trust is the weakest",
              u.provenance.trust == "statistical", "weakest rank wins");
        check("union backend names both",
              u.provenance.backend == "ptrace-dataflow+ibs-sample",
              "distinct backends join with +");
    }
    { // a torn part taints the union
        Recording t = load(std::string(kHdrExact) + "\n" +
                           R"({"k":"df_step","step":0,"off":0})" + "\n");
        check("fixture is torn", t.torn, "no end -> torn");
        Recording u = asmdesk::merge_session_recordings({t, b}, nullptr);
        check("torn part keeps the union truncated", u.torn,
              "a torn capture's absence of footer must not vanish");
    }
    if (failures == 0)
        std::printf("test_recording_union: OK\n");
    return failures == 0 ? 0 : 1;
}
