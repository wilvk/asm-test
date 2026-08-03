// shot_render_stub.cpp — the --shot implementation on a host with no EGL/GL
// development headers.
//
// A bare host must still build a working asmtest-desktop; it just cannot take
// screenshots. Saying so plainly beats either a link error (which looks like a
// broken build) or a silent no-op (which looks like a screenshot that vanished).
// Selected by mk/desktop.mk when DESKTOP_GL_MISSING is non-empty.
#include "ui/shot_render.h"

#include <cstdio>

namespace asmdesk {

int shot_run(const std::string &manifest_path, const std::string &out_dir) {
    (void)manifest_path;
    (void)out_dir;
    std::fprintf(stderr,
                 "shot: this build has no EGL/GL headers, so --shot was not "
                 "compiled in\n"
                 "      (make docker-desktop installs software Mesa + EGL and "
                 "can render the screenshots)\n");
    return 1;
}

} // namespace asmdesk
