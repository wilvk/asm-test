// perf_history.h — the per-box performance trend reader
// (docs/internal/gui/02-exporters-and-readers.md T5).
//
// `benchmarks/boxes/<box_id>/perf-history.jsonl` is APPEND-ONLY: one JSON line
// per `bench-report.sh --record` run, written by a process that can be killed
// mid-write. A torn final line is therefore a NORMAL state of a healthy file,
// not corruption — so a malformed line is skipped and COUNTED, never fatal and
// never silent. `PerfHistory::skipped` is what the view renders to say "there
// were N lines here I could not read", which is the difference between a trend
// with a gap and a trend that quietly lies about its own extent.
#ifndef ASMDESK_DATA_PERF_HISTORY_H
#define ASMDESK_DATA_PERF_HISTORY_H

#include <cstddef>
#include <istream>
#include <string>
#include <vector>

namespace asmdesk::data {

struct PerfPoint {
    std::string name;
    double median = 0;
    std::string unit;
};

// One line: the keys scripts/bench-report.sh appends.
struct PerfLine {
    std::string timestamp, commit, os, arch, unit;
    bool virtualized = false;
    std::vector<PerfPoint> native;
};

struct PerfHistory {
    std::vector<PerfLine> lines;
    std::size_t skipped = 0; // unparseable lines, including a torn final one
};

PerfHistory load_perf_history(std::istream &in);
PerfHistory load_perf_history_file(const std::string &path);

// One `benchmarks/boxes/<box_id>/` directory.
struct BoxRecord {
    std::string box_id, dir;
    bool has_features = false, has_history = false;
};

// Enumerate benchmarks/boxes/ under `repo_root`, sorted by box_id. An absent or
// unreadable directory yields an empty vector rather than throwing — the panel
// is usable from a tree that has no committed boxes.
std::vector<BoxRecord> scan_boxes(const std::string &repo_root);

} // namespace asmdesk::data
#endif // ASMDESK_DATA_PERF_HISTORY_H
