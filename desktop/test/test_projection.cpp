// test_projection.cpp — the address-space Hilbert projection
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T1). Null harness, no display:
// this binary links space/projection.o and NOTHING else, so its link line is the
// proof that the render-only viewer can place a recording's regions on the
// terrain plane with zero engine dependencies (D4) — the same argument
// test_slice.cpp makes for client-side slicing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "space/projection.h"

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}

static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// Build a Recording from an in-memory NDJSON string, through the real loader —
// the same pattern test_terrain.cpp's mk_rec uses, so `mem`/`df_step` events
// decode exactly as a producer's would.
static Recording mk_rec(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

static std::string mem_event(uint64_t ea, uint64_t size = 1,
                             const char *rw = "r") {
    char buf[128];
    std::snprintf(buf, sizeof buf,
                  "{\"k\":\"mem\",\"step\":0,\"ea\":%llu,\"size\":%llu,"
                  "\"rw\":\"%s\"}\n",
                  (unsigned long long)ea, (unsigned long long)size, rw);
    return buf;
}

// The three reference regions, deliberately sparse (a gap of gigabytes between
// each base) so compaction has real work to do — yet a small total so the plane
// maps 1:1 and the round trip is exact. Passed to the builder OUT of base order.
struct Ref {
    uint64_t base, len;
    Region::Kind kind;
};
static const Ref kCode = {0x0000000000400000ull, 0x20000, Region::Code}; // 128K
static const Ref kHeap = {0x0000000001000000ull, 0x08000, Region::Heap}; // 32K
static const Ref kStack = {0x00007fff00000000ull, 0x10000,
                           Region::Stack};          // 64K
static const Ref kRefs[3] = {kStack, kCode, kHeap}; // unsorted

// 61 T2: an ATLAS-layout projection over the given regions. Mirrors main()'s
// existing build, then switches layout — build_projection() itself stays
// layout-neutral.
static Projection atlas_of(const std::vector<Ref> &refs) {
    std::vector<Region> in;
    for (const Ref &r : refs) {
        Region reg;
        reg.base = r.base;
        reg.len = r.len;
        reg.kind = r.kind;
        in.push_back(reg);
    }
    Projection p = build_projection(std::move(in));
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    return p;
}

// The (x,y) cell a projected (u,v) lands in, for locality/neighbour arithmetic.
static void cell(const Projection &p, float u, float v, int64_t *x,
                 int64_t *y) {
    uint32_t n = 1u << p.order;
    *x = (int64_t)(u * n);
    *y = (int64_t)(v * n);
}

int main() {
    std::vector<Region> in;
    uint64_t total = 0;
    for (const Ref &r : kRefs) {
        Region reg;
        reg.base = r.base;
        reg.len = r.len;
        reg.kind = r.kind;
        in.push_back(reg);
        total += r.len;
    }
    Projection p = build_projection(std::move(in));

    // --- compaction is exact and sorted -------------------------------------
    // Sorted by base, so the internal order is code, heap, stack regardless of
    // the input order; the domain packs them contiguously with the gaps gone.
    check("three regions survive the build", p.regions.size() == 3,
          "got " + std::to_string(p.regions.size()));
    check("regions come out sorted by base",
          p.regions.size() == 3 && p.regions[0].base < p.regions[1].base &&
              p.regions[1].base < p.regions[2].base,
          "not ascending");
    const std::vector<uint64_t> want_off = {0, kCode.len, kCode.len + kHeap.len,
                                            kCode.len + kHeap.len + kStack.len};
    check("domain_off is the contiguous prefix sum (gaps compacted away)",
          p.domain_off == want_off, "compaction table differs");
    // order 9: 4^8 = 65536 < 229376 <= 4^9 = 262144, a 512x512 plane.
    check("order is ceil(log4(sum len)) clamped to [6,12]", p.order == 9,
          "got order " + std::to_string(p.order));

    // --- round trip: project then unproject is exact for 10k addresses -------
    // 61 T2: this loop asserts the BYTE-EXACT round trip, which is a
    // Hilbert-layout promise and not an atlas one (an atlas cell covers
    // bytes_per_cell bytes and unproject returns the first of them). Pinned to
    // Hilbert EXPLICITLY rather than relying on the struct default, because
    // 61 T10 changes that default to Atlas. It happens to survive the flip
    // unpinned — this fixture's plane exceeds its domain, so every region gets
    // bytes_per_cell == 1 — but that is an accident of three fixture lengths,
    // not a property, and a later fixture edit would turn it into a mystery
    // failure in an unrelated-looking test. The atlas's own contract, the
    // REGION-level round trip, is asserted in the atlas blocks below.
    Projection h = p;
    h.layout = Projection::Layout::Hilbert;
    rebuild_layout(h);
    std::mt19937_64 rng(0xA5A5A5A5u);
    for (int i = 0; i < 10000; i++) {
        const Ref &r = kRefs[rng() % 3];
        uint64_t addr = r.base + (rng() % r.len);
        float u, v;
        if (!h.project(addr, &u, &v)) {
            fail("round trip", "project failed for a mapped address");
            continue;
        }
        uint64_t back = 0;
        const Region *reg = nullptr;
        if (!h.unproject(u, v, &back, &reg)) {
            fail("round trip", "unproject failed for a projected cell");
            continue;
        }
        if (back != addr)
            fail("round trip", "address " + std::to_string(addr) +
                                   " came back as " + std::to_string(back));
        if (!reg || reg->base != r.base)
            fail("round trip", "unproject landed in the wrong region");
    }

    // A region's first and last byte both project and round-trip (the boundary
    // is where an off-by-one in the compaction offsets would show).
    for (const Ref &r : kRefs) {
        for (uint64_t addr : {r.base, r.base + r.len - 1}) {
            float u, v;
            uint64_t back = 0;
            const Region *reg = nullptr;
            check("boundary byte projects",
                  p.project(addr, &u, &v) && p.unproject(u, v, &back, &reg) &&
                      back == addr,
                  "addr " + std::to_string(addr) + " did not round-trip");
        }
    }

    // --- locality: two addresses 1 byte apart are the same or 4-neighbours ---
    for (int i = 0; i < 10000; i++) {
        const Ref &r = kRefs[rng() % 3];
        uint64_t off = rng() % (r.len - 1); // so addr+1 stays inside the region
        uint64_t a = r.base + off;
        float ua, va, ub, vb;
        if (!p.project(a, &ua, &va) || !p.project(a + 1, &ub, &vb)) {
            fail("locality", "a mapped in-region pair failed to project");
            continue;
        }
        int64_t ax, ay, bx, by;
        cell(p, ua, va, &ax, &ay);
        cell(p, ub, vb, &bx, &by);
        int64_t manhattan = std::llabs(ax - bx) + std::llabs(ay - by);
        if (manhattan > 1)
            fail("locality", "neighbouring bytes landed " +
                                 std::to_string(manhattan) + " cells apart");
    }

    // --- an unmapped address returns false ----------------------------------
    const uint64_t unmapped[] = {
        0x0ull,                   // below every region
        kCode.base - 1,           // just below the lowest region
        kCode.base + kCode.len,   // first byte past a region
        0x0000000000800000ull,    // in the code<->heap gap
        kHeap.base + kHeap.len,   // just past the heap
        0x0000000002000000ull,    // in the heap<->stack gap
        kStack.base + kStack.len, // just past the stack
        0x00007fffffffffffull,    // high, above every region
        0xffffffffffffffffull,    // the very top
    };
    for (uint64_t addr : unmapped) {
        float u, v;
        check("unmapped address is refused", !p.project(addr, &u, &v),
              "addr " + std::to_string(addr) + " projected but is unmapped");
    }

    // --- unproject rejects out-of-plane coordinates and padding cells --------
    {
        float junk_u = 0.5f, junk_v = 0.5f;
        uint64_t a = 0;
        const Region *reg = nullptr;
        check("u >= 1 is refused", !p.unproject(1.0f, junk_v, &a, &reg),
              "took it");
        check("v >= 1 is refused", !p.unproject(junk_u, 1.0f, &a, &reg),
              "took it");
        check("u < 0 is refused", !p.unproject(-0.01f, junk_v, &a, &reg),
              "took it");
        check("v < 0 is refused", !p.unproject(junk_u, -0.01f, &a, &reg),
              "took it");
    }

    // Every plane cell either holds exactly one domain byte or is padding: the
    // Hilbert map is a bijection over [0, n*n), so the number of cells unproject
    // accepts must equal the compacted byte count — no cell double-mapped, every
    // padding cell (n*n - total of them) refused.
    {
        uint32_t n = 1u << p.order;
        uint64_t accepted = 0;
        for (uint32_t y = 0; y < n; y++) {
            for (uint32_t x = 0; x < n; x++) {
                float u = (x + 0.5f) / (float)n;
                float v = (y + 0.5f) / (float)n;
                uint64_t a = 0;
                const Region *reg = nullptr;
                if (p.unproject(u, v, &a, &reg))
                    accepted++;
            }
        }
        check("exactly the compacted bytes are mapped, the rest is padding",
              accepted == total,
              "accepted " + std::to_string(accepted) + " cells, want " +
                  std::to_string(total));
    }

    // --- an empty projection is well-behaved, not a crash -------------------
    {
        Projection empty = build_projection({});
        float u, v;
        uint64_t a = 0;
        const Region *reg = nullptr;
        check("empty projection maps nothing", !empty.project(0x1000, &u, &v),
              "an empty projection projected something");
        check("empty projection unmaps nothing",
              !empty.unproject(0.5f, 0.5f, &a, &reg),
              "it unprojected something");
    }

    // --- region styles are total and named (the HUD legend) -----------------
    {
        const Region::Kind kinds[] = {Region::Code, Region::Stack,
                                      Region::Heap, Region::Data,
                                      Region::Mmap, Region::Unknown};
        for (Region::Kind k : kinds) {
            RegionStyle s = region_style(k);
            check("every kind has a legend name", s.name && *s.name,
                  "empty name");
            check("legend colour is in [0,1]",
                  s.r >= 0 && s.r <= 1 && s.g >= 0 && s.g <= 1 && s.b >= 0 &&
                      s.b <= 1,
                  "colour out of range");
        }
    }

    // --- 36 T1: the rel->abs anchor -----------------------------------------
    {
        auto code = [](uint64_t base, uint64_t len) {
            Region r;
            r.base = base;
            r.len = len;
            r.kind = Region::Code;
            return r;
        };
        auto data = [](uint64_t base, uint64_t len) {
            Region r;
            r.base = base;
            r.len = len;
            r.kind = Region::Data;
            return r;
        };

        // exactly one code span anchors, base/len from it, and places an offset.
        {
            Anchor a = resolve_anchor({code(0x400000, 0x1000)});
            check("one code span anchors", a.ok && a.reason.empty(),
                  "did not anchor a single code span");
            check("anchor base/len come from the span",
                  a.base == 0x400000 && a.len == 0x1000, "base/len wrong");
            uint64_t abs = 0;
            check("place maps off onto base+off",
                  a.place(0x10, &abs) && abs == 0x400010, "place wrong");
        }

        // a non-code region does not make a single code span ambiguous.
        {
            Anchor a =
                resolve_anchor({data(0x0, 0x1000), code(0x400000, 0x1000),
                                data(0x800000, 0x2000)});
            check("non-code regions do not spoil a single-code anchor",
                  a.ok && a.base == 0x400000, "a data region broke the anchor");
        }

        // zero code spans refuses with a stated reason and never places.
        {
            Anchor a = resolve_anchor({data(0x0, 0x1000)});
            check("zero code spans refuses", !a.ok && !a.reason.empty(),
                  "did not refuse with a reason");
            uint64_t abs = 0;
            check("an unanchored anchor never places", !a.place(0x10, &abs),
                  "placed against a refused anchor");
        }

        // two code spans refuse, and the reason names BOTH bases. Reverting the
        // >=2 branch to "pick the first" makes a.ok true and fails this.
        {
            Anchor a = resolve_anchor(
                {code(0x400000, 0x1000), code(0x800000, 0x2000)});
            check("two code spans refuse (not pick-the-first)", !a.ok,
                  "anchored an ambiguous two-span recording");
            check("the two-span reason names both hex bases",
                  a.reason.find("0x400000") != std::string::npos &&
                      a.reason.find("0x800000") != std::string::npos,
                  "reason did not name both bases: " + a.reason);
        }

        // the clamp boundary: place(len) is false; place(len-1) equals base+len-1.
        {
            Anchor a = resolve_anchor({code(0x400000, 0x1000)});
            uint64_t abs = 0;
            check("place(len) is refused (the 4096-byte clamp)",
                  !a.place(0x1000, &abs), "placed an out-of-span offset");
            check("place(len-1) is base+len-1",
                  a.place(0x0FFF, &abs) && abs == 0x400FFF, "boundary wrong");
        }
    }

    // --- 54 T1: observed_data_spans -----------------------------------------
    {
        // No mem events, no abs df values: empty output, empty note.
        {
            Recording rec = mk_rec(
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
                "{\"k\":\"end\",\"events\":0,\"truncated\":false,"
                "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
            std::string note = "sentinel";
            std::vector<Region> obs = observed_data_spans(rec, {}, &note);
            check("no observed addresses yields no spans", obs.empty(),
                  "got " + std::to_string(obs.size()));
            check("the note is cleared, not left stale", note.empty(),
                  "got: " + note);
        }

        // Two addresses one page apart share a span; two far apart do not.
        {
            const uint64_t A = 0x10000000ull;
            const uint64_t B = A + kObservedSpanGap;       // exactly one gap
            const uint64_t C = A + 2ull * 1000ull * 1000ull; // ~2MB away
            std::string body =
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
            body += mem_event(A);
            body += mem_event(B);
            body += mem_event(C);
            body += "{\"k\":\"end\",\"events\":3,\"truncated\":false,"
                    "\"drops\":{\"lost\":0,\"throttled\":false}}\n";
            Recording rec = mk_rec(body);
            std::string note;
            std::vector<Region> obs = observed_data_spans(rec, {}, &note);
            check("a one-page gap merges into the same span",
                  obs.size() == 2,
                  "got " + std::to_string(obs.size()) + " spans");
            if (obs.size() == 2) {
                check("the first span covers both A and B",
                      obs[0].base <= A && A - obs[0].base < obs[0].len &&
                          obs[0].base <= B && B - obs[0].base < obs[0].len,
                      "A/B did not share the first span");
                check("the second span is the far address C",
                      obs[1].base <= C && C - obs[1].base < obs[1].len,
                      "C did not land in the second span");
                check("spans are non-overlapping and ascending",
                      obs[0].base + obs[0].len <= obs[1].base,
                      "spans overlap or are out of order");
            }
            check("every span is labelled 'observed data', kind Unknown",
                  obs.size() == 2 &&
                      obs[0].kind == Region::Unknown &&
                      obs[0].label == "observed data" &&
                      obs[1].kind == Region::Unknown &&
                      obs[1].label == "observed data",
                  "an observed span must never claim allocation structure");
            check("the note is non-empty once spans exist", !note.empty(),
                  "note must explain the derivation");

            // Deterministic / byte-stable: calling again gives the same result.
            std::string note2;
            std::vector<Region> obs2 = observed_data_spans(rec, {}, &note2);
            check("observed_data_spans is deterministic",
                  obs.size() == obs2.size() &&
                      std::equal(obs.begin(), obs.end(), obs2.begin(),
                                 [](const Region &a, const Region &b) {
                                     return a.base == b.base && a.len == b.len;
                                 }) &&
                      note == note2,
                  "two calls over the same input disagreed");
        }

        // A span never overlaps an `existing` region — clipped around it.
        {
            const uint64_t A = 0x20000000ull;
            std::string body =
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
            body += mem_event(A);              // inside the existing region
            body += mem_event(A + 0x10000);     // outside it, same cluster? far enough to split
            body += "{\"k\":\"end\",\"events\":2,\"truncated\":false,"
                    "\"drops\":{\"lost\":0,\"throttled\":false}}\n";
            Recording rec = mk_rec(body);

            Region existing;
            existing.base = 0x1FFFF000ull; // covers a page below and above A
            existing.len = 0x3000ull;      // [0x1FFFF000, 0x20002000)
            existing.kind = Region::Data;
            existing.label = "heap";

            std::string note;
            std::vector<Region> obs =
                observed_data_spans(rec, {existing}, &note);
            for (const Region &r : obs)
                check("no observed span overlaps the existing region",
                      r.base + r.len <= existing.base ||
                          r.base >= existing.base + existing.len,
                      "span [" + std::to_string(r.base) + "," +
                          std::to_string(r.base + r.len) +
                          ") overlaps existing [" +
                          std::to_string(existing.base) + "," +
                          std::to_string(existing.base + existing.len) + ")");
            // A itself is inside `existing`; whatever remains must not cover it.
            for (const Region &r : obs)
                check("the address fully inside `existing` created no span "
                      "covering it",
                      !(r.base <= A && A - r.base < r.len),
                      "an address inside a known region created a shadow span");
        }

        // The cap merges the nearest neighbours rather than dropping spans.
        {
            std::string body =
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
            const size_t kCount = kObservedSpanCap + 8;
            const uint64_t kStride = 1ull << 20; // 1 MiB — far past the gap
            for (size_t i = 0; i < kCount; ++i)
                body += mem_event(0x100000000ull + i * kStride);
            body += "{\"k\":\"end\",\"events\":" + std::to_string(kCount) +
                    ",\"truncated\":false,\"drops\":{\"lost\":0,"
                    "\"throttled\":false}}\n";
            Recording rec = mk_rec(body);

            std::string note;
            std::vector<Region> obs = observed_data_spans(rec, {}, &note);
            check("the cap is respected, not exceeded",
                  obs.size() == kObservedSpanCap,
                  "got " + std::to_string(obs.size()) + " want " +
                      std::to_string(kObservedSpanCap));
            check("the note says the cap merged spans",
                  note.find("cap merged") != std::string::npos,
                  "note did not mention the cap: " + note);
            check("spans stay non-overlapping and ascending after the cap merge",
                  [&] {
                      for (size_t i = 0; i + 1 < obs.size(); ++i)
                          if (obs[i].base + obs[i].len > obs[i + 1].base)
                              return false;
                      return true;
                  }(),
                  "cap-merged spans overlap or are out of order");
        }

        // DataflowStream `abs` values place; `off` values are skipped (counted
        // in the note), never placed raw.
        {
            std::string body =
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
                "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,"
                "\"version\":0,\"when\":1,\"bytes\":\"90\"}\n"
                "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"ops\":[{"
                "\"space\":\"abs\",\"addr\":268435456,\"size\":8,"
                "\"write\":true}]}\n"
                "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"ops\":[{"
                "\"space\":\"off\",\"addr\":16,\"size\":8,\"write\":false}]}\n"
                "{\"k\":\"end\",\"events\":3,\"truncated\":false,"
                "\"drops\":{\"lost\":0,\"throttled\":false}}\n";
            Recording rec = mk_rec(body);
            std::string note;
            std::vector<Region> obs = observed_data_spans(rec, {}, &note);
            check("an abs df value places a span",
                  obs.size() == 1 && obs[0].base <= 268435456ull &&
                      268435456ull - obs[0].base < obs[0].len,
                  "the abs ValRec address did not place");
            check("the note counts the skipped off-space value",
                  note.find("1 dataflow-off skipped") != std::string::npos,
                  "note did not count the skipped off value: " + note);
        }
    }

    // --- 61 T2: the atlas layout -------------------------------------------
    // Every cell claimed, decodable, locally contiguous.
    {
        // 4096 : 61440 == 1 : 15, so the code rect gets 1/16 of the cell budget.
        static const Ref kSmall = {0x0000000000400000ull, 4096, Region::Code};
        static const Ref kBig = {0x0000000001000000ull, 61440, Region::Mmap};
        const Projection ap = atlas_of({kSmall, kBig});
        const uint32_t n = 1u << ap.order;

        // The whole point: no power-of-4 padding, so no empty three-quarters. In
        // cells, not area — the grid is what Terrain and every (u,v)-keyed layer
        // are indexed by, so cells are the unit the claim has to be true in.
        uint64_t claimed = 0;
        for (const AtlasRect &r : ap.rects)
            claimed += uint64_t(r.x1 - r.x0) * uint64_t(r.y1 - r.y0);
        check("the atlas claims every cell of the grid",
              claimed == uint64_t(n) * uint64_t(n),
              "claimed " + std::to_string(claimed) + " of " +
                  std::to_string(uint64_t(n) * uint64_t(n)) + " cells");

        // Disjoint as well as covering: a cell owned twice would make unproject
        // ambiguous and quietly hand a caller the wrong region.
        std::vector<uint8_t> seen(size_t(n) * n, 0);
        bool overlap = false;
        for (const AtlasRect &r : ap.rects)
            for (uint32_t y = r.y0; y < r.y1; y++)
                for (uint32_t x = r.x0; x < r.x1; x++)
                    if (seen[size_t(y) * n + x]++)
                        overlap = true;
        check("no two atlas rects share a cell", !overlap,
              "a cell was claimed by more than one region");

        // regions come out sorted by base, so index 0 is the code region.
        const AtlasRect &code = ap.rects[0];
        const double code_frac = double(code.x1 - code.x0) *
                                 double(code.y1 - code.y0) / (double(n) * n);
        check("rect area is proportional to Region::len",
              std::fabs(code_frac - 1.0 / 16.0) < 0.02,
              "code rect covered " + std::to_string(code_frac) +
                  ", wanted 0.0625");

        // The REGION round trip is the atlas's contract — cell quantisation
        // means the exact byte need not survive, but the region must, because
        // that is what makes the floor decodable.
        for (const Region &rg : ap.regions) {
            float u = 0, v = 0;
            check("a region base projects under the atlas",
                  ap.project(rg.base, &u, &v),
                  "project refused a base inside the domain");
            uint64_t back = 0;
            const Region *got = nullptr;
            check("a projected cell unprojects", ap.unproject(u, v, &back, &got),
                  "unproject refused a cell the atlas had just placed");
            check("the round trip lands in the same region",
                  got != nullptr && got->base == rg.base,
                  "a region base round-tripped into a different region");
        }

        // region_cells() walks the layout's own mapping, so it needs an atlas
        // branch. Under the atlas it returns the region's RECT — every cell the
        // region OWNS, a superset of the cells its addresses reach (the rect's
        // rounding tail is owned but does not decode). Ownership is what zoning
        // and labelling want, and it keeps test_focus's containment and
        // disjointness contracts true under both layouts.
        for (size_t i = 0; i < ap.regions.size(); i++) {
            const std::vector<uint32_t> cells = region_cells(ap, i);
            const AtlasRect &r = ap.rects[i];
            check("region_cells matches the region's rect under the atlas",
                  cells.size() == size_t(r.x1 - r.x0) * size_t(r.y1 - r.y0),
                  "region " + std::to_string(i) + " reported " +
                      std::to_string(cells.size()) + " cells for a " +
                      std::to_string(r.x1 - r.x0) + "x" +
                      std::to_string(r.y1 - r.y0) + " rect");
        }
    }
    {
        // Serpentine order within a region: neighbouring CELLS stay neighbours
        // ACROSS THE ROW BREAK. That break is the whole content of the claim —
        // plain row-major already keeps neighbours adjacent WITHIN a row.
        static const Ref kOne = {0x0000000000400000ull, 65536, Region::Code};
        const Projection ap = atlas_of({kOne});
        const uint32_t n = 1u << ap.order;
        const float cellw = 1.0f / static_cast<float>(n);
        // 65536 bytes over a 256x256 plane is exactly one byte per cell, so a
        // byte offset IS a cell ordinal here. STATED, not assumed: every other
        // fixture quantises, and the next block is the one that pins that.
        check("the serpentine fixture is 1 byte per cell",
              uint64_t(n) * n == 65536ull && ap.rects.size() == 1,
              "the fixture no longer maps one byte to one cell; the offsets "
              "below would stop being cell ordinals");
        const uint32_t w = ap.rects[0].x1 - ap.rects[0].x0;
        auto cells_apart = [&](uint64_t off_a, uint64_t off_b) {
            float ua = 0, va = 0, ub = 0, vb = 0;
            if (!ap.project(0x400000ull + off_a, &ua, &va) ||
                !ap.project(0x400000ull + off_b, &ub, &vb))
                return 1e9f;
            return std::hypot(ub - ua, vb - va) / cellw;
        };
        check("consecutive cells within a row are adjacent",
              cells_apart(0, 1) < 1.5f,
              "cells 0 and 1 landed " + std::to_string(cells_apart(0, 1)) +
                  " cells apart");
        // The discriminating case: under row-major these are w-1 cells apart.
        check("the row break stays adjacent — the serpentine reverses odd rows",
              cells_apart(w - 1, w) < 1.5f,
              "the last cell of row 0 and the first of row 1 landed " +
                  std::to_string(cells_apart(w - 1, w)) +
                  " cells apart; row-major would give " + std::to_string(w - 1));
    }
    {
        // The per-region byte->cell quantisation. A domain larger than the
        // order-12 ceiling (4^12 == 16777216 cells) forces bytes_per_cell > 1.
        static const Ref kBigOne = {0x0000000010000000ull, 64ull << 20,
                                    Region::Code};
        const Projection ap = atlas_of({kBigOne});
        check("a domain past the cell ceiling pins order at 12", ap.order == 12,
              "got order " + std::to_string(ap.order));
        // 64 MiB over 4^12 cells is 4 bytes per cell.
        float u0 = 0, v0 = 0, u3 = 0, v3 = 0, u4 = 0, v4 = 0;
        const uint64_t b = 0x10000000ull;
        check("offset 0 projects", ap.project(b, &u0, &v0), "refused");
        check("offset 3 projects", ap.project(b + 3, &u3, &v3), "refused");
        check("offset 4 projects", ap.project(b + 4, &u4, &v4), "refused");
        check("bytes inside one cell share that cell", u0 == u3 && v0 == v3,
              "offsets 0 and 3 should quantise to the same cell at 4 bytes/cell");
        check("the next cell's worth of bytes moves on", u0 != u4 || v0 != v4,
              "offset 4 should have crossed into the next cell");
        // And the region contract still holds where the byte-exact one cannot.
        // Every element spelled uint64_t: (64ull << 20) - 1 is unsigned long
        // long, uint64_t is unsigned long here, and a mixed initializer_list
        // cannot deduce.
        for (uint64_t off : {uint64_t(0), uint64_t(3), uint64_t(4),
                             uint64_t((64ull << 20) - 1)}) {
            float u = 0, v = 0;
            if (!ap.project(b + off, &u, &v)) {
                fail("quantised project", "refused an in-domain offset");
                continue;
            }
            uint64_t back = 0;
            const Region *got = nullptr;
            check("a quantised cell still decodes to its own region",
                  ap.unproject(u, v, &back, &got) && got != nullptr &&
                      got->base == b,
                  "offset " + std::to_string(off) + " left its region");
        }
    }
    {
        // A SATURATED plane: as many regions as the smallest plane has cells.
        // Unreachable from a real /proc/maps (every mapping is at least a page,
        // so `order` outruns the region count long before this), but a synthetic
        // Projection is exactly what this directory builds, so the guard is
        // tested rather than assumed.
        std::vector<Ref> many;
        for (uint64_t i = 0; i < 4000; i++)
            many.push_back({0x1000ull + i * 0x10000ull, 1, Region::Code});
        const Projection sat = atlas_of(many);
        check("a saturated plane still produces an atlas", sat.rects.size() == 4000,
              "rebuild_layout fell back, or built a partial rects vector");
        bool degenerate = false;
        for (const AtlasRect &r : sat.rects)
            if (r.x1 <= r.x0 || r.y1 <= r.y0)
                degenerate = true;
        check("no rect is empty or inverted at saturation", !degenerate,
              "a zero-area rect divides by zero in atlas_cell, and an inverted "
              "one wraps r.x1 - r.x0 to a huge unsigned width");
        // And the layout is still a layout: every region places, and lands home.
        for (const Region &rg : sat.regions) {
            float u = 0, v = 0;
            uint64_t back = 0;
            const Region *got = nullptr;
            check("a saturated-plane region round-trips",
                  sat.project(rg.base, &u, &v) &&
                      sat.unproject(u, v, &back, &got) && got != nullptr &&
                      got->base == rg.base,
                  "a region of a saturated plane lost its own cell");
        }
    }
    {
        // `order` keeps its meaning and its VALUE under the atlas: it is the
        // plane's cell quantisation, which is what every 1<<order call site
        // already reads it as. Only the address->cell mapping changed.
        const Projection a = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
        check("the layout does not change order", a.order == p.order,
              "atlas reported order " + std::to_string(a.order) +
                  ", hilbert reported " + std::to_string(p.order));
        check("Terrain's plane side is therefore unchanged",
              (1u << a.order) == (1u << p.order), "the cell grid resized");
    }

    // --- 61 T8: Component 1's deliverable — the floor names its rectangles --
    {
        // T2's first fixture, so the rects are the ones already pinned above:
        // code owns (0,0)-(16,256) = 4096 cells, mmap owns (16,0)-(256,256).
        static const Ref kSmall = {0x0000000000400000ull, 4096, Region::Code};
        static const Ref kBig = {0x0000000001000000ull, 61440, Region::Mmap};
        const Projection lp = atlas_of({kSmall, kBig});
        const std::vector<AtlasLabel> labels = atlas_labels(lp);
        check("every rect big enough to read gets a label", labels.size() == 2,
              "got " + std::to_string(labels.size()) + " labels for 2 regions");

        const uint32_t ln = 1u << lp.order;
        // The decode check below is only meaningful because THIS fixture gives
        // both regions one byte per cell (4096+61440 == 65536 == the order-8
        // plane exactly), so no rect has a rounding tail. STATED, not assumed:
        // a rect can be granted up to ~4x its region's bytes, and then its
        // centre cell can legitimately land past the region's last byte and
        // refuse — the anchor names a RECTANGLE, and decodability is a property
        // of the fixture, not a guarantee of atlas_labels.
        uint64_t fixture_cells = 0, fixture_bytes = 0;
        for (size_t i = 0; i < lp.rects.size(); i++) {
            const AtlasRect &fr = lp.rects[i];
            fixture_cells += uint64_t(fr.x1 - fr.x0) * uint64_t(fr.y1 - fr.y0);
            fixture_bytes += lp.regions[i].len;
        }
        check("the label fixture is 1 byte per cell",
              fixture_cells == fixture_bytes,
              "the fixture gained a rounding tail, so a rect's centre cell need "
              "no longer decode and the check below would fail for a reason "
              "having nothing to do with labelling");
        for (const AtlasLabel &l : labels) {
            // The anchor is the rect's GEOMETRIC centre, so the label sits ON
            // the thing it names rather than beside it.
            const AtlasRect &r = lp.rects[l.region];
            const uint32_t cx = uint32_t(l.u * ln), cy = uint32_t(l.v * ln);
            check("the label anchor lies inside the rect it names",
                  cx >= r.x0 && cx < r.x1 && cy >= r.y0 && cy < r.y1,
                  "anchor (" + std::to_string(l.u) + "," + std::to_string(l.v) +
                      ") fell outside region " + std::to_string(l.region) +
                      "'s rect");
            // And it points at that region under the layout's own inverse — the
            // label is not merely NEAR the rect, it decodes TO it.
            uint64_t back = 0;
            const Region *got = nullptr;
            check("the anchor unprojects to the region it names",
                  lp.unproject(l.u, l.v, &back, &got) && got != nullptr &&
                      got->base == lp.regions[l.region].base,
                  "a label anchored on a cell belonging to a different region");
            check("a label always says something", !l.text.empty(),
                  "an unnamed rectangle is an unlabelled floor");
        }
        // The fallback: these fixture Regions carry no `label`, so each must
        // fall back to its KIND name rather than to an empty string — reusing
        // the rule the HUD's side-panel legend already applies, never a second
        // naming convention. Guarded on the size, because indexing a short
        // vector to report a failure turns a clean FAIL into a crash that says
        // nothing.
        if (labels.size() == 2)
            check("an unlabelled region falls back to its kind name",
                  labels[0].text ==
                          std::string(region_style(Region::Code).name) &&
                      labels[1].text ==
                          std::string(region_style(Region::Mmap).name),
                  "got \"" + labels[0].text + "\" and \"" + labels[1].text +
                      "\"");
    }
    {
        // Hilbert has nowhere to put a label and must say so by REFUSING, not
        // by anchoring on a snake's centroid — that would be the fabricated
        // structure D7 forbids. It is also what makes the function safe to call
        // unconditionally from the shell.
        std::vector<Region> hin;
        Region hreg;
        hreg.base = 0x400000ull;
        hreg.len = 4096;
        hreg.kind = Region::Code;
        hin.push_back(hreg);
        // PINNED to Hilbert, not inherited from the struct default: 61 T10
        // flips that default to Atlas, and this block would then build a real
        // single-region atlas (order 6, one 64x64 rect = 4096 cells), sail past
        // atlas_labels' 64-cell threshold, return one label and FAIL — while
        // printing "a space-filling curve was given a label anchor", which
        // would be actively false about what happened. Same idiom as the
        // byte-exact loop above, for the same reason.
        Projection hp = build_projection(std::move(hin));
        hp.layout = Projection::Layout::Hilbert;
        rebuild_layout(hp);
        check("the Hilbert layout labels nothing", atlas_labels(hp).empty(),
              "a space-filling curve was given a label anchor it cannot "
              "support");
    }
    {
        // The legibility threshold, on the saturated plane T2 already builds:
        // 4000 regions on a 4096-cell grid, so the LARGEST rect is 2 cells.
        // Every one is dropped — 4000 strings over 4096 cells is not a labelled
        // floor. The side-panel legend still lists all 4000, which is the
        // disclosure that keeps a partial floor honest.
        std::vector<Ref> smany;
        for (uint64_t i = 0; i < 4000; i++)
            smany.push_back({0x1000ull + i * 0x10000ull, 1, Region::Code});
        const Projection sat2 = atlas_of(smany);
        check("rects too small to read are dropped, not piled up",
              atlas_labels(sat2).empty(),
              "a 1-cell rectangle was given a text label");
    }

    // --- 61 T9: the reflow notice ------------------------------------------
    // "a growing capture that reflows silently is the failure mode to avoid."
    {
        // Rebuilding the SAME region set is not a reflow. build_projection is a
        // pure function of the regions, so a weave that changes nothing
        // produces an identical layout — the spec's "recompute only when that
        // set changes" falls out of that, and this pins it rather than
        // assuming it.
        const Projection ra = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
        const Projection rb = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
        const LayoutFingerprint fa = layout_fingerprint(ra);
        const LayoutFingerprint fb = layout_fingerprint(rb);
        check("the fingerprint is a real one, not a default",
              fa.valid && fa.regions == 3 && fa.digest != 0,
              "a layout_fingerprint that returned LayoutFingerprint{} would "
              "satisfy every check in this block vacuously (0 == 0, and "
              "layout_reflow_note short-circuits on !valid)");
        check("an unchanged region set digests identically", fa.digest == fb.digest,
              "two builds of one region set disagreed");
        check("recomputing an unchanged layout is not a reflow",
              layout_reflow_note(fa, fb).empty(),
              "warned about a floor that did not move: \"" +
                  layout_reflow_note(fa, fb) + "\"");
    }
    {
        // A region APPEARS — the live-capture case the spec is about.
        static const Ref kOne = {0x0000000000400000ull, 4096, Region::Code};
        static const Ref kTwo = {0x0000000000900000ull, 4096, Region::Heap};
        const std::string note =
            layout_reflow_note(layout_fingerprint(atlas_of({kOne})),
                               layout_fingerprint(atlas_of({kOne, kTwo})));
        check("a new region reflows the floor, and says so", !note.empty(),
              "a growing capture re-laid its floor silently — the exact "
              "failure the spec's risk table names");
        check("the note names what changed",
              note.find("1 region became 2") != std::string::npos, note);
    }
    {
        // Same region COUNT, one of them GREW. Under Hilbert this shifts every
        // later region's domain offset, so the floor re-scrambles just as
        // surely as a treemap re-tiles — the notice is not an atlas feature.
        std::vector<Region> rsmall, rgrown;
        Region g0;
        g0.base = 0x400000ull;
        g0.len = 4096;
        g0.kind = Region::Code;
        Region g1 = g0;
        g1.base = 0x900000ull;
        g1.len = 4096;
        // Grow the FIRST region (the one that sorts first), so a LATER
        // region's domain offset actually moves — which is the mechanism the
        // comment and the failure message below both name. Growing the last
        // region would still change the digest (via domain_off.back()) and the
        // check would pass, but for a different reason than it claims.
        rsmall = {g0, g1};
        g0.len = 8192;
        rgrown = {g0, g1};
        // Both PINNED to Hilbert: this block's whole point is that the notice
        // is not an atlas feature, and inheriting the struct default would let
        // 61 T10's flip silently turn it into a second atlas test.
        Projection ps = build_projection(std::move(rsmall));
        Projection pg = build_projection(std::move(rgrown));
        ps.layout = Projection::Layout::Hilbert;
        pg.layout = Projection::Layout::Hilbert;
        rebuild_layout(ps);
        rebuild_layout(pg);
        check("the GREW fixture really is testing Hilbert",
              ps.layout == Projection::Layout::Hilbert &&
                  pg.layout == Projection::Layout::Hilbert && ps.rects.empty(),
              "this block must exercise the NON-atlas path");
        const std::string note =
            layout_reflow_note(layout_fingerprint(ps), layout_fingerprint(pg));
        check("a region that GREW reflows the floor under Hilbert too",
              !note.empty(),
              "compaction shifted every later region and the HUD said nothing");
        check("the same-count wording says what actually changed",
              note.find("extents changed") != std::string::npos, note);
    }
    {
        // Silence by RULE, not by threshold.
        const Projection rp = atlas_of({kRefs[0]});
        check("the first layout is not a reflow",
              layout_reflow_note(LayoutFingerprint{}, layout_fingerprint(rp))
                  .empty(),
              "warned about a floor the reader had never seen");
        const Projection rempty = build_projection({});
        check("a recording with no regions has no fingerprint",
              !layout_fingerprint(rempty).valid,
              "an empty plane is not a layout to compare against");
        check("appearing from nothing is not a reflow",
              layout_reflow_note(layout_fingerprint(rempty),
                                 layout_fingerprint(rp))
                  .empty(),
              "a floor drawn for the first time is not a floor that moved");
    }

    {
        // keep_order: the caller's order IS the domain order, and appending a
        // region never moves an existing region's domain slot — the hinge the
        // stable-plane-layout setting turns on (a grown live session's regions
        // pack append-only instead of re-sorting by base).
        Region hi;
        hi.base = 0x2000;
        hi.len = 64;
        hi.kind = Region::Code;
        Region lo;
        lo.base = 0x1000;
        lo.len = 128;
        lo.kind = Region::Code;
        Region add;
        add.base = 0x1800;
        add.len = 32;
        add.kind = Region::Code;
        const Projection p1 =
            build_projection({hi, lo}, /*keep_order=*/true);
        check("keep_order preserves caller order",
              p1.regions.size() == 2 && p1.regions[0].base == 0x2000,
              "first-given must own the first domain slot");
        const Projection p2 =
            build_projection({hi, lo, add}, /*keep_order=*/true);
        check("append keeps existing domain slots",
              p2.domain_off.size() == 4 &&
                  p2.domain_off[0] == p1.domain_off[0] &&
                  p2.domain_off[1] == p1.domain_off[1] &&
                  p2.regions[2].base == 0x1800,
              "an appended region must land AFTER the existing slots");
        check("append keeps the plane order stable",
              p2.order == p1.order,
              "this small a domain must not re-size the plane");
        const Projection ps = build_projection({hi, lo});
        check("default still sorts by base", ps.regions[0].base == 0x1000,
              "the one-arg default must stay byte-identical to today");
    }

    if (failures) {
        std::fprintf(stderr, "%d projection check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_projection: all checks passed\n");
    return 0;
}
