// recording.h — the .asmtrace document model every desktop view renders from
// (docs/internal/gui/03-desktop-shell.md T3). One NDJSON recording -> a
// Recording: its events grouped by kind, its mandatory provenance, and the
// honesty facts a reader MUST surface (truncation, drops, redaction, torn).
//
// Field names follow the schema (docs/internal/gui/asmtrace-schema.md, owned by
// 01); this file owns loader BEHAVIOUR. Where the schema and an older draft
// disagree the CODE wins: the event-kind field is "k" (not "kind"); the header
// carries a top-level `producer` {name,version} and a mandatory `provenance`
// {backend,exact,trust,...}; truncation and drops ride on the `end` footer; and
// a recording with no `end` event is TORN.
#ifndef ASMDESK_DOC_RECORDING_H
#define ASMDESK_DOC_RECORDING_H

#include <cstdint>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace asmdesk {

inline constexpr int kAsmtraceMajor = 1;

// The header's top-level producer object (schema Envelope): who wrote the stream.
struct Producer {
    std::string name;    // "asmspy" | "asmtrace_record"
    std::string version; // ASMTEST_VERSION at record time
};

// provenance — mandatory on every stream (schema Provenance). It is what lets a
// reader always answer "how do you know?".
struct Provenance {
    std::string
        backend; // measured producer id, e.g. "ptrace-syscalls" (mandatory)
    bool exact =
        true; // true = every event observed; false = a sample (mandatory)
    std::string
        trust; // "exact" | "statistical" | "weak" | "strong" (mandatory)
    bool redacted =
        false; // payload withheld AT RECORD TIME (optional; absent = false)
    nlohmann::json
        raw; // the whole provenance object, for chrome / the lane annex
};

// The header's optional `code` object (schema, 28 R1 T1): the identity of the
// routine's bytes, so a consumer can prove two recordings are — or refuse them
// as not — the same routine. `present` is false when the header omitted it (a
// producer without stable bytes), and a consumer then keeps its honest caveat
// rather than asserting sameness.
struct Code {
    bool present = false;
    std::string name;   // the routine name (informational)
    std::string sha256; // lowercase-hex SHA-256 of the bytes (authoritative)
    uint64_t len = 0;   // the byte length hashed
};

// One event line. `kind` is the schema "k" selector; `body` is the whole line so
// views decode fields lazily (D10: an optional "disasm" string rides in body
// untouched — absent is normal, and a view degrades to offsets).
//
// `seq` is its position in the STREAM, counted across every kind. by_kind is the
// right shape for a loader and the wrong one for the handful of facts that live
// in the interleaving: a region capture writes [trace…][coverage] per invocation
// (08-observer-views.md T6), so "which invocation was this instruction in" is
// answerable only in file order. Storing the ordinal keeps that answerable
// without a second, order-preserving copy of every event.
struct Event {
    std::string kind;
    nlohmann::json body;
    uint64_t seq = 0;
};

// A loaded recording. by_kind holds every event EXCEPT the `end` footer, whose
// facts are lifted into the honesty fields below.
struct Recording {
    int version = 0;
    Producer producer;
    Provenance provenance;
    std::string arch;
    Code code; // optional routine identity (absent => code.present == false)

    std::map<std::string, std::vector<Event>> by_kind;
    uint64_t unknown_kinds = 0; // events kept but outside the v1 kind registry
    uint64_t next_seq = 0;      // the stream position the next event will take

    // Honesty facts, off the `end` footer (schema `end`) or its absence.
    bool has_end = false;
    uint64_t declared_events = 0; // the footer's own event count (a self-check)
    bool end_truncated = false;
    uint64_t drops_lost = 0;
    bool drops_throttled = false;
    bool torn = false; // no `end` event was seen -> a TORN recording

    std::string path;

    // Authored + unsaved (18-breach-stops.md T3, F24). An Author run materialised
    // into the Workspace carries no file yet, so closing its tab would lose it
    // silently. `dirty` marks that: the tab title shows a `*`, and shell_close is
    // gated behind a save/discard/cancel choice. A successful save clears it. A
    // recording loaded from a file is never dirty (it already has a copy on disk).
    bool dirty = false;

    // A recording is truncated if the footer says so OR it is torn (incomplete);
    // dropped if the footer's drop record is non-zero; statistical if not exact.
    bool truncated() const { return end_truncated || torn; }
    bool dropped() const { return drops_lost > 0 || drops_throttled; }
    bool statistical() const { return !provenance.exact; }
    // Total real events (by_kind never holds the `end` footer).
    uint64_t event_count() const;
};

// The v1 event-kind registry (asmtrace-schema.md). A kind outside it still loads
// (forward compat) but is counted in Recording::unknown_kinds. `end` is the
// footer and is handled apart from the registry.
bool is_known_kind(const std::string &kind);

// Load one recording. Returns the Recording on success; on any reject rule an
// empty optional and a human-readable `err` (with a line number where relevant).
// Rejects: a non-object/kindless header, an integer `asmtrace` > 1 (newer major,
// by name), a missing `provenance` object, a missing/non-bool `provenance.exact`
// (never defaulted), and any event line that is not a JSON object or lacks a
// string "k" (except an unparseable FINAL line, kept as a torn tail).
std::optional<Recording> load_recording(std::istream &in, std::string &err);
std::optional<Recording> load_recording_file(const std::string &path,
                                             std::string &err);

// Serialize a Recording back to .asmtrace NDJSON text — the inverse of
// load_recording for everything the model keeps: the header (asmtrace major,
// producer, the whole `provenance` object, arch), every event in stream (`seq`)
// order, and — only when the recording actually carried one — the `end` footer.
// A torn recording is written WITHOUT a footer, so it reloads torn rather than
// presenting an incomplete capture as complete. Not byte-identical to the
// original wire text (JSON key order is normalized, and header fields the model
// does not decode — a live serve header's pid/cmd/ts — are dropped), but
// semantically faithful: the result reloads to an equal Recording. This is what
// lets the Inspect door put an in-memory live capture on disk (07-serve-live-
// host.md): the desktop keeps recordings in memory, and a `--record` run would
// have written this same NDJSON.
std::string recording_to_asmtrace(const Recording &rec);

// Write recording_to_asmtrace(rec) to `path`. Returns true, or false with a
// human-readable `err` (the path and the reason) on an open/write failure.
bool save_recording_file(const Recording &rec, const std::string &path,
                         std::string &err);

} // namespace asmdesk
#endif // ASMDESK_DOC_RECORDING_H
