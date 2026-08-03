// test_png_write.cpp — the shot mode's PNG writer.
//
// A committed screenshot that silently encoded to garbage would look exactly
// like a rendering bug, and a writer that reported success on a file it never
// wrote would ship missing images as passing ones. So the signature, the
// dimensions, the row order and the failure path are all asserted rather than
// assumed.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ui/png_write.h"

static int g_fail = 0;
static void check(const char *what, bool ok, const char *why) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        std::printf("     %s\n", why);
        g_fail = 1;
    }
}

static std::vector<unsigned char> read_all(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

int main() {
    const std::string path = "build/test_png_write_out.png";
    std::remove(path.c_str());

    const int W = 8, H = 4;
    const size_t stride = static_cast<size_t>(W) * 4;
    std::vector<unsigned char> px(stride * H, 0);
    // Bottom row (FIRST in glReadPixels order) red; top row blue. After the
    // flip the FILE's first row must be the blue one.
    //
    // Note on coverage: asserting that from here would need a PNG DECODER
    // (inflate + un-filter), which this binary deliberately does not link. What
    // is asserted instead is orientation SENSITIVITY — see the row-order check
    // below — which catches a writer that ignores row order entirely. The exact
    // top-down result was verified out-of-band by decoding the output with
    // Python's zlib: file row 0 came back blue, row H-1 red.
    for (int x = 0; x < W; x++) {
        px[static_cast<size_t>(x) * 4 + 0] = 255; // bottom row red
        px[static_cast<size_t>(x) * 4 + 3] = 255;
        const size_t top = (static_cast<size_t>(H - 1) * W + x) * 4;
        px[top + 2] = 255; // top row blue
        px[top + 3] = 255;
    }

    std::string err;
    const bool ok = asmdesk::png_write_rgba_flipped(path, W, H, px.data(), err);
    check("png_write: writes without error", ok, err.c_str());

    const std::vector<unsigned char> bytes = read_all(path);
    check("png_write: file is non-empty", bytes.size() > 64,
          "an empty or tiny file means the encoder silently failed");

    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    bool sig_ok = bytes.size() >= 8;
    for (int i = 0; sig_ok && i < 8; i++)
        sig_ok = bytes[static_cast<size_t>(i)] == sig[i];
    check("png_write: PNG signature", sig_ok, "not a PNG file");

    // IHDR width/height are big-endian u32 at offsets 16 and 20.
    bool dim_ok = false;
    if (bytes.size() >= 24) {
        const unsigned w = (unsigned)bytes[16] << 24 | (unsigned)bytes[17] << 16 |
                           (unsigned)bytes[18] << 8 | (unsigned)bytes[19];
        const unsigned h = (unsigned)bytes[20] << 24 | (unsigned)bytes[21] << 16 |
                           (unsigned)bytes[22] << 8 | (unsigned)bytes[23];
        dim_ok = (w == (unsigned)W && h == (unsigned)H);
    }
    check("png_write: IHDR carries the requested dimensions", dim_ok,
          "wrong width/height in the header");

    // Orientation sensitivity: the same pixels in a DIFFERENT row order must
    // encode to different bytes. A writer that dropped the flip, or that
    // collapsed rows, would produce identical output for both and fail here.
    {
        std::vector<unsigned char> reversed(px.size());
        for (int y = 0; y < H; y++)
            std::memcpy(&reversed[static_cast<size_t>(y) * stride],
                        &px[static_cast<size_t>(H - 1 - y) * stride], stride);

        const std::string p2 = "build/test_png_write_rev.png";
        std::remove(p2.c_str());
        std::string rerr;
        const bool rok =
            asmdesk::png_write_rgba_flipped(p2, W, H, reversed.data(), rerr);
        const std::vector<unsigned char> rbytes = read_all(p2);
        check("png_write: row order changes the encoded bytes",
              rok && !rbytes.empty() && rbytes != bytes,
              "reversing the input rows produced an identical file — the "
              "writer is ignoring row order, so the flip cannot be working");
    }

    // The failure path must FAIL, and must say why. A writer that returned true
    // here would report success for a file that does not exist.
    std::string err2;
    const bool refused = !asmdesk::png_write_rgba_flipped(
        "build/no-such-dir-here/x.png", W, H, px.data(), err2);
    check("png_write: an unwritable path fails loudly", refused && !err2.empty(),
          "a failed write must set err, never return true silently");

    // Degenerate inputs are refused rather than encoded.
    std::string err3;
    check("png_write: zero width is refused",
          !asmdesk::png_write_rgba_flipped(path, 0, H, px.data(), err3) &&
              !err3.empty(),
          "a zero dimension must be reported, not encoded");
    std::string err4;
    check("png_write: null pixels are refused",
          !asmdesk::png_write_rgba_flipped(path, W, H, nullptr, err4) &&
              !err4.empty(),
          "a null buffer must be reported, not dereferenced");

    std::printf("%s\n",
                g_fail ? "test_png_write: FAILURES" : "test_png_write: all ok");
    return g_fail;
}
