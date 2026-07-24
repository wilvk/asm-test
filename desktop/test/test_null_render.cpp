// test_null_render.cpp — proves Dear ImGui builds and renders headlessly with no
// display, no GL context and no app backend (the example_null pattern:
// $(IMGUI_HOME)/examples/example_null/main.cpp). This is the smoke that keeps the
// desktop build honest on any host with only a C++17 compiler — the same null
// backend every desktop-test drives (03-desktop-shell.md T2).
//
// No GLFW, no OpenGL, no engines: it links the four ImGui core TUs and nothing
// else.
#include <cstdio>

#include "imgui.h"

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini written into the working tree

    // A font atlas is mandatory before NewFrame; build it with no GPU upload.
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    if (!px || w <= 0 || h <= 0) {
        std::fprintf(stderr, "test_null_render: FAIL: empty font atlas\n");
        return 1;
    }

    for (int n = 0; n < 3; n++) {
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Begin("null-render smoke");
        ImGui::Text("frame %d", n);
        ImGui::End();
        ImGui::Render();
        if (ImGui::GetDrawData() == nullptr) {
            std::fprintf(stderr, "test_null_render: FAIL: null draw data\n");
            return 1;
        }
    }

    ImGui::DestroyContext();
    std::printf("test_null_render: PASS\n");
    return 0;
}
