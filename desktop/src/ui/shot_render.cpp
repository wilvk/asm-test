#include "ui/shot_render.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "imsearch.h"

// The FBO entry points (glGenFramebuffers and friends) are GL 3 core but reach
// the compiler through glext.h prototypes on Linux — the same include shape
// desktop/test/test_scene_fbo.cpp uses for the same reason.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "ui/fonts.h"
#include "ui/gl_scene_host.h"
#include "ui/png_write.h"
#include "ui/shell.h"
#include "ui/shot.h"
#include "ui/theme.h"

#ifndef ASMTEST_JBM_TTF
#define ASMTEST_JBM_TTF ""
#endif
#ifndef ASMTEST_CODICON_TTF
#define ASMTEST_CODICON_TTF ""
#endif
#ifndef ASMTEST_FA_TTF
#define ASMTEST_FA_TTF ""
#endif

namespace asmdesk {
namespace {

void say(const std::string &m) { std::fprintf(stderr, "shot: %s\n", m.c_str()); }

// A surfaceless EGL + OpenGL 3.3 context. Same construction as the FBO smoke in
// desktop/test/test_scene_fbo.cpp, which is what proves this path works here.
struct Egl {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    bool ok = false;

    bool init(std::string &err) {
        auto getPD = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (getPD == nullptr) {
            err = "eglGetPlatformDisplayEXT unavailable";
            return false;
        }
        dpy = getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
            err = "no surfaceless EGL display";
            return false;
        }
        eglBindAPI(EGL_OPENGL_API);
        EGLint cfga[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        if (!eglChooseConfig(dpy, cfga, &cfg, 1, &n) || n < 1) {
            err = "no usable EGL config";
            return false;
        }
        EGLint ctxa[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION,
                         3, EGL_NONE};
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxa);
        if (ctx == EGL_NO_CONTEXT) {
            err = "could not create a GL 3.3 context";
            return false;
        }
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
        ok = true;
        return true;
    }
    ~Egl() {
        if (ok) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroyContext(dpy, ctx);
            eglTerminate(dpy);
        }
    }
};

struct Fbo {
    GLuint fbo = 0, tex = 0, rb = 0;

    bool make(int W, int H) {
        destroy();
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rb);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
               GL_FRAMEBUFFER_COMPLETE;
    }
    void destroy() {
        if (fbo != 0)
            glDeleteFramebuffers(1, &fbo);
        if (tex != 0)
            glDeleteTextures(1, &tex);
        if (rb != 0)
            glDeleteRenderbuffers(1, &rb);
        fbo = tex = rb = 0;
    }
    ~Fbo() { destroy(); }
};

std::string slurp(const std::string &p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Render one shot into `out_dir`. Returns false with the reason already said.
bool render_one(const ShotSpec &s, const std::string &out_dir) {
    Fbo fb;
    if (!fb.make(s.width, s.height)) {
        say("incomplete framebuffer for " + s.name);
        return false;
    }

    // A FRESH ShellState per shot: no persisted workspace, no settings store,
    // nothing carried over from the previous image. Determinism here is what
    // makes a committed screenshot diffable.
    ShellState st;
    std::unique_ptr<SceneHost> host = make_gl_scene_host();
    host->init();
    st.scene_host = host.get();

    // shell_open, NOT ws.open: the raw workspace call only appends to
    // ws.recordings, leaving the shell's parallel arrays (streams, observers,
    // stepidx, seg_df and — the one that matters here — scenes) empty. The app
    // then renders correctly but sits on the Home rail with no Recording pane,
    // and every scene shot comes out identical because there is no SceneView to
    // set a kind on. Measured: that produced six byte-identical PNGs.
    int first = -1;
    for (const std::string &p : s.open) {
        std::string oerr;
        const int idx = shell_open(st, p, oerr);
        if (idx < 0) {
            say("cannot open " + p + ": " + oerr);
            return false;
        }
        if (first < 0)
            first = idx;
    }
    // -1 would mean the Home rail. The A side is the tab the shot is OF; a
    // second recording (the Divergence B side) stays open but unselected.
    st.active_tab = first;

    ImGuiIO &io = ImGui::GetIO();
    for (int frame = 0; frame <= s.warmup; frame++) {
        io.DisplaySize = ImVec2(static_cast<float>(s.width),
                                static_cast<float>(s.height));
        io.DeltaTime = 1.0f / 60.0f;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        // Re-assert the intent EVERY frame: the shell consumes and resets
        // want_view_id at the end of each frame (shell.cpp), so setting it once
        // before the loop would be silently dropped.
        st.want_view_id = s.view;
        if (st.active_tab >= 0 &&
            static_cast<size_t>(st.active_tab) < st.scenes.size()) {
            SceneView &sv = st.scenes[static_cast<size_t>(st.active_tab)];
            // BOTH fields, every frame. req_kind alone does NOTHING: the shell
            // only acts on it when req_kind_change is set, then clears the flag.
            // Setting the value without the flag leaves every 3D shot on the
            // default Plane — a silent wrong-substrate bug, not a visible one.
            sv.hud.req_kind = s.scene;
            sv.hud.req_kind_change = true;
            if (!s.layers.empty()) {
                sv.hud.layers = scene3d::SceneLayers{};
                std::string lerr;
                shot_apply_layers(s.layers, sv.hud.layers, lerr);
            }
        }

        draw_shell(st);

        ImGui::Render();
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        glViewport(0, 0, s.width, s.height);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    std::vector<unsigned char> px(static_cast<size_t>(s.width) * s.height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s.width, s.height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    const std::string path = out_dir + "/" + s.name + ".png";
    std::string werr;
    if (!png_write_rgba_flipped(path, s.width, s.height, px.data(), werr)) {
        say(werr);
        return false;
    }
    say("wrote " + path);
    host->shutdown();
    return true;
}

} // namespace

int shot_run(const std::string &manifest_path, const std::string &out_dir) {
    const std::string text = slurp(manifest_path);
    if (text.empty()) {
        say("cannot read manifest " + manifest_path);
        return 1;
    }
    std::vector<ShotSpec> shots;
    std::string err;
    if (!shot_manifest_parse(text, shots, err)) {
        say(err);
        return 1;
    }

    Egl egl;
    if (!egl.init(err)) {
        say(err + " — --shot needs a working EGL/GL 3.3 stack");
        return 1;
    }
    // Provenance: two people comparing these images should be able to see
    // whether they are comparing two renderers.
    say(std::string("GL renderer: ") +
        reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    say(std::string("GL version : ") +
        reinterpret_cast<const char *>(glGetString(GL_VERSION)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImSearch::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    // No .ini and no log: a developer's persisted dock layout must never leak
    // into a committed screenshot, and the shot run must not write one either.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    load_fonts(io, ASMTEST_JBM_TTF, ASMTEST_CODICON_TTF, ASMTEST_FA_TTF, 15.0f,
               1.0f);
    dt_set_light_theme(false);
    ImGui::StyleColorsDark();
    ImGui_ImplOpenGL3_Init("#version 130");

    int rc = 0;
    for (const ShotSpec &s : shots)
        if (!render_one(s, out_dir)) {
            rc = 1;
            break;
        }

    ImGui_ImplOpenGL3_Shutdown();
    ImSearch::DestroyContext();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return rc;
}

} // namespace asmdesk
