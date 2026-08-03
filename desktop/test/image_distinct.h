// image_distinct.h — pairwise frame distinctness. Deliberately crude: this
// answers "would a reader see two different pictures", not "are these images
// similar", so it counts differing pixels rather than computing a perceptual
// metric. A threshold, not a score, because the gate is a yes/no.
#ifndef ASMDESK_TEST_IMAGE_DISTINCT_H
#define ASMDESK_TEST_IMAGE_DISTINCT_H

#include <cstdlib>

#include "gl_offscreen.h" // Image

namespace asmdesk::testing {

// The fraction of pixels two frames disagree on. Exposed separately from the
// yes/no below so a FAILING check can say what it actually measured — "changed
// nothing" and "changed less than the threshold" are different findings, and a
// gate that cannot tell them apart invites someone to lower the threshold
// blind.
inline float image_diff_fraction(const Image &a, const Image &b) {
    if (a.w != b.w || a.h != b.h || a.px.empty())
        return 1.0f;
    size_t differing = 0;
    const size_t n = size_t(a.w) * size_t(a.h);
    for (size_t i = 0; i < n; ++i) {
        const int dr = int(a.px[i * 4 + 0]) - int(b.px[i * 4 + 0]);
        const int dg = int(a.px[i * 4 + 1]) - int(b.px[i * 4 + 1]);
        const int db = int(a.px[i * 4 + 2]) - int(b.px[i * 4 + 2]);
        if (std::abs(dr) + std::abs(dg) + std::abs(db) > 24)
            differing++;
    }
    return float(differing) / float(n);
}

inline bool images_distinct(const Image &a, const Image &b,
                            float min_fraction = 0.02f) {
    if (a.w != b.w || a.h != b.h)
        return true; // different geometry is trivially distinct
    return image_diff_fraction(a, b) >= min_fraction;
}

} // namespace asmdesk::testing
#endif // ASMDESK_TEST_IMAGE_DISTINCT_H
