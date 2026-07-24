// recording.cpp — the .asmtrace NDJSON loader (03-desktop-shell.md T3). The
// reject rules and the honesty accounting here make the schema's D7 laws
// executable: a stream without provenance is refused, a newer major is refused
// by name, and truncation/drops/redaction/torn each survive into the model
// rather than being silently smoothed away.
#include "doc/recording.h"

#include <array>
#include <fstream>
#include <sstream>

namespace asmdesk {

using nlohmann::json;

// The v1 registry (asmtrace-schema.md "Event kinds (v1)"), incl. the `end`
// footer. A `k` outside this set loads but counts as unknown (forward compat);
// the schema's still-RESERVED kinds (mem, fpenv, blame, codeimage, …) are
// unknown to a v1 reader, which is exactly why they are NOT listed here.
//
// session/cmd/err are the exception: they were reserved for 07 and are now
// DEFINED (asmtrace-schema.md "Serve protocol"), so a live session's lifecycle
// lines are known kinds rather than unknown-kind noise. They are serve-only —
// no `--record` file contains them — but the loader is the one that reads a
// serve stream too, and a client that counted every lifecycle line as "unknown"
// would report a healthy session as a partly-unreadable one.
//
// `codeimage` joined them for the same reason (asmtrace-schema.md, "`codeimage`
// — captured code bytes at a version", owned by 08): it is now a DEFINED kind
// with a producer, and unlike the serve-only three it is an ordinary recording
// event that the `end` footer counts.
static const std::array<const char *, 20> kKnownKinds = {
    {"trace",  "coverage", "syscall", "stream",  "call",     "graph",    "topo",
     "survey", "watch",    "df_step", "df_edge", "regstate", "result",   "note",
     "stitch", "end",      "session", "cmd",     "err",      "codeimage"}};

bool is_known_kind(const std::string &kind) {
    for (const char *k : kKnownKinds)
        if (kind == k)
            return true;
    return false;
}

uint64_t Recording::event_count() const {
    uint64_t n = 0;
    for (const auto &kv : by_kind)
        n += kv.second.size();
    return n;
}

// Non-throwing parse (nlohmann returns a discarded value instead of throwing).
static json parse_line(const std::string &line) {
    return json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
}

static uint64_t as_u64(const json &v, uint64_t dflt) {
    return v.is_number() ? v.get<uint64_t>() : dflt;
}

std::optional<Recording> load_recording(std::istream &in, std::string &err) {
    Recording rec;
    err.clear();

    std::string line;
    if (!std::getline(in, line)) {
        err = "empty stream: no header line";
        return std::nullopt;
    }

    // --- line 1: the header ---
    json header = parse_line(line);
    if (header.is_discarded() || !header.is_object()) {
        err = "line 1: header is not a JSON object";
        return std::nullopt;
    }
    if (!header.contains("asmtrace") ||
        !header["asmtrace"].is_number_integer()) {
        err = "line 1: header has no integer \"asmtrace\" major";
        return std::nullopt;
    }
    int major = header["asmtrace"].get<int>();
    if (major > kAsmtraceMajor) {
        err = "asmtrace major " + std::to_string(major) +
              " is newer than this reader's major " +
              std::to_string(kAsmtraceMajor) + " — refusing by name";
        return std::nullopt;
    }
    rec.version = major;

    // provenance is mandatory (D7): a stream without it is not a recording.
    if (!header.contains("provenance") || !header["provenance"].is_object()) {
        err =
            "line 1: header has no \"provenance\" object (D7: not a recording)";
        return std::nullopt;
    }
    const json &prov = header["provenance"];
    rec.provenance.raw = prov;
    // `exact` encodes a fact that cannot be inferred; the schema forbids
    // defaulting it, so a missing/non-bool exact is a reject, not a guess.
    if (!prov.contains("exact") || !prov["exact"].is_boolean()) {
        err = "line 1: provenance.exact is mandatory and must not be defaulted";
        return std::nullopt;
    }
    rec.provenance.exact = prov["exact"].get<bool>();
    if (prov.contains("backend") && prov["backend"].is_string())
        rec.provenance.backend = prov["backend"].get<std::string>();
    if (prov.contains("trust") && prov["trust"].is_string())
        rec.provenance.trust = prov["trust"].get<std::string>();
    if (prov.contains("redacted") && prov["redacted"].is_boolean())
        rec.provenance.redacted = prov["redacted"].get<bool>();

    if (header.contains("producer") && header["producer"].is_object()) {
        const json &p = header["producer"];
        if (p.contains("name") && p["name"].is_string())
            rec.producer.name = p["name"].get<std::string>();
        if (p.contains("version") && p["version"].is_string())
            rec.producer.version = p["version"].get<std::string>();
    }
    if (header.contains("arch") && header["arch"].is_string())
        rec.arch = header["arch"].get<std::string>();

    // --- event lines ---
    size_t lineno = 1;
    while (std::getline(in, line)) {
        lineno++;
        if (line.empty())
            continue; // tolerate a stray blank line from a trailing newline

        json ev = parse_line(line);
        if (ev.is_discarded()) {
            // An unparseable line with no trailing newline (eof already set) is a
            // producer that died mid-write: keep it as a torn tail rather than
            // erroring. Any OTHER unparseable line is a hard error.
            if (in.eof()) {
                rec.torn = true;
                break;
            }
            err = "line " + std::to_string(lineno) + ": not valid JSON";
            return std::nullopt;
        }
        if (!ev.is_object()) {
            err = "line " + std::to_string(lineno) +
                  ": event is not a JSON object";
            return std::nullopt;
        }
        if (!ev.contains("k") || !ev["k"].is_string()) {
            err = "line " + std::to_string(lineno) +
                  ": event has no string \"k\"";
            return std::nullopt;
        }
        std::string k = ev["k"].get<std::string>();

        if (k == "end") {
            rec.has_end = true;
            if (ev.contains("events"))
                rec.declared_events = as_u64(ev["events"], 0);
            if (ev.contains("truncated") && ev["truncated"].is_boolean())
                rec.end_truncated = ev["truncated"].get<bool>();
            if (ev.contains("drops") && ev["drops"].is_object()) {
                const json &d = ev["drops"];
                if (d.contains("lost"))
                    rec.drops_lost = as_u64(d["lost"], 0);
                if (d.contains("throttled") && d["throttled"].is_boolean())
                    rec.drops_throttled = d["throttled"].get<bool>();
            }
            continue; // the footer is not stored in by_kind
        }

        rec.by_kind[k].push_back(Event{k, std::move(ev), rec.next_seq++});
        if (!is_known_kind(k))
            rec.unknown_kinds++;
    }

    // A file without an `end` event is TORN (schema Compatibility rules), and a
    // reader must say so rather than present a partial recording as complete.
    if (!rec.has_end)
        rec.torn = true;

    return rec;
}

std::optional<Recording> load_recording_file(const std::string &path,
                                             std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "cannot open " + path;
        return std::nullopt;
    }
    std::optional<Recording> rec = load_recording(in, err);
    if (rec)
        rec->path = path;
    return rec;
}

} // namespace asmdesk
