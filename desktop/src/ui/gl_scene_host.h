// gl_scene_host.h — the GL implementation of the SceneHost bridge (scene_host.h).
// Constructed by main.cpp for the full app AND the render-only viewer (both link
// the GL scene, which links no engine — D4), and NEVER by the headless tests. The
// concrete class lives entirely in gl_scene_host.cpp so the GL headers stay in one
// TU; here we expose only a factory, so a caller that wants a host pulls in no GL.
#ifndef ASMDESK_UI_GL_SCENE_HOST_H
#define ASMDESK_UI_GL_SCENE_HOST_H

#include <memory>

#include "ui/scene_host.h"

namespace asmdesk {

// A GL-backed scene host. init() must be called once with a current GL context
// (main.cpp does, right after the ImGui GL backend is up); reset the returned
// pointer BEFORE the GL context is destroyed so the scene's GL objects are freed
// while the context is still current.
std::unique_ptr<SceneHost> make_gl_scene_host();

} // namespace asmdesk
#endif // ASMDESK_UI_GL_SCENE_HOST_H
