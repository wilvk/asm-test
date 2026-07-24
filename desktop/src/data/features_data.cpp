// features_data.cpp — the readers of features_data.h.
#include "data/features_data.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace asmdesk::data {

namespace {

std::string str_or(const nlohmann::json &j, const char *key,
                   const char *dflt = "") {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : dflt;
}

bool bool_or(const nlohmann::json &j, const char *key, bool dflt) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : dflt;
}

// The three-state read that makes this library worth having: ABSENT and null
// are both "not measured" and yield nullopt. Only a real value is a value.
std::optional<bool> opt_bool(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_boolean())
        return std::nullopt;
    return it->get<bool>();
}

std::optional<std::int64_t> opt_i64(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number_integer())
        return std::nullopt;
    return it->get<std::int64_t>();
}

std::optional<std::string> opt_str(const nlohmann::json &j, const char *key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_string())
        return std::nullopt;
    return it->get<std::string>();
}

BoxSystem parse_system(const nlohmann::json &j) {
    BoxSystem s;
    s.box_id = str_or(j, "box_id");
    s.os = str_or(j, "os");
    s.os_version = str_or(j, "os_version");
    s.arch = str_or(j, "arch");
    s.cpu = str_or(j, "cpu");
    s.uarch = str_or(j, "uarch");
    s.vendor = str_or(j, "vendor");
    s.cc = str_or(j, "cc");
    s.asmtest_version = str_or(j, "asmtest_version");
    s.commit = str_or(j, "commit");
    s.timestamp = str_or(j, "timestamp");
    s.virtualized = bool_or(j, "virtualized", false);
    return s;
}

} // namespace

FeaturesDoc load_features(const nlohmann::json &doc) {
    if (!doc.is_object())
        throw std::runtime_error("not a JSON object");

    FeaturesDoc out;
    auto schema = doc.find("schema");
    if (schema != doc.end() && schema->is_string())
        out.source = schema->get<std::string>();
    else if (doc.contains("system"))
        out.source = "box record";
    else
        out.source = "asmfeatures stdout";

    auto sys = doc.find("system");
    if (sys != doc.end() && sys->is_object())
        out.system = parse_system(*sys);

    auto feats = doc.find("features");
    if (feats == doc.end() || !feats->is_array())
        throw std::runtime_error(
            "no \"features\" array — this is not an asmfeatures sweep, a box "
            "record, or an asmtest-bench-report/v1 report");

    for (const auto &f : *feats) {
        if (!f.is_object())
            continue;
        FeatureRow r;
        r.tier = str_or(f, "tier");
        r.backend = str_or(f, "backend");
        r.arch = str_or(f, "arch");
        r.scope = str_or(f, "scope");
        r.available = bool_or(f, "available", false);
        r.skip_reason = str_or(f, "skip_reason");
        r.fidelity = str_or(f, "fidelity");
        r.complete = opt_bool(f, "complete");
        r.trace_insns = opt_i64(f, "trace_insns");
        r.insns_truth = opt_i64(f, "insns_truth");
        r.note = opt_str(f, "note");
        out.features.push_back(std::move(r));
    }
    return out;
}

FeaturesDoc load_features_file(const std::string &path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error(path + ": cannot open");
    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception &e) {
        throw std::runtime_error(path + ": " + e.what());
    }
    try {
        return load_features(doc);
    } catch (const std::exception &e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

} // namespace asmdesk::data
