// test_ui.cpp — the imgui_test_engine interaction lane (docs/internal/gui/
// 17-interaction-testing-and-editor.md T1).
//
// This is the ONE binary that links the Dear ImGui Test Engine (Test Engine
// License v1.04 — test-lane-only, fetched-at-build, never bundled; see
// licenses/README.md). It runs HEADLESS on the null backend the rest of
// desktop-test already uses: no GLFW, no GL, no display. It drives the real
// draw code through simulated clicks and keypresses — the layer the golden-text
// tests cannot reach — and writes JUnit XML for CI.
//
// It is built in the `uitest` tree (mk/desktop.mk), i.e. the shell-test object
// set recompiled with -DIMGUI_ENABLE_TEST_ENGINE so imgui's item hooks fire.
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

#include "imgui_te_context.h"
#include "imgui_te_engine.h"
#include "imgui_te_exporters.h" // ImGuiTestEngineExportFormat_JUnitXml

#include "nav.h"       // dt_view / dt_link — asserted by the keymap tests
#include "ui/shell.h"  // ShellState, draw_shell, shell_open, shell_a (17-T1)

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using asmdesk::dt_link;
using asmdesk::dt_view;
using asmdesk::ShellState;

// One shell for the keymap tests, loaded once with three recordings: a dataflow
// trace (idx 0, for step/cone/Enter) and a diverging pair (idx 1/2, for d/x/n/p).
// Each test sets the preconditions it needs, so order between tests never leaks.
static ShellState g_keymap_shell;
static bool g_keymap_loaded = false;
static void ensure_keymap_shell() {
    if (g_keymap_loaded)
        return;
    g_keymap_loaded = true;
    const std::string dir = ASMTEST_GOLDEN_DIR;
    std::string err;
    asmdesk::shell_open(g_keymap_shell, dir + "/sum_via_rbx.asmtrace", err);
    asmdesk::shell_open(g_keymap_shell, dir + "/views/pair-a.asmtrace", err);
    asmdesk::shell_open(g_keymap_shell, dir + "/views/pair-b.asmtrace", err);
}

// Registered by each test section below; a trivial counter proves the harness
// actually drives the GuiFunc + locates items headlessly.
static int g_button_presses = 0;

// A frame of the REAL shell over the keymap fixture. The engine calls this each
// frame; the keymap tests inject keys and assert the resulting ShellState.
static void keymap_gui(ImGuiTestContext *) {
    ensure_keymap_shell();
    asmdesk::draw_shell(g_keymap_shell);
}

// Keymap enforcement (17-T1 step 5) — the real payoff: each advertised binding
// (dt_nav_bindings) gets a test that presses it and asserts the resulting
// ShellState. The bindings are pure state moves (handle_keymap), so this drives
// the MODEL, not pixels. `[`/`]` are covered by the scrubber/slice draws' own
// tests, not here. Before these existed, ten of the twelve bindings were dead.
static void register_keymap_tests(ImGuiTestEngine *engine) {
    ImGuiTest *t = nullptr;

    // 1/2/3/4 — switch the active recording's view (want_view -> the view tab
    // bar's SetSelected -> s.view, all within the frame).
    t = IM_REGISTER_TEST(engine, "keymap", "view_1234");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_2);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.view == dt_view::timeline);
        ctx->KeyPress(ImGuiKey_3);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.view == dt_view::slice);
        ctx->KeyPress(ImGuiKey_4);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.view == dt_view::diff);
        ctx->KeyPress(ImGuiKey_1);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.view == dt_view::canvas);
    };

    // j/k, Down/Up — next / previous step within the step space.
    t = IM_REGISTER_TEST(engine, "keymap", "step_nav");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.selected_step = 0u;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_J);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selected_step.value_or(99u) == 1u);
        ctx->KeyPress(ImGuiKey_K);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selected_step.value_or(99u) == 0u);
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selected_step.value_or(99u) == 1u);
    };

    // Enter — open the slice explorer at the selection, cone lit.
    t = IM_REGISTER_TEST(engine, "keymap", "enter_opens_slice");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.view = dt_view::canvas;
        g_keymap_shell.cone_active = false;
        g_keymap_shell.selected_step = 0u;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.view == dt_view::slice);
        IM_CHECK(g_keymap_shell.cone_active);
    };

    // b / f — light the backward / forward cone; c — clear it.
    t = IM_REGISTER_TEST(engine, "keymap", "cones_bfc");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.selected_step = 0u;
        g_keymap_shell.cone_active = false;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_B);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.cone_active && !g_keymap_shell.cone_fwd);
        ctx->KeyPress(ImGuiKey_F);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.cone_active && g_keymap_shell.cone_fwd);
        ctx->KeyPress(ImGuiKey_C);
        ctx->Yield();
        IM_CHECK(!g_keymap_shell.cone_active);
    };

    // d — attach/detach a second recording; x — swap A and B.
    t = IM_REGISTER_TEST(engine, "keymap", "diff_d_x");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.b_index = -1;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_D);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.b_index == 1); // the first OTHER open recording
        ctx->KeyPress(ImGuiKey_X);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.active_tab == 1 && g_keymap_shell.b_index == 0);
        ctx->KeyPress(ImGuiKey_D); // a B is attached -> detach
        ctx->Yield();
        IM_CHECK(g_keymap_shell.b_index == -1);
    };

    // n / p — walk to a divergent offset of the attached pair, or say there is
    // none to walk to — never a silent no-op.
    t = IM_REGISTER_TEST(engine, "keymap", "divergence_np");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 1; // select pair-a (recording 1) as A
        g_keymap_shell.b_index = 2;       // pair-b as B
        g_keymap_shell.selected_off.reset();
        g_keymap_shell.status.clear();
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_N);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selected_off.has_value() ||
                 !g_keymap_shell.status.empty());
    };

    // y — copy a deep link to the current position to the clipboard.
    t = IM_REGISTER_TEST(engine, "keymap", "copy_link_y");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        dt_link cur;
        cur.view = dt_view::slice;
        cur.rec = "sum_via_rbx.asmtrace";
        cur.step = 2;
        g_keymap_shell.nav.current = cur;
        ImGui::SetClipboardText("");
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Y);
        ctx->Yield();
        const char *clip = ImGui::GetClipboardText();
        IM_CHECK(clip != nullptr && std::strlen(clip) > 0);
    };

    // Ctrl+G — open the go-to modal (show_goto stays true while it is up).
    t = IM_REGISTER_TEST(engine, "keymap", "goto_ctrl_g");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.show_goto = false;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_G);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.show_goto);
        // Dismiss it so it does not hold text-input focus into later runs.
        ctx->ItemClick("Go to/Cancel");
        ctx->Yield();
    };
}

// register_tests — every interaction test hangs off here. T1 seeds it with a
// harness self-check; the keymap tests (T1 step 5) and the door/flow tests
// (step 4) extend this same function.
static void register_tests(ImGuiTestEngine *engine) {
    register_keymap_tests(engine);

    ImGuiTest *t = IM_REGISTER_TEST(engine, "harness", "button_click");
    t->GuiFunc = [](ImGuiTestContext *) {
        ImGui::Begin("Harness Window", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button("Press Me"))
            g_button_presses++;
        ImGui::Text("presses=%d", g_button_presses);
        ImGui::End();
    };
    t->TestFunc = [](ImGuiTestContext *ctx) {
        ctx->SetRef("Harness Window");
        ctx->ItemClick("Press Me");
        // If the item hooks are compiled in and the engine drives the null
        // backend, this click lands and the app-side handler runs.
        IM_CHECK_EQ(g_button_presses, 1);
    };
}

int main() {
    // 1) A null-backend ImGui context, exactly as the other desktop tests build
    // one: no backend, no ini file, an explicit display size, and a built atlas
    // (the engine's first NewFrame asserts the atlas exists).
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // 2) Create + configure the engine, then bind it to the current context.
    // Fast + no-throttle so it runs at CPU speed (Cinematic inserts real-time
    // sleeps and looks like a hang); no screenshots headless.
    ImGuiTestEngine *engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO &te = ImGuiTestEngine_GetIO(engine);
    te.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
    te.ConfigNoThrottle = true;
    te.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
    te.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Info;
    te.ConfigLogToTTY = true;
    te.ConfigCaptureEnabled = false;
    te.ExportResultsFilename = "build/desktop-ui-test-results.xml";
    te.ExportResultsFormat = ImGuiTestEngineExportFormat_JUnitXml;
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());

    register_tests(engine);

    // 3) Queue every test and pump frames until the queue drains. The engine
    // advances the running test's coroutine in the NewFrame hooks, so the loop
    // MUST call NewFrame each iteration; a frame cap turns a genuinely stuck
    // test into a failure instead of an infinite spin.
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr,
                               ImGuiTestRunFlags_RunFromCommandLine);
    int frame = 0;
    const int kMaxFrames = 20000;
    while (!ImGuiTestEngine_IsTestQueueEmpty(engine) && frame < kMaxFrames) {
        io.DisplaySize = ImVec2(1280, 800);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::Render();
        ImGuiTestEngine_PostSwap(engine);
        frame++;
    }

    // 4) Results, then Stop (which flushes the JUnit XML and joins the coroutine
    // thread), then tear down — imgui's context BEFORE the engine's.
    int tested = 0, ok = 0;
    ImGuiTestEngine_GetResult(engine, tested, ok);
    const bool queue_drained = ImGuiTestEngine_IsTestQueueEmpty(engine);
    std::printf("test_ui: %d/%d test(s) passed (%d frames)\n", ok, tested, frame);
    if (!queue_drained)
        std::fprintf(stderr,
                     "test_ui: FAIL — test queue did not drain in %d frames "
                     "(a test hung)\n",
                     kMaxFrames);

    ImGuiTestEngine_Stop(engine);
    ImGui::DestroyContext();
    ImGuiTestEngine_DestroyContext(engine);

    const bool passed = queue_drained && tested > 0 && (tested - ok) == 0;
    if (!passed) {
        std::fprintf(stderr, "test_ui: %d FAILURE(S)\n",
                     (tested - ok) + (queue_drained ? 0 : 1));
        return 1;
    }
    std::printf("test_ui: all checks passed\n");
    return 0;
}
