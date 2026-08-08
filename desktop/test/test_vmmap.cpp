// test_vmmap.cpp — the address-space naming overlay (vmmap design, 2026-08-08).
//
// Pure: no ImGui, no GL. The load-bearing assertion is LAYOUT INVARIANCE — the
// overlay may change what a region is CALLED and never where it sits. If that
// one fails, the overlay is mutating base/len, the floor moves under the reader
// on every weave, and the feature is worse than the "observed data (unknown)"
// it replaced.
#include <cstdio>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/projection.h"
#include "space/vmmap.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static space::VmMap fixture_map() {
    space::VmMap m;
    m.present = true;
    m.readable = true;
    m.spans_total = 4;
    m.spans = {
        {0x400000, 0x10000, "r-xp", "victim", "/tmp/victim"},
        {0x500000, 0x40000, "rw-p", "[heap]", ""},
        {0x7f0000000000, 0x200000, "r-xp", "libc.so.6", "/usr/lib/libc.so.6"},
        {0x7ffd00000000, 0x21000, "rw-p", "[stack]", ""},
    };
    return m;
}

int main() {
    using K = space::Region::Kind;

    // --- kind resolution: a pure function of (perms, name) ------------------
    check("kind/heap", space::vmmap_kind_of("rw-p", "[heap]") == K::Heap, "");
    check("kind/stack", space::vmmap_kind_of("rw-p", "[stack]") == K::Stack, "");
    check("kind/stack-tid", space::vmmap_kind_of("rw-p", "[stack:7]") == K::Stack,
          "a per-thread stack is still a stack");
    check("kind/code", space::vmmap_kind_of("r-xp", "libc.so.6") == K::Code, "");
    check("kind/data", space::vmmap_kind_of("rw-p", "libc.so.6") == K::Data,
          "a file-backed non-exec mapping is that module's data");
    check("kind/anon", space::vmmap_kind_of("rw-p", "") == K::Mmap, "");
    // An anonymous EXECUTABLE mapping is a JIT arena. It stays Mmap: calling it
    // Code would claim a module that does not exist. The perms layer marks it.
    check("kind/anon-exec-is-not-code",
          space::vmmap_kind_of("rwxp", "") == K::Mmap,
          "an anonymous rwx arena must not be promoted to Code");
    check("kind/vdso-is-not-code",
          space::vmmap_kind_of("r-xp", "[vdso]") == K::Mmap,
          "a bracket pseudo-mapping is not a module, whatever its perms");

    // --- the overlay renames, and only renames ------------------------------
    {
        std::vector<space::Region> regs;
        space::Region touched; // an observed-data span inside [heap]
        touched.base = 0x501000;
        touched.len = 0x2000;
        touched.kind = space::Region::Unknown;
        touched.label = space::kObservedDataLabel;
        regs.push_back(touched);

        const size_t n = space::vmmap_apply_names(regs, fixture_map());
        check("overlay/renamed-one", n == 1, "expected exactly one relabel");
        check("overlay/kind", regs[0].kind == K::Heap, "kind must become Heap");
        check("overlay/label", regs[0].label == "[heap]", regs[0].label);
        check("overlay/base-untouched", regs[0].base == 0x501000, "base moved");
        check("overlay/len-untouched", regs[0].len == 0x2000, "len moved");
        check("overlay/extent",
              regs[0].extent_base == 0x500000 && regs[0].extent_len == 0x40000,
              "the mapping's true extent must be carried");
        check("overlay/perms", regs[0].perms == "rw-p", regs[0].perms);
    }

    // --- boundary conditions on containment ---------------------------------
    {
        space::VmMap m = fixture_map();
        std::vector<space::Region> regs;
        // Exactly at the first byte of [heap]: inside.
        space::Region lo;
        lo.base = 0x500000;
        lo.len = 8;
        lo.kind = space::Region::Unknown;
        lo.label = space::kObservedDataLabel;
        // Exactly at the first byte PAST [heap]: outside, and NOT the next span.
        space::Region hi;
        hi.base = 0x540000;
        hi.len = 8;
        hi.kind = space::Region::Unknown;
        hi.label = space::kObservedDataLabel;
        regs.push_back(lo);
        regs.push_back(hi);
        space::vmmap_apply_names(regs, m);
        check("overlay/inclusive-low-edge", regs[0].kind == K::Heap,
              "base == span base is inside");
        check("overlay/exclusive-high-edge",
              regs[1].kind == K::Unknown &&
                  regs[1].label == space::kObservedDataLabel,
              "base == span end is OUTSIDE — a half-open span, or every gap "
              "inherits its predecessor's name");
    }

    // --- a span STRADDLING two mappings is named by neither ------------------
    // observed_data_spans page-rounds and gap-merges its clusters, so two
    // touched pages in two ADJACENT kernel mappings arrive as one span. libc's
    // rw-p data segment followed immediately by its anonymous .bss overflow is
    // exactly this shape and exists in /proc/self/maps on any glibc host.
    // Attributing the whole span to the first mapping would label anonymous
    // memory "libc.so.6" AND report "touched at least 8192 of 4096 bytes".
    {
        space::VmMap m;
        m.present = true;
        m.readable = true;
        m.spans_total = 2;
        m.spans = {
            {0x7f0000000000, 0x1000, "rw-p", "libc.so.6", "/usr/lib/libc.so.6"},
            {0x7f0000001000, 0x1000, "rw-p", "", ""},
        };
        std::vector<space::Region> regs;
        space::Region straddle;
        straddle.base = 0x7f0000000000;
        straddle.len = 0x2000; // crosses out of libc into the anon mapping
        straddle.kind = space::Region::Unknown;
        straddle.label = space::kObservedDataLabel;
        regs.push_back(straddle);
        const size_t n = space::vmmap_apply_names(regs, m);
        check("straddle/not-named", n == 0 &&
                                        regs[0].kind == space::Region::Unknown,
              "a span crossing a mapping boundary belongs to neither — naming "
              "it after the first would claim anonymous memory is libc");
        check("straddle/no-contradictory-extent", regs[0].extent_len == 0,
              "and it must carry no extent, or the readout says 'touched at "
              "least 8192 of 4096'");
    }

    // --- what it must NOT touch ---------------------------------------------
    {
        std::vector<space::Region> regs;
        space::Region code; // a codeimage region: already names its provenance
        code.base = 0x400000;
        code.len = 0x100;
        code.kind = space::Region::Code;
        code.label = "code@0x400000";
        regs.push_back(code);
        space::Region orphan; // covered by no mapping
        orphan.base = 0xdead0000;
        orphan.len = 0x1000;
        orphan.kind = space::Region::Unknown;
        orphan.label = space::kObservedDataLabel;
        regs.push_back(orphan);

        space::vmmap_apply_names(regs, fixture_map());
        check("overlay/leaves-codeimage-alone", regs[0].label == "code@0x400000",
              "a codeimage region states its own captured provenance; "
              "overwriting it would launder it into a guess from a maps row");
        check("overlay/uncovered-stays-unknown",
              regs[1].kind == K::Unknown &&
                  regs[1].label == space::kObservedDataLabel,
              "a span no mapping covers is still unknown — never guessed");
    }

    // --- an absent or unreadable map changes NOTHING ------------------------
    {
        std::vector<space::Region> regs;
        space::Region s;
        s.base = 0x501000;
        s.len = 0x2000;
        s.kind = space::Region::Unknown;
        s.label = space::kObservedDataLabel;
        regs.push_back(s);

        check("overlay/absent-map-is-a-noop",
              space::vmmap_apply_names(regs, space::VmMap{}) == 0 &&
                  regs[0].label == space::kObservedDataLabel,
              "no vmmap event -> byte-identical behaviour");

        space::VmMap unreadable = fixture_map();
        unreadable.readable = false;
        unreadable.spans.clear();
        check("overlay/unreadable-map-is-a-noop",
              space::vmmap_apply_names(regs, unreadable) == 0 &&
                  regs[0].label == space::kObservedDataLabel,
              "maps_readable:false is ABSENT MEASUREMENT, never measured zero");
    }

    // --- LAYOUT INVARIANCE: the whole safety argument, in one assertion -----
    {
        std::vector<space::Region> plain;
        space::Region r0;
        r0.base = 0x400000;
        r0.len = 0x1000;
        r0.kind = space::Region::Code;
        space::Region r1;
        r1.base = 0x501000;
        r1.len = 0x2000;
        r1.kind = space::Region::Unknown;
        r1.label = space::kObservedDataLabel;
        plain.push_back(r0);
        plain.push_back(r1);
        std::vector<space::Region> named = plain;
        const size_t n = space::vmmap_apply_names(named, fixture_map());
        check("layout/precondition", n == 1,
              "the invariance test is vacuous unless the overlay renamed "
              "something");

        space::Projection pa = space::build_projection(std::move(plain));
        space::Projection pb = space::build_projection(std::move(named));
        const space::LayoutFingerprint fa = space::layout_fingerprint(pa);
        const space::LayoutFingerprint fb = space::layout_fingerprint(pb);
        check("layout/invariant", fa.valid && fb.valid && fa.digest == fb.digest,
              "naming a region must NOT move the floor — if this fails the "
              "overlay is mutating base/len and the reader's mental map breaks");
        check("layout/reflow-note-silent", space::layout_reflow_note(fa, fb).empty(),
              "an unmoved floor must not announce a reflow");
    }

    // --- NAMING A REGION MUST NOT MOVE THE ANCHOR ---------------------------
    // resolve_anchor derives the rel->abs base by counting Region::Code spans,
    // and refuses when there are two or more. So an overlay that renames an
    // observed-data span inside an executable mapping to Code manufactures a
    // second one — and a rel-basis recording loses its anchored path entirely,
    // while the refusal blames a "codeimage code span" that is really a data
    // touch. Layout invariance cannot catch this: the fingerprint never mixes
    // `kind`.
    {
        std::vector<space::Region> regs;
        space::Region code; // the ONE real codeimage span
        code.base = 0x400000;
        code.len = 0x1000;
        code.kind = space::Region::Code;
        code.label = "code@0x400000";
        regs.push_back(code);
        space::Region touched; // an observed touch inside libc's r-xp mapping
        touched.base = 0x7f0000000000;
        touched.len = 0x1000;
        touched.kind = space::Region::Unknown;
        touched.label = space::kObservedDataLabel;
        regs.push_back(touched);

        const space::Anchor before = space::resolve_anchor(regs);
        check("anchor/precondition", before.ok && before.base == 0x400000,
              "the fixture must anchor before the overlay runs");
        const size_t n = space::vmmap_apply_names(regs, fixture_map());
        check("anchor/precondition-renamed", n == 1 &&
                                                 regs[1].kind ==
                                                     space::Region::Code,
              "the overlay must actually have named it Code, or this is "
              "vacuous");
        const space::Anchor after = space::resolve_anchor(regs);
        check("anchor/survives-naming", after.ok && after.base == 0x400000,
              "naming a data touch inside an executable mapping must NOT "
              "create a second anchor candidate: " + after.reason);
    }

    // --- the wire shape, end to end, from a hand-authored fixture -----------
    {
        std::string err;
        auto rec = load_recording_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/vmmap-named.asmtrace", err);
        check("fixture/loads", rec.has_value(), err);
        if (rec) {
            const space::VmMap m = space::vmmap_from_recording(*rec);
            check("fixture/present", m.present && m.readable, "");
            check("fixture/spans", m.spans.size() == 3,
                  std::to_string(m.spans.size()));
            check("fixture/hex-base-parsed",
                  m.spans.size() == 3 && m.spans[1].base == 0x500000,
                  "base is a hex STRING on the wire and must parse as one");
            check("fixture/len-is-a-number",
                  m.spans.size() == 3 && m.spans[1].len == 262144, "");
            check("fixture/total", m.spans_total == 3, "");
            check("fixture/not-truncated", !m.truncated, "");
        }
        // A recording with no vmmap at all: present stays false, and that is
        // distinct from a vmmap that measured nothing.
        auto plain = load_recording_file(
            std::string(ASMTEST_FIXTURE_DIR) + "/blame-attribution.asmtrace", err);
        if (plain)
            check("fixture/absent-is-not-present",
                  !space::vmmap_from_recording(*plain).present,
                  "no vmmap event must not look like an empty measurement");
    }

    if (failures) {
        std::fprintf(stderr, "test_vmmap: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_vmmap: all checks passed\n");
    return 0;
}
