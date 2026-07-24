// perf_history.cpp — the readers of perf_history.h.
#include "data/perf_history.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

namespace asmdesk::data {

namespace {

std::string str_or(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

} // namespace

PerfHistory load_perf_history(std::istream &in) {
    PerfHistory h;
    std::string line;
    while (std::getline(in, line)) {
        // A blank line is not data and is not damage; it is just a blank line.
        if (line.find_first_not_of(" \t\r\n") == std::string::npos)
            continue;
        nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            h.skipped++; // a torn final line lands here, counted not fatal
            continue;
        }
        PerfLine p;
        p.timestamp = str_or(j, "timestamp");
        p.commit = str_or(j, "commit");
        p.os = str_or(j, "os");
        p.arch = str_or(j, "arch");
        p.unit = str_or(j, "unit");
        auto v = j.find("virtualized");
        if (v != j.end() && v->is_boolean())
            p.virtualized = v->get<bool>();
        auto nat = j.find("native");
        if (nat != j.end() && nat->is_array()) {
            for (const auto &e : *nat) {
                if (!e.is_object())
                    continue;
                PerfPoint pt;
                pt.name = str_or(e, "name");
                auto m = e.find("median");
                if (m != e.end() && m->is_number())
                    pt.median = m->get<double>();
                pt.unit = str_or(e, "unit");
                p.native.push_back(std::move(pt));
            }
        }
        h.lines.push_back(std::move(p));
    }
    return h;
}

PerfHistory load_perf_history_file(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        return PerfHistory{};
    return load_perf_history(in);
}

std::vector<BoxRecord> scan_boxes(const std::string &repo_root) {
    namespace fs = std::filesystem;
    std::vector<BoxRecord> out;
    std::error_code ec;
    fs::path root = fs::path(repo_root) / "benchmarks" / "boxes";
    if (!fs::is_directory(root, ec))
        return out;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!entry.is_directory())
            continue;
        BoxRecord b;
        b.box_id = entry.path().filename().string();
        b.dir = entry.path().string();
        b.has_features = fs::exists(entry.path() / "features.json", ec);
        b.has_history = fs::exists(entry.path() / "perf-history.jsonl", ec);
        out.push_back(std::move(b));
    }
    // Sorted by box_id: the panel's combo must not reorder itself between runs
    // just because the filesystem returned directory entries in another order.
    std::sort(out.begin(), out.end(),
              [](const BoxRecord &a, const BoxRecord &b) {
                  return a.box_id < b.box_id;
              });
    return out;
}

} // namespace asmdesk::data
