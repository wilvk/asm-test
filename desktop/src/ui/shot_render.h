// shot_render.h — the --shot offscreen capture host
// (docs/guides/desktop-gui-scenes.md).
//
// Renders the REAL shell (ui/shell.cpp's draw_shell) through the REAL OpenGL3
// ImGui backend into a surfaceless-EGL framebuffer, then reads it back as PNG.
// The pixels are the ones the app ships, not a reimplementation.
//
// Surfaceless EGL rather than a hidden GLFW window: it needs no X display at
// all, so the same command works on a desktop session, over ssh, and inside the
// docker-desktop lane without Xvfb. desktop/test/test_scene_fbo.cpp already
// proves this exact path (surfaceless context, FBO, glReadPixels) in this tree.
//
// ImGui's PLATFORM backend is replaced by setting DisplaySize/DeltaTime by hand
// — the standard ImGui pattern, and the same shape the null-backend tests use.
// The RENDERER backend stays the real imgui_impl_opengl3.
//
// FULL BINARY ONLY: never compiled into asmtest-viewer (D4).
#ifndef ASMDESK_UI_SHOT_RENDER_H
#define ASMDESK_UI_SHOT_RENDER_H

#include <string>

namespace asmdesk {

// Render every shot in `manifest_path` into `out_dir`. Returns 0 on success,
// non-zero on the first failure (message already printed to stderr).
int shot_run(const std::string &manifest_path, const std::string &out_dir);

} // namespace asmdesk
#endif // ASMDESK_UI_SHOT_RENDER_H
