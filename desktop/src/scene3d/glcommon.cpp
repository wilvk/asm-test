// glcommon.cpp — the GL implementation of glcommon.h. Reaches GL exactly as
// scene.cpp does (GL_GLEXT_PROTOTYPES + libGL on Linux/Mesa, the OpenGL
// framework on Darwin), includes no glfw3.h, and links no engine (D4).
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "scene3d/glcommon.h"

namespace asmdesk::scene3d {

namespace {

GLuint compile_shader(GLenum type, const char *src, std::string &err) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log) - 1, nullptr, log);
        err = std::string("shader compile failed: ") + log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

unsigned gl_link_program(const char *vs, const char *fs, bool has_vid, bool pick,
                         std::string &err, bool has_col) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs, err);
    if (!v)
        return 0;
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs, err);
    if (!f) {
        glDeleteShader(v);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    // Both spellings map to slot 0: the terrain program's vertex input is
    // "cell", every line/point program's is "pos". Binding a name a program
    // does not declare is a no-op, so one call site covers both.
    glBindAttribLocation(p, kAttrPos, "cell");
    glBindAttribLocation(p, kAttrPos, "pos");
    // 55 T6 step 1: bound unconditionally, on the same reasoning as the two
    // spellings above — binding a name that is not an active attribute is not
    // an error, it simply has no effect — so the quad-expanded line layout
    // needs no second link path.
    glBindAttribLocation(p, kAttrOther, "other");
    glBindAttribLocation(p, kAttrSide, "side");
    // 61 T5: same reasoning again — the trace step rides the line and point
    // programs, and binding it where it is not declared costs nothing.
    glBindAttribLocation(p, kAttrStep, "step");
    if (has_vid)
        glBindAttribLocation(p, kAttrVid, "vid");
    if (has_col)
        glBindAttribLocation(p, kAttrCol, "col");
    if (pick)
        glBindFragDataLocation(p, kFragId, "fragid");
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        err = std::string("program link failed: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void GlPickTarget::ensure(int w, int h) {
    if (fbo_ && w_ == w && h_ == h)
        return;
    if (!fbo_)
        glGenFramebuffers(1, &fbo_);
    if (!tex_)
        glGenTextures(1, &tex_);
    if (!rbo_depth_)
        glGenRenderbuffers(1, &rbo_depth_);
    w_ = w;
    h_ = h;
    glBindTexture(GL_TEXTURE_2D, tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, rbo_depth_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

int GlPickTarget::bind_and_clear(int w, int h) {
    ensure(w, h);
    GLint prev_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, w, h);
    const GLuint zero[4] = {0, 0, 0, 0};
    glClearBufferuiv(GL_COLOR, 0, zero);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    return static_cast<int>(prev_fbo);
}

uint32_t GlPickTarget::read_pixel(int x, int y, int fbh) const {
    uint32_t id = 0;
    const int ry = fbh - 1 - y;
    if (x >= 0 && x < w_ && ry >= 0 && ry < h_) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(x, ry, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &id);
    }
    return id;
}

void GlPickTarget::read_all(int w, int h, std::vector<uint32_t> &out) const {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, w, h, GL_RED_INTEGER, GL_UNSIGNED_INT, out.data());
}

void GlPickTarget::restore(int prev_fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

void GlPickTarget::free_gl() {
    if (fbo_)
        glDeleteFramebuffers(1, &fbo_);
    if (tex_)
        glDeleteTextures(1, &tex_);
    if (rbo_depth_)
        glDeleteRenderbuffers(1, &rbo_depth_);
    fbo_ = tex_ = rbo_depth_ = 0;
    w_ = h_ = 0;
}

} // namespace asmdesk::scene3d
