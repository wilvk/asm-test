// honesty.h — the ONE graded honesty vocabulary, decided purely
// (docs/internal/gui/23-graded-truth-layer.md T1, F5).
//
// Before this, honesty chrome PROLIFERATED: a redaction placard, a statistical
// chip, a coarse-provenance chip, a bounded-window note, an identity note and a
// torn banner all rendered EQUALLY LOUD and non-collapsible, even though the
// schema grades them very differently — so the one signal that means "do not
// trust the tail of this data" (torn) drowned among four that mean "this is
// normal" (redacted / statistical / skip / bounded). This header is the fix: it
// grades every honesty signal into THREE tiers, so loudness follows the schema's
// own severity gradient. It RESTRUCTURES, it removes NO truth (D7) — every field
// still renders; the tier only decides how loud.
//
// The three tiers (schema severity gradient):
//   - NEUTRAL   : skip=success · coarse-provenance rung · bounded window ·
//                 redacted-by-policy · statistical (trust:"statistical").
//                 A quiet chip, no warn colour. These are NORMAL.
//   - CAUTION   : truncated-but-usable (truncated:true with a usable prefix) ·
//                 paused_dropped. An amber banner, collapsible to a chip AFTER
//                 first read (never gone).
//   - INTEGRITY : torn (no `end`) · mixed-basis refusal · a DROP on an exact
//                 capture (dropped-prefix -> UNKNOWN). A red banner, loud, and
//                 NON-collapsible.
//
// Pure + header-only + ImGui-free (like progress.h), so honesty_severity() is
// unit-tested against the committed dishonesty fixtures with no context (D4) and
// the fixtures pin the grading. The `severity` schema field is DERIVABLE from the
// existing honesty fields (see honesty_facts_of): a producer need not emit it,
// old recordings still grade, and when it IS present the reader honours it.
#ifndef ASMDESK_UI_HONESTY_H
#define ASMDESK_UI_HONESTY_H

#include <optional>
#include <string>

#include "doc/recording.h"

namespace asmdesk {

// The three graded tiers, ordered by loudness so tier arithmetic (max) is just
// integer comparison — a recording's DOMINANT tier is the loudest active signal.
enum class HonestyTier { Neutral = 0, Caution = 1, Integrity = 2 };

// The wire spelling of the derivable `severity` field (asmtrace-schema.md). This
// is the ONE table mapping tier <-> string, so the schema, the parser and the
// renderer cannot drift.
inline const char *honesty_tier_name(HonestyTier t) {
    switch (t) {
    case HonestyTier::Neutral:
        return "neutral";
    case HonestyTier::Caution:
        return "caution";
    case HonestyTier::Integrity:
        return "integrity";
    }
    return "neutral";
}

// Collapsibility (T1 step 5): only a CAUTION banner may collapse to its chip
// after first read — its text is unchanged, so it is never gone (D7). An INTEGRITY
// banner NEVER collapses (the refusal path was always non-dismissable); a NEUTRAL
// signal is already a minimal chip with nothing to collapse.
inline bool honesty_tier_collapsible(HonestyTier t) {
    return t == HonestyTier::Caution;
}

// The NON-colour second channel (24 T5.2's rule): a short word the tier reads by,
// so the grade never rides on colour alone. Paired with a per-tier glyph in the
// draw half (canvas_draw.cpp).
inline const char *honesty_tier_token(HonestyTier t) {
    switch (t) {
    case HonestyTier::Neutral:
        return "note";
    case HonestyTier::Caution:
        return "caution";
    case HonestyTier::Integrity:
        return "integrity";
    }
    return "note";
}

// Parse the optional wire `severity` string. Unknown / empty -> nullopt (a reader
// then derives the tier from the honesty facts, so an old or partial recording
// still grades). Never guesses a tier from a malformed value.
inline std::optional<HonestyTier> honesty_tier_from_wire(const std::string &s) {
    if (s == "neutral")
        return HonestyTier::Neutral;
    if (s == "caution")
        return HonestyTier::Caution;
    if (s == "integrity")
        return HonestyTier::Integrity;
    return std::nullopt;
}

// The derivable honesty facts of a recording / stream. Every field is a fact the
// schema already carries; the tier is DERIVED from them, never a truth of its
// own. A caller that has a fact the Recording model does not expose (a mixed
// basis from the canvas, a coarse rung, a bounded window) sets it here directly.
struct HonestyFacts {
    bool torn = false;          // no `end` footer -> a torn recording (integrity)
    bool truncated = false;     // end.truncated with a usable prefix (caution)
    bool statistical = false;   // trust:"statistical" / !exact (neutral)
    bool dropped = false;       // drops.lost>0 || throttled
    bool redacted = false;      // payload withheld at record time (neutral)
    bool has_skip = false;      // a successful skip (neutral — schema:98)
    bool basis_error = false;   // mixed-basis refusal (integrity)
    bool paused_dropped = false;// events dropped while paused (caution)
    bool bounded_window = false;// capture scoped to a region (neutral)
    bool coarse = false;        // coarse-provenance rung (neutral)

    // The parsed wire `severity`, when the producer emitted one. Present ->
    // honesty_severity honours it verbatim; absent -> the tier is derived. This
    // is what makes the field additive and derivable (D5 / schema append-only).
    std::optional<HonestyTier> declared;
};

// The DOMINANT tier for a recording's chrome: the loudest active signal, or the
// declared wire tier when present. A drop on an EXACT capture is integrity (its
// addresses become UNKNOWN); a drop on a STATISTICAL survey is neutral (sampling
// drops are expected and the survey never claimed completeness), so `dropped`
// only raises the tier when the capture was exact.
inline HonestyTier honesty_severity(const HonestyFacts &f) {
    if (f.declared)
        return *f.declared;
    if (f.torn || f.basis_error || (f.dropped && !f.statistical))
        return HonestyTier::Integrity;
    if (f.truncated || f.paused_dropped)
        return HonestyTier::Caution;
    return HonestyTier::Neutral;
}

// The tier of ONE signal in isolation, so a view can render each honesty field at
// its own tier (a statistical survey that also dropped shows a neutral chip for
// the survey and — because it is statistical — still a neutral drop record, never
// hidden: D7). `dropped_on_exact` distinguishes the integrity drop from the
// expected statistical one.
enum class HonestySignal {
    Torn,
    Truncated,
    Statistical,
    DroppedExact,
    DroppedStatistical,
    Redacted,
    Skip,
    BasisError,
    PausedDropped,
    BoundedWindow,
    Coarse,
};
inline HonestyTier honesty_signal_tier(HonestySignal s) {
    switch (s) {
    case HonestySignal::Torn:
    case HonestySignal::BasisError:
    case HonestySignal::DroppedExact:
        return HonestyTier::Integrity;
    case HonestySignal::Truncated:
    case HonestySignal::PausedDropped:
        return HonestyTier::Caution;
    default:
        return HonestyTier::Neutral;
    }
}

// Derive the honesty facts of a loaded recording. Reads only the fields the
// Recording model already lifts off the header / `end` footer — plus, when the
// producer emitted one, the optional `severity` string off the provenance object
// (provenance.raw), which then OVERRIDES the derivation. Additive: a recording
// with no `severity` field grades exactly as before.
inline HonestyFacts honesty_facts_of(const Recording &r) {
    HonestyFacts f;
    f.torn = r.torn;
    f.truncated = r.end_truncated && !r.torn; // torn is louder; do not double-count
    f.statistical = r.statistical();
    f.dropped = r.dropped();
    f.redacted = r.provenance.redacted;
    f.bounded_window =
        r.provenance.raw.is_object() && r.provenance.raw.contains("window");
    if (r.provenance.raw.is_object() && r.provenance.raw.contains("severity") &&
        r.provenance.raw["severity"].is_string())
        f.declared =
            honesty_tier_from_wire(r.provenance.raw["severity"].get<std::string>());
    return f;
}

} // namespace asmdesk
#endif // ASMDESK_UI_HONESTY_H
