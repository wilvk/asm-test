// test_loom_annex.cpp — the lane annex's place-indexed join and its two-verdict
// law (05-loom-day-one.md T4). Render-only: fabric.o + annex.o + the doc model.
#include <cstdio>
#include <string>
#include <vector>

#include "loom/annex.h"
#include "loom_fixture.h"

using namespace asmdesk;
using namespace loomfx;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// The verdict enum is DELIBERATELY closed at two. This function is the pin: it
// has no `default`, so adding a third enumerator makes the build fail under
// -Wall -Wextra rather than silently letting a "conflicts" verdict exist.
static const char *verdict_is_closed(annex_verdict v) {
    switch (v) {
    case annex_verdict::corroborates:
        return "corroborates";
    case annex_verdict::unconfirmed:
        return "unconfirmed";
    }
    return nullptr; // unreachable while the enum has exactly two enumerators
}
static_assert(static_cast<int>(annex_verdict::corroborates) == 0, "");
static_assert(static_cast<int>(annex_verdict::unconfirmed) == 1, "");

// --- hand-built companion recordings ---------------------------------------
static Recording rec(const char *path, const char *backend, bool exact,
                     std::vector<std::pair<std::string, nlohmann::json>> ev) {
    Recording r;
    r.version = 1;
    r.path = path;
    r.arch = "x86_64";
    r.provenance.backend = backend;
    r.provenance.exact = exact;
    r.provenance.trust = exact ? "exact" : "statistical";
    r.has_end = true;
    for (auto &e : ev)
        r.by_kind[e.first].push_back(Event{e.first, e.second});
    return r;
}

int main() {
    Fixture fx;
    loom_fabric_t f = build(fx);
    const uint32_t band = static_cast<uint32_t>(band_lane(f));
    const uint32_t rax = static_cast<uint32_t>(lane_named(f, "rax"));

    // Bytes the fabric DID write (the step-1 store at SLOT..SLOT+8) and bytes
    // it did not (the return-address slot at SP, which it only read).
    Workspace ws;
    ws.recordings.push_back(rec("/rec/watch-b.asmtrace", "hwdebug-watch", true,
                                {
                                    {"watch",
                                     {{"hit_no", 1},
                                      {"tid", 42},
                                      {"pc", 0x100003},
                                      {"addr", SLOT},
                                      {"is_write", 1},
                                      {"value_ok", true},
                                      {"value_len", 8},
                                      {"value", 7},
                                      {"func", "df_chain"},
                                      {"module", "victim"},
                                      {"off", 3}}},
                                    {"watch",
                                     {{"hit_no", 2},
                                      {"tid", 42},
                                      {"pc", 0x100014},
                                      {"addr", SP},
                                      {"is_write", 0},
                                      {"value_ok", true},
                                      {"value_len", 8},
                                      {"value", 0xdeadbeef}}},
                                }));
    ws.recordings.push_back(rec(
        "/rec/survey-c.asmtrace", "ibs-op", false,
        {
            {"survey",
             {{"sampler", "ibs-op"},
              {"samples", 10442},
              {"lost", 17},
              {"edges",
               nlohmann::json::array({
                   nlohmann::json{
                       {"from_addr", 0x03}, {"to_addr", 0x11}, {"count", 812}},
                   nlohmann::json{{"from_addr", 0xdead0000},
                                  {"to_addr", 0xdead0100},
                                  {"count", 5}},
               })}}},
        }));
    ws.recordings[1].drops_lost = 17;
    ws.recordings.push_back(rec("/rec/taint-d.asmtrace", "ptrace-dataflow",
                                true,
                                {
                                    {"taint",
                                     {{"off", 0x03},
                                      {"ea", SLOT},
                                      {"seed_off", 0},
                                      {"tag", 1},
                                      {"depth", 2}}},
                                }));
    ws.recordings.push_back(rec("/rec/src-e.asmtrace", "ptrace-region", true,
                                {
                                    {"srcmap",
                                     {{"off", 0x03},
                                      {"value", 42},
                                      {"kind", 1},
                                      {"file", "df_chain.s"},
                                      {"col", 0}}},
                                    {"srcmap",
                                     {{"off", 0xfffff},
                                      {"value", 99},
                                      {"kind", 1},
                                      {"file", "elsewhere.s"}}},
                                }));

    // --- the band's data-space join ----------------------------------------
    std::vector<annex_entry_t> rows;
    loom_annex_join(ws, f, band, -1, &rows);
    std::string dump = loom_annex_dump(rows);

    {
        size_t corrob = 0, unconf = 0;
        for (const annex_entry_t &e : rows)
            if (e.kind == annex_kind::watch_hit)
                (e.verdict == annex_verdict::corroborates ? corrob : unconf)++;
        check("a watch hit on bytes the fabric WROTE corroborates", corrob == 1,
              "got " + std::to_string(corrob) + "\n" + dump);
        check("a watch hit on bytes it only READ is unconfirmed", unconf == 1,
              "got " + std::to_string(unconf) + "\n" + dump);
    }
    check("the taint hit joins the band by effective address",
          dump.find("taint_hit space=data corroborates") != std::string::npos,
          dump);
    check("an IBS edge never joins a DATA lane",
          dump.find("ibs_edge") == std::string::npos,
          "a branch edge is a code fact and joined a memory band:\n" + dump);
    check("a srcmap row never joins a DATA lane",
          dump.find("srcmap_row") == std::string::npos, dump);

    // --- a register lane's code-space join ---------------------------------
    loom_annex_join(ws, f, rax, -1, &rows);
    dump = loom_annex_dump(rows);
    check("the IBS edge joins the code key",
          dump.find("ibs_edge space=code") != std::string::npos, dump);
    check("an out-of-range IBS edge does not join",
          dump.find("0xdead0000") == std::string::npos, dump);
    check("the statistical chip carries its drop accounting",
          dump.find("drops{lost=17}") != std::string::npos, dump);
    check("a sampled edge NEVER corroborates",
          dump.find("ibs_edge space=code corroborates") == std::string::npos,
          "a histogram bucket was presented as an observation:\n" + dump);
    check("the srcmap row inside the code range joins",
          dump.find("srcmap_row space=code corroborates") != std::string::npos,
          dump);
    check("the srcmap row outside it does not",
          dump.find("elsewhere.s") == std::string::npos, dump);

    // A register lane joins a watch hit only by VALUE, and says so.
    check("a register lane's watch join is labelled informational",
          dump.find("same value, different place — informational") !=
              std::string::npos,
          dump);
    check("that join is not a data-space claim",
          dump.find("watch_hit space=data") == std::string::npos, dump);

    // --- corroboration, never contradiction --------------------------------
    for (const annex_entry_t &e : rows) {
        check("every verdict renders one of exactly two texts",
              verdict_is_closed(e.verdict) != nullptr, "");
        std::string t = annex_verdict_text(e);
        check("the unconfirmed text names the producer",
              e.verdict == annex_verdict::corroborates ||
                  t.find(e.producer) != std::string::npos,
              t);
    }
    for (uint32_t lane = 0; lane < f.lanes.size(); lane++) {
        loom_annex_join(ws, f, lane, -1, &rows);
        std::string d = loom_annex_dump(rows);
        check("no annex output ever says 'contradict'",
              d.find("contradict") == std::string::npos, d);
        check("no annex output ever says 'conflict'",
              d.find("conflict") == std::string::npos, d);
    }

    // --- a recording never corroborates itself -----------------------------
    {
        std::vector<annex_entry_t> a, b;
        loom_annex_join(ws, f, band, -1, &a);
        loom_annex_join(ws, f, band, 0, &b);
        check("skipping the fabric's own recording drops its entries",
              b.size() < a.size(),
              std::to_string(a.size()) + " vs " + std::to_string(b.size()));
    }

    // --- determinism --------------------------------------------------------
    {
        std::vector<annex_entry_t> a, b;
        loom_annex_join(ws, f, band, -1, &a);
        loom_annex_join(ws, f, band, -1, &b);
        check("two joins are byte-identical",
              loom_annex_dump(a) == loom_annex_dump(b), "");
    }

    if (failures) {
        std::fprintf(stderr, "%d loom annex check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_annex: all checks passed\n");
    return 0;
}
