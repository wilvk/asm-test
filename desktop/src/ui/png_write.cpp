#include "ui/png_write.h"

#include <cerrno>
#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace asmdesk {

bool png_write_rgba_flipped(const std::string &path, int w, int h,
                            const unsigned char *px, std::string &err) {
    err.clear();
    if (w <= 0 || h <= 0 || px == nullptr) {
        err = "png_write: bad dimensions or null pixels";
        return false;
    }

    // glReadPixels gives bottom-up rows; a PNG is top-down. Flip by whole rows
    // here rather than calling stbi_flip_vertically_on_write, which sets
    // PROCESS-WIDE state: a second writer (or a future thread) would silently
    // inherit it and emit upside-down images with nothing to point at.
    const size_t stride = static_cast<size_t>(w) * 4;
    std::vector<unsigned char> flipped(stride * static_cast<size_t>(h));
    for (int y = 0; y < h; y++)
        std::memcpy(&flipped[static_cast<size_t>(y) * stride],
                    px + static_cast<size_t>(h - 1 - y) * stride, stride);

    errno = 0;
    if (stbi_write_png(path.c_str(), w, h, 4, flipped.data(),
                       static_cast<int>(stride)) == 0) {
        err = "png_write: stbi_write_png failed for \"" + path + "\"";
        if (errno != 0)
            err += std::string(" (") + std::strerror(errno) + ")";
        return false;
    }
    return true;
}

} // namespace asmdesk
