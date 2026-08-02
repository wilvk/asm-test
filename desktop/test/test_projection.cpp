// test_projection.cpp — the address-space Hilbert projection
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T1). Null harness, no display:
// this binary links space/projection.o and NOTHING else, so its link line is the
// proof that the render-only viewer can place a recording's regions on the
// terrain plane with zero engine dependencies (D4) — the same argument
// test_slice.cpp makes for client-side slicing.
#include <algorithm>
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
    std::mt19937_64 rng(0xA5A5A5A5u);
    for (int i = 0; i < 10000; i++) {
        const Ref &r = kRefs[rng() % 3];
        uint64_t addr = r.base + (rng() % r.len);
        float u, v;
        if (!p.project(addr, &u, &v)) {
            fail("round trip", "project failed for a mapped address");
            continue;
        }
        uint64_t back = 0;
        const Region *reg = nullptr;
        if (!p.unproject(u, v, &back, &reg)) {
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

    if (failures) {
        std::fprintf(stderr, "%d projection check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_projection: all checks passed\n");
    return 0;
}
