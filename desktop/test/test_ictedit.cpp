// test_ictedit.cpp — the Author-door code editor (17-T2). The point of replacing
// InputTextMultiline was to end the fixed-capacity SILENT TRUNCATION; this pins
// that the editor round-trips a source far larger than the old 64 KB buffer, and
// that read-only toggles. Runs headless (SetText/GetText are buffer ops).
#include <cstdio>
#include <string>

#include "imgui.h"

#include "TextEditor.h"

static int fails;
static void check(const char *what, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    TextEditor ed;
    // ~80 KB of source — well past the old 64 KB InputTextMultiline capacity.
    std::string big;
    for (int i = 0; i < 20000; i++)
        big += "nop\n";
    check("source is past the old 64 KB cap", big.size() > 64u * 1024u);

    ed.SetText(big);
    std::string out = ed.GetText();
    check("editor does not truncate a >64 KB source", out.size() >= 64u * 1024u);
    check("content survives the round-trip",
          out.find("nop") != std::string::npos);

    ed.SetReadOnlyEnabled(true);
    check("read-only toggles on", ed.IsReadOnlyEnabled());
    ed.SetReadOnlyEnabled(false);
    check("read-only toggles off", !ed.IsReadOnlyEnabled());

    ImGui::DestroyContext();
    if (fails) {
        std::fprintf(stderr, "test_ictedit: %d FAILURE(S)\n", fails);
        return 1;
    }
    std::printf("test_ictedit: all checks passed\n");
    return 0;
}
