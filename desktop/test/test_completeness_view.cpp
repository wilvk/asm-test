// test_completeness_view.cpp — the backend-completeness view model
// (docs/internal/gui/02-exporters-and-readers.md T6).
//
// The golden compare is the D7 test with teeth: the rendered table is compared
// BYTE for byte, so editing a skip_reason by one character fails here. That is
// the mechanism by which "skip_reason is rendered verbatim" stops being a
// convention and starts being a property.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "data/features_data.h"
#include "views/completeness_model.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_REPO_ROOT
#error "ASMTEST_REPO_ROOT must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

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

static void golden(const std::string &name, const std::string &got) {
    std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
    const char *update = std::getenv("UPDATE_GOLDEN");
    if (update && std::string(update) == "1") {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            fail("golden " + name, "cannot write " + path);
            return;
        }
        out << got;
        std::printf("  regenerated %s\n", path.c_str());
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("golden " + name, "no expected file at " + path +
                                   " — regenerate with UPDATE_GOLDEN=1");
        return;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    if (buf.str() != got)
        fail("golden " + name, "the render differs from " + path);
}

// One hand-built row, so the cell rule's branches can be exercised in isolation.
static data::FeatureRow row(std::optional<bool> complete,
                            std::optional<std::int64_t> trace_insns,
                            std::optional<std::int64_t> insns_truth) {
    data::FeatureRow r;
    r.tier = "t";
    r.backend = "b";
    r.available = true;
    r.complete = complete;
    r.trace_insns = trace_insns;
    r.insns_truth = insns_truth;
    return r;
}

int main() {
    // --- the completeness cell's four branches ------------------------------
    {
        bool trunc = false;
        // 1. Not measured at all: an em dash. NEVER "0".
        eq("no trace_insns",
           completeness_cell(row(std::nullopt, std::nullopt, std::nullopt),
                             &trunc),
           "—");
        check("an unmeasured row is not called truncated", !trunc, "it was");

        // 2. Measured, with no truth to compare against (the emulator IS the
        //    truth, so insns_truth is null on its rows).
        eq("trace_insns without truth",
           completeness_cell(row(true, 4, std::nullopt), &trunc),
           "4 insns complete");
        check("a complete row is not truncated", !trunc, "it was");

        // 3. Both measured and equal: complete.
        eq("counts agree", completeness_cell(row(true, 242, 242), &trunc),
           "242/242 complete");
        check("agreeing counts are not truncated", !trunc, "they were");

        // 4. Both measured and SHORT: the AMD-LBR plateau, made loud.
        eq("counts disagree",
           completeness_cell(row(std::nullopt, 16, 242), &trunc),
           "16/242 TRUNCATED");
        check("a short capture IS truncated", trunc, "it was not flagged");

        // complete:false alone is enough, even with no truth count — the
        // backend admitting incompleteness is a measurement in its own right.
        eq("complete:false with no truth",
           completeness_cell(row(false, 16, std::nullopt), &trunc),
           "16 insns TRUNCATED");
        check("complete:false alone flags truncation", trunc, "it did not");
    }

    // --- the dishonesty fixture: skip_reason survives BYTE for byte ---------
    {
        data::FeaturesDoc doc = data::load_features_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/features-dishonest.json");
        CompletenessTable t = build_completeness(doc);
        std::string text = render_completeness_text(t);

        const std::string reason =
            "Intel PT is an Intel feature; this host is AuthenticAMD. "
            "perf_event_paranoid is 4, which would also refuse the AUX mapping "
            "even on Intel silicon; and libipt is not linked into this build.";
        check("the multi-clause skip_reason appears VERBATIM",
              text.find(reason) != std::string::npos,
              "the reason was truncated, wrapped or paraphrased:\n" + text);
        check("the truncated capture is loud",
              text.find("16/242 TRUNCATED") != std::string::npos,
              "16/242 TRUNCATED not in the render:\n" + text);
        check("a measured-without-truth row shows its count",
              text.find("242 insns") != std::string::npos, text);
        check("the box header names the box",
              text.find("dishonest-fixture") != std::string::npos, text);
        eq("truncated row count", std::to_string(t.n_truncated), "1");
        golden("completeness-dishonest.txt", text);
    }

    // --- the committed REAL amd box record ----------------------------------
    {
        data::FeaturesDoc doc = data::load_features_file(
            std::string(ASMTEST_REPO_ROOT) +
            "/benchmarks/boxes/amd-linux-x86_64-9e05f0f2/features.json");
        CompletenessTable t = build_completeness(doc);

        // Row order is PRODUCER order (tools/asmfeatures.c's narrative), not
        // alphabetical: the emulator floor comes first, always.
        check("there are rows", !t.rows.empty(), "none");
        eq("the first row is the emulator floor", t.rows.front().tier,
           "emulator");
        check(
            "row order is not alphabetical by tier",
            [&] {
                for (std::size_t i = 1; i < t.rows.size(); i++)
                    if (t.rows[i - 1].tier > t.rows[i].tier)
                        return true;
                return false;
            }(),
            "the rows came out sorted, which would mean the producer's "
            "narrative order was lost");

        // Every unavailable row's status IS its skip_reason, unedited.
        for (std::size_t i = 0; i < t.rows.size(); i++) {
            const CompletenessRow &r = t.rows[i];
            if (r.available) {
                eq("available row " + std::to_string(i) + " status", r.status,
                   "ok");
            } else {
                eq("unavailable row " + std::to_string(i) +
                       " carries its reason",
                   r.status, doc.features[i].skip_reason);
            }
        }
        check("the header names the box and its CPU",
              t.box_label.find("amd-linux-x86_64-9e05f0f2") !=
                      std::string::npos &&
                  t.box_label.find("Ryzen") != std::string::npos,
              t.box_label);
        golden("completeness-amd.txt", render_completeness_text(t));
    }

    // --- a live sweep is labelled as one, not as a box ----------------------
    {
        data::FeaturesDoc doc = data::load_features_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/features-bare.json");
        CompletenessTable t = build_completeness(doc);
        eq("a system-less sweep is labelled live", t.box_label,
           "live sweep (this host)");
    }

    // --- a virtualized box says so ------------------------------------------
    {
        data::FeaturesDoc doc = data::load_features_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/features-report.json");
        CompletenessTable t = build_completeness(doc);
        check("a virtualized box is flagged in the header",
              t.box_label.find("(virtualized)") != std::string::npos,
              t.box_label);
    }

    if (failures) {
        std::fprintf(stderr, "%d completeness check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_completeness_view: all checks passed\n");
    return 0;
}
