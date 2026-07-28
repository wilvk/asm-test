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

#include "nav.h"                 // dt_view / dt_link — asserted by the keymap tests
#include "ui/layout.h"           // 19: kPane* names, layout_build, LayoutPreset
#include "ui/shell.h"            // ShellState, draw_shell, shell_open, shell_a (17-T1)
#include "views/observer_draw.h" // draw_obs_syscalls — the reveal-all flow test
#include "views/syscalls.h"      // SyscallView / SyscallRow

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
        g_keymap_shell.selection.step = 0u;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_J);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(99u) == 1u);
        ctx->KeyPress(ImGuiKey_K);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(99u) == 0u);
        ctx->KeyPress(ImGuiKey_DownArrow);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(99u) == 1u);
    };

    // Enter — open the slice explorer at the selection, cone lit.
    t = IM_REGISTER_TEST(engine, "keymap", "enter_opens_slice");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // select recording 0's outer tab
        g_keymap_shell.view = dt_view::canvas;
        g_keymap_shell.cone_active = false;
        g_keymap_shell.selection.step = 0u;
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
        g_keymap_shell.selection.step = 0u;
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
        g_keymap_shell.selection.off.reset();
        g_keymap_shell.status.clear();
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_N);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.off.has_value() ||
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

    // --- 18-breach-stops.md T1: the convention-alignment keys --------------

    // Shift+F — fit / frame the current selection (want_fit intent). Plain `f`
    // stays the forward cone (the cones_bfc test above), so this must NOT fire it.
    t = IM_REGISTER_TEST(engine, "keymap", "fit_shift_f");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.want_fit = false;
        g_keymap_shell.cone_active = false;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_F);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.want_fit);
        IM_CHECK(!g_keymap_shell.cone_active); // Shift+F is fit, not the cone
    };

    // , / . — step to the previous / next sibling (adjacent step).
    t = IM_REGISTER_TEST(engine, "keymap", "sibling_comma_period");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.selection.step = 1u;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_Period);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(0u) == 2u);
        ctx->KeyPress(ImGuiKey_Comma);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(9u) == 1u);
    };

    // F10 / F11 — step / step back (j / k aliases).
    t = IM_REGISTER_TEST(engine, "keymap", "step_f10_f11");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.selection.step = 1u;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_F10);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(0u) == 2u);
        ctx->KeyPress(ImGuiKey_F11);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.selection.step.value_or(9u) == 1u);
    };

    // Ctrl+C — copy a deep link, an alias of `y` (and NOT the cone-clear `c`).
    t = IM_REGISTER_TEST(engine, "keymap", "copy_link_ctrl_c");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.cone_active = true;
        dt_link cur;
        cur.view = dt_view::slice;
        cur.rec = "sum_via_rbx.asmtrace";
        cur.step = 2;
        g_keymap_shell.nav.current = cur;
        ImGui::SetClipboardText("");
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_C);
        ctx->Yield();
        const char *clip = ImGui::GetClipboardText();
        IM_CHECK(clip != nullptr && std::strlen(clip) > 0);
        IM_CHECK(g_keymap_shell.cone_active); // Ctrl+C must not clear the cone
    };

    // W/S/A/D — camera zoom/pan ONLY in a spatial context (wasd_context); there
    // `d` drives pan and does NOT toggle the diff — the labelled context switch.
    t = IM_REGISTER_TEST(engine, "keymap", "wasd_camera_context");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.wasd_context = true;
        g_keymap_shell.wasd_zoom = 0;
        g_keymap_shell.wasd_pan = 0;
        g_keymap_shell.b_index = -1;
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_W);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.wasd_zoom == 1); // W zoomed
        ctx->KeyPress(ImGuiKey_D);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.wasd_pan == 1);   // D panned...
        IM_CHECK(g_keymap_shell.b_index == -1);   // ...and did NOT toggle the diff
        // Leave the shared shell as the diff test expects it (camera off).
        g_keymap_shell.wasd_context = false;
    };

    // --- 18-breach-stops.md T2: the reset-layout intent --------------------
    t = IM_REGISTER_TEST(engine, "keymap", "reset_layout_ctrl_shift_r");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_layout_reset = false;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.want_layout_reset);
    };

    // --- 18-breach-stops.md T6: Alt+Left / Alt+Right drive the router ------
    t = IM_REGISTER_TEST(engine, "keymap", "history_alt_arrows");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        g_keymap_shell.want_open_tab = 0;
        // Build a two-step history through the real router.
        g_keymap_shell.nav.back.clear();
        g_keymap_shell.nav.forward.clear();
        dt_link a;
        a.rec = "sum_via_rbx.asmtrace";
        a.view = dt_view::canvas;
        dt_link b = a;
        b.view = dt_view::timeline;
        dt_nav_go(g_keymap_shell.nav, a);
        dt_nav_go(g_keymap_shell.nav, b); // back=[a], current=b
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_LeftArrow);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.nav.current.has_value());
        IM_CHECK(dt_nav_format(*g_keymap_shell.nav.current) == dt_nav_format(a));
        ctx->KeyPress(ImGuiMod_Alt | ImGuiKey_RightArrow);
        ctx->Yield();
        IM_CHECK(dt_nav_format(*g_keymap_shell.nav.current) == dt_nav_format(b));
    };

    // --- a96f64a: Ctrl+<letter> chords must NOT leak into plain-letter handlers.
    // ImGui's IsKeyPressed() ignores modifiers, so before the fix a Ctrl chord
    // ALSO fired the bare-letter action. Each test drives the REAL handle_keymap
    // through draw_shell and asserts the plain-letter side effect did NOT fire —
    // these FAIL on the pre-fix code (they observe the leaked state directly).

    // Fix 1 — Ctrl+F opens the global find, and must NOT light the forward cone
    // (plain `f`) nor push the spurious cone undo command that leak produced.
    t = IM_REGISTER_TEST(engine, "keymap", "ctrl_f_no_cone_leak");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.selection.step = 0u;
        g_keymap_shell.cone_active = false;
        g_keymap_shell.cone_fwd = false;
        g_keymap_shell.find = asmdesk::FindState{};
        g_keymap_shell.undo.clear();
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.find.open);        // Ctrl+F opened find...
        IM_CHECK(!g_keymap_shell.cone_active);     // ...and did NOT light the cone
        IM_CHECK(!g_keymap_shell.undo.can_undo()); // ...nor push a cone undo command
        g_keymap_shell.find.open = false;          // release the find bar's focus
        ctx->Yield();
    };

    // Fix 2 — Ctrl+P opens the command palette, and must NOT walk the n/p
    // divergence of the attached pair (plain `p`). A/B carry real divergences, so a
    // leaked walk WOULD move selection.off or set status; assert neither happened.
    t = IM_REGISTER_TEST(engine, "keymap", "ctrl_p_no_divergence_walk");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 1; // pair-a (recording 1) as A
        g_keymap_shell.b_index = 2;       // pair-b as B
        g_keymap_shell.selection.off.reset();
        g_keymap_shell.status.clear();
        g_keymap_shell.show_palette = false;
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_P);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.show_palette);               // Ctrl+P opened it...
        IM_CHECK(!g_keymap_shell.selection.off.has_value()); // ...no divergence walk
        IM_CHECK(g_keymap_shell.status.empty());             // ...and no n/p status
        // Dismiss the palette through its OWN dispatch/close path (the proven idiom
        // the command_palette flow test uses), so no modal holds focus into later
        // tests; then undo the harmless side effect that close command raised.
        ctx->SetRef("Command palette");
        ctx->ItemInput("##palettequery");
        ctx->KeyCharsReplace("Reset panel");
        ctx->Yield();
        ctx->ItemClick("Reset panel layout");
        ctx->Yield();
        IM_CHECK(!g_keymap_shell.show_palette); // the palette closed
        g_keymap_shell.want_layout_reset = false;
        ctx->Yield();
    };

    // Fix 3 — Ctrl+Y redoes, and must NOT copy a deep link to the clipboard
    // (plain `y`). A deep link IS available (nav.current), so before the fix
    // plain-`y` fired and overwrote the clipboard; the fix leaves it untouched.
    t = IM_REGISTER_TEST(engine, "keymap", "ctrl_y_no_copy_link");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.undo.clear(); // empty stack -> Ctrl+Y's redo is a pure no-op
        dt_link cur;
        cur.view = dt_view::slice;
        cur.rec = "sum_via_rbx.asmtrace";
        cur.step = 2;
        g_keymap_shell.nav.current = cur;
        const char *kSentinel = "SENTINEL-not-a-link";
        ImGui::SetClipboardText(kSentinel);
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y);
        ctx->Yield();
        const char *clip = ImGui::GetClipboardText();
        IM_CHECK(clip != nullptr && std::strcmp(clip, kSentinel) == 0);
    };
}

// The syscall view under test for the reveal-all flow (its reveal state lives
// in the view, so it persists across frames — must outlive the GuiFunc).
static asmdesk::SyscallView g_sys;
static asmdesk::ObserverState g_sys_os;
static void syscalls_gui(ImGuiTestContext *) {
    ImGui::Begin("SyscallFlow", nullptr, ImGuiWindowFlags_NoSavedSettings);
    asmdesk::draw_obs_syscalls(g_sys, g_sys_os);
    ImGui::End();
}

// The task rail under test (20 T2): its mode + pending_preset live in the shell,
// so the shell outlives the GuiFunc.
static asmdesk::ShellState g_rail_shell;
static void rail_gui(ImGuiTestContext *) {
    ImGui::Begin("RailHarness", nullptr, ImGuiWindowFlags_NoSavedSettings);
    asmdesk::draw_home_rail(g_rail_shell);
    ImGui::End();
}

// Interaction flow tests (17-T1 step 4): the multi-step confirmations the
// golden-text tests cannot reach, driven by real clicks. The syscall reveal-all
// is the sharpest example — a DESTRUCTIVE-feeling action (unredact every
// payload) deliberately gated behind a two-step arm/confirm (D7), which only a
// click-driver can exercise end to end.
static void register_flow_tests(ImGuiTestEngine *engine) {
    ImGuiTest *t = IM_REGISTER_TEST(engine, "flow", "syscall_reveal_all_two_step");
    t->GuiFunc = syscalls_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        // A view with one payload-bearing row, redacted by default.
        g_sys = SyscallView{};
        SyscallRow r;
        r.line = "openat(AT_FDCWD, <path>) = 3";
        r.has_payload = true;
        r.payload = "/etc/shadow";
        g_sys.rows.push_back(r);
        g_sys.revealed.assign(1, 0);
        ctx->SetRef("SyscallFlow");
        ctx->Yield();
        // First press only ARMS (never reveals on a single click).
        ctx->ItemClick("Reveal every payload...");
        ctx->Yield();
        IM_CHECK(g_sys.reveal_all_armed);
        IM_CHECK(!g_sys.reveal_all);
        // The confirm performs it.
        ctx->ItemClick("Yes, reveal them");
        ctx->Yield();
        IM_CHECK(g_sys.reveal_all);
    };

    // 20 T2: the rail CTA changes the mode and requests its perspective. An empty
    // shell auto-lands in Learn; clicking a task sentence sets that mode.
    t = IM_REGISTER_TEST(engine, "flow", "rail_cta_sets_mode");
    t->GuiFunc = rail_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        // ShellState is not copyable (its LiveSession is move-only), so reset only
        // the fields under test rather than reassigning the whole struct. It is
        // default-constructed with mode == Learn — the empty-workspace auto-land.
        g_rail_shell.mode = Mode::Learn;
        g_rail_shell.pending_preset.reset();
        ctx->Yield();
        IM_CHECK_EQ((int)g_rail_shell.mode, (int)Mode::Learn); // auto-land
        ctx->SetRef("RailHarness");
        ctx->ItemClick("Author a routine");
        ctx->Yield();
        IM_CHECK_EQ((int)g_rail_shell.mode, (int)Mode::Author);
        IM_CHECK(g_rail_shell.pending_preset.has_value());
    };

    // 21 T1: the command palette. Ctrl+Shift+P opens the modal fuzzy finder over
    // the real shell; typing narrows it and clicking a command dispatches through
    // the spine — asserted at the MODEL (the resulting ShellState), not pixels.
    t = IM_REGISTER_TEST(engine, "flow", "command_palette");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        g_keymap_shell.want_open_tab = 0; // recording 0's outer tab
        g_keymap_shell.show_palette = false;
        g_keymap_shell.palette_buf[0] = '\0';
        g_keymap_shell.want_view.reset();
        g_keymap_shell.view = dt_view::canvas;
        ctx->Yield();
        // Ctrl+Shift+P opens it (Ctrl+P is the alias; both raise show_palette).
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.show_palette);
        // Narrow to the timeline view-switch command and run it by click.
        ctx->SetRef("Command palette");
        ctx->ItemInput("##palettequery");
        ctx->KeyCharsReplace("timeline view");
        ctx->Yield();
        ctx->ItemClick("Show timeline view");
        ctx->Yield();
        ctx->Yield(); // the tab bar consumes want_view -> s.view next frame
        IM_CHECK(!g_keymap_shell.show_palette);          // dispatched + closed
        IM_CHECK(g_keymap_shell.view == dt_view::timeline); // via want_view
    };
}

// --- 22-selection-and-search.md interaction sections -------------------------
// The 3D-camera harness (T2): a dedicated shell over a scene fixture, so the
// keyboard camera is driven through the REAL draw_scene_overview under the null
// backend — no GL, no mouse. Separate from g_keymap_shell so it never disturbs
// the keymap tests' state.
static asmdesk::ShellState g_scene_shell;
static bool g_scene_loaded = false;
static void ensure_scene_shell() {
    if (g_scene_loaded)
        return;
    g_scene_loaded = true;
    std::string err;
    asmdesk::shell_open(g_scene_shell,
                        std::string(ASMTEST_GOLDEN_DIR) + "/scene-abs-loop.asmtrace",
                        err);
    g_scene_shell.active_tab = 0;
}
static void scene_gui(ImGuiTestContext *) {
    ensure_scene_shell();
    ImGui::Begin("SceneHarness", nullptr, ImGuiWindowFlags_NoSavedSettings);
    if (!g_scene_shell.ws.recordings.empty()) {
        const asmdesk::Recording &r = g_scene_shell.ws.recordings[0];
        const asmdesk::Streams *a = asmdesk::shell_a(g_scene_shell);
        if (a != nullptr)
            asmdesk::draw_scene_overview(g_scene_shell, r, *a);
    }
    ImGui::End();
}

// The Loom takes-gutter harness (T4): a bare LoomState + UndoStack drawn through
// the REAL draw_loom_takes_gutter, so the per-take remove + clear-forks buttons
// are exercised by real clicks (they push reversible undo Commands).
static asmdesk::LoomState g_gutter_loom;
static asmdesk::UndoStack g_gutter_undo;
static void gutter_gui(ImGuiTestContext *) {
    ImGui::Begin("GutterHarness", nullptr, ImGuiWindowFlags_NoSavedSettings);
    asmdesk::draw_loom_takes_gutter(g_gutter_loom, &g_gutter_undo);
    ImGui::End();
}

// The Reweave form harness (30 R3 T3): a bare form drawn through the REAL
// draw_loom_reweave_form, so the fork-from-step-K gesture (step K + register +
// value + submit) is exercised by real inputs and a real click.
static asmdesk::loom_reweave_form_t g_reweave_form;
static void reweave_gui(ImGuiTestContext *) {
    ImGui::Begin("ReweaveHarness", nullptr, ImGuiWindowFlags_NoSavedSettings);
    asmdesk::draw_loom_reweave_form(g_reweave_form);
    ImGui::End();
}

static void register_search_tests(ImGuiTestEngine *engine) {
    ImGuiTest *t = nullptr;

    // T3 (F17) — Ctrl+F opens the global find; it MEASURES (count + aggregate
    // cost), and Next cycles the active match. Model state, D4.
    t = IM_REGISTER_TEST(engine, "find", "ctrl_f_measures_and_cycles");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0; // an active recording with dataflow
        g_keymap_shell.find = asmdesk::FindState{};
        ctx->Yield();
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
        ctx->Yield();
        IM_CHECK(g_keymap_shell.find.open);
        // Type a query through the engine so ImGui's own InputText state is the
        // source of truth (every timeline row carries an "0x" offset, so "0x"
        // matches every step); the real draw_find_bar then runs the pure find.
        ctx->SetRef("Find");
        ctx->ItemInput("##findq");
        ctx->KeyCharsAppend("0x");
        ctx->Yield();
        IM_CHECK(!g_keymap_shell.find.hits.empty());   // it found matches
        IM_CHECK(g_keymap_shell.find.total_cost > 0);  // and MEASURED them
        const int before = g_keymap_shell.find.active;
        ctx->ItemClick("Next"); // Enter/Next cycles the active match
        ctx->Yield();
        IM_CHECK(g_keymap_shell.find.active != before);
        g_keymap_shell.find.open = false; // release the find bar's text focus
        ctx->Yield();
    };

    // T4 (F12) — Ctrl+Z / Ctrl+Y reverse and replay a cone change (a reversible
    // view-model command), driven by REAL keys through handle_keymap.
    t = IM_REGISTER_TEST(engine, "undo", "ctrl_z_y_reverses_cone");
    t->GuiFunc = keymap_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        g_keymap_shell.want_open_tab = 0;
        g_keymap_shell.cone_active = false;
        g_keymap_shell.cone_fwd = false;
        g_keymap_shell.undo.clear();
        ctx->Yield();
        ctx->KeyPress(ImGuiKey_B); // light the backward cone (pushes a command)
        ctx->Yield();
        IM_CHECK(g_keymap_shell.cone_active);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z); // undo
        ctx->Yield();
        IM_CHECK(!g_keymap_shell.cone_active);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Y); // redo
        ctx->Yield();
        IM_CHECK(g_keymap_shell.cone_active);
    };

    // T2 (F18) — the keyboard camera and the Tab-reachable viewport target, driven
    // through the REAL draw_scene_overview under the null backend (no GL, no mouse).
    t = IM_REGISTER_TEST(engine, "camera", "keyboard_orbit_reset_topdown");
    t->GuiFunc = scene_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        ctx->Yield(); // weave the scene, draw the HUD + the viewport target
        IM_CHECK(!g_scene_shell.scenes.empty());
        // Tab-reach: the viewport hit-target exists and is reachable (ItemClick
        // locates it or the test fails) — the accessibility substitute for ImGui's
        // absent screen-reader tree.
        ctx->ItemClick("SceneHarness/3d-viewport");
        ctx->Yield();
        SceneView &sv = g_scene_shell.scenes[0];
        // Focus the 3D HUD window; its keys then drive the camera with no mouse.
        ctx->WindowFocus("3D overview");
        ctx->Yield();
        const float yaw0 = sv.cam.yaw;
        ctx->KeyPress(ImGuiKey_RightArrow); // orbit
        ctx->Yield();
        IM_CHECK(sv.cam.yaw != yaw0); // the camera moved with NO mouse
        ctx->KeyPress(ImGuiKey_R);    // reset to the default pose
        ctx->Yield();
        IM_CHECK(sv.cam.yaw == scene3d::Camera{}.yaw);
        ctx->KeyPress(ImGuiKey_T); // the honest top-down 2D-ish fallback
        ctx->Yield();
        IM_CHECK(sv.cam.pitch >= scene3d::Camera::kPitchLimit - 1e-4f);
    };

    // T4 (F12) — the Loom takes gutter's per-take remove + clear forks, driven by
    // real clicks (each pushes a reversible undo Command).
    t = IM_REGISTER_TEST(engine, "loom", "takes_gutter_remove_clear");
    t->GuiFunc = gutter_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        g_gutter_loom.takes.clear();
        g_gutter_undo.clear();
        {
            loom_take_node_t a;
            a.label = "arg0 := 11";
            g_gutter_loom.takes.push_back(a);
        }
        {
            loom_take_node_t b;
            b.label = "code patch";
            b.err = "did not assemble"; // a loud refusal, never dropped (D7)
            g_gutter_loom.takes.push_back(b);
        }
        ctx->SetRef("GutterHarness");
        ctx->Yield();
        IM_CHECK(g_gutter_loom.takes.size() == 2);
        ctx->ItemClick("**/remove"); // the first take's [remove]
        ctx->Yield();
        IM_CHECK(g_gutter_loom.takes.size() == 1); // per-take remove works
        IM_CHECK(g_gutter_undo.can_undo());        // and pushed a reversible command
        ctx->ItemClick("clear forks");             // the gutter-level clear
        ctx->Yield();
        IM_CHECK(g_gutter_loom.takes.empty()); // clear forks empties the set
    };

    // 30 R3 T3 — the Reweave gesture: pick a step K, a register and a value and
    // submit, asserting the fork-from-step-K request the full app applies
    // (loom_take_run_from_step). Driven by real inputs/clicks through the REAL
    // draw_loom_reweave_form; the disclosure the take carries is pinned in
    // test_loom_forks, this drives the gesture that produces it.
    t = IM_REGISTER_TEST(engine, "loom", "reweave_form_emits_request");
    t->GuiFunc = reweave_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        g_reweave_form = loom_reweave_form_t{};
        ctx->SetRef("ReweaveHarness");
        ctx->Yield();
        // step K := 3
        ctx->ItemInput("step K");
        ctx->KeyCharsReplace("3");
        // register := rcx (a real combo selection, not a model poke)
        ctx->ComboClick("register/rcx");
        // value := 0x40
        ctx->ItemInput("value");
        ctx->KeyCharsReplace("0x40");
        ctx->Yield();
        IM_CHECK(!g_reweave_form.requested); // nothing emitted until submit
        ctx->ItemClick("Reweave from step K");
        ctx->Yield();
        IM_CHECK(g_reweave_form.requested);        // the gesture emitted a request
        IM_CHECK(g_reweave_form.req_step == 3);     // K captured from the input
        IM_CHECK(g_reweave_form.req_reg == "rcx");  // register captured from combo
        IM_CHECK(g_reweave_form.req_value == 0x40); // value parsed from hex
    };
}

// A second shell for the docking tear-out test, loaded once. Separate from
// g_keymap_shell so enabling docking here never leaks into the keymap tests'
// non-docked expectations (and this test is registered LAST for the same reason).
static asmdesk::ShellState g_dock_shell;
static bool g_dock_loaded = false;
static void ensure_dock_shell() {
    if (g_dock_loaded)
        return;
    g_dock_loaded = true;
    std::string err;
    asmdesk::shell_open(g_dock_shell,
                        std::string(ASMTEST_GOLDEN_DIR) + "/sum_via_rbx.asmtrace",
                        err);
    g_dock_shell.active_tab = 0;
}
static void dock_gui(ImGuiTestContext *) {
    // Docking is off in the shared context (the keymap tests need the non-docked
    // path); flip it on HERE so draw_shell renders the real panes. The TestFunc
    // clears it again at the end, and this test runs last, so nothing downstream
    // sees it.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ensure_dock_shell();
    asmdesk::draw_shell(g_dock_shell);
}

// The tear-out -> Reset round-trip (19 T3): the ONE assertion that needs a real
// backend drag, so it belongs in this interaction lane, not the null smoke.
// Model/window state, not pixels — a docked pane is undocked, then Reset re-docks
// it into its region.
static void register_dock_tests(ImGuiTestEngine *engine) {
    ImGuiTest *t = IM_REGISTER_TEST(engine, "dock", "tearout_reset_roundtrip");
    t->GuiFunc = dock_gui;
    t->TestFunc = [](ImGuiTestContext *ctx) {
        using namespace asmdesk;
        ctx->Yield();
        ctx->Yield();
        ctx->Yield(); // build the default layout + let docking settle
        ImGuiWindow *scr = ImGui::FindWindowByName(kPaneScrubber);
        IM_CHECK(scr != nullptr);
        IM_CHECK(scr->DockId != 0); // the preset docked it into a region
        const ImGuiID home_dock = scr->DockId;
        // Tear it out — undock the pane to a floating window.
        ctx->UndockWindow(kPaneScrubber);
        ctx->Yield();
        IM_CHECK(scr->DockId == 0); // now floating
        // Reset layout (the View menu's own call) re-docks it into its region.
        layout_build(g_dock_shell.dockspace_id, ImVec2(1280, 800),
                     LayoutPreset::ReplayInspect);
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(scr->DockId != 0);         // re-docked
        IM_CHECK(scr->DockId == home_dock); // back into its original node
        // Leave the shared context as we found it (docking off) for any later run.
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
    };
}

// register_tests — every interaction test hangs off here: a harness self-check,
// the keymap enforcement (T1 step 5), the interaction-flow tests (step 4), and
// the docking tear-out round-trip (19 T3, registered LAST — it toggles docking).
static void register_tests(ImGuiTestEngine *engine) {
    register_keymap_tests(engine);
    register_flow_tests(engine);
    register_search_tests(engine); // 22: find / undo / camera / takes-gutter

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

    // Registered LAST on purpose: it toggles ImGuiConfigFlags_DockingEnable on the
    // shared context, so it must not run before the (non-docked) keymap tests.
    register_dock_tests(engine);
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
