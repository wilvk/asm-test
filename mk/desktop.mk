# desktop.mk — desktop GUI: full app + render-only viewer + headless tests
# (docs/internal/gui/03-desktop-shell.md). Two binaries share one source tree:
#   asmtest-desktop  the full app — links the Author-tier engines, so GPL-2.0 as
#                    a whole (D4); imgui + GLFW/OpenGL3 backends + doc/ + ui/.
#   asmtest-viewer   the render-only viewer — ZERO engine objects or libs, stays
#                    permissively distributable (D4); built with
#                    -DASMTEST_DESKTOP_RENDER_ONLY=1.
# The headless tests (desktop-test) drive ImGui through its null backend and need
# no display, no GL and no engines — they run on any host with a C++17 compiler.
#
# Included BEFORE mk/bindings.mk (Makefile), so $(CXX)/$(CLANG_FORMAT) are
# referenced lazily (recipes only, never :=). Additive rules only.

# Docking branch (13-foundation-moves.md F1): same 1.91.9, adds dockable/tearable
# panes + layout persistence. The `b` hotfix fixes .ini table-load asserts that
# become reachable exactly when persistence lands. Digest pinned in
# scripts/third-party-digests.txt; the switch is this one line + that row.
IMGUI_VERSION ?= 1.91.9b-docking
IMGUI_HOME    ?= $(BUILD)/imgui/imgui-$(IMGUI_VERSION)
JSON_VERSION  ?= 3.11.3
JSON_HOME     ?= $(BUILD)/nlohmann-json/$(JSON_VERSION)
# linmath.h: the 3D spacetime overview's camera math (10-spacetime-3d-overview.md
# T4, desktop/src/scene3d/). A pinned single header (commit short-token; the fetch
# script resolves the full commit) — see scripts/fetch-linmath.sh.
LINMATH_VERSION ?= 26211bb
LINMATH_HOME    ?= $(BUILD)/linmath/$(LINMATH_VERSION)

# -MMD -MP: per-object header deps so an incremental build stays correct across
# the many desktop/ and imgui headers. Recursive (=), never := (CXX is set by
# mk/bindings.mk, which loads after this file).
# -DIMGUI_USER_CONFIG (F2): inject desktop/src/imconfig_user.h into every imgui +
# addon TU (32-bit ImDrawIdx) without touching the digest-pinned tarball.
DESKTOP_CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -MMD -MP \
  -Icli -Iinclude -Idesktop/src -I$(IMGUI_HOME) -I$(IMGUI_HOME)/backends \
  -I$(JSON_HOME) -I$(LINMATH_HOME) \
  -DIMGUI_USER_CONFIG='"imconfig_user.h"' \
  $(DESKTOP_ADDON_INCLUDES)

# --- pinned, digest-verified third-party sources (D2) ------------------------
# One fetch-imgui.sh run extracts ALL of imgui's TUs at once, so they are a
# GROUPED target (&:, make 4.3+): make then knows a clean tree (build/ is
# dockerignored, so `make docker-desktop` starts with none of them) can produce
# every imgui source by running the single fetch, not just imgui.cpp.
IMGUI_SRCS := $(IMGUI_HOME)/imgui.cpp $(IMGUI_HOME)/imgui_draw.cpp \
  $(IMGUI_HOME)/imgui_tables.cpp $(IMGUI_HOME)/imgui_widgets.cpp \
  $(IMGUI_HOME)/backends/imgui_impl_glfw.cpp \
  $(IMGUI_HOME)/backends/imgui_impl_opengl3.cpp
# Pass IMGUI_VERSION through: fetch-imgui.sh defaults to plain 1.91.9 on its own,
# so the docking pin (F1) only takes effect if make hands the version to the
# script — otherwise IMGUI_HOME points at imgui-<docking> while the fetch writes
# imgui-1.91.9 and every compile fails "no such file".
# misc/freetype/imgui_freetype.cpp ships INSIDE the imgui tarball, so the same
# fetch produces it — declare it a grouped output too, or the freetype gate's
# imgui_freetype.o rule (F3) has "No rule to make target" on a clean tree (it is
# not in IMGUI_SRCS, which are the always-compiled core/backends). Harmless when
# freetype is off: present but unused.
$(IMGUI_SRCS) $(IMGUI_HOME)/misc/freetype/imgui_freetype.cpp &: scripts/fetch-imgui.sh scripts/third-party-digests.txt
	IMGUI_VERSION=$(IMGUI_VERSION) sh scripts/fetch-imgui.sh >/dev/null
$(JSON_HOME)/nlohmann/json.hpp: scripts/fetch-json.sh scripts/third-party-digests.txt
	sh scripts/fetch-json.sh >/dev/null
$(LINMATH_HOME)/linmath.h: scripts/fetch-linmath.sh scripts/third-party-digests.txt
	sh scripts/fetch-linmath.sh >/dev/null

# --- vendored Dear ImGui addons (12-addon-supply-chain.md) --------------------
# Pattern per addon: a home var, a fetch rule (thin wrapper -> fetch-addon.sh),
# an -I appended to DESKTOP_ADDON_INCLUDES (so every desktop TU can #include it —
# headers are cheap), the header as an ORDER-ONLY prereq on the specific user
# objects (so a clean tree fetches it before those compile), and — for an
# imgui_internal.h dependent — an ADDON_PROBE_FLAGS line so the repin gate
# rebuilds it. These three accumulators accrue across addons; the compile-check
# target (below) uses ADDON_PROBE_FLAGS/DEPS, DESKTOP_CXXFLAGS uses INCLUDES.
DESKTOP_ADDON_INCLUDES :=
ADDON_PROBE_FLAGS :=
ADDON_PROBE_DEPS :=

# ImZoomSlider (14 T5): one header from the ImGuizmo repo; uses imgui_internal.h.
IMZOOM_VERSION ?= dc25afb98bc3ebe00dfc9a23ba7235fead2ccb1d
IMZOOM_HOME    ?= $(BUILD)/addons/imzoomslider-$(IMZOOM_VERSION)
$(IMZOOM_HOME)/ImZoomSlider.h: scripts/fetch-imzoomslider.sh scripts/third-party-digests.txt
	sh scripts/fetch-imzoomslider.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IMZOOM_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_IMZOOMSLIDER -I$(IMZOOM_HOME)
ADDON_PROBE_DEPS  += $(IMZOOM_HOME)/ImZoomSlider.h
# fabric_imgui.cpp (the Loom draw half, all three trees) is the ImZoomSlider user.
$(BUILD)/desktop/app/lo/fabric_imgui.o \
$(BUILD)/desktop/render/lo/fabric_imgui.o \
$(BUILD)/desktop/test/lo/fabric_imgui.o \
$(BUILD)/desktop/uitest/lo/fabric_imgui.o: | $(IMZOOM_HOME)/ImZoomSlider.h
# timeline_draw.cpp adopts the same ImZoomSlider as the timeline's window control
# (21-spine-navigation.md T3, completing 14 T5), in every tree that draws it.
$(BUILD)/desktop/app/vw/timeline_draw.o \
$(BUILD)/desktop/render/vw/timeline_draw.o \
$(BUILD)/desktop/test/vw/timeline_draw.o \
$(BUILD)/desktop/uitest/vw/timeline_draw.o: | $(IMZOOM_HOME)/ImZoomSlider.h

# imgui_memory_editor (14 T4): one header from imgui_club, PUBLIC API only — no
# imgui_internal.h, so NOT in the compile-probe. Used by observer_draw.cpp.
IMMEMEDIT_VERSION ?= a436e793fe44a2c8e827bfcbf138fcbe11940476
IMMEMEDIT_HOME    ?= $(BUILD)/addons/imgui_club-$(IMMEMEDIT_VERSION)
$(IMMEMEDIT_HOME)/imgui_memory_editor.h: scripts/fetch-imguimemedit.sh scripts/third-party-digests.txt
	sh scripts/fetch-imguimemedit.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IMMEMEDIT_HOME)
$(BUILD)/desktop/app/vw/observer_draw.o \
$(BUILD)/desktop/render/vw/observer_draw.o \
$(BUILD)/desktop/test/vw/observer_draw.o: | $(IMMEMEDIT_HOME)/imgui_memory_editor.h

# ImGuiTextSelect v1.1.6 (14 T6): a COMPILED addon (textselect.cpp) + its utfcpp
# header dep (<utf8.h>). Uses imgui_internal.h -> compile-probe. PIN v1.1.6 — NOT
# latest (v1.2.0+ require ImGui 1.92). Its object must link into every binary
# that links observer_draw.o (its user); those three link sites add it below.
TEXTSELECT_VERSION ?= 1.1.6
TEXTSELECT_HOME    ?= $(BUILD)/addons/textselect-$(TEXTSELECT_VERSION)
UTFCPP_VERSION     ?= 4.0.6
UTFCPP_HOME        ?= $(BUILD)/addons/utfcpp-$(UTFCPP_VERSION)
$(TEXTSELECT_HOME)/textselect.cpp $(TEXTSELECT_HOME)/textselect.hpp &: scripts/fetch-textselect.sh scripts/third-party-digests.txt
	sh scripts/fetch-textselect.sh >/dev/null
$(UTFCPP_HOME)/source/utf8.h: scripts/fetch-utfcpp.sh scripts/third-party-digests.txt
	sh scripts/fetch-utfcpp.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(TEXTSELECT_HOME) -I$(UTFCPP_HOME)/source
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_TEXTSELECT -I$(TEXTSELECT_HOME) -I$(UTFCPP_HOME)/source
ADDON_PROBE_DEPS  += $(TEXTSELECT_HOME)/textselect.hpp $(UTFCPP_HOME)/source/utf8.h
# textselect.cpp compiled per tree (the render-only define is harmless to it).
# -w: it is vendored third-party code we do not modify; -Wall -Wextra noise from
# it (sign-compare, unused vars) is not actionable and not ours to fix.
$(BUILD)/desktop/%/addon/textselect.o: $(TEXTSELECT_HOME)/textselect.cpp | $(TEXTSELECT_HOME)/textselect.hpp $(UTFCPP_HOME)/source/utf8.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@

# ImGuiFileDialog v0.6.8 (14 T7): a COMPILED addon (ImGuiFileDialog.cpp), pure
# ImGui (no zenity/osascript, so docker-desktop tests it). Uses imgui_internal.h
# -> compile-probe. Works on 1.91.9 via its own <19201 guard. Its object rides
# the shell.o / inspect_door.o link sites (the open + save dialogs).
IFD_VERSION ?= 0.6.8
IFD_HOME    ?= $(BUILD)/addons/imguifiledialog-$(IFD_VERSION)
$(IFD_HOME)/ImGuiFileDialog.cpp $(IFD_HOME)/ImGuiFileDialog.h $(IFD_HOME)/ImGuiFileDialogConfig.h &: scripts/fetch-imguifiledialog.sh scripts/third-party-digests.txt
	sh scripts/fetch-imguifiledialog.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IFD_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_IMGUIFILEDIALOG -I$(IFD_HOME)
ADDON_PROBE_DEPS  += $(IFD_HOME)/ImGuiFileDialog.h
$(BUILD)/desktop/%/addon/imguifiledialog.o: $(IFD_HOME)/ImGuiFileDialog.cpp | $(IFD_HOME)/ImGuiFileDialog.h $(IFD_HOME)/ImGuiFileDialogConfig.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@

# ImPlot v1.0 (15 T1): plotting chassis, TWO compiled TUs. implot.h is PUBLIC API
# (not in the compile-probe); implot_internal.h (pulled by the .cpp) has
# imgui_internal.h. Objects ride the observer_draw.o link sites (hotedges heatmap).
# The draws are guarded on ImPlot::GetCurrentContext() so the null test backends
# (which create no ImPlot context) degrade to text, and only the app plots.
IMPLOT_VERSION ?= v1.0
IMPLOT_HOME    ?= $(BUILD)/addons/implot-$(IMPLOT_VERSION)
$(IMPLOT_HOME)/implot.cpp $(IMPLOT_HOME)/implot_items.cpp $(IMPLOT_HOME)/implot.h $(IMPLOT_HOME)/implot_internal.h &: scripts/fetch-implot.sh scripts/third-party-digests.txt
	sh scripts/fetch-implot.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IMPLOT_HOME)
$(BUILD)/desktop/%/addon/implot.o: $(IMPLOT_HOME)/implot.cpp | $(IMPLOT_HOME)/implot.h $(IMPLOT_HOME)/implot_internal.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
$(BUILD)/desktop/%/addon/implot_items.o: $(IMPLOT_HOME)/implot_items.cpp | $(IMPLOT_HOME)/implot.h $(IMPLOT_HOME)/implot_internal.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
DESKTOP_IMPLOT_OBJ_app    := $(BUILD)/desktop/app/addon/implot.o $(BUILD)/desktop/app/addon/implot_items.o
DESKTOP_IMPLOT_OBJ_render := $(BUILD)/desktop/render/addon/implot.o $(BUILD)/desktop/render/addon/implot_items.o
DESKTOP_IMPLOT_OBJ_test   := $(BUILD)/desktop/test/addon/implot.o $(BUILD)/desktop/test/addon/implot_items.o

# ImSearch (16 T2): client-side filtering, ONE compiled TU (imsearch.cpp). Uses
# imgui_internal.h -> compile-probe. Its object rides the learn_door.o link sites
# (the Learn-door catalog filter). Guarded on ImSearch::GetCurrentContext() so
# the null test backends degrade to the plain list.
IMSEARCH_VERSION ?= 7596ac5
IMSEARCH_HOME    ?= $(BUILD)/addons/imsearch-$(IMSEARCH_VERSION)
$(IMSEARCH_HOME)/imsearch.cpp $(IMSEARCH_HOME)/imsearch.h $(IMSEARCH_HOME)/imsearch_internal.h &: scripts/fetch-imsearch.sh scripts/third-party-digests.txt
	sh scripts/fetch-imsearch.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IMSEARCH_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_IMSEARCH -I$(IMSEARCH_HOME)
ADDON_PROBE_DEPS  += $(IMSEARCH_HOME)/imsearch.h
$(BUILD)/desktop/%/addon/imsearch.o: $(IMSEARCH_HOME)/imsearch.cpp | $(IMSEARCH_HOME)/imsearch.h $(IMSEARCH_HOME)/imsearch_internal.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@

# imgui_canvas (15 T2) + the full imgui-node-editor (15 T3): the graph pan/zoom
# canvas from one fetch (fetch-nodeeditor.sh, pinned in 53b3b6f). ONE grouped
# fetch produces every file, so imgui_canvas.cpp (T2's standalone de-risk) and
# the node editor's THREE more TUs (T3 — imgui_node_editor.cpp, its api layer,
# and the bundled crude_json.cpp settings serialiser: the "4 TUs" of the doc,
# imgui_canvas being the first) all come from the same &: rule — declare them all
# as grouped outputs or a clean tree would run the fetch once per group. All use
# imgui_internal.h (via imgui_canvas.h / imgui_node_editor_internal.h) -> the
# compile-probe. PIN master 021aa0ea: the last release v0.9.3 (2023) FAILS to
# compile on 1.91.9 (the operator== redefinition); this master sha compiles clean
# (doc 11, reproduced). observer_draw.cpp includes only the public
# imgui_node_editor.h; objects ride its link sites like ImPlot. -w: vendored.
NODEEDITOR_VERSION ?= 021aa0ea
NODEEDITOR_HOME    ?= $(BUILD)/addons/imgui-node-editor-$(NODEEDITOR_VERSION)
NODEEDITOR_SRCS    := imgui_node_editor imgui_node_editor_api crude_json
$(NODEEDITOR_HOME)/imgui_canvas.cpp $(NODEEDITOR_HOME)/imgui_canvas.h \
$(NODEEDITOR_HOME)/imgui_node_editor.cpp $(NODEEDITOR_HOME)/imgui_node_editor_api.cpp \
$(NODEEDITOR_HOME)/crude_json.cpp $(NODEEDITOR_HOME)/imgui_node_editor.h &: \
    scripts/fetch-nodeeditor.sh scripts/third-party-digests.txt
	sh scripts/fetch-nodeeditor.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(NODEEDITOR_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_IMGUI_CANVAS -DASMDESK_HAVE_IMGUI_NODE_EDITOR -I$(NODEEDITOR_HOME)
ADDON_PROBE_DEPS  += $(NODEEDITOR_HOME)/imgui_canvas.h $(NODEEDITOR_HOME)/imgui_node_editor.h
$(BUILD)/desktop/%/addon/imgui_canvas.o: $(NODEEDITOR_HOME)/imgui_canvas.cpp | $(NODEEDITOR_HOME)/imgui_canvas.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
# The node editor's three TUs, one pattern rule each (the source basename differs
# so a single %-rule cannot cover them without also matching imgui_canvas above).
$(BUILD)/desktop/%/addon/imgui_node_editor.o: $(NODEEDITOR_HOME)/imgui_node_editor.cpp | $(NODEEDITOR_HOME)/imgui_node_editor.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
$(BUILD)/desktop/%/addon/imgui_node_editor_api.o: $(NODEEDITOR_HOME)/imgui_node_editor_api.cpp | $(NODEEDITOR_HOME)/imgui_node_editor.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
$(BUILD)/desktop/%/addon/crude_json.o: $(NODEEDITOR_HOME)/crude_json.cpp | $(NODEEDITOR_HOME)/imgui_node_editor.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@
DESKTOP_NODEEDITOR_OBJ_app    := $(addprefix $(BUILD)/desktop/app/addon/,$(addsuffix .o,$(NODEEDITOR_SRCS)))
DESKTOP_NODEEDITOR_OBJ_render := $(addprefix $(BUILD)/desktop/render/addon/,$(addsuffix .o,$(NODEEDITOR_SRCS)))
DESKTOP_NODEEDITOR_OBJ_test   := $(addprefix $(BUILD)/desktop/test/addon/,$(addsuffix .o,$(NODEEDITOR_SRCS)))

# goossens ImGuiColorTextEdit (17 T2): the Author-door code editor. ONE compiled
# TU now (TextEditor.cpp); TextDiff.cpp is the diff-view follow-on (fetched, not
# built yet). fetch-ictedit.sh applies the verified 2-line 1.92 guard. Uses
# imgui_internal.h -> compile-probe. Its object rides author_door.o's link sites.
ICTEDIT_VERSION ?= f67e5bc
ICTEDIT_HOME    ?= $(BUILD)/addons/ictedit-$(ICTEDIT_VERSION)
$(ICTEDIT_HOME)/TextEditor.cpp $(ICTEDIT_HOME)/TextEditor.h &: scripts/fetch-ictedit.sh scripts/third-party-digests.txt
	sh scripts/fetch-ictedit.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(ICTEDIT_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_ICTEDIT -I$(ICTEDIT_HOME)
ADDON_PROBE_DEPS  += $(ICTEDIT_HOME)/TextEditor.h
$(BUILD)/desktop/%/addon/TextEditor.o: $(ICTEDIT_HOME)/TextEditor.cpp | $(ICTEDIT_HOME)/TextEditor.h $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -w -c $< -o $@

# ImGuiNotify (16 T1): HEADER-ONLY toasts (included by ui/shell.cpp) + its bundled
# FontAwesome6 header (merged by ui/fonts.cpp) + fa-solid-900.ttf (loaded at
# runtime). ImGuiNotify.hpp uses imgui_internal.h -> compile-probe; the probe also
# forces NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false (the shell's setting).
IMGUINOTIFY_VERSION ?= d00e45f
IMGUINOTIFY_HOME    ?= $(BUILD)/addons/imguinotify-$(IMGUINOTIFY_VERSION)
$(IMGUINOTIFY_HOME)/ImGuiNotify.hpp $(IMGUINOTIFY_HOME)/IconsFontAwesome6.h $(IMGUINOTIFY_HOME)/fa-solid-900.ttf &: scripts/fetch-imguinotify.sh scripts/third-party-digests.txt
	sh scripts/fetch-imguinotify.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(IMGUINOTIFY_HOME)
ADDON_PROBE_FLAGS += -DASMDESK_HAVE_IMGUINOTIFY -DNOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false -I$(IMGUINOTIFY_HOME)
ADDON_PROBE_DEPS  += $(IMGUINOTIFY_HOME)/ImGuiNotify.hpp

# Fonts + icons (13 F3): JetBrains Mono + Codicons TTFs (loaded at RUNTIME by
# ui/fonts.cpp via stb_truetype — so they work on every lane, no freetype needed)
# + IconFontCppHeaders' IconsCodicons.h (compile-time, the ICON_CI_* macros +
# merge range). Freetype itself is a separate, Docker-only rasteriser gate below.
JBM_VERSION      ?= v2.304
JBM_HOME         ?= $(BUILD)/addons/jetbrainsmono-$(JBM_VERSION)
CODICON_VERSION  ?= 0.0.35
CODICON_HOME     ?= $(BUILD)/addons/codicons-$(CODICON_VERSION)
ICONFONT_VERSION ?= 210b5a3
ICONFONT_HOME    ?= $(BUILD)/addons/iconfontcppheaders-$(ICONFONT_VERSION)
$(JBM_HOME)/JetBrainsMono-Regular.ttf: scripts/fetch-jetbrainsmono.sh scripts/third-party-digests.txt
	sh scripts/fetch-jetbrainsmono.sh >/dev/null
$(CODICON_HOME)/codicon.ttf: scripts/fetch-codicons.sh scripts/third-party-digests.txt
	sh scripts/fetch-codicons.sh >/dev/null
$(ICONFONT_HOME)/IconsCodicons.h: scripts/fetch-iconfontcppheaders.sh scripts/third-party-digests.txt
	sh scripts/fetch-iconfontcppheaders.sh >/dev/null
DESKTOP_ADDON_INCLUDES += -I$(ICONFONT_HOME)
# The runtime TTF paths, injected as defines into the font loader's users.
DESKTOP_FONT_DEFS = -DASMTEST_JBM_TTF='"$(JBM_HOME)/JetBrainsMono-Regular.ttf"' \
                    -DASMTEST_CODICON_TTF='"$(CODICON_HOME)/codicon.ttf"' \
                    -DASMTEST_FA_TTF='"$(IMGUINOTIFY_HOME)/fa-solid-900.ttf"'
# fonts.cpp #includes IconsCodicons.h + IconsFontAwesome6.h; main.o carries the
# TTF-path defines and calls load_fonts. Both depend on their fetched inputs so a
# clean tree pulls them (the clean-build lesson).
$(BUILD)/desktop/app/ui/fonts.o $(BUILD)/desktop/render/ui/fonts.o $(BUILD)/desktop/test/ui/fonts.o: | $(ICONFONT_HOME)/IconsCodicons.h $(IMGUINOTIFY_HOME)/IconsFontAwesome6.h
$(BUILD)/desktop/app/src/main.o $(BUILD)/desktop/render/src/main.o: DESKTOP_CXXFLAGS += $(DESKTOP_FONT_DEFS)
$(BUILD)/desktop/app/src/main.o $(BUILD)/desktop/render/src/main.o: | $(JBM_HOME)/JetBrainsMono-Regular.ttf $(CODICON_HOME)/codicon.ttf $(IMGUINOTIFY_HOME)/fa-solid-900.ttf

# Clean-tree fetch ordering (docker/CI): these objects #include addon headers, so
# they must wait for the addon fetches. A fresh build/ (dockerignored) compiles
# before the fetch otherwise; an incremental host build masks it (headers already
# present). These accrue as addons gain users — each addon-header include needs
# its fetch as an order-only prereq of the including object (additive to any
# per-object prereq already declared above).
$(BUILD)/desktop/app/src/main.o $(BUILD)/desktop/render/src/main.o: | $(IMPLOT_HOME)/implot.h $(IMSEARCH_HOME)/imsearch.h
$(BUILD)/desktop/app/vw/observer_draw.o $(BUILD)/desktop/render/vw/observer_draw.o $(BUILD)/desktop/test/vw/observer_draw.o: | $(IMPLOT_HOME)/implot.h $(TEXTSELECT_HOME)/textselect.hpp $(NODEEDITOR_HOME)/imgui_node_editor.h
$(BUILD)/desktop/app/ui/learn_door.o $(BUILD)/desktop/render/ui/learn_door.o $(BUILD)/desktop/test/ui/learn_door.o: | $(IMSEARCH_HOME)/imsearch.h
$(BUILD)/desktop/app/ui/shell.o $(BUILD)/desktop/render/ui/shell.o $(BUILD)/desktop/test/ui/shell.o: | $(IFD_HOME)/ImGuiFileDialog.h $(IMGUINOTIFY_HOME)/ImGuiNotify.hpp
$(BUILD)/desktop/app/ui/inspect_door.o $(BUILD)/desktop/render/ui/inspect_door.o $(BUILD)/desktop/test/ui/inspect_door.o: | $(IFD_HOME)/ImGuiFileDialog.h
$(BUILD)/desktop/app/vw/slice_view_draw.o $(BUILD)/desktop/render/vw/slice_view_draw.o $(BUILD)/desktop/test/vw/slice_view_draw.o: | $(NODEEDITOR_HOME)/imgui_canvas.h
# timeline_draw.cpp now #includes implot.h (the overview density strip, 21 T3), in
# every tree that draws it.
$(BUILD)/desktop/app/vw/timeline_draw.o $(BUILD)/desktop/render/vw/timeline_draw.o \
$(BUILD)/desktop/test/vw/timeline_draw.o $(BUILD)/desktop/uitest/vw/timeline_draw.o: | $(IMPLOT_HOME)/implot.h
# palette.cpp's app-only ImSearch relevance path (21 T1) mirrors terms.o: the
# macro + the fetch prereq are scoped to app/render, so the test trees compile the
# plain-list fallback and need no imsearch link for a standalone palette test.
$(BUILD)/desktop/app/ui/palette.o $(BUILD)/desktop/render/ui/palette.o: \
    DESKTOP_CXXFLAGS += -DASMDESK_HAVE_IMSEARCH
$(BUILD)/desktop/app/ui/palette.o $(BUILD)/desktop/render/ui/palette.o: | $(IMSEARCH_HOME)/imsearch.h
# author_door.cpp now #includes ImGuiFileDialog.h too (18-breach-stops.md T3: the
# reused confirm-overwrite save dialog), so the fetch must land before it compiles
# on a clean tree — same order-only prereq shell.o / inspect_door.o already carry.
$(BUILD)/desktop/app/ui/author_door.o $(BUILD)/desktop/render/ui/author_door.o $(BUILD)/desktop/test/ui/author_door.o: | $(ICTEDIT_HOME)/TextEditor.h $(IFD_HOME)/ImGuiFileDialog.h
# ui/asm_language.cpp is the Author editor's per-dialect syntax highlighting (17
# T2 follow-on): its header #includes TextEditor.h, so it carries the same
# order-only fetch prereq. The uitest tree builds it too (it path-rewrites
# DESKTOP_TEST_SHELL_OBJ), hence the fourth entry.
$(BUILD)/desktop/app/ui/asm_language.o $(BUILD)/desktop/render/ui/asm_language.o \
$(BUILD)/desktop/test/ui/asm_language.o $(BUILD)/desktop/uitest/ui/asm_language.o: | $(ICTEDIT_HOME)/TextEditor.h
# canvas_draw.cpp now #includes IconsCodicons.h for the ONE glyph set per honesty
# tier (23-graded-truth-layer.md T1), so the codicon header fetch must land before
# it compiles on a clean tree — the same order-only prereq fonts.o carries.
$(BUILD)/desktop/app/vw/canvas_draw.o $(BUILD)/desktop/render/vw/canvas_draw.o \
$(BUILD)/desktop/test/vw/canvas_draw.o $(BUILD)/desktop/uitest/vw/canvas_draw.o: | $(ICONFONT_HOME)/IconsCodicons.h

# --- in-app term registry, GENERATED from the ONE glossary (24 T3) -----------
# scripts/gen-terms.py parses docs/project/glossary.md's {glossary} directive
# into ui/terms_generated.h (a {term -> definition} table) — the same one-source
# pattern the keymap help uses (dt_nav_bindings -> help). ui/terms.cpp is its
# only #includer, so it alone gets the generated header as an order-only prereq,
# in every tree. Generated under $(BUILD) (never committed); -I puts it on the
# `ui/` include path so `#include "ui/terms_generated.h"` resolves.
DESKTOP_TERMS_GEN := $(BUILD)/desktop/gen/ui/terms_generated.h
$(DESKTOP_TERMS_GEN): scripts/gen-terms.py docs/project/glossary.md
	@mkdir -p $(@D)
	python3 scripts/gen-terms.py docs/project/glossary.md > $@
DESKTOP_CXXFLAGS += -I$(BUILD)/desktop/gen
$(BUILD)/desktop/app/ui/terms.o $(BUILD)/desktop/render/ui/terms.o \
$(BUILD)/desktop/test/ui/terms.o $(BUILD)/desktop/uitest/ui/terms.o: \
    | $(DESKTOP_TERMS_GEN)
# The shipped app + viewer surface the Terms pane through the doc-16 ImSearch
# idiom (guarded at runtime on the ImSearch context); the null test + uitest
# trees compile the plain-list fallback, so they need neither the macro nor a
# link to imsearch.o for a standalone terms test.
$(BUILD)/desktop/app/ui/terms.o $(BUILD)/desktop/render/ui/terms.o: \
    DESKTOP_CXXFLAGS += -DASMDESK_HAVE_IMSEARCH
$(BUILD)/desktop/app/ui/terms.o $(BUILD)/desktop/render/ui/terms.o: \
    | $(IMSEARCH_HOME)/imsearch.h

# --- source basenames --------------------------------------------------------
DESKTOP_IMGUI_CORE := imgui imgui_draw imgui_tables imgui_widgets
DESKTOP_IMGUI_BACK := imgui_impl_glfw imgui_impl_opengl3

# THREE object trees — shared sources compile per-binary (render adds
# -DASMTEST_DESKTOP_RENDER_ONLY=1; test has no backends), so .o are never shared:
# $(BUILD)/desktop/{app,render,test}/. Each source dir maps to a distinct object
# subdir (ig/ igb/ src/ doc/ ui/ t/) so every object has exactly one applicable
# pattern rule (no vpath ambiguity), and each imgui object's source is the
# grouped fetch output above.  $(1)=tree, $(2)=extra CXXFLAGS.
define desktop_rules
$$(BUILD)/desktop/$(1)/ig/%.o:  $$(IMGUI_HOME)/%.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/igb/%.o: $$(IMGUI_HOME)/backends/%.cpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
# src/, ui/ and t/ list linmath.h as an order-only prereq too: ui/shell.h now
# transitively includes scene3d/camera.h -> linmath.h (the 3D-overview pane), so
# main.o, shell.o, gl_scene_host.o and the shell/golden tests need the pinned
# header fetched before they compile on a clean tree.
$$(BUILD)/desktop/$(1)/src/%.o: desktop/src/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp $$(LINMATH_HOME)/linmath.h
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/doc/%.o: desktop/src/doc/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/ui/%.o:  desktop/src/ui/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp $$(LINMATH_HOME)/linmath.h
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/an/%.o:  desktop/src/analysis/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/vw/%.o:  desktop/src/views/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/da/%.o:  desktop/src/data/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/lo/%.o:  desktop/src/loom/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/lv/%.o:  desktop/src/live/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/sp/%.o:  desktop/src/space/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/s3/%.o:  desktop/src/scene3d/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp $$(LINMATH_HOME)/linmath.h
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/t/%.o:   desktop/test/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp $$(LINMATH_HOME)/linmath.h
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) $$(DESKTOP_TEST_EXTRA) -c $$< -o $$@
endef
# Freetype rasteriser (13-foundation-moves.md F3): OFF by default. The fonts load
# via stb_truetype regardless; DESKTOP_FREETYPE=1 (the Docker desktop lane, which
# has libfreetype-dev) links imgui's freetype rasteriser for higher quality. It
# is scoped to the app + viewer ONLY — the null-backend test tree stays
# stb_truetype-only, so `make desktop-test` needs no libfreetype on any host.
DESKTOP_FREETYPE ?= 0
FREETYPE_CXX  :=
FREETYPE_LIBS :=
ifeq ($(DESKTOP_FREETYPE),1)
  FREETYPE_CXX  := -DIMGUI_ENABLE_FREETYPE $(shell pkg-config --cflags freetype2 2>/dev/null)
  FREETYPE_LIBS := $(shell pkg-config --libs freetype2 2>/dev/null || echo -lfreetype)
endif
$(eval $(call desktop_rules,app,$(FREETYPE_CXX)))
$(eval $(call desktop_rules,render,-DASMTEST_DESKTOP_RENDER_ONLY=1 $(FREETYPE_CXX)))
$(eval $(call desktop_rules,test,))

# ---------------------------------------------------------------------------
# Dear ImGui Test Engine (17-interaction-testing-and-editor.md T1) — the
# interaction-layer test harness. TEST-LANE ONLY, fetched-at-build, NEVER
# bundled/linked into a shipped binary (the same posture as Pin/SDE; the one
# admitted non-MIT dep). Tag v1.91.9 is tag-matched to the imgui pin — move both
# in one commit (doc 13 F1/F4).
#
# The engine's item hooks live behind IMGUI_ENABLE_TEST_ENGINE, which adds a few
# ImGuiItemStatusFlags enum values and one ImGuiContext field the hooks poke. So
# the flag must be seen by EVERY TU in the ui-test binary, imgui core included —
# it must NOT leak into the shipped app/render/test trees. A whole `uitest` tree
# (the test tree + the flag) keeps that boundary clean and correct-by-
# construction, and its object set is just the shell test's, path-rewritten.
ITE_VERSION ?= 1.91.9
ITE_HOME    ?= $(BUILD)/addons/imgui-test-engine-$(ITE_VERSION)
ITE_SRCDIR  := $(ITE_HOME)/imgui_test_engine
# std::thread coroutine backend (the stock one) -> needs -lpthread at link.
ITE_CXX     := -DIMGUI_ENABLE_TEST_ENGINE \
               -DIMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1 \
               -I$(ITE_SRCDIR)
ITE_ENGINE_SRC := imgui_te_engine imgui_te_context imgui_te_coroutine \
                  imgui_te_ui imgui_te_utils imgui_te_exporters \
                  imgui_te_perftool imgui_capture_tool

# One fetch produces the whole tree, so ALL engine sources are grouped outputs
# of it (&:, like the imgui fetch): make must know each .cpp is producible or the
# per-source object rule below reports "no rule to make target" on a cold tree
# (only the first listed .cpp would otherwise have a rule). GNU Make 4.3+.
ITE_ENGINE_CPP := $(ITE_ENGINE_SRC:%=$(ITE_SRCDIR)/%.cpp)
$(ITE_ENGINE_CPP) &: scripts/fetch-imgui-test-engine.sh scripts/third-party-digests.txt scripts/fetch-addon.sh
	ITE_VERSION=$(ITE_VERSION) sh scripts/fetch-imgui-test-engine.sh >/dev/null

$(eval $(call desktop_rules,uitest,$(ITE_CXX)))

# The engine's own TUs are the only ones that #include engine headers +
# thirdparty/Str, so they alone need the fetch (the app/imgui-core uitest TUs
# just see -DIMGUI_ENABLE_TEST_ENGINE, which only toggles imgui's own headers).
# Vendored third-party -> -w (benign upstream -Wunused-result on system()/fread).
$(BUILD)/desktop/uitest/ite/%.o: $(ITE_SRCDIR)/%.cpp | $(ITE_SRCDIR)/imgui_te_engine.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) $(ITE_CXX) -w -c $< -o $@

# The test driver #includes the engine headers, so the fetch must land before it
# compiles too — a warm host masks this, a cold docker build fails "No such
# file" (the clean-tree ordering hazard). The app/imgui-core uitest TUs do NOT
# include engine headers, so they need no such prereq. It also opens fixtures,
# so it needs ASMTEST_FIXTURE_DIR (target-specific, like the other test .o).
$(BUILD)/desktop/uitest/t/test_ui.o: | $(ITE_SRCDIR)/imgui_te_engine.cpp
$(BUILD)/desktop/uitest/t/test_ui.o: DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'

# imgui_freetype.o (from misc/freetype/) — the one extra imgui TU freetype needs,
# built with the freetype cflags and linked into the app + viewer ONLY (added to
# their object sets below when DESKTOP_FREETYPE=1).
$(BUILD)/desktop/app/ig/imgui_freetype.o $(BUILD)/desktop/render/ig/imgui_freetype.o: $(BUILD)/desktop/%/ig/imgui_freetype.o: $(IMGUI_HOME)/misc/freetype/imgui_freetype.cpp | $(IMGUI_HOME)/imgui.cpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) $(FREETYPE_CXX) -c $< -o $@

# The test fixtures + golden corpus reach their tests through compile defines, so
# the tests need no argv wiring (and run identically host + docker).
$(BUILD)/desktop/test/t/test_recording.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
# test_shell reads BOTH trees: the honesty fixtures (banners, the render smoke)
# and the golden corpus, for the regstate recording + the ABI x-ray pair that
# exercise the two surfaced doc-09 tabs (Scrubber / ABI x-ray) end to end.
$(BUILD)/desktop/test/t/test_shell.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"' \
                         -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop/test/t/test_golden.o:    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
# test_theme's source-lint (24 T1) reads the drift files straight from the tree,
# so it needs the source root as a compile define (it runs from the repo root).
$(BUILD)/desktop/test/t/test_theme.o:     DESKTOP_TEST_EXTRA = -DASMTEST_DESKTOP_SRC_DIR='"desktop/src"'
# test_honesty (23 T1) loads the committed dishonesty fixtures from the corpus.
$(BUILD)/desktop/test/t/test_honesty.o:   DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop/test/t/test_live_session.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_inspect.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_converge.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_drillin.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
# The 3D-scene FBO smoke reads BOTH trees (10-spacetime-3d-overview.md T7): the
# two GENERATED golden scenes (and the hand-authored rich-`mem` one under
# scenes/) from the corpus, and the reused obs-survey-ibs fixture for the
# statistical scene — rather than a second survey fixture saying the same thing.
$(BUILD)/desktop/test/t/test_scene_fbo.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"' \
                         -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_loom_golden.o \
$(BUILD)/desktop/test/t/test_loom_draw.o: DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop/test/t/test_walkthrough.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_WALKTHROUGH_DIR='"$(WALKTHROUGH_DIR)"'
# learn_door.cpp's compiled-in default walkthrough directory, in all three trees.
$(BUILD)/desktop/app/ui/learn_door.o $(BUILD)/desktop/render/ui/learn_door.o \
$(BUILD)/desktop/test/ui/learn_door.o: \
    DESKTOP_CXXFLAGS += -DASMTEST_WALKTHROUGH_DIR='"$(WALKTHROUGH_DIR)"'

# --- object lists ------------------------------------------------------------
# Every view is split in two (04-replay-views.md: "a pure view-model builder + a
# thin ImGui draw"): `<view>.cpp` builds and dumps the model with no ImGui and no
# I/O, `<view>_draw.cpp` renders it. Only the pure half is linked into the view
# tests, so "the builder carries all the logic" is enforced by the link line
# rather than by discipline — a builder that reached for ImGui would fail to
# link in its own test.
DESKTOP_VIEW_PURE := canvas timeline slice_view diff_view
DESKTOP_VIEW_DRAW := canvas_draw timeline_draw slice_view_draw diff_view_draw \
                     completeness

# The live Observer views (08-observer-views.md). Same split, same rule: every
# one of these builds from the Recording document model with no ImGui, no I/O
# and no engine, which is what lets each be asserted on a host with nothing to
# attach to — and is why a live view and a replayed one cannot drift apart.
# graph_nav (15 T3) is a pure builder like the rest: it turns the topo/tree/
# hotedges models into a deterministic node-editor layout (positions + edges +
# the deep link a click routes through) with NO ImGui and NO node-editor, which
# is what lets its layout be asserted headless — the library never chose a
# position. It rides the pure list so it links wherever observer_draw.o does.
DESKTOP_OBS_PURE := observer syscalls watch topo hotedges tree region disasm \
                    graph_nav
DESKTOP_OBS_DRAW := observer_draw

# loom/ — the Loom fabric (05-loom-day-one.md). Every TU here except forks.cpp
# is pure and engine-free, which is what lets asmtest-viewer weave a recording
# with zero engine deps (D4); forks.cpp is full-build-only and linked separately.
DESKTOP_LOOM_PURE := fabric feed fabric_plan lineage annex take_view
DESKTOP_LOOM_DRAW := fabric_imgui
# forks.cpp calls asmtest_assemble / emu_* / asmtest_dataflow_emu_run, so it is
# GPL-side and compiles ONLY into the full app (D4/D7/D9). asmtest-viewer never
# sees this TU, which is what keeps its `ldd` free of unicorn/keystone/capstone.
DESKTOP_LOOM_APP  := forks

# live/ — the capture host (07-serve-live-host.md T3/T4). It spawns
# `asmspy --serve` and speaks its protocol; it links NO engine, which is what
# lets asmtest-viewer host live sessions while staying engine-free (D4/D9).
DESKTOP_LIVE := session budget inspect ptslice

# The Learn door's bundled walkthroughs (06-doors-and-learning.md T2-T4).
WALKTHROUGH_DIR := tests/golden-asmtrace/walkthroughs

# asmtest-desktop / asmtest-viewer: imgui core + glfw/opengl3 backends + src/
# (vm_compat + main + nav) + doc/ + analysis/ + data/ + views/ + ui/. The app
# additionally links the Author-tier engine objects and their libs -> GPL-2.0 as
# a whole (D4); the viewer links NONE of them and stays permissive.
desktop_app_objs = \
  $(addprefix $(BUILD)/desktop/$(1)/ig/,$(addsuffix .o,$(DESKTOP_IMGUI_CORE))) \
  $(addprefix $(BUILD)/desktop/$(1)/igb/,$(addsuffix .o,$(DESKTOP_IMGUI_BACK))) \
  $(BUILD)/desktop/$(1)/src/vm_compat.o $(BUILD)/desktop/$(1)/src/main.o \
  $(BUILD)/desktop/$(1)/src/nav.o \
  $(BUILD)/desktop/$(1)/doc/recording.o $(BUILD)/desktop/$(1)/doc/workspace.o \
  $(BUILD)/desktop/$(1)/doc/streams.o \
  $(BUILD)/desktop/$(1)/an/slice.o $(BUILD)/desktop/$(1)/an/diff.o \
  $(BUILD)/desktop/$(1)/an/stepindex.o \
  $(BUILD)/desktop/$(1)/da/features_data.o \
  $(BUILD)/desktop/$(1)/da/perf_history.o \
  $(DESKTOP_VIEW_PURE:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(BUILD)/desktop/$(1)/vw/overview.o \
  $(BUILD)/desktop/$(1)/vw/scrubber.o $(BUILD)/desktop/$(1)/vw/scrubber_draw.o \
  $(BUILD)/desktop/$(1)/vw/abixray.o $(BUILD)/desktop/$(1)/vw/abixray_draw.o \
  $(DESKTOP_OBS_PURE:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_OBS_DRAW:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(BUILD)/desktop/$(1)/addon/textselect.o \
  $(BUILD)/desktop/$(1)/addon/imgui_canvas.o \
  $(DESKTOP_NODEEDITOR_OBJ_$(1)) \
  $(DESKTOP_IMPLOT_OBJ_$(1)) \
  $(BUILD)/desktop/$(1)/addon/imguifiledialog.o \
  $(DESKTOP_LOOM_PURE:%=$(BUILD)/desktop/$(1)/lo/%.o) \
  $(DESKTOP_LOOM_DRAW:%=$(BUILD)/desktop/$(1)/lo/%.o) \
  $(BUILD)/desktop/$(1)/src/walkthrough.o $(BUILD)/desktop/$(1)/src/capview.o \
  $(BUILD)/desktop/$(1)/src/author_vm.o \
  $(BUILD)/desktop/$(1)/ui/author_door.o \
  $(BUILD)/desktop/$(1)/ui/asm_language.o \
  $(BUILD)/desktop/$(1)/addon/TextEditor.o \
  $(BUILD)/desktop/$(1)/ui/shell.o $(BUILD)/desktop/$(1)/ui/layout.o \
  $(BUILD)/desktop/$(1)/ui/palette.o $(BUILD)/desktop/$(1)/ui/wayfinding.o \
  $(BUILD)/desktop/$(1)/ui/fonts.o \
  $(BUILD)/desktop/$(1)/ui/learn_door.o \
  $(BUILD)/desktop/$(1)/addon/imsearch.o \
  $(BUILD)/desktop/$(1)/ui/capability_panel.o \
  $(BUILD)/desktop/$(1)/ui/inspect_door.o \
  $(BUILD)/desktop/$(1)/ui/legend.o \
  $(BUILD)/desktop/$(1)/ui/cvd.o \
  $(BUILD)/desktop/$(1)/ui/terms.o \
  $(BUILD)/desktop/$(1)/ui/filter.o \
  $(BUILD)/desktop/$(1)/ui/timepos.o \
  $(BUILD)/desktop/$(1)/ui/primer.o \
  $(BUILD)/desktop/$(1)/ui/view_presence.o \
  $(BUILD)/desktop/$(1)/ui/settings.o \
  $(BUILD)/desktop/$(1)/ui/perspectives.o \
  $(BUILD)/desktop/$(1)/doc/workspace_state.o \
  $(BUILD)/desktop/$(1)/ui/gl_scene_host.o \
  $(DESKTOP_LIVE:%=$(BUILD)/desktop/$(1)/lv/%.o) \
  $(BUILD)/desktop/$(1)/sp/projection.o $(BUILD)/desktop/$(1)/sp/terrain.o \
  $(BUILD)/desktop/$(1)/sp/trajectory.o $(BUILD)/desktop/$(1)/sp/converge.o \
  $(BUILD)/desktop/$(1)/s3/scene.o $(BUILD)/desktop/$(1)/s3/pick.o \
  $(BUILD)/desktop/$(1)/s3/hud.o
# regsynth.o (30 R3 T4) is the Scrubber's register-history synthesiser: it links
# the emulator (emu.o), so — like forks.o — it is APP-ONLY (never in the viewer's
# object set, which is what keeps asmtest-viewer engine-free, D4).
DESKTOP_VIEW_APP := regsynth
DESKTOP_APP_OBJ    := $(call desktop_app_objs,app) \
                      $(DESKTOP_LOOM_APP:%=$(BUILD)/desktop/app/lo/%.o) \
                      $(DESKTOP_VIEW_APP:%=$(BUILD)/desktop/app/vw/%.o)
DESKTOP_RENDER_OBJ := $(call desktop_app_objs,render)
# Freetype rasteriser TU, app + viewer only, when DESKTOP_FREETYPE=1 (F3).
ifeq ($(DESKTOP_FREETYPE),1)
DESKTOP_APP_OBJ    += $(BUILD)/desktop/app/ig/imgui_freetype.o
DESKTOP_RENDER_OBJ += $(BUILD)/desktop/render/ig/imgui_freetype.o
endif
# The Author-tier engine objects (emu/assemble link unicorn/keystone, disasm
# links capstone) — they carry the GPL engine linkage that makes the app GPL-2.0
# as a whole (D4) and are self-contained (dataflow.o is the pure L0/L1/L2 sink,
# as mk/cli.mk's test_view links it alone). NOTE: $(FRAMEWORK_OBJS) is
# deliberately NOT here — asmtest.o is the test-runner and defines its own main()
# (src/asmtest.c), which would collide with the desktop's main.cpp; the desktop
# is not a test binary and needs no runner.
# dataflow_operands.o + dataflow_emu.o join the set with the Loom's fork engine
# (05-loom-day-one.md T5): forks.cpp re-runs the emulator L0 value producer, and
# that producer is the operand enumerator plus the Unicorn driver.
#
# dataflow_pt.o + dataflow_blockstep.o join for the PT-replay slice
# (08-observer-views.md T8): the app replays a RECORDED PT path through the
# emulator, which needs the producer and the purity/replayability gates it
# reuses — but no PT silicon, because the path was decoded when it was captured.
#
# dataflow_resume.o joins for the Loom's Reweave (30 R3 T3): forks.cpp's
# fork-from-step-K checkpoints + resumes the value producer on an emu_t through
# this seam, and regsynth.cpp (T4) re-derives a register history the same way.
DESKTOP_ENGINE_OBJ := $(BUILD)/emu.o $(BUILD)/trace.o \
                      $(BUILD)/disasm.o $(BUILD)/assemble.o \
                      $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                      $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_blockstep.o \
                      $(BUILD)/dataflow_pt.o $(BUILD)/dataflow_resume.o

# The capability panel (06-doors-and-learning.md T6) reads the library's own
# status APIs — asmtest_trace_resolve / asmtest_hwtrace_status / the IBS reasons
# — so the full app links the native-trace tier's objects.
#
# D9 NOTE, stated rather than glossed. D9 says the desktop app never links the
# ptrace ENGINES, and $(HWTRACE_OBJS) contains ptrace_backend.o. The distinction
# the panel relies on is capture vs QUERY: nothing in the app calls a capture
# entry point (the Observer's capture host is still the `asmspy --serve`
# subprocess), and what is linked here is the availability cascade a panel that
# "never re-probes" must be able to ask. A panel that shipped without it would
# have to invent its own probes, which is the outcome D2 exists to prevent.
DESKTOP_CAP_OBJ := $(HWTRACE_OBJS)

# test-tree shared objects (no backends, no main, no engines).
DESKTOP_TEST_IG  := $(addprefix $(BUILD)/desktop/test/ig/,$(addsuffix .o,$(DESKTOP_IMGUI_CORE)))
DESKTOP_TEST_DOC := $(BUILD)/desktop/test/doc/recording.o \
                    $(BUILD)/desktop/test/doc/workspace.o \
                    $(BUILD)/desktop/test/doc/workspace_state.o \
                    $(BUILD)/desktop/test/doc/streams.o
# The analysis/ + views/ builders under test. They are pure (no ImGui, no I/O)
# and engine-free, which is what lets the same objects link into asmtest-viewer.
DESKTOP_TEST_AN  := $(BUILD)/desktop/test/an/slice.o $(BUILD)/desktop/test/an/diff.o
DESKTOP_TEST_VW  := $(DESKTOP_VIEW_PURE:%=$(BUILD)/desktop/test/vw/%.o) \
                    $(BUILD)/desktop/test/src/nav.o
# The Observer builders, for the shell/golden binaries that draw them.
DESKTOP_TEST_OBS := $(DESKTOP_OBS_PURE:%=$(BUILD)/desktop/test/vw/%.o)
DESKTOP_TEST_DA  := $(BUILD)/desktop/test/da/features_data.o \
                    $(BUILD)/desktop/test/da/perf_history.o
DESKTOP_TEST_LOOM := $(DESKTOP_LOOM_PURE:%=$(BUILD)/desktop/test/lo/%.o)
DESKTOP_TEST_LIVE := $(DESKTOP_LIVE:%=$(BUILD)/desktop/test/lv/%.o)
# The shared visual-language ui/ helpers (24-one-visual-language.md): the
# palette legend (T1/T2), the glossary-sourced term registry + headings (T3), the
# one filter + column sort (T4), the one time-position widget (T4), the first-
# open primer (T5). Any draw-half binary that hosts a coined surface links these,
# so they are one list rather than five ad-hoc additions. (legend.o was listed
# standalone before; it now lives here — never list it twice on a link line.)
DESKTOP_TEST_UI     := legend terms filter timepos primer
DESKTOP_TEST_UI_OBJ := $(DESKTOP_TEST_UI:%=$(BUILD)/desktop/test/ui/%.o)

# --- missing-dependency probes (mirror mk/cli.mk:32-38) -----------------------
# The render-only viewer + the full app need GLFW/GL; only the full app needs the
# engines. desktop-test needs neither. Absence -> friendly guidance, never a raw
# compiler error (a bare host builds+runs desktop-test regardless).
DESKTOP_MISSING :=
ifneq ($(shell pkg-config --exists glfw3 2>/dev/null && echo ok),ok)
DESKTOP_MISSING += libglfw3-dev
endif
GLFW_LIBS ?= $(shell pkg-config --libs glfw3 2>/dev/null || echo -lglfw)
# GL is a system FRAMEWORK on Darwin, not a library on a -l path: there is no
# libGL there, so -lGL is a link error rather than a missing-dep the probe above
# could have caught. (GLFW itself resolves normally — brew's dylib carries its
# own Cocoa/IOKit linkage — so only the GL half needs the split.)
ifeq ($(UNAME_S),Darwin)
GL_LIBS   ?= -framework OpenGL
# Quartz, not X11: naming DISPLAY/WAYLAND_DISPLAY in the setup epilogue would
# send a macOS reader looking for an env var that is never set here.
DESKTOP_DISPLAY_SAY := — on macOS that is the desktop session you are logged into
else
GL_LIBS   ?= -lGL
DESKTOP_DISPLAY_SAY := (DISPLAY / WAYLAND_DISPLAY)
endif
EGL_LIBS  ?= -lEGL

# The 3D-scene FBO smoke (10-spacetime-3d-overview.md T4) renders offscreen via
# EGL surfaceless + software Mesa, so it needs the EGL + GL 3.x headers (the app
# already links -lGL; the smoke adds -lEGL). Absence -> the smoke is not built and
# desktop-test prints why (the binary would ALSO self-skip at runtime on a host
# with the headers but no GL device — a machine with no GL at all still runs the
# pure camera test). Header-probed like keystone: glext.h and EGL/egl.h ship in
# libgl1-mesa-dev / libegl1-mesa-dev, which have no reliable pkg-config here.
#
# The probe is deliberately NOT given a Darwin branch: macOS ships no EGL at all
# (and no GL/glext.h — its headers live in the OpenGL framework), so it reports
# both as missing, which is the true answer. The smoke self-skips there with the
# same message any GL-less host gets, and the rest of desktop-test still runs.
DESKTOP_GL_MISSING :=
ifeq ($(shell ls /usr/include/GL/glext.h /usr/local/include/GL/glext.h 2>/dev/null | head -1),)
DESKTOP_GL_MISSING += libgl1-mesa-dev
endif
ifeq ($(shell ls /usr/include/EGL/egl.h /usr/local/include/EGL/egl.h 2>/dev/null | head -1),)
DESKTOP_GL_MISSING += libegl1-mesa-dev
endif

# The full app links unicorn/keystone/capstone (D4). Keystone's kit ships no
# reliable pkg-config, so it is not probed separately — the trio is installed
# together (make deps / the bindings base), so unicorn+capstone presence implies
# it, and KEYSTONE_LIBS falls back to -lkeystone.
# REPLAY vs the full trio. The PT-replay slice (08-observer-views.md T8) needs
# Unicorn + Capstone and NOT Keystone: it replays a recorded path, it assembles
# nothing. Gating it on the whole trio would have made it self-skip on a host
# with two of the three — and a test that skips where it could have run is not a
# gate, it is an absence (CLAUDE.md).
DESKTOP_REPLAY_MISSING :=
ifneq ($(shell pkg-config --exists unicorn 2>/dev/null && echo ok),ok)
DESKTOP_REPLAY_MISSING += libunicorn-dev
endif
ifneq ($(shell pkg-config --exists capstone 2>/dev/null && echo ok),ok)
DESKTOP_REPLAY_MISSING += libcapstone-dev
endif
DESKTOP_ENGINE_MISSING := $(DESKTOP_REPLAY_MISSING)

# ...and a HOST gate on top of the dependency one, kept deliberately separate.
# src/dataflow_pt.c compiles to a DF_PT_ENOSYS stub off Linux x86-64, and so do
# the blockstep purity/replayability scanners it reuses (both are guarded on
# __linux__ && __x86_64__), so off that platform the replay lane would build a
# binary whose only possible output is "the producer is a stub here" — a test
# that cannot pass, which is worse than one that says why it did not run. This
# is the hardware/platform class of gate CLAUDE.md keeps: recorded and
# self-skipped, not a dependency anyone can install.
#
# It is NOT folded into DESKTOP_ENGINE_MISSING above, and the order matters:
# that variable gates `desktop` itself, and the full app builds and runs fine
# here. The PT slice is one VIEW inside it, and that view already explains a
# stub producer at runtime rather than pretending to replay.
#
# BOTH halves of the producer's condition are checked. The OS half alone would
# leave the gate open on aarch64 Linux — which is not hypothetical, because it
# is exactly what `make docker-desktop` becomes on an Apple Silicon Mac
# (DOCKER_PLATFORM is empty by default, so the image is linux/arm64): inside the
# container UNAME_S is Linux, the base supplies unicorn+capstone, the lane opens,
# and the stub fails the test. Recommending that lane while it cannot pass would
# be worse than not gating at all. File-local like CLI_ARCH / SDE_ARCH; there is
# no global UNAME_M.
DESKTOP_REPLAY_ARCH := $(shell uname -m)
DESKTOP_REPLAY_HOST_MISSING :=
ifneq ($(UNAME_S),Linux)
DESKTOP_REPLAY_HOST_MISSING += a-Linux-host(this-is-$(UNAME_S))
endif
ifneq ($(DESKTOP_REPLAY_ARCH),x86_64)
DESKTOP_REPLAY_HOST_MISSING += an-x86-64-host(this-is-$(DESKTOP_REPLAY_ARCH))
endif
# Keystone IS probed after all. The Loom's fork engine (05-loom-day-one.md T5)
# is the first desktop TU to call asmtest_assemble, and a host with
# unicorn+capstone but no keystone (a `make deps` that ran without --asm) would
# otherwise reach a raw linker error instead of the guidance recipe below.
#
# pkg-config FIRST, header scan as the fallback. The scan alone hardcodes two
# prefixes, which is wrong on every Homebrew that is not Intel's: an arm64 Mac
# puts the header under /opt/homebrew, so the probe reported keystone missing
# and `make desktop` refused to build a link that would have SUCCEEDED —
# KEYSTONE_LIBS resolves through pkg-config, and brew's keystone (like the
# tree's own build-keystone.sh) installs a keystone.pc. Deciding presence the
# same way build-keystone.sh:19 and install-deps.sh do keeps the three answers
# from disagreeing; the header scan stays for a kit that really ships no .pc.
ifeq ($(strip $(shell pkg-config --exists keystone 2>/dev/null && echo ok \
        || ls /usr/include/keystone/keystone.h \
              /usr/local/include/keystone/keystone.h 2>/dev/null | head -1)),)
DESKTOP_ENGINE_MISSING += libkeystone-dev
endif

# DESKTOP_JOBS: the app/viewer object trees are ~90 independent C++ TUs (imgui +
# addons + desktop/src/) with no inter-TU dependency until the final link, so a
# plain `make desktop` — no -j anywhere in this file — compiles them one at a
# time. Default to the host's CPU count; override on a memory-constrained host
# (`make DESKTOP_JOBS=4 desktop`) since the heavier TUs — imgui itself, the node
# editor, TextEditor, ImPlot — each peak over 1GB RSS under -O2 -g.
DESKTOP_JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Guidance recipe (mirrors mk/cli.mk:100-110): print the apt line + the docker
# lane, then fail — so a bare host never sees a raw header/link error.
define DESKTOP_GUIDE
	@echo "$(1) is not buildable here — missing:$(2)"
	@echo ""
	@echo "  Recommended — build + run it in a container (no host deps):"
	@echo "      make docker-desktop"
	@echo ""
	@echo "  Or install the toolchain and retry (Debian/Ubuntu):"
	@echo "      sudo apt-get install -y libglfw3-dev libgl1-mesa-dev"
	@echo "      make deps                 # unicorn/keystone/capstone (full app)"
	@false
endef

.PHONY: desktop desktop-render desktop-test desktop-ui-test desktop-fmt desktop-fmt-check \
        docker-desktop desktop-setup desktop-setup-render \
        addon-fetch-test desktop-addon-compile-check

# --- addon supply chain (12-addon-supply-chain.md) ---------------------------
# addon-fetch-test: prove scripts/fetch-addon.sh fetches + verifies a pinned
# artifact and REFUSES an unpinned one, using the already-pinned linmath header
# as the fixture (network required, like the fetch scripts themselves).
addon-fetch-test:
	sh tests/fetch-addon-test.sh

# desktop-addon-compile-check: the imgui-repin gate (T3). Compile the probe with
# the desktop lane's EXACT flags at the CURRENT imgui pin, so any repin that
# breaks a vendored imgui_internal.h dependent fails here before it can land.
# ADDON_PROBE_FLAGS is appended by each addon's adopting task
# (`-DASMDESK_HAVE_<addon> -I<its build/addons home>`); empty until the first
# internal-header addon lands, where the probe still validates imgui_internal.h
# itself against the pin. Order-only dep on the grouped imgui fetch so a clean
# tree fetches imgui first.
# filter-out -MMD -MP: a -fsyntax-only check with no -o would otherwise litter a
# stray depfile in the repo root.
desktop-addon-compile-check: | $(IMGUI_HOME)/imgui.cpp $(ADDON_PROBE_DEPS)
	$(CXX) $(filter-out -MMD -MP,$(DESKTOP_CXXFLAGS)) $(ADDON_PROBE_FLAGS) \
	  -fsyntax-only desktop/test/addon_compile_probe.cpp
	@echo "desktop-addon-compile-check: OK (imgui $(IMGUI_VERSION); probe compiles with desktop flags)"

$(BUILD)/desktop/app/ui/capability_panel.o: \
    DESKTOP_CXXFLAGS += -DASMTEST_DESKTOP_CAN_PROBE=1
# The PT-replay slice (08-observer-views.md T8) is full-app only, twice over:
# it links the PT replay producer (ASMTEST_DESKTOP_HAVE_PT_REPLAY) and it asks
# the library's hwtrace status why live capture is unavailable
# (ASMTEST_DESKTOP_CAN_PROBE). The render tree compiles the SAME TU without
# either, so the viewer still explains itself — D4 kept, and the explanation
# blames the build rather than the host.
$(BUILD)/desktop/app/lv/ptslice.o: \
    DESKTOP_CXXFLAGS += -DASMTEST_DESKTOP_HAVE_PT_REPLAY=1 \
                        -DASMTEST_DESKTOP_CAN_PROBE=1
# The Author door's two engine calls compile in for the APP tree only; the
# viewer and the headless tests get the static licence tile instead (D4).
$(BUILD)/desktop/app/ui/author_door.o: \
    DESKTOP_CXXFLAGS += -DASMTEST_DESKTOP_CAN_AUTHOR=1
# The Scrubber's synthesise-register-history action (30 R3 T4) is the same story:
# the app tree gets the engine call (regsynth), the viewer + tests get the honest
# "full app only" note. Only the app object references regsynth.o.
$(BUILD)/desktop/app/vw/scrubber_draw.o: \
    DESKTOP_CXXFLAGS += -DASMTEST_DESKTOP_CAN_AUTHOR=1

$(BUILD)/asmtest-desktop: $(DESKTOP_APP_OBJ) $(DESKTOP_ENGINE_OBJ) \
                          $(DESKTOP_CAP_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) \
	  $(CAPSTONE_LIBS) $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(LINK_LIBBPF) \
	  $(GLFW_LIBS) $(GL_LIBS) $(FREETYPE_LIBS) -ldl -lpthread -o $@
	@echo "built $@ — the full app (GPL-2.0 as a whole; links the engines)"

$(BUILD)/asmtest-viewer: $(DESKTOP_RENDER_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(GLFW_LIBS) $(GL_LIBS) $(FREETYPE_LIBS) -o $@
	@echo "built $@ — the render-only viewer (engine-free; permissively distributable)"

# Both recipes recurse through a sub-$(MAKE) -j$(DESKTOP_JOBS) rather than
# listing the binary as a plain prerequisite: a bare `desktop: $(BUILD)/asmtest-
# desktop` would build that prerequisite under whatever -j (or none) the
# invoking `make desktop` itself was given, so a plain `make desktop` would stay
# single-threaded. The sub-make is cheap to re-run when already up to date (the
# same tradeoff desktop-setup's sub-make above makes), and DESKTOP_JOBS is still
# overridable per invocation (`make DESKTOP_JOBS=4 desktop`).
ifeq ($(strip $(DESKTOP_MISSING)$(DESKTOP_ENGINE_MISSING)),)
desktop:
	$(MAKE) -j$(DESKTOP_JOBS) $(BUILD)/asmtest-desktop
else
desktop:
	$(call DESKTOP_GUIDE,asmtest-desktop (full app),$(DESKTOP_MISSING) $(DESKTOP_ENGINE_MISSING))
endif

ifeq ($(strip $(DESKTOP_MISSING)),)
desktop-render:
	$(MAKE) -j$(DESKTOP_JOBS) $(BUILD)/asmtest-viewer
else
desktop-render:
	$(call DESKTOP_GUIDE,asmtest-viewer (render-only),$(DESKTOP_MISSING))
endif

# --- desktop-setup: bare host -> a GUI you can launch, in one command ---------
# The gates above (DESKTOP_MISSING / DESKTOP_ENGINE_MISSING) are $(shell) probes
# that make expands while READING this file, so their answers are fixed before
# any recipe runs. A setup target that installed the deps and then merely
# *depended* on `desktop` would still be judged against the pre-install probe and
# print the guidance text it just made obsolete. The build is therefore a
# RECURSIVE $(MAKE) — a second make process, which re-runs the probes against the
# host as it now is. This is the one place in this file that needs a sub-make.
#
# Every step is idempotent and self-skipping, so re-running on a set-up host is a
# plain incremental build: install-deps.sh skips what pkg-config already finds,
# and build-capstone.sh / build-keystone.sh exit early when their engine is
# installed. The two source builds are not optional extras — on every Linux
# package manager capstone and keystone have no distro package (see
# install-deps.sh's capstone_pkg/keystone_pkg comment), so a target that only ran
# the package manager would leave `make desktop` still gated.
#
# The pinned imgui + nlohmann/json sources need no step here: they are ordinary
# prerequisites of every desktop object (the fetch rules at the top of this file),
# so the sub-make fetches them on demand.
desktop-setup:
	@echo "== 1/3  host packages (glfw + GL + engines + build tools) =="
	sh scripts/install-deps.sh --desktop
	@echo "== 2/3  pinned engine source builds (skip if already installed) =="
	sh scripts/build-capstone.sh
	sh scripts/build-keystone.sh
	@echo "== 3/3  build both binaries (imgui/json fetched on demand) =="
	$(MAKE) desktop desktop-render
	@echo ""
	@echo "desktop setup complete — run it with:"
	@echo "    $(BUILD)/asmtest-desktop     # full app"
	@echo "    $(BUILD)/asmtest-viewer      # render-only viewer"
	@echo "Both open a window, so they need a display $(DESKTOP_DISPLAY_SAY)."
	@echo "Open a recording from the home screen's Learn door — the committed"
	@echo "corpus is in tests/golden-asmtrace/. Verify headlessly: make desktop-test"

# The viewer half alone: app backends, no engines, no source builds — so it works
# on a host where the GPL engines are unwanted (D4) and finishes far quicker.
desktop-setup-render:
	@echo "== 1/2  host packages (glfw + GL) =="
	sh scripts/install-deps.sh --desktop-render
	@echo "== 2/2  build the render-only viewer =="
	$(MAKE) desktop-render
	@echo ""
	@echo "viewer setup complete — run it with:"
	@echo "    $(BUILD)/asmtest-viewer      # needs a display; engine-free"

# desktop-test: the null-backend headless tests. No GLFW, no GL, no engines — the
# gate above never applies here (a test that could only self-skip is not a test,
# CLAUDE.md). vm_compat.o compiling in the test tree IS the regression test that
# keeps the reused asmspy headers C++-clean (03-desktop-shell.md T5).
DESKTOP_TESTS := $(BUILD)/desktop_test_null $(BUILD)/desktop_test_recording \
                 $(BUILD)/desktop_test_shell $(BUILD)/desktop_test_golden \
                 $(BUILD)/desktop_test_layout \
                 $(BUILD)/desktop_test_fonts \
                 $(BUILD)/desktop_test_palette \
                 $(BUILD)/desktop_test_wayfinding \
                 $(BUILD)/desktop_test_overview \
                 $(BUILD)/desktop_test_view_presence \
                 $(BUILD)/desktop_test_workspace_state \
                 $(BUILD)/desktop_test_settings \
                 $(BUILD)/desktop_test_perspectives \
                 $(BUILD)/desktop_test_ictedit \
                 $(BUILD)/desktop_test_asm_language \
                 $(BUILD)/desktop_test_theme \
                 $(BUILD)/desktop_test_honesty \
                 $(BUILD)/desktop_test_progress \
                 $(BUILD)/desktop_test_cvd \
                 $(BUILD)/desktop_test_terms \
                 $(BUILD)/desktop_test_filter \
                 $(BUILD)/desktop_test_primer \
                 $(BUILD)/desktop_test_slice_view_draw \
                 $(BUILD)/desktop_test_slice $(BUILD)/desktop_test_nav \
                 $(BUILD)/desktop_test_projection \
                 $(BUILD)/desktop_test_terrain \
                 $(BUILD)/desktop_test_trajectory \
                 $(BUILD)/desktop_test_converge \
                 $(BUILD)/desktop_test_drillin \
                 $(BUILD)/desktop_test_camera \
                 $(BUILD)/desktop_test_diff $(BUILD)/desktop_test_canvas \
                 $(BUILD)/desktop_test_timeline \
                 $(BUILD)/desktop_test_scrubber \
                 $(BUILD)/desktop_test_scrubber_draw \
                 $(BUILD)/desktop_test_abixray \
                 $(BUILD)/desktop_test_abixray_draw \
                 $(BUILD)/desktop_test_slice_view \
                 $(BUILD)/desktop_test_diff_view \
                 $(BUILD)/desktop_test_selection \
                 $(BUILD)/desktop_test_find \
                 $(BUILD)/desktop_test_undo \
                 $(BUILD)/desktop_test_loom_gutter \
                 $(BUILD)/desktop_test_data_readers \
                 $(BUILD)/desktop_test_completeness_view \
                 $(BUILD)/desktop_test_slice_diff \
                 $(BUILD)/desktop_test_loom_fabric \
                 $(BUILD)/desktop_test_loom_plan \
                 $(BUILD)/desktop_test_loom_chrome \
                 $(BUILD)/desktop_test_loom_draw \
                 $(BUILD)/desktop_test_loom_lineage \
                 $(BUILD)/desktop_test_loom_parity \
                 $(BUILD)/desktop_test_loom_annex \
                 $(BUILD)/desktop_test_loom_takeview \
                 $(BUILD)/desktop_test_loom_golden \
                 $(BUILD)/desktop_test_walkthrough \
                 $(BUILD)/desktop_test_capview \
                 $(BUILD)/desktop_test_author_vm \
                 $(BUILD)/desktop_test_live_session \
                 $(BUILD)/desktop_test_budget \
                 $(BUILD)/desktop_test_inspect \
                 $(BUILD)/desktop_test_obs_syscalls \
                 $(BUILD)/desktop_test_obs_watch \
                 $(BUILD)/desktop_test_obs_topo \
                 $(BUILD)/desktop_test_obs_hotedges \
                 $(BUILD)/desktop_test_obs_tree \
                 $(BUILD)/desktop_test_obs_region \
                 $(BUILD)/desktop_test_obs_disasm \
                 $(BUILD)/desktop_test_obs_ptslice \
                 $(BUILD)/desktop_test_obs_draw

# The fabric model links fabric.o and NOTHING else — that link line is the proof
# that asmtest-viewer can weave a recording with zero engine deps (D4), the same
# argument $(BUILD)/desktop_test_slice makes for the client-side closure.
$(BUILD)/desktop_test_loom_fabric: $(BUILD)/desktop/test/t/test_loom_fabric.o \
    $(BUILD)/desktop/test/lo/fabric.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_plan: $(BUILD)/desktop/test/t/test_loom_plan.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/fabric_plan.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_chrome: $(BUILD)/desktop/test/t/test_loom_chrome.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/fabric_plan.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_lineage: \
    $(BUILD)/desktop/test/t/test_loom_lineage.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/lineage.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The SECOND binary here that links an engine object (build/dataflow.o, the pure
# L0/L1/L2 sink with no Capstone or Unicorn undefined symbols), for the same
# reason $(BUILD)/desktop_test_slice_diff does: the Loom's generation walk adds
# BFS depth to the closure relation, and a check that the depth did not change
# WHICH steps are reached is only worth anything against the real slicer.
$(BUILD)/desktop_test_loom_parity: \
    $(BUILD)/desktop/test/t/test_loom_parity.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/lineage.o \
    $(BUILD)/dataflow.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_annex: $(BUILD)/desktop/test/t/test_loom_annex.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/annex.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_takeview: \
    $(BUILD)/desktop/test/t/test_loom_takeview.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/fabric_plan.o \
    $(BUILD)/desktop/test/lo/take_view.o $(BUILD)/desktop/test/an/diff.o \
    $(BUILD)/desktop/test/an/slice.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_golden: \
    $(BUILD)/desktop/test/t/test_loom_golden.o \
    $(DESKTOP_TEST_LOOM) $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_AN)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# fabric_imgui.o now also carries draw_loom (the panel), so the painter smoke
# links the whole loom half plus the doc model it reads from — still no GL, no
# GLFW and no engines.
$(BUILD)/desktop_test_walkthrough: \
    $(BUILD)/desktop/test/t/test_walkthrough.o \
    $(BUILD)/desktop/test/src/walkthrough.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# capview.o is a pure function of the status data a caller already probed, which
# is what makes the panel's two UI laws assertable on a container with no PT, no
# LBR and no AMD silicon. It now links lv/inspect.o too (18-breach-stops.md T4):
# capview_remedy REUSES the inspect_door attach_verdict remedy map so the two
# panels never give divergent advice. inspect.o references only libc/libstdc++
# (no engine, no session object), so the pure closure holds.
$(BUILD)/desktop_test_capview: $(BUILD)/desktop/test/t/test_capview.o \
    $(BUILD)/desktop/test/src/capview.o $(BUILD)/desktop/test/lv/inspect.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# author_vm.o maps engine STRUCTS, never calls engine functions, so the Author
# door's rules are pinned without Keystone or Unicorn on the host. It now links
# doc/recording.o too (18-breach-stops.md T3): author_recording materialises a
# run into a Recording so the save path reuses save_recording_file — pure JSON
# assembly, no engine, so the round-trip is testable on any host.
$(BUILD)/desktop_test_author_vm: $(BUILD)/desktop/test/t/test_author_vm.o \
    $(BUILD)/desktop/test/src/author_vm.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The live capture host links session.o + the doc model and NOTHING else — no
# ImGui, no GL, no engines. That link line is the standing proof that hosting a
# live session costs the render-only viewer no engine dependency (D9): the
# capture host is the `asmspy --serve` SUBPROCESS, not a linked tracer.
$(BUILD)/desktop_test_live_session: \
    $(BUILD)/desktop/test/t/test_live_session.o \
    $(BUILD)/desktop/test/lv/session.o $(BUILD)/desktop/test/vw/tree.o \
    $(BUILD)/desktop/test/vw/observer.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# budget.o is a pure decision table — it links nothing at all, which is what
# makes every mode-pair assertable on a host with no target to attach to.
$(BUILD)/desktop_test_budget: $(BUILD)/desktop/test/t/test_budget.o \
    $(BUILD)/desktop/test/lv/budget.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# --- the live Observer views (08-observer-views.md) --------------------------
# Every one of these links its own builder + observer.o + the doc model, and
# NOTHING else: no ImGui, no GL, no engine, and no live session object. That
# link line is the doc's central claim made mechanical — each view renders
# identically from a recording, which is how CI tests views of hardware it does
# not have (an AMD IBS survey, an arm64 watchpoint refusal, a JIT code image).
$(BUILD)/desktop_test_obs_syscalls: \
    $(BUILD)/desktop/test/t/test_obs_syscalls.o \
    $(BUILD)/desktop/test/vw/syscalls.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_watch: $(BUILD)/desktop/test/t/test_obs_watch.o \
    $(BUILD)/desktop/test/vw/watch.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# topo additionally links nav.o: its drill-in is a deep LINK through 04's
# router, not a direct call into another view, so the link's round trip is part
# of what this test pins.
# topo additionally links nav.o (the drill-in is a deep LINK through 04's
# router) and graph_nav.o (15 T3): the graph layout's node positions ARE the
# app's, not the library's, and this test pins that + the click-routing + the
# large-fixture cull, all on the pure model (no ImGui, no node-editor).
$(BUILD)/desktop_test_obs_topo: $(BUILD)/desktop/test/t/test_obs_topo.o \
    $(BUILD)/desktop/test/vw/topo.o $(BUILD)/desktop/test/vw/observer.o \
    $(BUILD)/desktop/test/vw/graph_nav.o \
    $(BUILD)/desktop/test/src/nav.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_hotedges: \
    $(BUILD)/desktop/test/t/test_obs_hotedges.o \
    $(BUILD)/desktop/test/vw/hotedges.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# tree also links graph_nav.o (15 T3) — the call-tree graph layout — plus nav.o
# (the click router this test drives) and topo.o (graph_nav's shared drill-in
# link builder). Still no ImGui, no node-editor: the layout is pure and asserted
# as the app's own, never the library's.
$(BUILD)/desktop_test_obs_tree: $(BUILD)/desktop/test/t/test_obs_tree.o \
    $(BUILD)/desktop/test/vw/tree.o $(BUILD)/desktop/test/vw/observer.o \
    $(BUILD)/desktop/test/vw/graph_nav.o $(BUILD)/desktop/test/vw/topo.o \
    $(BUILD)/desktop/test/src/nav.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_region: $(BUILD)/desktop/test/t/test_obs_region.o \
    $(BUILD)/desktop/test/vw/region.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_disasm: $(BUILD)/desktop/test/t/test_obs_disasm.o \
    $(BUILD)/desktop/test/vw/disasm.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The DRAW half of the Observer deck, under ImGui's null backend: the model
# tests assert what each view decides, this one asserts the deck draws it —
# including the refusal paths (a revealed payload, a refused arm, an illegal
# filter, an invocation with no footer, a missing code image), which are exactly
# the ones a happy-path smoke never reaches. No GL, no GLFW, no engines.
$(BUILD)/desktop_test_obs_draw: $(BUILD)/desktop/test/t/test_obs_draw.o \
    $(DESKTOP_TEST_OBS) \
    $(DESKTOP_OBS_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(BUILD)/desktop/test/addon/textselect.o \
    $(BUILD)/desktop/test/addon/imgui_canvas.o \
    $(DESKTOP_NODEEDITOR_OBJ_test) \
    $(DESKTOP_IMPLOT_OBJ_test) \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) \
    $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(BUILD)/desktop/test/vw/overview.o \
    $(DESKTOP_TEST_UI_OBJ) \
    $(DESKTOP_TEST_DA) $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The PT slice's GATE and input assembly, in a build with NO producer — which is
# the render-only viewer's situation exactly, and the one where the explanation
# has to be right. The replay itself is desktop_test_ptslice_run below.
$(BUILD)/desktop_test_obs_ptslice: $(BUILD)/desktop/test/t/test_obs_ptslice.o \
    $(BUILD)/desktop/test/lv/ptslice.o $(BUILD)/desktop/test/vw/disasm.o \
    $(BUILD)/desktop/test/vw/observer.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The Inspect door's two decisions. Links inspect.o + session.o + the doc model
# — still no ImGui, no GL, no engines: reading /proc and labelling evidence are
# things the VIEWER does, which is why the door works with no tracer linked.
$(BUILD)/desktop_test_inspect: $(BUILD)/desktop/test/t/test_inspect.o \
    $(BUILD)/desktop/test/lv/inspect.o $(BUILD)/desktop/test/lv/session.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_loom_draw: $(BUILD)/desktop/test/t/test_loom_draw.o \
    $(DESKTOP_TEST_LOOM) $(BUILD)/desktop/test/lo/fabric_imgui.o \
    $(BUILD)/desktop/test/vw/overview.o \
    $(DESKTOP_TEST_UI_OBJ) \
    $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The differential slice-parity test is the ONE binary here that links an engine
# object — build/dataflow.o, the pure L0/L1/L2 sink, which has no Capstone or
# Unicorn undefined symbols and so builds with cc alone. It rides desktop-test
# unconditionally: it is what pins the viewer's closure to the engine's, and a
# check that could only self-skip would not pin anything (CLAUDE.md). Nothing in
# desktop/src/ links it, which is why asmtest-viewer stays engine-free (D4).
$(BUILD)/desktop_test_slice_diff: $(BUILD)/desktop/test/t/test_slice_diff.o \
    $(BUILD)/desktop/test/an/slice.o $(BUILD)/dataflow.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_slice: $(BUILD)/desktop/test/t/test_slice.o \
    $(BUILD)/desktop/test/an/slice.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The projection (10-spacetime-3d-overview.md T1) links space/projection.o and
# NOTHING else — the same engine-free closure proof test_slice makes above, now
# for the address-space terrain plane.
$(BUILD)/desktop_test_projection: $(BUILD)/desktop/test/t/test_projection.o \
    $(BUILD)/desktop/test/sp/projection.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The terrain (10-spacetime-3d-overview.md T2) links space/terrain.o +
# space/projection.o + the trace canvas builder it REUSES for per-offset heat
# (04-T3) + the document model, and NOTHING else — the same engine-free closure
# proof test_projection makes for the plane, now for the density over it (D4).
$(BUILD)/desktop_test_terrain: $(BUILD)/desktop/test/t/test_terrain.o \
    $(BUILD)/desktop/test/sp/terrain.o $(BUILD)/desktop/test/sp/projection.o \
    $(BUILD)/desktop/test/vw/canvas.o $(BUILD)/desktop/test/an/diff.o \
    $(BUILD)/desktop/test/an/slice.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The trajectory builder (10-spacetime-3d-overview.md T3) links space/trajectory.o
# + space/projection.o (it projects abs vertices to prove they land on the plane)
# + doc/recording.o (it loads NDJSON fixtures) and NOTHING else — no ImGui, no
# GL, no engine — the same engine-free closure proof test_projection makes.
$(BUILD)/desktop_test_trajectory: $(BUILD)/desktop/test/t/test_trajectory.o \
    $(BUILD)/desktop/test/sp/trajectory.o $(BUILD)/desktop/test/sp/projection.o \
    $(BUILD)/desktop/test/doc/recording.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The drill-in router + the two honesty invariants (10-spacetime-3d-overview.md T6)
# links the GL-free pick resolver (scene3d/pick.o) + the space/ models it routes
# from (terrain + projection + trajectory) + nav.o (04's deep-link router the pick
# lands in) + the trace canvas builder (it reads the truncation banner the torn
# drill-in must carry, 04-T3) + the doc model — and NOTHING else: no ImGui, no GL,
# no engine. The same engine-free closure proof test_projection makes, now for the
# pick path that reaches 04's router (D4). diff.o/slice.o ride canvas.o's closure.
$(BUILD)/desktop_test_drillin: $(BUILD)/desktop/test/t/test_drillin.o \
    $(BUILD)/desktop/test/s3/pick.o $(BUILD)/desktop/test/sp/terrain.o \
    $(BUILD)/desktop/test/sp/projection.o $(BUILD)/desktop/test/sp/trajectory.o \
    $(BUILD)/desktop/test/vw/canvas.o $(BUILD)/desktop/test/an/diff.o \
    $(BUILD)/desktop/test/an/slice.o $(BUILD)/desktop/test/src/nav.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The convergence detector + the incremental live feed (10-spacetime-3d-overview.md
# T5) links space/converge.o + trajectory.o + projection.o (it places PC vertices
# on the plane) + the live capture host session.o + the doc model — and NOTHING
# else: no ImGui, no GL, no engine. session.o is the `asmspy --serve` SUBPROCESS
# host (D9), so even the fake-serve growth case stays engine-free — the same closure
# proof test_trajectory and test_live_session both make.
$(BUILD)/desktop_test_converge: $(BUILD)/desktop/test/t/test_converge.o \
    $(BUILD)/desktop/test/sp/converge.o $(BUILD)/desktop/test/sp/trajectory.o \
    $(BUILD)/desktop/test/sp/projection.o $(BUILD)/desktop/test/lv/session.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The orbit camera (10-spacetime-3d-overview.md T4 step 4) is pure header-only
# math over the pinned linmath.h, so its test links NOTHING but its own object —
# the engine- AND GL-free closure proof for the camera. Runs on any host (no GL),
# so it rides DESKTOP_TESTS unconditionally. The linmath fetch is an order-only
# prereq (like imgui/json), so a clean tree fetches it on demand.
$(BUILD)/desktop/test/t/test_camera.o \
$(BUILD)/desktop/test/t/test_scene_fbo.o: | $(LINMATH_HOME)/linmath.h
$(BUILD)/desktop_test_camera: $(BUILD)/desktop/test/t/test_camera.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The GL FBO smoke (T4): scene.o + pick.o + the pure space/ + doc model it renders
# from, linked with EGL + GL for the surfaceless offscreen context. GATED on the
# GL/EGL headers (DESKTOP_GL_MISSING) — built and run under docker-desktop (which
# installs software Mesa + EGL), skipped-with-a-reason on a bare host. Nothing in
# desktop/src/ but scene.o links GL, so asmtest-viewer stays engine-free (D4).
$(BUILD)/desktop_test_scene_fbo: $(BUILD)/desktop/test/t/test_scene_fbo.o \
    $(BUILD)/desktop/test/s3/scene.o $(BUILD)/desktop/test/s3/pick.o \
    $(BUILD)/desktop/test/sp/terrain.o $(BUILD)/desktop/test/sp/projection.o \
    $(BUILD)/desktop/test/sp/trajectory.o $(BUILD)/desktop/test/sp/converge.o \
    $(BUILD)/desktop/test/vw/canvas.o $(BUILD)/desktop/test/an/diff.o \
    $(BUILD)/desktop/test/an/slice.o $(BUILD)/desktop/test/src/nav.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(EGL_LIBS) $(GL_LIBS) -o $@

$(BUILD)/desktop_test_nav: $(BUILD)/desktop/test/t/test_nav.o \
    $(BUILD)/desktop/test/src/nav.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The dock layout manager (13-foundation-moves.md T2): layout.o + imgui core,
# nothing else — the manager is self-contained (DockBuilder only).
$(BUILD)/desktop_test_layout: $(BUILD)/desktop/test/t/test_layout.o \
    $(BUILD)/desktop/test/ui/layout.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The font loader (13-foundation-moves.md F3): fonts.o + imgui core. Loads the
# real TTFs via stb_truetype, so it runs on any lane (freetype is a Docker-only
# rasteriser gate, not needed here). Carries the TTF-path defines + IconsCodicons.h.
$(BUILD)/desktop/test/t/test_fonts.o: DESKTOP_CXXFLAGS += $(DESKTOP_FONT_DEFS)
$(BUILD)/desktop/test/t/test_fonts.o: | $(ICONFONT_HOME)/IconsCodicons.h \
    $(IMGUINOTIFY_HOME)/IconsFontAwesome6.h \
    $(JBM_HOME)/JetBrainsMono-Regular.ttf $(CODICON_HOME)/codicon.ttf \
    $(IMGUINOTIFY_HOME)/fa-solid-900.ttf
$(BUILD)/desktop_test_fonts: $(BUILD)/desktop/test/t/test_fonts.o \
    $(BUILD)/desktop/test/ui/fonts.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_diff: $(BUILD)/desktop/test/t/test_diff.o \
    $(BUILD)/desktop/test/an/diff.o $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_canvas: $(BUILD)/desktop/test/t/test_canvas.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_timeline: $(BUILD)/desktop/test/t/test_timeline.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_slice_view: $(BUILD)/desktop/test/t/test_slice_view.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# --- 22-selection-and-search.md tests ----------------------------------------
# (test_selection links DESKTOP_TEST_SHELL_OBJ, defined ~130 lines below, so its
# rule lives beside test_shell — a prerequisite list expands at parse time, so it
# must come AFTER the variable is set.)
#
# T3: the global find MODEL over the decoded streams + a hand-built Observer deck
# — the pure view/analysis builders + the observer pure set, engine-free (D4).
$(BUILD)/desktop_test_find: $(BUILD)/desktop/test/t/test_find.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_OBS) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T4: the app-level undo STACK is header-only (ui/undo.h), so its test links
# nothing but its own object — the pure closure proof for the command stack.
$(BUILD)/desktop_test_undo: $(BUILD)/desktop/test/t/test_undo.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T4: the Loom takes-gutter accumulator + its reversible remove/clear are pure
# list edits over LoomState::takes (header-only helpers), so its test links alone.
$(BUILD)/desktop_test_loom_gutter: $(BUILD)/desktop/test/t/test_loom_gutter.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The canvas-wrapped slice DRAW (15 T2), null-backend smoke: slice_view_draw.o +
# canvas_draw.o (draw_banner) + imgui_canvas.o + imgui core.
$(BUILD)/desktop_test_slice_view_draw: $(BUILD)/desktop/test/t/test_slice_view_draw.o \
    $(BUILD)/desktop/test/vw/slice_view_draw.o $(BUILD)/desktop/test/vw/canvas_draw.o \
    $(BUILD)/desktop/test/ui/legend.o \
    $(BUILD)/desktop/test/addon/imgui_canvas.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The one semantic palette (24-one-visual-language.md T1): the accessor values +
# the source-lint over the drift files + the shared legend's null-backend smoke.
# Links ONLY legend.o + imgui core — theme.h is header-only, engine-free (D4), so
# this link line is itself the proof the palette carries no engine dependency.
$(BUILD)/desktop_test_theme: $(BUILD)/desktop/test/t/test_theme.o \
    $(BUILD)/desktop/test/ui/legend.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The CVD-safe palette + second-channel gate (24 T2): cvd.o (pure maths) +
# legend.o (the ONE encoding table the assert reads) + imgui core. Engine-free.
$(BUILD)/desktop_test_cvd: $(BUILD)/desktop/test/t/test_cvd.o \
    $(BUILD)/desktop/test/ui/cvd.o $(BUILD)/desktop/test/ui/legend.o \
    $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The glossary-sourced term registry (24 T3): terms.o (generated table +
# lookup/meta) + imgui core. Reads the SAME glossary the build step parses (path
# via a compile define) to prove one source. terms.o (test tree) is the plain-
# list build, so no imsearch link is needed.
$(BUILD)/desktop/test/t/test_terms.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GLOSSARY_MD='"docs/project/glossary.md"'
$(BUILD)/desktop_test_terms: $(BUILD)/desktop/test/t/test_terms.o \
    $(BUILD)/desktop/test/ui/terms.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The one filter + column sort + discrete-time reason (24 T4): filter.o +
# timepos.o + imgui core. Pure model asserts (N of M, sort order, reasons).
$(BUILD)/desktop_test_filter: $(BUILD)/desktop/test/t/test_filter.o \
    $(BUILD)/desktop/test/ui/filter.o $(BUILD)/desktop/test/ui/timepos.o \
    $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The first-open primer (24 T5): primer.o + imgui core. State transitions +
# a null-backend draw smoke.
$(BUILD)/desktop_test_primer: $(BUILD)/desktop/test/t/test_primer.o \
    $(BUILD)/desktop/test/ui/primer.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The Author-door code editor (17 T2): TextEditor.o + imgui core. The test needs
# TextEditor.h fetched (+ the guard applied by fetch-ictedit.sh).
$(BUILD)/desktop/test/t/test_ictedit.o: | $(ICTEDIT_HOME)/TextEditor.h
$(BUILD)/desktop_test_ictedit: $(BUILD)/desktop/test/t/test_ictedit.o \
    $(BUILD)/desktop/test/addon/TextEditor.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The Author editor's assembly syntax highlighting (17 T2 follow-on): the
# language definitions + TextEditor.o + imgui core, and NO engine — asm_language
# takes arch/dialect as ints, so the whole per-dialect rule set is asserted on a
# host with neither Keystone nor Unicorn. The colourings themselves are checked
# by driving the real colorizer (SetText colorizes eagerly) and reading back
# IterateIdentifiers, so this pins behaviour rather than table contents alone.
$(BUILD)/desktop/test/t/test_asm_language.o: | $(ICTEDIT_HOME)/TextEditor.h
$(BUILD)/desktop_test_asm_language: $(BUILD)/desktop/test/t/test_asm_language.o \
    $(BUILD)/desktop/test/ui/asm_language.o \
    $(BUILD)/desktop/test/addon/TextEditor.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The register time-travel scrubber (09-teaching-producers.md T3). The PURE
# builder links scrubber.o + stepindex.o + the doc model and NOTHING else — no
# ImGui, no engine — the same engine-free closure proof test_timeline makes, now
# for the register deck. stepindex.o is the shared index (05's now-column reads
# the same one), which is why it links here rather than into a view group.
$(BUILD)/desktop_test_scrubber: $(BUILD)/desktop/test/t/test_scrubber.o \
    $(BUILD)/desktop/test/vw/scrubber.o $(BUILD)/desktop/test/an/stepindex.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The scrubber's DRAW half under ImGui's null backend — the same split the
# Observer deck's draw smoke uses (test_obs_draw): the model test asserts what
# the scrubber decides, this one asserts the deck draws it, including the torn
# placard and the producer-absent message. No GL, no GLFW, no engines.
# canvas_draw.o carries the shared draw_banner placard (the refusal/truncation
# banner the torn-edge and producer-absent paths raise); it links here for that
# one symbol, no canvas model builder needed.
$(BUILD)/desktop_test_scrubber_draw: \
    $(BUILD)/desktop/test/t/test_scrubber_draw.o \
    $(BUILD)/desktop/test/vw/scrubber_draw.o \
    $(BUILD)/desktop/test/vw/scrubber.o $(BUILD)/desktop/test/an/stepindex.o \
    $(BUILD)/desktop/test/vw/canvas_draw.o \
    $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The ABI x-ray (09-teaching-producers.md T4). The PURE builder links abixray.o
# + the two scrubbers it composes (scrubber.o + the shared stepindex.o) +
# walkthrough.o (the rail it is driven by) + the doc model, and NOTHING else — no
# ImGui, no engine: the same engine-free closure proof test_scrubber makes, now
# for the locked two-pane x-ray (D4).
$(BUILD)/desktop_test_abixray: $(BUILD)/desktop/test/t/test_abixray.o \
    $(BUILD)/desktop/test/vw/abixray.o $(BUILD)/desktop/test/vw/scrubber.o \
    $(BUILD)/desktop/test/an/stepindex.o \
    $(BUILD)/desktop/test/src/walkthrough.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The x-ray's DRAW half under ImGui's null backend — the same split the scrubber
# uses (test_scrubber_draw): the model test asserts what the x-ray decides, this
# one asserts the two-pane deck draws it, including the refusal paths (unaligned
# panes, a producer-absent pane, a torn pane). canvas_draw.o carries the shared
# draw_banner placard those paths raise. No GL, no GLFW, no engines.
$(BUILD)/desktop_test_abixray_draw: \
    $(BUILD)/desktop/test/t/test_abixray_draw.o \
    $(BUILD)/desktop/test/vw/abixray_draw.o $(BUILD)/desktop/test/vw/abixray.o \
    $(BUILD)/desktop/test/vw/scrubber.o $(BUILD)/desktop/test/an/stepindex.o \
    $(BUILD)/desktop/test/src/walkthrough.o \
    $(BUILD)/desktop/test/vw/canvas_draw.o \
    $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_diff_view: $(BUILD)/desktop/test/t/test_diff_view.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_data_readers: \
    $(BUILD)/desktop/test/t/test_data_readers.o $(DESKTOP_TEST_DA)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_completeness_view: \
    $(BUILD)/desktop/test/t/test_completeness_view.o $(DESKTOP_TEST_DA)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The ONE graded honesty vocabulary (23-graded-truth-layer.md T1): the pure grader
# against the committed dishonesty fixtures + the graded chrome (banner/chip/glyph)
# drawing under the null backend. Links canvas_draw.o (the components), the doc
# model (to load the fixtures) and imgui — no engine, no GL.
$(BUILD)/desktop_test_honesty: $(BUILD)/desktop/test/t/test_honesty.o \
    $(BUILD)/desktop/test/vw/canvas_draw.o $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The uniform busy signal + the 3D-scrub degrade decision (23 T4): pure and
# header-only, so it links NOTHING — the same closure argument test_budget makes.
$(BUILD)/desktop_test_progress: $(BUILD)/desktop/test/t/test_progress.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_null: $(BUILD)/desktop/test/t/test_null_render.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_recording: $(BUILD)/desktop/test/t/test_recording.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# shell.o now draws the replay views, so the shell test links the pure builders,
# their draw halves and the data readers. It still needs no GL and no engines:
# ImGui's null backend renders every one of those paths.
DESKTOP_TEST_SHELL_OBJ := $(BUILD)/desktop/test/ui/shell.o \
    $(BUILD)/desktop/test/ui/layout.o \
    $(BUILD)/desktop/test/ui/palette.o \
    $(BUILD)/desktop/test/ui/wayfinding.o \
    $(BUILD)/desktop/test/vw/overview.o \
    $(BUILD)/desktop/test/ui/learn_door.o \
    $(BUILD)/desktop/test/addon/imsearch.o \
    $(BUILD)/desktop/test/ui/capability_panel.o \
    $(BUILD)/desktop/test/ui/inspect_door.o \
    $(BUILD)/desktop/test/ui/view_presence.o \
    $(BUILD)/desktop/test/ui/settings.o \
    $(BUILD)/desktop/test/ui/perspectives.o \
    $(DESKTOP_TEST_UI_OBJ) \
    $(BUILD)/desktop/test/addon/imguifiledialog.o \
    $(DESKTOP_TEST_LIVE) \
    $(BUILD)/desktop/test/src/walkthrough.o \
    $(BUILD)/desktop/test/src/capview.o \
    $(BUILD)/desktop/test/src/author_vm.o \
    $(BUILD)/desktop/test/ui/author_door.o $(BUILD)/desktop/test/ui/asm_language.o \
    $(BUILD)/desktop/test/addon/TextEditor.o $(DESKTOP_TEST_DOC) \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DA) \
    $(BUILD)/desktop/test/an/stepindex.o \
    $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(BUILD)/desktop/test/vw/scrubber.o \
    $(BUILD)/desktop/test/vw/scrubber_draw.o \
    $(BUILD)/desktop/test/vw/abixray.o \
    $(BUILD)/desktop/test/vw/abixray_draw.o \
    $(DESKTOP_TEST_OBS) \
    $(DESKTOP_OBS_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(BUILD)/desktop/test/addon/textselect.o \
    $(BUILD)/desktop/test/addon/imgui_canvas.o \
    $(DESKTOP_NODEEDITOR_OBJ_test) \
    $(DESKTOP_IMPLOT_OBJ_test) \
    $(DESKTOP_TEST_LOOM) \
    $(DESKTOP_LOOM_DRAW:%=$(BUILD)/desktop/test/lo/%.o) \
    $(BUILD)/desktop/test/sp/projection.o \
    $(BUILD)/desktop/test/sp/terrain.o \
    $(BUILD)/desktop/test/sp/trajectory.o \
    $(BUILD)/desktop/test/sp/converge.o \
    $(BUILD)/desktop/test/s3/hud.o \
    $(BUILD)/desktop/test/s3/pick.o $(DESKTOP_TEST_IG)

$(BUILD)/desktop_test_shell: $(BUILD)/desktop/test/t/test_shell.o \
    $(DESKTOP_TEST_SHELL_OBJ) $(BUILD)/desktop/test/src/vm_compat.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# --- 21-spine-navigation.md tests --------------------------------------------
# T1: the command palette. build_palette + palette_parse_goto need a real
# ShellState (opened recordings, the wired router), so they reuse the shell object
# set — the same engine-free closure the shell test links, plus the golden corpus
# for the go-to/dispatch/enumeration assertions.
$(BUILD)/desktop/test/t/test_palette.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop_test_palette: $(BUILD)/desktop/test/t/test_palette.o \
    $(DESKTOP_TEST_SHELL_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T2: the persistent wayfinding chrome. disambiguated_label is a pure function of
# paths (hand-built Workspace, no I/O); breadcrumb_model reads nav.current after a
# real dt_nav_go jump, so it links the shell set + the corpus.
$(BUILD)/desktop/test/t/test_wayfinding.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop_test_wayfinding: $(BUILD)/desktop/test/t/test_wayfinding.o \
    $(DESKTOP_TEST_SHELL_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T3: the overview/minimap model. overview.o is pure; it links the timeline
# builder (to derive a strip from a REAL recording's rows and pin no-fabrication)
# + the doc model, and NOTHING else — the same engine-free closure proof
# test_timeline makes, now for the minimap projection. The fabric case is a hand-
# built loom_fabric_t, so no loom builder is needed.
$(BUILD)/desktop/test/t/test_overview.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop_test_overview: $(BUILD)/desktop/test/t/test_overview.o \
    $(BUILD)/desktop/test/vw/overview.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# 22 T1: the ONE shared selection projects consistently to the timeline / slice /
# Loom models. It builds a real shell (shell_open), so it links the shell obj set
# — the same engine-free closure test_shell links (and must come AFTER
# DESKTOP_TEST_SHELL_OBJ is defined, since prerequisites expand at parse time).
$(BUILD)/desktop_test_selection: $(BUILD)/desktop/test/t/test_selection.o \
    $(DESKTOP_TEST_SHELL_OBJ) $(BUILD)/desktop/test/src/vm_compat.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# --- 20-workspace-and-settings.md tests --------------------------------------
# T1: the data-driven view set. view_presence.o reads observer_has_any (the
# observer draw TU) + regions_from_codeimage (space/terrain), so it reuses the
# shell object set — the same engine-free closure the shell test links, plus the
# fixtures + golden corpus for the min-trace / codeimage cases.
$(BUILD)/desktop/test/t/test_view_presence.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"' \
                         -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop_test_view_presence: \
    $(BUILD)/desktop/test/t/test_view_presence.o $(DESKTOP_TEST_SHELL_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T3/T4: the persisted workspace store. workspace_state.o links nlohmann/json
# only — no ImGui, no engine — so the round-trip is a pure model assert.
$(BUILD)/desktop_test_workspace_state: \
    $(BUILD)/desktop/test/t/test_workspace_state.o \
    $(BUILD)/desktop/test/doc/workspace_state.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T5: the Settings model. settings.o + imgui core (it touches io.FontGlobalScale).
$(BUILD)/desktop_test_settings: $(BUILD)/desktop/test/t/test_settings.o \
    $(BUILD)/desktop/test/ui/settings.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# T4: named perspectives + filter presets. perspectives.o + layout.o (it applies
# a preset perspective through layout_build) + imgui core.
$(BUILD)/desktop_test_perspectives: $(BUILD)/desktop/test/t/test_perspectives.o \
    $(BUILD)/desktop/test/ui/perspectives.o $(BUILD)/desktop/test/ui/layout.o \
    $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The ui-test binary (17-T1): the SAME app object set as the shell test, but in
# the `uitest` tree (built with -DIMGUI_ENABLE_TEST_ENGINE), plus the engine TUs
# and the test driver. Path-rewrite the shell-obj set rather than restate it, so
# it can never drift from what the shell test links. Null backend, no GL, no
# engines — the interaction layer is exercised headless. -lpthread for the
# engine's std::thread coroutine backend.
DESKTOP_UITEST_APP_OBJ := \
    $(subst /desktop/test/,/desktop/uitest/,$(DESKTOP_TEST_SHELL_OBJ)) \
    $(BUILD)/desktop/uitest/src/vm_compat.o
DESKTOP_UITEST_ENGINE_OBJ := $(ITE_ENGINE_SRC:%=$(BUILD)/desktop/uitest/ite/%.o)

$(BUILD)/desktop_ui_test: $(BUILD)/desktop/uitest/t/test_ui.o \
    $(DESKTOP_UITEST_APP_OBJ) $(DESKTOP_UITEST_ENGINE_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $(ITE_CXX) $^ -o $@ -lpthread

$(BUILD)/desktop_test_golden: $(BUILD)/desktop/test/t/test_golden.o \
    $(DESKTOP_TEST_SHELL_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The FULL-BUILD half of desktop-test: the Loom's fork engine (05 T5/T7) is the
# one desktop TU that links the engines, so its test needs unicorn + keystone.
# This is a host-capability gate, NOT a self-skip lane: `make docker-desktop`
# installs all three and runs it every time (CLAUDE.md — a test that can only
# ever self-skip is not a test), and the gate below prints why on a bare host.
DESKTOP_ENGINE_TESTS := $(BUILD)/desktop_test_loom_forks \
                        $(BUILD)/desktop_test_regsynth
# ...and the REPLAY half, which needs only Unicorn + Capstone (no assembler), so
# it runs on strictly more hosts than the fork tests do.
DESKTOP_REPLAY_TESTS := $(BUILD)/desktop_test_ptslice_run

# The PT-replay slice, actually replayed (08-observer-views.md T8). A FOURTH
# object of ptslice.cpp, compiled with the producer defines — the app tree's
# object carries the same flags but lives in a binary with a main() of its own.
#
# Worth stating why this is not a hardware-gated lane: CAPTURING a PT path needs
# PT silicon, REPLAYING one does not. The path is decoded at capture time and
# recorded (`stitch`), the bytes with it (`codeimage`), so everything after the
# capture runs anywhere the full app builds — and the honest half of "a live
# slice with zero single-steps" gets tested on hosts with no Intel PT at all.
# The object sits one level deeper than its tree name suggests (testpt/lv/)
# because the -MMD dependency include at the foot of this file globs
# $(BUILD)/desktop/*/*/*.d — an object outside that shape would silently stop
# tracking its headers, which is exactly the kind of staleness a hand-written
# rule invites.
$(BUILD)/desktop/testpt/lv/ptslice.o: desktop/src/live/ptslice.cpp \
    | $(IMGUI_HOME)/imgui.cpp $(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $(@D)
	$(CXX) $(DESKTOP_CXXFLAGS) -DASMTEST_DESKTOP_HAVE_PT_REPLAY=1 \
	  -DASMTEST_DESKTOP_CAN_PROBE=1 -c $< -o $@

$(BUILD)/desktop_test_ptslice_run: $(BUILD)/desktop/test/t/test_ptslice_run.o \
    $(BUILD)/desktop/testpt/lv/ptslice.o $(BUILD)/desktop/test/vw/disasm.o \
    $(BUILD)/desktop/test/vw/observer.o $(BUILD)/desktop/test/an/slice.o \
    $(DESKTOP_TEST_DOC) \
    $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
    $(BUILD)/dataflow_blockstep.o $(BUILD)/dataflow_pt.o $(DESKTOP_CAP_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) \
	  $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(LINK_LIBBPF) -ldl -lpthread -o $@

$(BUILD)/desktop_test_loom_forks: $(BUILD)/desktop/test/t/test_loom_forks.o \
    $(BUILD)/desktop/test/lo/fabric.o $(BUILD)/desktop/test/lo/fabric_plan.o \
    $(BUILD)/desktop/test/lo/take_view.o $(BUILD)/desktop/test/lo/forks.o \
    $(BUILD)/desktop/test/an/diff.o $(BUILD)/desktop/test/an/slice.o \
    $(DESKTOP_TEST_DOC) \
    $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o $(BUILD)/assemble.o \
    $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o \
    $(BUILD)/dataflow_resume.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) \
	  $(CAPSTONE_LIBS) -o $@

# The Scrubber's register-history synthesiser (30 R3 T4). Like the fork test it
# links the emulator (emu.o -> Unicorn) — but NOT the assembler (no Keystone): it
# re-runs recorded bytes under the per-step ring, it assembles nothing. Same
# host-capability gate as the fork test (docker-desktop runs it every time).
$(BUILD)/desktop_test_regsynth: $(BUILD)/desktop/test/t/test_regsynth.o \
    $(BUILD)/desktop/test/vw/regsynth.o \
    $(BUILD)/desktop/test/vw/scrubber.o $(BUILD)/desktop/test/an/stepindex.o \
    $(DESKTOP_TEST_DOC) \
    $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(CAPSTONE_LIBS) -o $@

DESKTOP_ALL_TESTS  = $(DESKTOP_TESTS)
DESKTOP_ENGINE_SAY = :
DESKTOP_REPLAY_SAY = :
DESKTOP_GL_SAY     = :
# The 3D-scene FBO smoke rides desktop-test where the GL/EGL headers exist (the
# docker-desktop lane installs them). This is NOT a self-skip-only lane: the smoke
# renders and reads back real pixels under software Mesa there (CLAUDE.md — a test
# that can only ever self-skip is not a test). On a bare host without the headers
# it is not built and the reason is printed; the pure camera test still runs.
DESKTOP_GL_TESTS := $(BUILD)/desktop_test_scene_fbo
ifeq ($(strip $(DESKTOP_GL_MISSING)),)
desktop-test: $(DESKTOP_GL_TESTS)
DESKTOP_ALL_TESTS += $(DESKTOP_GL_TESTS)
else
DESKTOP_GL_SAY = echo "desktop-test: the 3D-scene FBO smoke needs:$(DESKTOP_GL_MISSING) — not built here; 'make docker-desktop' installs software Mesa + EGL and runs it"
endif

# The two gates are separate because their dependencies are (see
# DESKTOP_REPLAY_MISSING above): a host with Unicorn + Capstone runs the PT
# replay even where the assembler is absent.
ifeq ($(strip $(DESKTOP_REPLAY_MISSING)$(DESKTOP_REPLAY_HOST_MISSING)),)
desktop-test: $(DESKTOP_REPLAY_TESTS)
DESKTOP_ALL_TESTS += $(DESKTOP_REPLAY_TESTS)
else
# The docker lane only satisfies the arch half if the image is x86-64, and on an
# arm64 host it defaults to the host's own architecture — so name the platform
# override there rather than send the reader to a lane that fails the same way.
DESKTOP_REPLAY_DOCKER = $(if $(filter x86_64,$(DESKTOP_REPLAY_ARCH)),make docker-desktop,DOCKER_PLATFORM=linux/amd64 make docker-desktop)
DESKTOP_REPLAY_SAY = echo "desktop-test: the PT-replay slice test needs:$(DESKTOP_REPLAY_MISSING)$(DESKTOP_REPLAY_HOST_MISSING) — not built here; '$(DESKTOP_REPLAY_DOCKER)' provides them and runs it"
endif

ifeq ($(strip $(DESKTOP_ENGINE_MISSING)),)
desktop-test: $(DESKTOP_ENGINE_TESTS)
DESKTOP_ALL_TESTS += $(DESKTOP_ENGINE_TESTS)
else
DESKTOP_ENGINE_SAY = echo "desktop-test: the full-build fork tests need:$(DESKTOP_ENGINE_MISSING) — not built here; 'make docker-desktop' installs them and runs them"
endif

desktop-test: $(DESKTOP_TESTS)
	@$(DESKTOP_ENGINE_SAY)
	@$(DESKTOP_REPLAY_SAY)
	@$(DESKTOP_GL_SAY)
	@$(DESKTOP_UITEST_SAY)
	@for t in $(DESKTOP_ALL_TESTS); do echo "== $$t =="; $$t || exit 1; done

# desktop-ui-test (17-T1): the imgui_test_engine interaction lane. Kept SEPARATE
# from `desktop-test` on purpose — it fetches the one non-MIT dependency at build
# (Test Engine License v1.04, test-lane-only, never bundled), so a plain
# `make desktop-test` stays 100% MIT and network-free. It runs headless (null
# backend) and writes JUnit XML for CI. docker-desktop runs it (Dockerfile.desktop).
# `desktop-test` only NOTES it, so the interaction coverage is never silently absent.
desktop-ui-test: $(BUILD)/desktop_ui_test
	@echo "== $(BUILD)/desktop_ui_test (imgui_test_engine, headless) =="
	$(BUILD)/desktop_ui_test
	@echo "desktop-ui-test: JUnit XML -> $(BUILD)/desktop-ui-test-results.xml"
DESKTOP_UITEST_SAY = echo "desktop-test: the imgui_test_engine interaction lane is separate (fetches the one non-MIT, test-lane-only dep) — run 'make desktop-ui-test' or 'make docker-desktop'"

# ---------------------------------------------------------------------------
# gen_walkthroughs — the Learn door's bundled walkthroughs, as recordings
# (docs/internal/gui/06-doors-and-learning.md T2/T3).
#
# A C tool (not C++): it drives the assembler + emulator + the shared .asmtrace
# writer, exactly like tools/asmtrace_record.c does for the golden corpus, and
# for the same reason — one writer TU owns field order for the whole tree.
$(BUILD)/gen_walkthroughs.o: desktop/test/gen_walkthroughs.c                              cli/asmtrace_ndjson.h include/asmtest_assemble.h                              include/asmtest_emu.h | $(BUILD)
	$(CC) $(CFLAGS) -Icli -c $< -o $@

$(BUILD)/gen_walkthroughs: $(BUILD)/gen_walkthroughs.o                            $(BUILD)/asmtrace_ndjson.o $(BUILD)/assemble.o                            $(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/disasm.o
	$(CC) $(CFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) $(CAPSTONE_LIBS) -o $@

.PHONY: asmtrace-walkthroughs
ifeq ($(strip $(DESKTOP_ENGINE_MISSING)),)
asmtrace-walkthroughs: $(BUILD)/gen_walkthroughs
	@mkdir -p $(WALKTHROUGH_DIR)
	$(BUILD)/gen_walkthroughs $(WALKTHROUGH_DIR)
	@# Byte-stability gate (D6): write a SECOND copy to a temp dir and compare.
	@# A generator that is not deterministic makes every future diff of these
	@# files unreadable, so the check runs on every regeneration, not in CI only.
	@tmp=$$(mktemp -d) && $(BUILD)/gen_walkthroughs $$tmp >/dev/null && 	  for f in $(WALKTHROUGH_DIR)/*.asmtrace; do 	    b=$$(basename $$f); 	    cmp -s $$f $$tmp/$$b || { 	      echo "asmtrace-walkthroughs: $$b is NOT byte-stable across two runs"; 	      diff $$f $$tmp/$$b | head -10; rm -rf $$tmp; exit 1; }; 	  done; rm -rf $$tmp; 	  echo "asmtrace-walkthroughs: $$(ls $(WALKTHROUGH_DIR)/*.asmtrace | wc -l) recording(s), byte-stable"
else
asmtrace-walkthroughs:
	@echo "# SKIP $@: the walkthrough generator needs$(DESKTOP_ENGINE_MISSING)"
	@echo "#   (it assembles its own sources and runs them). The committed"
	@echo "#   recordings are authoritative; regenerate with make docker-desktop."
endif

# D8: desktop/ C++ uses the repo .clang-format (Language: Cpp). The check is
# `-`-prefixed (informational, never gates); desktop/ stays out of FMT_SOURCES.
desktop-fmt:
	$(CLANG_FORMAT) -i $$(find desktop -name '*.cpp' -o -name '*.h')
desktop-fmt-check:
	-$(CLANG_FORMAT) --dry-run -Werror $$(find desktop -name '*.cpp' -o -name '*.h')

# Build the desktop image (bindings base + libglfw3-dev + libgl1-mesa-dev) and
# run its CMD (make desktop desktop-render desktop-test). Mirrors docker-cli.
docker-desktop: docker-bindings-base
	$(DOCKER) build $(_docker_plat) -f Dockerfile.desktop \
	  --build-arg BASE_IMAGE=$(DOCKER_BINDINGS_BASE) -t asmtest-desktop .
	$(DOCKER) run --rm $(_docker_plat) asmtest-desktop

-include $(wildcard $(BUILD)/desktop/*/*/*.d)

# ---------------------------------------------------------------------------
# asmtrace_export — .asmtrace -> speedscope / Perfetto / lcov / DOT
# (docs/internal/gui/02-exporters-and-readers.md T1-T4).
#
# One TU, libc only: no engine objects, no Capstone, no JSON library, so it
# builds and runs on every lane and wherever a recording landed. Its suite needs
# only cc + python3, so it rides desktop-test rather than gating a lane.
$(BUILD)/asmtrace_export: tools/asmtrace_export.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

.PHONY: asmtrace-export asmtrace-export-test
asmtrace-export: $(BUILD)/asmtrace_export

asmtrace-export-test: $(BUILD)/asmtrace_export
	BUILD=$(BUILD) sh scripts/test-asmtrace-export.sh

# The exporter suite rides desktop-test (D3: one lane runs the whole GUI Phase-1
# tail); it needs neither GLFW nor the engines, exactly like the null-backend
# tests above.
desktop-test: asmtrace-export-test

# Fixture/golden roots reach the new tests the same way as 03's (compile
# defines, so no argv wiring and identical behaviour host + docker).
$(BUILD)/desktop/test/t/test_canvas.o \
$(BUILD)/desktop/test/t/test_timeline.o \
$(BUILD)/desktop/test/t/test_scrubber.o \
$(BUILD)/desktop/test/t/test_scrubber_draw.o \
$(BUILD)/desktop/test/t/test_abixray.o \
$(BUILD)/desktop/test/t/test_abixray_draw.o \
$(BUILD)/desktop/test/t/test_slice_view.o \
$(BUILD)/desktop/test/t/test_diff.o \
$(BUILD)/desktop/test/t/test_selection.o \
$(BUILD)/desktop/test/t/test_find.o \
$(BUILD)/desktop/test/t/test_diff_view.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"' \
                         -DASMTEST_EXPECTED_DIR='"desktop/test/expected"'
$(BUILD)/desktop/test/t/test_data_readers.o \
$(BUILD)/desktop/test/t/test_completeness_view.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"' \
                         -DASMTEST_GOLDEN_DIR='"desktop/test/golden"' \
                         -DASMTEST_REPO_ROOT='"."'

# The Observer views read hand-written LIVE-capture fixtures (syscall payloads,
# an arm64 watchpoint refusal, an IBS survey, a JIT code image): none of those
# can come from the deterministic golden corpus, because none of them can be
# produced by an emulator on a machine that has no such hardware.
$(BUILD)/desktop/test/t/test_obs_syscalls.o \
$(BUILD)/desktop/test/t/test_obs_watch.o \
$(BUILD)/desktop/test/t/test_obs_topo.o \
$(BUILD)/desktop/test/t/test_obs_hotedges.o \
$(BUILD)/desktop/test/t/test_obs_tree.o \
$(BUILD)/desktop/test/t/test_obs_region.o \
$(BUILD)/desktop/test/t/test_obs_disasm.o \
$(BUILD)/desktop/test/t/test_obs_ptslice.o \
$(BUILD)/desktop/test/t/test_obs_draw.o \
$(BUILD)/desktop/test/t/test_ptslice_run.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"' \
                         -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"' \
                         -DASMTEST_EXPECTED_DIR='"desktop/test/expected"'
