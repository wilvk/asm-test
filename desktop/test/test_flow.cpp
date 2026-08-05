// test_flow.cpp — the narrow-pane layout primitives (ui/flow.h), driven headless
// under the ImGui null backend. The whole point of the module is what happens at
// a width no screenshot lane ever renders, so the test drives the SAME row and
// the SAME rail pair at a wide and a narrow window and asserts the two behave
// differently — a test that only ever ran at 1280px would pass against the very
// bug this replaces.
#include <cstdio>

#include "imgui.h"

#include "ui/flow.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

// Begin a frame with a window of exactly `w` x `h` content, run `body`, end.
template <typename F> static void in_window(float w, float h, F body) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w + 200.0f, h + 200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("pane", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
    body();
    ImGui::End();
    ImGui::Render();
}

// Draw the four-button control row the Live-capture pane draws, counting how
// many buttons ended up on a line of their own. Returns the wrap count.
static int draw_button_row(const char *const *labels, int n) {
    int wraps = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && flow_same_line(flow_button_w(labels[i])))
            wraps++;
        ImGui::Button(labels[i]);
    }
    return wraps;
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int tw = 0, th = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &tw, &th);

    // The transport row from the Live-capture pane (ui/inspect_door.cpp).
    const char *row[] = {"Start", "Stop", "Pause", "Resume"};

    // --- measuring ----------------------------------------------------------
    // The widths have to be positive and to ORDER like their labels, or the fit
    // test downstream is measuring nothing.
    in_window(800, 600, [&] {
        check("measure/positive", flow_button_w("Start") > 0.0f,
              "a button must measure wider than zero");
        check("measure/orders", flow_button_w("Resume") > flow_button_w("Stop"),
              "a longer label must measure wider");
        // "##id" is chrome, not text: it must not inflate the measurement.
        check("measure/hides-id",
              flow_button_w("Cancel##swap") == flow_button_w("Cancel"),
              "the ##id suffix must not count toward a button's width");
        // Text does NOT hide it — Text draws the string verbatim.
        check("measure/text-verbatim",
              flow_text_w("Cancel##swap") > flow_text_w("Cancel"),
              "Text draws ## verbatim, so it must measure wider");
        check("measure/checkbox-has-box",
              flow_checkbox_w("") >= ImGui::GetFrameHeight(),
              "an unlabelled checkbox is still a box wide");
    });

    // --- wide: the row stays on one line ------------------------------------
    int wide_wraps = -1;
    float wide_h = 0.0f;
    in_window(800, 600, [&] {
        const float y0 = ImGui::GetCursorPosY();
        wide_wraps = draw_button_row(row, 4);
        wide_h = ImGui::GetCursorPosY() - y0;
    });
    check("wide/no-wrap", wide_wraps == 0,
          "at 800px the four transport buttons must share one line");

    // --- narrow: the row wraps instead of vanishing -------------------------
    int narrow_wraps = -1;
    float narrow_h = 0.0f;
    in_window(160, 600, [&] {
        const float y0 = ImGui::GetCursorPosY();
        narrow_wraps = draw_button_row(row, 4);
        narrow_h = ImGui::GetCursorPosY() - y0;
    });
    check("narrow/wraps", narrow_wraps > 0,
          "at 160px the transport row must wrap rather than run off the edge");
    // Wrapping is only a fix if it SPENDS the vertical axis — the one every
    // pane scrolls. Same controls, more rows.
    check("narrow/grows-taller", narrow_h > wide_h,
          "a wrapped row must be taller than the same row unwrapped");

    // Every button must land inside the content region: the actual contract, and
    // the one the old bare SameLine() broke. Checked on the rects ImGui itself
    // computed, not on the caller's arithmetic.
    in_window(160, 600, [&] {
        const float left = ImGui::GetCursorScreenPos().x;
        const float right = left + ImGui::GetContentRegionAvail().x;
        bool all_in = true;
        for (int i = 0; i < 4; i++) {
            if (i > 0)
                flow_same_line(flow_button_w(row[i]));
            ImGui::Button(row[i]);
            if (ImGui::GetItemRectMin().x < left - 0.5f)
                all_in = false;
            // A button that STARTS past the right edge is the unreachable case.
            if (ImGui::GetItemRectMin().x > right + 0.5f)
                all_in = false;
        }
        check("narrow/all-reachable", all_in,
              "no control may start outside the pane's content region");
    });

    // A single item wider than the whole pane still starts at the left edge —
    // clipped at its tail, but legible and clickable, not drawn off-screen.
    in_window(80, 600, [&] {
        ImGui::Button("x");
        const float left = ImGui::GetCursorScreenPos().x;
        const bool wrapped =
            flow_same_line(flow_button_w("Start anyway (accept the risk)"));
        ImGui::Button("Start anyway (accept the risk)");
        check("overwide/wraps", wrapped,
              "an item wider than the pane must wrap, not trail off the edge");
        check("overwide/starts-at-left",
              ImGui::GetItemRectMin().x <= left + 0.5f,
              "an over-wide item must begin at the content region's left edge");
    });

    // --- the rail pair ------------------------------------------------------
    // Wide: side by side, the rail keeping the width it asked for.
    in_window(800, 600, [&] {
        FlowRail r = flow_rail(320.0f, 240.0f);
        check("rail/wide-side-by-side", !r.stacked,
              "at 800px a 320px rail must stay beside its main area");
        check("rail/wide-keeps-width", r.rail.x == 320.0f,
              "a rail that fits must keep its requested width");
        check("rail/wide-main-fills", r.main.x == 0.0f && r.main.y == 0.0f,
              "the main area must fill whatever the rail leaves");
    });

    // Narrow: stacked, both full width, the rail height-bounded. This is the
    // case where the old code drew the main area entirely off the right edge.
    in_window(360, 600, [&] {
        FlowRail r = flow_rail(320.0f, 240.0f);
        check("rail/narrow-stacks", r.stacked,
              "at 360px a 320px rail + 240px main cannot sit side by side");
        check("rail/narrow-full-width", r.rail.x == 0.0f && r.main.x == 0.0f,
              "a stacked rail and main area both span the full width");
        check("rail/narrow-rail-bounded", r.rail.y > 0.0f && r.rail.y < 600.0f,
              "a stacked rail must take a bounded share of the height");
        check("rail/narrow-main-survives",
              ImGui::GetContentRegionAvail().y - r.rail.y > 0.0f,
              "the main area must have height left after the rail");
    });

    // The rail floor holds even when the fraction would go below it.
    in_window(360, 400, [&] {
        FlowRail r = flow_rail(320.0f, 240.0f, 0.40f, 120.0f);
        check("rail/floor", r.rail.y >= 120.0f,
              "the rail must never fall below its minimum height");
    });

    // Too SHORT for both: each gets its floor and the overflow goes to the
    // pane's own vertical scrollbar. Squeezing the main area to zero here would
    // reintroduce the vanished-pane bug on the other axis.
    in_window(360, 150, [&] {
        FlowRail r = flow_rail(320.0f, 240.0f, 0.40f, 120.0f);
        check("rail/short-both-nonzero", r.rail.y > 0.0f && r.main.y > 0.0f,
              "in a short pane BOTH children must still get height");
    });

    // The boundary is the rail + the spacing + the main minimum, not the rail
    // alone: a pane with room for the rail but not the main area must stack.
    in_window(340.0f + 2.0f * ImGui::GetStyle().WindowPadding.x, 600, [&] {
        FlowRail r = flow_rail(320.0f, 240.0f);
        check("rail/boundary-counts-main", r.stacked,
              "room for the rail alone is not room for the pair");
    });

    ImGui::DestroyContext();

    if (failures) {
        std::fprintf(stderr, "test_flow: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_flow: all checks passed\n");
    return 0;
}
