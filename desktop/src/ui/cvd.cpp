// cvd.cpp — see cvd.h. Pure maths; no ImGui context, no engine.
#include "ui/cvd.h"

#include <algorithm>
#include <cmath>

namespace asmdesk {

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// sRGB channel -> linear light (WCAG 2.1 / IEC 61966-2-1).
float to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Machado, Oliveira & Fernandes (2009) severity-1.0 dichromacy matrices. They
// are applied to the sRGB values directly here (the common simplification used
// by many colour-blind simulators); this is an APPROXIMATION sufficient to tell
// whether two categorical hues remain separable, which is all the redundancy
// test asks of it — it is never used to recolour what the user sees.
struct Mat3 {
    float m[9];
};
const Mat3 kProtan = {{0.152286f, 1.052583f, -0.204868f, 0.114503f, 0.786281f,
                       0.099216f, -0.003882f, -0.048116f, 1.051998f}};
const Mat3 kDeuter = {{0.367322f, 0.860646f, -0.227968f, 0.280085f, 0.672501f,
                       0.047413f, -0.011820f, 0.042940f, 0.968881f}};
const Mat3 kTritan = {{1.255528f, -0.076749f, -0.178779f, -0.078411f, 0.930809f,
                       0.147602f, 0.004733f, 0.691367f, 0.303900f}};

ImVec4 apply(const Mat3 &t, ImVec4 c) {
    return ImVec4(clamp01(t.m[0] * c.x + t.m[1] * c.y + t.m[2] * c.z),
                  clamp01(t.m[3] * c.x + t.m[4] * c.y + t.m[5] * c.z),
                  clamp01(t.m[6] * c.x + t.m[7] * c.y + t.m[8] * c.z), c.w);
}

} // namespace

ImVec4 dt_cvd_simulate(ImVec4 c, dt_cvd_kind kind) {
    switch (kind) {
    case dt_cvd_kind::protan:
        return apply(kProtan, c);
    case dt_cvd_kind::deuter:
        return apply(kDeuter, c);
    case dt_cvd_kind::tritan:
        return apply(kTritan, c);
    case dt_cvd_kind::none:
        break;
    }
    return c;
}

float dt_contrast_ratio(ImVec4 a, ImVec4 b) {
    auto lum = [](ImVec4 c) {
        return 0.2126f * to_linear(c.x) + 0.7152f * to_linear(c.y) +
               0.0722f * to_linear(c.z);
    };
    float la = lum(a), lb = lum(b);
    float hi = std::max(la, lb), lo = std::min(la, lb);
    return (hi + 0.05f) / (lo + 0.05f);
}

float dt_cvd_distance(ImVec4 a, ImVec4 b, dt_cvd_kind kind) {
    ImVec4 sa = dt_cvd_simulate(a, kind), sb = dt_cvd_simulate(b, kind);
    float dr = sa.x - sb.x, dg = sa.y - sb.y, db = sa.z - sb.z;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

} // namespace asmdesk
