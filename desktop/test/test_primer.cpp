// test_primer.cpp — the first-open primer (24-one-visual-language.md T5 / F4/F3).
// Model state, not pixels (D4):
//  (a) `dismissed` is false on first open (the primer shows); dismissing sets it
//      true (hidden thereafter within the session);
//  (b) the per-view "?" re-open action clears `dismissed` (the primer shows
//      again — the teaching is never lost after first dismiss);
//  (c) while unacknowledged the view holds its lean default (dt_primer_lean).
//  Plus a null-backend smoke that the card draws (engine-free).
#include <cstdio>
#include <string>

#include "imgui.h"

#include "ui/primer.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    dt_primer_state st;

    // --- (a) first open shows; lean default holds ----------------------------
    check("first-open-active", dt_primer_active(st),
          "a fresh primer must be active on first open");
    check("first-open-lean", dt_primer_lean(st),
          "an unacknowledged view must hold its lean default");
    check("not-yet-shown", !st.ever_shown, "ever_shown starts false");

    // Draw one frame: the card renders under the null backend, and drawing marks
    // it ever_shown (still active until dismissed).
    ImGui::NewFrame();
    ImGui::Begin("host");
    bool drew = dt_primer(
        "test-primer", "Title", "Body paragraph.", [] {}, st);
    ImGui::End();
    ImGui::Render();
    check("drew-when-active", drew, "the card should draw while active");
    check("ever-shown", st.ever_shown, "drawing must set ever_shown");
    check("still-active-after-draw", dt_primer_active(st),
          "drawing alone (no dismiss) must not hide the primer");

    // --- dismiss: hidden thereafter, lean default lifts -----------------------
    dt_primer_dismiss(st);
    check("dismissed-inactive", !dt_primer_active(st),
          "after 'Got it' the primer must be hidden");
    check("dismissed-not-lean", !dt_primer_lean(st),
          "after acknowledgement the view may show its full self");
    // A dismissed primer draws nothing and returns false.
    ImGui::NewFrame();
    ImGui::Begin("host");
    bool drew2 = dt_primer(
        "test-primer", "Title", "Body.", [] {}, st);
    ImGui::End();
    ImGui::Render();
    check("no-draw-when-dismissed", !drew2,
          "a dismissed primer must not draw");

    // --- (b) the "?" re-open action brings it back ----------------------------
    dt_primer_reopen(st);
    check("reopen-active", dt_primer_active(st),
          "re-open must show the primer again");
    check("reopen-lean", dt_primer_lean(st),
          "re-opened, the view holds the lean default again until acknowledged");

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d test_primer check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_primer: all checks passed\n");
    return 0;
}
