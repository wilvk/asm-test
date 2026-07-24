// fabric_plan.cpp — the pure draw planner of fabric_plan.h. No ImGui.
#include "loom/fabric_plan.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

const char *const kLoomFadeOutText = "alive at trace end";
const char *const kLoomBornUntracedText =
    "born of untraced state — provenance starts at instrumentation";
const char *const kLoomGuestBadgeText =
    "isolated guest — emulator replay, not silicon";

namespace {

// A span thinner than this collapses into its lane's density ribbon: below it
// the rectangle is narrower than its own border and reads as noise.
constexpr float kMinSpanPx = 3.0f;
// A memory band explodes into per-byte rows once each byte can own this many
// vertical pixels.
constexpr float kByteRowPx = 12.0f;
// Ribbon resolution. "Per-column" at one prim per physical pixel would put tens
// of thousands of prims in a plan for no visible gain; a 4px bucket still reads
// as a ribbon and keeps the plan bounded (and the golden dumps readable).
constexpr float kRibbonColPx = 4.0f;
// Rough advance of the default ImGui font — only used to decide whether a value
// chip's text FITS, never to lay text out (the painter measures for real).
constexpr float kCharPx = 7.0f;

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string f2(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", static_cast<double>(v));
    return b;
}

} // namespace

const char *loom_prim_name(loom_prim k) {
    switch (k) {
    case loom_prim::lane_header:
        return "lane_header";
    case loom_prim::span:
        return "span";
    case loom_prim::span_hollow:
        return "span_hollow";
    case loom_prim::hop:
        return "hop";
    case loom_prim::knot:
        return "knot";
    case loom_prim::value_chip:
        return "value_chip";
    case loom_prim::density_ribbon:
        return "density_ribbon";
    case loom_prim::byte_row:
        return "byte_row";
    case loom_prim::torn_edge:
        return "torn_edge";
    case loom_prim::fade_out:
        return "fade_out";
    case loom_prim::born_untraced_glyph:
        return "born_untraced_glyph";
    case loom_prim::guest_badge:
        return "guest_badge";
    case loom_prim::take_dim:
        return "take_dim";
    case loom_prim::take_hot:
        return "take_hot";
    case loom_prim::take_dashed_tail:
        return "take_dashed_tail";
    case loom_prim::patient_zero:
        return "patient_zero";
    case loom_prim::fault_card:
        return "fault_card";
    }
    return "?";
}

std::string loom_torn_text(const loom_provenance_t &p) {
    if (p.steps_total > p.steps_recorded)
        return "trace truncated: " + std::to_string(p.steps_recorded) + " of " +
               std::to_string(p.steps_total) + " steps recorded";
    // The v1 schema carries no dataflow step total, so a REPLAYED truncated
    // recording usually cannot supply M. Saying "N of N" would claim the run
    // ended where the buffer did; naming the gap is the honest form.
    return "trace truncated: " + std::to_string(p.steps_recorded) +
           " steps recorded; this feed did not record the total";
}

size_t loom_plan(const loom_fabric_t &f, const loom_view_t &v,
                 std::vector<loom_prim_t> *out) {
    if (out == nullptr)
        return 0;
    out->clear();
    const double spp = v.steps_per_px > 0 ? v.steps_per_px : 1.0;
    const float lane_h = v.lane_h > 0 ? v.lane_h : 1.0f;

    auto x_of = [&](double step) {
        return static_cast<float>((step - v.step0) / spp);
    };
    // A span that is alive at the end still has to be drawn to SOME right edge;
    // it is drawn to the last recorded step, and the fade_out prim beside it is
    // what says the value did not end there — the fabric did.
    auto span_end = [&](const loom_span_t &s) {
        return s.t_end == kLoomAlive ? static_cast<double>(f.steps) : s.t_end;
    };

    const bool dimming = !v.selected_steps.empty();
    auto selected = [&](uint32_t step) {
        return std::binary_search(v.selected_steps.begin(),
                                  v.selected_steps.end(), step);
    };

    auto push = [&](loom_prim k, float x0, float y0, float x1, float y1,
                    uint32_t a, uint32_t b, std::string text) {
        loom_prim_t p;
        p.kind = k;
        p.x0 = x0;
        p.y0 = y0;
        p.x1 = x1;
        p.y1 = y1;
        p.a = a;
        p.b = b;
        p.text = std::move(text);
        out->push_back(std::move(p));
    };

    // --- persistent chrome, first so it can never be scrolled away ----------
    if (f.prov.isolated_guest)
        push(loom_prim::guest_badge, 0, 0, 0, 0, 0, 0, kLoomGuestBadgeText);
    if (f.prov.truncated) {
        float x = x_of(static_cast<double>(f.steps));
        push(loom_prim::torn_edge, x, 0, x, v.px_h, f.steps, 0,
             loom_torn_text(f.prov));
    }

    const int lanes_visible =
        std::max(1, static_cast<int>(v.px_h / lane_h) + 1);
    const int lane_hi = std::min<int>(static_cast<int>(f.lanes.size()),
                                      v.lane0 + lanes_visible);

    for (int li = std::max(0, v.lane0); li < lane_hi; li++) {
        const loom_lane_t &lane = f.lanes[li];
        const float y0 = (li - v.lane0) * lane_h;
        const float y1 = y0 + lane_h;
        push(loom_prim::lane_header, 0, y0, 0, y1, static_cast<uint32_t>(li), 0,
             lane.name);

        // Collect this lane's visible spans once.
        std::vector<uint32_t> mine;
        for (uint32_t si = 0; si < f.spans.size(); si++) {
            const loom_span_t &s = f.spans[si];
            if (s.lane != static_cast<uint32_t>(li))
                continue;
            float sx0 = x_of(s.t_write), sx1 = x_of(span_end(s));
            if (sx1 < 0 || sx0 > v.px_w)
                continue;
            mine.push_back(si);
        }

        // Zoomed out: any span thinner than kMinSpanPx collapses the WHOLE lane
        // into a density ribbon. Mixing rectangles and ribbon in one lane would
        // make the wide ones look like the only thing that happened.
        bool collapse = false;
        for (uint32_t si : mine) {
            const loom_span_t &s = f.spans[si];
            if (x_of(span_end(s)) - x_of(s.t_write) < kMinSpanPx) {
                collapse = true;
                break;
            }
        }

        if (collapse) {
            for (float bx = 0; bx < v.px_w; bx += kRibbonColPx) {
                double s_lo = v.step0 + bx * spp;
                double s_hi = v.step0 + (bx + kRibbonColPx) * spp;
                uint32_t live = 0;
                for (uint32_t si : mine) {
                    const loom_span_t &s = f.spans[si];
                    if (static_cast<double>(s.t_write) < s_hi &&
                        span_end(s) > s_lo)
                        live++;
                }
                if (live == 0)
                    continue;
                push(loom_prim::density_ribbon, bx, y0,
                     std::min(bx + kRibbonColPx, v.px_w), y1, live,
                     static_cast<uint32_t>(li), std::string());
            }
            continue;
        }

        // Zoomed in: a memory band whose bytes each own kByteRowPx explodes into
        // per-byte rows, each attributed to the span that last wrote that byte.
        const uint64_t band_bytes =
            lane.kind == loom_lane_kind::mem_band ? lane.hi - lane.lo : 0;
        const bool byte_rows =
            band_bytes > 0 &&
            lane_h / static_cast<float>(band_bytes) >= kByteRowPx;
        const float byte_h =
            band_bytes > 0 ? lane_h / static_cast<float>(band_bytes) : lane_h;

        for (uint32_t si : mine) {
            const loom_span_t &s = f.spans[si];
            float sx0 = std::max(0.0f, x_of(s.t_write));
            float sx1 = std::min(v.px_w, x_of(span_end(s)));
            uint32_t dim = dimming && !selected(s.t_write) ? 1u : 0u;

            if (byte_rows && !s.born_untraced) {
                for (uint64_t byte = s.lo; byte < s.hi; byte++) {
                    if (byte < lane.lo || byte >= lane.hi)
                        continue;
                    float by = y0 + (byte - lane.lo) * byte_h;
                    push(loom_prim::byte_row, sx0, by, sx1, by + byte_h, si,
                         static_cast<uint32_t>(byte - lane.lo), hex(byte));
                }
            } else {
                push(s.value_valid ? loom_prim::span : loom_prim::span_hollow,
                     sx0, y0, sx1, y1, si, dim, std::string());
            }

            if (s.born_untraced)
                push(loom_prim::born_untraced_glyph, sx0, y0, sx0 + lane_h, y1,
                     si, 0, kLoomBornUntracedText);
            if (s.t_end == kLoomAlive)
                push(loom_prim::fade_out, sx1, y0, sx1, y1, si, 0,
                     kLoomFadeOutText);

            // A chip only when its own text fits inside the span. A clipped
            // value is a misread value.
            if (s.value_valid) {
                std::string chip = hex(s.value);
                if (sx1 - sx0 >= static_cast<float>(chip.size()) * kCharPx)
                    push(loom_prim::value_chip, sx0, y0, sx1, y1, si, dim,
                         std::move(chip));
            }
        }
    }

    // --- hops: only when BOTH endpoints are on screen -----------------------
    for (const loom_hop_t &h : f.hops) {
        if (h.to_span == kLoomNoSpan)
            continue;
        const loom_span_t &a = f.spans[h.from_span];
        const loom_span_t &b = f.spans[h.to_span];
        int la = static_cast<int>(a.lane), lb = static_cast<int>(b.lane);
        if (la < v.lane0 || la >= lane_hi || lb < v.lane0 || lb >= lane_hi)
            continue;
        float ax = x_of(a.t_write), bx = x_of(b.t_write);
        if ((ax < 0 && bx < 0) || (ax > v.px_w && bx > v.px_w))
            continue;
        uint32_t dim =
            dimming && !(selected(a.t_write) && selected(b.t_write)) ? 1u : 0u;
        push(loom_prim::hop, ax, (la - v.lane0) * lane_h + lane_h / 2, bx,
             (lb - v.lane0) * lane_h + lane_h / 2, h.from_span, dim,
             std::string());
    }

    // --- knots --------------------------------------------------------------
    for (const loom_knot_t &k : f.knots) {
        float x = x_of(k.step);
        if (x < 0 || x > v.px_w)
            continue;
        push(loom_prim::knot, x, 0, x, v.px_h, k.step,
             dimming && !selected(k.step) ? 1u : 0u, std::string());
    }
    return out->size();
}

std::string loom_plan_dump(const std::vector<loom_prim_t> &prims) {
    std::string o;
    for (const loom_prim_t &p : prims) {
        o += loom_prim_name(p.kind);
        o += " [" + f2(p.x0) + "," + f2(p.y0) + " " + f2(p.x1) + "," +
             f2(p.y1) + "]";
        o += " a=" + std::to_string(p.a) + " b=" + std::to_string(p.b);
        if (!p.text.empty())
            o += " \"" + p.text + "\"";
        o += "\n";
    }
    return o;
}

} // namespace asmdesk
