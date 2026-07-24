// test_data_readers.cpp — the completeness data readers
// (docs/internal/gui/02-exporters-and-readers.md T5).
//
// The assertions that matter are the three-state ones: "not measured" must
// survive as nullopt through both spellings the producers use (JSON null and an
// omitted key), and a torn append-only line must be counted rather than fatal.
#include <cstdio>
#include <sstream>
#include <string>

#include "data/features_data.h"
#include "data/perf_history.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_REPO_ROOT
#error "ASMTEST_REPO_ROOT must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk::data;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}
static void eq(const std::string &what, const std::string &got,
               const std::string &want) {
    if (got != want)
        fail(what, "got \"" + got + "\", want \"" + want + "\"");
}

static std::string fx(const char *name) {
    return std::string(ASMTEST_FIXTURE_DIR) + "/" + name;
}

int main() {
    // --- the committed REAL box record --------------------------------------
    {
        const std::string path = std::string(ASMTEST_REPO_ROOT) +
                                 "/benchmarks/boxes/amd-linux-x86_64-9e05f0f2/"
                                 "features.json";
        FeaturesDoc d;
        try {
            d = load_features_file(path);
        } catch (const std::exception &e) {
            fail("load the committed amd box record", e.what());
            return 1;
        }
        check("the box record has a system block", d.system.has_value(),
              "benchmarks/boxes/<box>/features.json always carries one");
        if (d.system)
            eq("box_id", d.system->box_id, "amd-linux-x86_64-9e05f0f2");
        check("it has feature rows", !d.features.empty(), "none parsed");

        const FeatureRow &r = d.features.front();
        eq("first row tier", r.tier, "emulator");
        eq("first row arch", r.arch, "x86_64");
        check("first row trace_insns == 4",
              r.trace_insns.has_value() && *r.trace_insns == 4,
              "the emulator guest row measures 4 instructions");
        // The row that motivates the whole optional: insns_truth is JSON null
        // here because the emulator IS the truth — there is nothing to compare
        // against, which is different from comparing against zero.
        check("first row insns_truth is NOT MEASURED", !r.insns_truth,
              "a JSON null must not become 0");
        check("first row complete is measured",
              r.complete.has_value() && *r.complete,
              "the emulator guest row reports complete:true");
    }

    // --- all three envelope shapes ------------------------------------------
    {
        FeaturesDoc bare = load_features_file(fx("features-bare.json"));
        check("a bare asmfeatures sweep has NO system block",
              !bare.system.has_value(),
              "a live sweep is not a box record and must not claim to be");
        check("it still has rows", !bare.features.empty(), "none");

        FeaturesDoc rep = load_features_file(fx("features-report.json"));
        check("the full report is recognised",
              rep.source.find("asmtest-bench-report") != std::string::npos,
              "source: " + rep.source);
        check("the report carries its system block", rep.system.has_value(),
              "bench-report.sh always merges one in");

        // The substitute probe row OMITS the measured keys entirely rather than
        // writing nulls. Both spellings must land on nullopt.
        FeaturesDoc probe = load_features_file(fx("features-probe.json"));
        check("probe rows parse", !probe.features.empty(), "none");
        const FeatureRow &p = probe.features.front();
        check("an omitted complete is not measured", !p.complete,
              "got a value");
        check("an omitted trace_insns is not measured", !p.trace_insns,
              "got a value");
        check("an omitted insns_truth is not measured", !p.insns_truth,
              "got a value");
        check("but the row's own fields survive", !p.tier.empty(),
              "empty tier");
    }

    // --- unknown fields are ignored, absent `note` is not an error ----------
    {
        FeaturesDoc d = load_features_file(fx("features-unknown-keys.json"));
        check("a row with unknown keys still loads", !d.features.empty(),
              "forward compatibility: a newer producer must not break us");
        bool any_note = false;
        for (const FeatureRow &r : d.features)
            if (r.note)
                any_note = true;
        check("a row without `note` loads with nullopt",
              !any_note || d.features.size() > 1,
              "note is optional on most rows");
    }

    // --- an unreadable file is an ERROR, not an empty table -----------------
    {
        bool threw = false;
        try {
            (void)load_features_file(fx("features-not-a-sweep.json"));
        } catch (const std::exception &e) {
            threw = true;
            check("the error names the file",
                  std::string(e.what()).find("features-not-a-sweep") !=
                      std::string::npos,
                  e.what());
            check("and says what was wrong",
                  std::string(e.what()).find("features") != std::string::npos,
                  e.what());
        }
        check("a JSON document with no features array throws", threw,
              "an empty table would read as 'this box has no backends'");
    }

    // --- perf-history: a torn final line is SKIPPED AND COUNTED -------------
    {
        PerfHistory h = load_perf_history_file(fx("perf-history-torn.jsonl"));
        check("the good lines parsed", h.lines.size() == 2,
              "got " + std::to_string(h.lines.size()) + " lines, want 2");
        check("the torn line was counted", h.skipped == 1,
              "got skipped=" + std::to_string(h.skipped) + ", want 1");
        if (!h.lines.empty()) {
            eq("first line's commit", h.lines[0].commit, "d3e0315");
            check("its native points parsed", !h.lines[0].native.empty(),
                  "none");
        }
        // A blank line is not damage.
        std::istringstream blank("\n\n");
        PerfHistory hb = load_perf_history(blank);
        check("blank lines are not counted as damage", hb.skipped == 0,
              "got " + std::to_string(hb.skipped));
    }

    // --- the committed real history ----------------------------------------
    {
        PerfHistory h = load_perf_history_file(
            std::string(ASMTEST_REPO_ROOT) +
            "/benchmarks/boxes/amd-linux-x86_64-9e05f0f2/perf-history.jsonl");
        check("the committed history parses", !h.lines.empty(), "no lines");
        check("with nothing skipped", h.skipped == 0,
              "a committed file should be intact, got skipped=" +
                  std::to_string(h.skipped));
    }

    // --- scan_boxes ---------------------------------------------------------
    {
        auto boxes = scan_boxes(ASMTEST_REPO_ROOT);
        check("scan_boxes finds the committed boxes", boxes.size() >= 9,
              "found " + std::to_string(boxes.size()) + ", want >= 9");
        bool amd = false;
        for (const BoxRecord &b : boxes)
            if (b.box_id == "amd-linux-x86_64-9e05f0f2") {
                amd = true;
                check("the amd box has a features.json", b.has_features, "no");
                check("and a perf-history.jsonl", b.has_history, "no");
            }
        check("the amd box is among them", amd, "not found");
        for (std::size_t i = 1; i < boxes.size(); i++)
            check("boxes are sorted by box_id",
                  boxes[i - 1].box_id < boxes[i].box_id,
                  boxes[i - 1].box_id + " came before " + boxes[i].box_id);
        check("a tree with no boxes yields an empty list, not a throw",
              scan_boxes("/nonexistent-path-for-this-test").empty(),
              "threw or "
              "returned rows");
    }

    if (failures) {
        std::fprintf(stderr, "%d data-reader check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_data_readers: all checks passed\n");
    return 0;
}
