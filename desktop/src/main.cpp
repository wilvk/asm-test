// main.cpp — the desktop entry point, shared by BOTH binaries. A compile-time
// ASMTEST_DESKTOP_RENDER_ONLY guard flips the window title (and, via the shell,
// disables the engine doors); the build (mk/desktop.mk) sets it for the
// render-only viewer and links no engines there (D4). The draw code lives in
// ui/shell.cpp and is backend-free, so the null-backend tests exercise the same
// frames this loop drives. Stock Dear ImGui GLFW + OpenGL3 example shape
// (03-desktop-shell.md T6).
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"   // the plotting chassis context (15 T1)
#include "imsearch.h" // the client-side filtering context (16 T2)
#include "ui/fonts.h" // real monospace font + Codicons (13 F3)

#include <GLFW/glfw3.h> // drags in the system OpenGL headers

#include "doc/workspace_state.h" // persisted workspace store (20 T3)
#include "ui/app_icon.h"         // compiled-in window icon RGBA (dock/titlebar)
#include "ui/gl_scene_host.h"
#include "ui/settings.h"         // user text-scale / theme / window size (20 T5)
#include "ui/shell.h"
#include "ui/theme.h"            // dt_set_light_theme (20 T5)
#include "views/observer_draw.h" // obs_graph_enable (node-editor canvas, 15 T3)

// The persistence stores (20 T3/T5): both live under build/ beside the dock
// `.ini`, so they are git-ignored and a corrupt/stale one degrades to defaults.
static const char *kWorkspaceStore = "build/desktop-workspace.json";
static const char *kSettingsStore = "build/desktop-settings.json";

static std::string read_text_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void write_text_file(const char *path, const std::string &s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f)
        f << s;
}

// A content-scale (DPI) change flags an atlas re-bake; the loop does it where the
// GL context is current (20 T5). File-scope so the GLFW callback can set it.
static bool g_fonts_dirty = false;

// The vendored TTF paths (injected by mk/desktop.mk), kept file-scope so the
// atlas re-bake in the loop can reach them.
#ifndef ASMTEST_JBM_TTF
#define ASMTEST_JBM_TTF ""
#endif
#ifndef ASMTEST_CODICON_TTF
#define ASMTEST_CODICON_TTF ""
#endif
#ifndef ASMTEST_FA_TTF
#define ASMTEST_FA_TTF ""
#endif

// GLFW callbacks (20 T3/T5). Each reaches ShellState through the window user
// pointer. Drag-drop and window-size are the two manual-smoke seams the brief
// notes: they need a real window, add no logic of their own over already-tested
// targets (shell_open / the Settings model), so nothing testable self-skips.
static void drop_cb(GLFWwindow *w, int count, const char **paths) {
    auto *st = static_cast<asmdesk::ShellState *>(glfwGetWindowUserPointer(w));
    if (!st)
        return;
    for (int i = 0; i < count; i++) {
        std::string err;
        st->want_open_tab = asmdesk::shell_open(*st, paths[i], err);
        if (st->want_open_tab < 0 && !err.empty())
            st->status = err;
    }
}
static void size_cb(GLFWwindow *w, int width, int height) {
    auto *st = static_cast<asmdesk::ShellState *>(glfwGetWindowUserPointer(w));
    if (st && width > 0 && height > 0) {
        st->settings.win_w = width;
        st->settings.win_h = height;
        st->settings_dirty = true;
    }
}
static void content_scale_cb(GLFWwindow *w, float xscale, float) {
    auto *st = static_cast<asmdesk::ShellState *>(glfwGetWindowUserPointer(w));
    if (st) {
        st->settings.content_scale = xscale > 0.0f ? xscale : 1.0f;
        st->settings_dirty = true;
    }
    g_fonts_dirty = true;
}

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
    const char *app_id = "asmtest-viewer";
#else
    const char *title = "asmtest desktop";
    const char *app_id = "asmtest-desktop";
#endif

    // The window's application identity, set BEFORE the window exists (these are
    // window hints, latched at creation). On X11 it becomes WM_CLASS; a GNOME
    // dash matches that against the installed .desktop's StartupWMClass to show
    // the launcher's icon and group the window under it. On Wayland it is the
    // app_id, the ONLY handle the compositor has to that same .desktop (there is
    // no per-window icon protocol there, so glfwSetWindowIcon below is a no-op
    // and this is what makes the dock icon appear at all). GLFW_X11_CLASS_NAME/
    // _INSTANCE_NAME exist since 3.2; the dedicated Wayland hint only since 3.4,
    // so it is probed — on 3.3 the X11 class name doubles as the Wayland app_id.
    glfwWindowHintString(GLFW_X11_CLASS_NAME, app_id);
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME, app_id);
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID, app_id);
#endif

    // Load the persisted settings BEFORE the window so it opens at the size the
    // user left it (20 T5, retiring the hardcoded 1280x720). A corrupt/absent
    // store degrades to the defaults in the struct.
    asmdesk::Settings settings;
    asmdesk::settings_parse(read_text_file(kSettingsStore), settings);

    GLFWwindow *window =
        glfwCreateWindow(settings.win_w, settings.win_h, title, nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "asmtest-desktop: window creation failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    // The window icon (titlebar + X11 taskbar/dock). GLFW copies the pixels, so
    // the compiled-in RGBA mips (ui/app_icon.h) can be stack-local. GLFW picks
    // the nearest size per surface. A no-op on Wayland (feature-unavailable) and
    // on macOS (the .app bundle's icon wins) — harmless there, and the .desktop
    // `Icon=` carries the Wayland case.
    {
        size_t icon_count = 0;
        const asmdesk::AppIconImage *icons = asmdesk::app_icon_images(&icon_count);
        std::vector<GLFWimage> imgs(icon_count);
        for (size_t i = 0; i < icon_count; ++i) {
            imgs[i].width = icons[i].size;
            imgs[i].height = icons[i].size;
            imgs[i].pixels = const_cast<unsigned char *>(icons[i].rgba);
        }
        if (!imgs.empty())
            glfwSetWindowIcon(window, static_cast<int>(imgs.size()), imgs.data());
    }

    // The GLFW content (DPI) scale of this window drives the atlas bake size, so
    // a HiDPI display gets a crisp atlas rather than an upscaled 15px one (T5).
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    if (xscale > 0.0f)
        settings.content_scale = xscale;

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
    // ...which forces OFF imgui 1.91's id-conflict highlight. That debug feature
    // flags a "Programmer error: N visible items with conflicting ID" tooltip when
    // one hovered ImGui id was submitted more than once in a frame — and imgui-
    // node-editor legitimately does exactly that: its End() replays each node's
    // captured content, re-submitting the hovered node/pin id, so the count trips
    // whenever the pointer is over the graph. Our node ids are unique (the pure
    // graph_from_* builders key them on tgid / emission index / interned rank), so
    // this is a false positive on vendored, maintenance-mode code we do not patch.
    // It is a developer aid, not a correctness guard — imgui itself recommends
    // disabling it for non-programmer builds (imgui.cpp, BeginErrorTooltip note) —
    // and this is a shipping end-user app. The detector cannot be scoped to the
    // canvas: NewFrame() reads the flag once, before any view draws.
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    // Real monospace font + merged Codicons (13 F3), baked DPI-aware at the
    // window's content scale (20 T5). Paths are compiled in (ASMTEST_*_TTF); a
    // stripped install degrades gracefully to the bitmap font.
    asmdesk::load_fonts(ImGui::GetIO(), ASMTEST_JBM_TTF, ASMTEST_CODICON_TTF,
                        ASMTEST_FA_TTF, 15.0f, settings.content_scale);
    // The base style + fidelity-chrome theme follow the persisted preference (T5).
    asmdesk::dt_set_light_theme(settings.light_theme);
    if (settings.light_theme)
        ImGui::StyleColorsLight();
    else
        ImGui::StyleColorsDark();
    asmdesk::settings_apply_text_scale(settings, ImGui::GetIO());
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    asmdesk::ShellState state;
    state.settings = settings;
    // docs/internal/archive/gui/45-launch-and-window-target.md T7: the opaque handle
    // shell_set_crosshair_cursor/shell_restore_cursor (below) and the Home
    // rail's crosshair drag (ui/shell.cpp) use — void* so ui/shell.h needs no
    // GLFW include (ui/scene_host.h's split, mirrored here).
    state.glfw_window = window;
    // Seed the Inspect door's connection buffers from the persisted Settings so
    // the "Capture a live process" auto-connect and the Connect-pane fallback both
    // start from the saved asmspy path / ssh host. Blank stays blank (the Connect
    // pane resolve-prefills a concrete path on demand, session-only).
    std::snprintf(state.inspect.asmspy_path, sizeof state.inspect.asmspy_path,
                  "%s", settings.asmspy_path.c_str());
    std::snprintf(state.inspect.ssh_host, sizeof state.inspect.ssh_host, "%s",
                  settings.ssh_host.c_str());

    // Restore the workspace (20 T3): reopen the recordings, replay the active
    // position + per-pane selection, and bring back recents/perspectives/presets
    // — a vanished recording is kept in recents with its error (D7). A corrupt
    // store parses to defaults, so a bad file never strands the app.
    {
        asmdesk::WorkspaceState ws;
        asmdesk::workspace_state_parse(read_text_file(kWorkspaceStore), ws);
        asmdesk::shell_restore_workspace(state, ws);
        state.ws_dirty = false; // a restore is not a change to persist back
    }
    // Seed the application log with a startup line, so the persistent Log pane has
    // content from the first frame (and the app's own lifecycle — not just the live
    // session — is recorded there).
    asmdesk::shell_log_push(
        state,
        std::string(title) + " started — " +
            std::to_string(state.ws.recordings.size()) + " recording(s) restored",
        asmdesk::ToastKind::Info);

    // GLFW callbacks reach ShellState through the user pointer (T3/T5): drag-drop
    // opens a file, a resize remembers the size, a content-scale change re-bakes.
    glfwSetWindowUserPointer(window, &state);
    glfwSetDropCallback(window, drop_cb);
    glfwSetWindowSizeCallback(window, size_cb);
    glfwSetWindowContentScaleCallback(window, content_scale_cb);

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

        // A content-scale change (or a user "crisp rebuild") re-bakes the atlas at
        // the scaled px and recreates the backend texture (20 T5). Done here where
        // the GL context is current, before NewFrame consumes the atlas.
        if (g_fonts_dirty) {
            g_fonts_dirty = false;
            asmdesk::load_fonts(ImGui::GetIO(), ASMTEST_JBM_TTF,
                                ASMTEST_CODICON_TTF, ASMTEST_FA_TTF, 15.0f,
                                state.settings.content_scale);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }

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

        // Debounced persistence (20 T3/T5): flush the store only when something
        // changed (an open/close/save/preset, or a settings edit), not every
        // frame. The full active-position + per-pane selection is captured on
        // clean exit below regardless.
        if (state.ws_dirty) {
            write_text_file(kWorkspaceStore, asmdesk::workspace_state_serialize(
                                                 asmdesk::shell_capture_workspace(
                                                     state)));
            state.ws_dirty = false;
        }
        if (state.settings_dirty) {
            write_text_file(kSettingsStore,
                            asmdesk::settings_serialize(state.settings));
            state.settings_dirty = false;
        }
    }

    // Clean exit: capture the whole workspace (open set + active position + each
    // pane's selection as asmtrace-links) and the settings, so the next launch
    // reopens exactly where this one left off (20 T3/T5).
    write_text_file(kWorkspaceStore, asmdesk::workspace_state_serialize(
                                         asmdesk::shell_capture_workspace(state)));
    write_text_file(kSettingsStore, asmdesk::settings_serialize(state.settings));

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
