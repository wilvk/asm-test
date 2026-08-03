// png_write.h — PNG output for the --shot screenshot mode
// (docs/guides/desktop-gui-scenes.md).
//
// Lives in the FULL binary only. asmtest-viewer is the permissive render-only
// build and must gain no new third-party object, so mk/desktop.mk links this
// into asmtest-desktop alone (the DESKTOP_UI_APP list, the same seam regsynth
// uses) and every call site is guarded by !ASMTEST_DESKTOP_RENDER_ONLY.
#ifndef ASMDESK_UI_PNG_WRITE_H
#define ASMDESK_UI_PNG_WRITE_H

#include <string>

namespace asmdesk {

// Write an RGBA8 buffer as a PNG at `path`.
//
// `px` holds w*h*4 bytes in the order glReadPixels produces: the FIRST row in
// memory is the BOTTOM row of the image. This function flips to the top-down
// order a PNG file wants, so callers hand it the read-back buffer unmodified
// and no caller has to remember the convention.
//
// Returns false with `err` set on any failure — an unwritable path, a bad size,
// or an encoder refusal. It never returns true without having written a file,
// because a screenshot pipeline that reported success on a file it did not
// write would ship missing images as passing ones.
bool png_write_rgba_flipped(const std::string &path, int w, int h,
                            const unsigned char *px, std::string &err);

} // namespace asmdesk
#endif // ASMDESK_UI_PNG_WRITE_H
