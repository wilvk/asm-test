// completeness_model.h — the backend-completeness view model
// (docs/internal/gui/02-exporters-and-readers.md T6).
//
// A pure header (the cli/asmspy_logview.h pattern): no ImGui, no I/O, every
// function inline, so the model is unit-testable without linking a view.
//
// Three rules here are the whole point of the panel, and each has a test:
//
//   1. ROW ORDER IS PRODUCER ORDER. tools/asmfeatures.c already emits the rows
//      in a narrative order — emulator guests, then the native capture ladder,
//      then native-oop, the static hwtrace backends, DynamoRIO, disasm. That
//      order is information. Sorting alphabetically would destroy it.
//   2. `skip_reason` IS RENDERED VERBATIM. Never truncated, never paraphrased,
//      never replaced with a generic "unavailable". The reason a backend is
//      absent is the single most useful thing this table can tell anyone, and
//      it is a measured string — CoreSight's row says what CoreSight said.
//   3. TRUNCATION IS LOUD. `trace_insns < insns_truth` means the backend
//      captured less than ran (AMD LBR's plateau is exactly this), and so does
//      `complete == false`. Either one appends " TRUNCATED" to the cell. A
//      completeness table that renders 16-of-242 as a bare "16" is worse than
//      no table at all.
//
// The fourth rule is the one about ABSENCE: "not measured" renders as an em
// dash, never as 0. std::optional carries that all the way from the JSON null.
#ifndef ASMDESK_VIEWS_COMPLETENESS_MODEL_H
#define ASMDESK_VIEWS_COMPLETENESS_MODEL_H

#include <cstddef>
#include <string>
#include <vector>

#include "data/features_data.h"

namespace asmdesk {

struct CompletenessRow {
    std::string tier, backend, arch, scope;
    std::string status; // "ok", or the skip_reason VERBATIM
    std::string
        completeness; // "—" | "N insns" | "N/M" (+ " TRUNCATED"/" complete")
    std::string note; // "" when the row carried none
    bool available = false;
    bool truncated = false; // this row's capture is known to be incomplete
};

struct CompletenessTable {
    std::string box_label;
    std::vector<CompletenessRow> rows; // PRODUCER order
    std::size_t n_rows = 0, n_available = 0, n_measured = 0, n_complete = 0,
                n_truncated = 0;
};

// The completeness cell. Its four branches are exactly the four states the
// producer can be in, and none of them collapses into another.
inline std::string completeness_cell(const data::FeatureRow &f,
                                     bool *truncated_out) {
    bool truncated = false;
    std::string cell;
    if (!f.trace_insns) {
        cell = "—"; // not measured. NOT zero, and never rendered as zero.
    } else if (!f.insns_truth) {
        cell = std::to_string(*f.trace_insns) + " insns";
    } else {
        cell = std::to_string(*f.trace_insns) + "/" +
               std::to_string(*f.insns_truth);
    }
    // Either signal alone is enough: a backend may report complete=false
    // without a truth count, and a count short of the truth is truncation
    // whether or not the backend admitted it.
    if ((f.complete && !*f.complete) ||
        (f.trace_insns && f.insns_truth && *f.trace_insns < *f.insns_truth))
        truncated = true;

    if (truncated)
        cell += " TRUNCATED";
    else if (f.complete && *f.complete && f.trace_insns)
        cell += " complete";
    if (truncated_out)
        *truncated_out = truncated;
    return cell;
}

inline CompletenessTable build_completeness(const data::FeaturesDoc &doc) {
    CompletenessTable t;
    if (doc.system) {
        const data::BoxSystem &s = *doc.system;
        t.box_label = s.box_id + " — " + s.cpu + " — " + s.os_version + " — " +
                      s.commit + " " + s.timestamp;
        if (s.virtualized)
            t.box_label += " (virtualized)";
    } else {
        // No `system` block means this is a live sweep of whatever host ran it,
        // which is a different claim from a committed box record and is labelled
        // as one.
        t.box_label = "live sweep (this host)";
    }
    for (const data::FeatureRow &f : doc.features) {
        CompletenessRow r;
        r.tier = f.tier;
        r.backend = f.backend;
        r.arch = f.arch;
        r.scope = f.scope;
        r.available = f.available;
        r.status = f.available ? "ok" : f.skip_reason;
        r.completeness = completeness_cell(f, &r.truncated);
        r.note = f.note.value_or("");
        t.rows.push_back(std::move(r));

        t.n_rows++;
        if (f.available)
            t.n_available++;
        if (f.trace_insns)
            t.n_measured++;
        if (f.complete && *f.complete)
            t.n_complete++;
    }
    for (const CompletenessRow &r : t.rows)
        if (r.truncated)
            t.n_truncated++;
    return t;
}

// Fixed-width plain-text rendering of the same model — the golden-test surface,
// and what makes rule 2 checkable byte-for-byte.
inline std::string render_completeness_text(const CompletenessTable &t) {
    static const char *kHead[] = {"TIER",  "BACKEND",      "ARCH",
                                  "SCOPE", "COMPLETENESS", "STATUS"};
    // Column width is measured in DISPLAY columns, not bytes: the not-measured
    // marker is an em dash, three UTF-8 bytes wide and one column wide, and a
    // byte-counting pad would knock the whole table out of alignment on exactly
    // the rows that matter most.
    auto width = [](const std::string &s) {
        std::size_t n = 0;
        for (unsigned char c : s)
            if ((c & 0xC0) != 0x80) // not a UTF-8 continuation byte
                n++;
        return n;
    };
    std::size_t w[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; i++)
        w[i] = std::string(kHead[i]).size();
    for (const CompletenessRow &r : t.rows) {
        const std::string *cols[] = {&r.tier, &r.backend, &r.arch, &r.scope,
                                     &r.completeness};
        for (int i = 0; i < 5; i++)
            if (width(*cols[i]) > w[i])
                w[i] = width(*cols[i]);
    }
    auto pad = [&width](const std::string &s, std::size_t n) {
        std::size_t have = width(s);
        return s + std::string(n > have ? n - have : 0, ' ');
    };

    std::string out = t.box_label + "\n";
    out += std::to_string(t.n_rows) + " rows, " +
           std::to_string(t.n_available) + " available, " +
           std::to_string(t.n_measured) + " measured, " +
           std::to_string(t.n_complete) + " complete, " +
           std::to_string(t.n_truncated) + " truncated\n";
    for (int i = 0; i < 5; i++)
        out += pad(kHead[i], w[i]) + "  ";
    out += kHead[5];
    out += "\n";
    for (const CompletenessRow &r : t.rows) {
        out += pad(r.tier, w[0]) + "  " + pad(r.backend, w[1]) + "  " +
               pad(r.arch, w[2]) + "  " + pad(r.scope, w[3]) + "  " +
               pad(r.completeness, w[4]) + "  ";
        // The status column is LAST and unpadded, so a long skip_reason is
        // never clipped by a column width — the reason must survive intact.
        out += r.status;
        if (!r.note.empty())
            out += "  [" + r.note + "]";
        out += "\n";
    }
    return out;
}

} // namespace asmdesk
#endif // ASMDESK_VIEWS_COMPLETENESS_MODEL_H
