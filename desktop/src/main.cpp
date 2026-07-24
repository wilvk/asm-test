// main.cpp — the desktop entry point, shared by BOTH binaries. A compile-time
// ASMTEST_DESKTOP_RENDER_ONLY guard flips the window title (and, via the shell,
// disables the engine doors); the build (mk/desktop.mk) sets it for the
// render-only viewer and links no engines there (D4). The draw code lives in
// ui/shell.cpp and is backend-free, so the null-backend tests exercise the same
// frames this loop drives. Stock Dear ImGui GLFW + OpenGL3 example shape
// (03-desktop-shell.md T6).
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h> // drags in the system OpenGL headers

#include "ui/shell.h"

static void glfw_error(int code, const char *desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

int main() {
    glfwSetErrorCallback(glfw_error);
    if (!glfwInit()) {
        std::fprintf(stderr, "asmtest-desktop: glfwInit failed\n");
        return 1;
    }

    // GL 3.0 + GLSL 130 — the widest desktop baseline the ImGui OpenGL3 backend
    // supports out of the box (its bundled loader needs no glad/glew).
    const char *glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

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
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini side-effect
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    asmdesk::ShellState state;

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

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
