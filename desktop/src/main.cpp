// main.cpp — the desktop entry point, shared by BOTH binaries. A compile-time
// ASMTEST_DESKTOP_RENDER_ONLY guard flips the window title (and, via the shell,
// disables the engine doors); the build (mk/desktop.mk) sets it for the
// render-only viewer and links no engines there (D4). The draw code lives in
// ui/shell.cpp and is backend-free, so the null-backend tests exercise the same
// frames this loop drives. Stock Dear ImGui GLFW + OpenGL3 example shape
// (03-desktop-shell.md T6).
#include <cstdio>
#include <memory>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"   // the plotting chassis context (15 T1)
#include "imsearch.h" // the client-side filtering context (16 T2)
#include "ui/fonts.h" // real monospace font + Codicons (13 F3)

#include <GLFW/glfw3.h> // drags in the system OpenGL headers

#include "ui/gl_scene_host.h"
#include "ui/shell.h"
#include "views/observer_draw.h" // obs_graph_enable (node-editor canvas, 15 T3)

// The vendored font paths are injected by mk/desktop.mk (DESKTOP_FONT_DEFS);
// default to empty so a build without them still compiles (load_fonts then
// degrades to the built-in bitmap font).
#ifndef ASMTEST_JBM_TTF
#define ASMTEST_JBM_TTF ""
#endif
#ifndef ASMTEST_CODICON_TTF
#define ASMTEST_CODICON_TTF ""
#endif
#ifndef ASMTEST_FA_TTF
#define ASMTEST_FA_TTF ""
#endif

static void glfw_error(int code, const char *desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

int main() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "asmtest-desktop: glfwInit failed\n");
        return 1;
    }

#ifdef __APPLE__
    // macOS ships no compatibility profile above 2.1 and no GLSL 130: asking for
    // 3.0 there yields a legacy 2.1 context whose shader compile then fails at
    // RUNTIME, which is why this is a real branch and not a portability nicety.
    // 3.2 core + GLSL 150 is the only modern context the platform offers, and
    // core profiles there must be forward-compatible.
    const char *glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    // GL 3.0 + GLSL 130 — the widest desktop baseline the ImGui OpenGL3 backend
    // supports out of the box (its bundled loader needs no glad/glew).
    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

#ifdef ASMTEST_DESKTOP_RENDER_ONLY
    const char *title = "asmtest viewer (render-only)";
#else
    const char *title = "asmtest desktop";
#endif

    GLFWwindow *window = glfwCreateWindow(1280, 720, title, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "asmtest-desktop: window creation failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Docking (13-foundation-moves.md F1/T2): dockable/tearable panes + a
    // persisted layout, for the REAL app only. The headless null-backend tests
    // create their own contexts and deliberately leave both off, so they stay
    // deterministic and write no file. Multi-viewports stay OFF by design (dead
    // on Wayland, broken on X11 per the official wiki).
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // A real ini now persists window/dock layout (the `b` docking hotfix makes
    // loading table settings safe). Kept under build/ so it is git-ignored.
    ImGui::GetIO().IniFilename = "build/desktop-imgui.ini";
    // The ImPlot context lives beside the ImGui one (15 T1). Only the real app
    // creates it; the headless view tests create none, so every ImPlot draw is
    // guarded on ImPlot::GetCurrentContext() and degrades to text there.
    ImPlot::CreateContext();
    ImSearch::
        CreateContext(); // client-side filtering (16 T2), app-only like ImPlot
    // The graph views' node-editor canvas (15 T3) is app-only too: node-editor
    // reaches into ImGui internals, so the headless null-backend view tests never
    // enable it (they fall back to the list/table). The real app + viewer do.
    asmdesk::obs_graph_enable(true);
    // Real monospace font + merged Codicons (13 F3). Paths are compiled in
    // (ASMTEST_*_TTF); a stripped install degrades honestly to the bitmap font.
    asmdesk::load_fonts(ImGui::GetIO(), ASMTEST_JBM_TTF, ASMTEST_CODICON_TTF,
                        ASMTEST_FA_TTF);
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    asmdesk::ShellState state;

    // The 3D-overview GL host (10-spacetime-3d-overview.md — the integration
    // surfacing pass): a render-to-texture bridge the backend-free shell reaches
    // through ShellState::scene_host. Built here, where the GL context is current,
    // and reset BEFORE the context is torn down so the scene's GL objects free
    // while it still exists. Links no engine, so it is in the viewer too (D4).
    std::unique_ptr<asmdesk::SceneHost> scene_host =
        asmdesk::make_gl_scene_host();
    scene_host->init();
    state.scene_host = scene_host.get();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        asmdesk::draw_shell(state);

        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Free the scene's GL objects while the context is still current, then the
    // backends and window.
    state.scene_host = nullptr;
    scene_host->shutdown();
    scene_host.reset();

    ImSearch::DestroyContext();
    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
