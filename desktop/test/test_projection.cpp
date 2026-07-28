// test_projection.cpp — the address-space Hilbert projection
// (docs/internal/gui/10-spacetime-3d-overview.md T1). Null harness, no display:
// this binary links space/projection.o and NOTHING else, so its link line is the
// proof that the render-only viewer can place a recording's regions on the
// terrain plane with zero engine dependencies (D4) — the same argument
// test_slice.cpp makes for client-side slicing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "space/projection.h"

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
            Anchor a = resolve_anchor({data(0x0, 0x1000), code(0x400000, 0x1000),
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

    if (failures) {
        std::fprintf(stderr, "%d projection check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_projection: all checks passed\n");
    return 0;
}
