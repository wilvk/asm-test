// addon_compile_probe.cpp — the imgui-repin compile gate
// (docs/internal/archive/gui/12-addon-supply-chain.md T3).
//
// This TU is compiled by `make desktop-addon-compile-check` with the desktop
// lane's EXACT flags ($(DESKTOP_CXXFLAGS)) at whatever imgui version is pinned.
// Its job: prove that every vendored addon which reaches into imgui_internal.h
// still compiles against the current pin. Five recommended addons are internal-
// header dependents (TextSelect, ImGuiFileDialog, node-editor/imgui_canvas,
// ImGuiNotify, ImSearch — doc 11's verification appendix) and are only known
// good at the frozen pin; without this gate a silent imgui repin ships one that
// no longer builds.
//
// CONTRACT (README-addons.md step 5): every addon that includes
// imgui_internal.h MUST add an include here behind its own ASMDESK_HAVE_<addon>
// guard, and its adopting task wires `-DASMDESK_HAVE_<addon> -I<its home>` into
// the check's ADDON_PROBE_FLAGS in mk/desktop.mk. A repin that breaks the probe
// is a LANDING BLOCKER, not a runtime surprise.
//
// Baseline (zero addons vendored): we still include imgui_internal.h itself,
// because that header — deliberately internal and churn-prone across the
// announced docking rewrite — is the surface every dependent stands on, so a
// repin that changed it must fail HERE first.

#define IMGUI_DEFINE_MATH_OPERATORS // ImZoomSlider et al. need the operators
#include "imgui.h"
#include "imgui_internal.h"

// --- vendored internal-header addons (each lands with its own guard) ---------
#ifdef ASMDESK_HAVE_IMZOOMSLIDER
#include "ImZoomSlider.h"
#endif
#ifdef ASMDESK_HAVE_TEXTSELECT
#include "textselect.hpp"
#endif
#ifdef ASMDESK_HAVE_IMGUIFILEDIALOG
#include "ImGuiFileDialog.h"
#endif
#ifdef ASMDESK_HAVE_IMGUI_CANVAS
#include "imgui_canvas.h"
#endif
#ifdef ASMDESK_HAVE_IMGUI_NODE_EDITOR
// The full node editor's public API (15 T3). Its own imgui_internal.h surface is
// imgui_canvas.h / imgui_extra_math.h (already guarded above, same repo/pin), so
// a repin that broke the internal headers fails there; this entry pins that the
// public header observer_draw.cpp consumes also still compiles at the pin.
#include "imgui_node_editor.h"
#endif
#ifdef ASMDESK_HAVE_IMGUINOTIFY
#include "ImGuiNotify.hpp"
#endif
#ifdef ASMDESK_HAVE_IMSEARCH
#include "imsearch.h"
#endif
#ifdef ASMDESK_HAVE_ICTEDIT
#include "TextEditor.h"
#endif

// A public-API addon (no imgui_internal.h) needs no gate — imgui_memory_editor
// and ImPlot compile against the stable public API and are proven by the app's
// own build. They are intentionally NOT probed here.

// No main(): the check compiles with -fsyntax-only. This symbol just gives the
// TU external linkage so a future link-time variant has something to name.
extern const int asmdesk_addon_probe_ok;
const int asmdesk_addon_probe_ok = 1;
