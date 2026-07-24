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

IMGUI_VERSION ?= 1.91.9
IMGUI_HOME    ?= $(BUILD)/imgui/imgui-$(IMGUI_VERSION)
JSON_VERSION  ?= 3.11.3
JSON_HOME     ?= $(BUILD)/nlohmann-json/$(JSON_VERSION)

# -MMD -MP: per-object header deps so an incremental build stays correct across
# the many desktop/ and imgui headers. Recursive (=), never := (CXX is set by
# mk/bindings.mk, which loads after this file).
DESKTOP_CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -g -MMD -MP \
  -Icli -Iinclude -Idesktop/src -I$(IMGUI_HOME) -I$(IMGUI_HOME)/backends \
  -I$(JSON_HOME)

# --- pinned, digest-verified third-party sources (D2) ------------------------
# One fetch-imgui.sh run extracts ALL of imgui's TUs at once, so they are a
# GROUPED target (&:, make 4.3+): make then knows a clean tree (build/ is
# dockerignored, so `make docker-desktop` starts with none of them) can produce
# every imgui source by running the single fetch, not just imgui.cpp.
IMGUI_SRCS := $(IMGUI_HOME)/imgui.cpp $(IMGUI_HOME)/imgui_draw.cpp \
  $(IMGUI_HOME)/imgui_tables.cpp $(IMGUI_HOME)/imgui_widgets.cpp \
  $(IMGUI_HOME)/backends/imgui_impl_glfw.cpp \
  $(IMGUI_HOME)/backends/imgui_impl_opengl3.cpp
$(IMGUI_SRCS) &: scripts/fetch-imgui.sh scripts/third-party-digests.txt
	sh scripts/fetch-imgui.sh >/dev/null
$(JSON_HOME)/nlohmann/json.hpp: scripts/fetch-json.sh scripts/third-party-digests.txt
	sh scripts/fetch-json.sh >/dev/null

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
$$(BUILD)/desktop/$(1)/src/%.o: desktop/src/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/doc/%.o: desktop/src/doc/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) -c $$< -o $$@
$$(BUILD)/desktop/$(1)/ui/%.o:  desktop/src/ui/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
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
$$(BUILD)/desktop/$(1)/t/%.o:   desktop/test/%.cpp | $$(IMGUI_HOME)/imgui.cpp $$(JSON_HOME)/nlohmann/json.hpp
	@mkdir -p $$(@D)
	$$(CXX) $$(DESKTOP_CXXFLAGS) $(2) $$(DESKTOP_TEST_EXTRA) -c $$< -o $$@
endef
$(eval $(call desktop_rules,app,))
$(eval $(call desktop_rules,render,-DASMTEST_DESKTOP_RENDER_ONLY=1))
$(eval $(call desktop_rules,test,))

# The test fixtures + golden corpus reach their tests through compile defines, so
# the tests need no argv wiring (and run identically host + docker).
$(BUILD)/desktop/test/t/test_recording.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_shell.o:     DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_golden.o:    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'
$(BUILD)/desktop/test/t/test_live_session.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop/test/t/test_inspect.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
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
DESKTOP_OBS_PURE := observer syscalls watch topo hotedges tree region disasm
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
  $(BUILD)/desktop/$(1)/da/features_data.o \
  $(BUILD)/desktop/$(1)/da/perf_history.o \
  $(DESKTOP_VIEW_PURE:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_OBS_PURE:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_OBS_DRAW:%=$(BUILD)/desktop/$(1)/vw/%.o) \
  $(DESKTOP_LOOM_PURE:%=$(BUILD)/desktop/$(1)/lo/%.o) \
  $(DESKTOP_LOOM_DRAW:%=$(BUILD)/desktop/$(1)/lo/%.o) \
  $(BUILD)/desktop/$(1)/src/walkthrough.o $(BUILD)/desktop/$(1)/src/capview.o \
  $(BUILD)/desktop/$(1)/src/author_vm.o \
  $(BUILD)/desktop/$(1)/ui/author_door.o \
  $(BUILD)/desktop/$(1)/ui/shell.o $(BUILD)/desktop/$(1)/ui/learn_door.o \
  $(BUILD)/desktop/$(1)/ui/capability_panel.o \
  $(BUILD)/desktop/$(1)/ui/inspect_door.o \
  $(DESKTOP_LIVE:%=$(BUILD)/desktop/$(1)/lv/%.o)
DESKTOP_APP_OBJ    := $(call desktop_app_objs,app) \
                      $(DESKTOP_LOOM_APP:%=$(BUILD)/desktop/app/lo/%.o)
DESKTOP_RENDER_OBJ := $(call desktop_app_objs,render)
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
DESKTOP_ENGINE_OBJ := $(BUILD)/emu.o $(BUILD)/trace.o \
                      $(BUILD)/disasm.o $(BUILD)/assemble.o \
                      $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o \
                      $(BUILD)/dataflow_emu.o $(BUILD)/dataflow_blockstep.o \
                      $(BUILD)/dataflow_pt.o

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

# --- missing-dependency probes (mirror mk/cli.mk:32-38) -----------------------
# The render-only viewer + the full app need GLFW/GL; only the full app needs the
# engines. desktop-test needs neither. Absence -> friendly guidance, never a raw
# compiler error (a bare host builds+runs desktop-test regardless).
DESKTOP_MISSING :=
ifneq ($(shell pkg-config --exists glfw3 2>/dev/null && echo ok),ok)
DESKTOP_MISSING += libglfw3-dev
endif
GLFW_LIBS ?= $(shell pkg-config --libs glfw3 2>/dev/null || echo -lglfw)
GL_LIBS   ?= -lGL

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
# Keystone IS probed after all — by header presence, since its kit ships no
# reliable pkg-config. The Loom's fork engine (05-loom-day-one.md T5) is the
# first desktop TU to call asmtest_assemble, and a host with unicorn+capstone
# but no keystone (a `make deps` that ran without --asm) would otherwise reach a
# raw linker error instead of the guidance recipe below.
ifeq ($(shell ls /usr/include/keystone/keystone.h /usr/local/include/keystone/keystone.h 2>/dev/null | head -1),)
DESKTOP_ENGINE_MISSING += libkeystone-dev
endif

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

.PHONY: desktop desktop-render desktop-test desktop-fmt desktop-fmt-check \
        docker-desktop desktop-setup desktop-setup-render

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

$(BUILD)/asmtest-desktop: $(DESKTOP_APP_OBJ) $(DESKTOP_ENGINE_OBJ) \
                          $(DESKTOP_CAP_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) \
	  $(CAPSTONE_LIBS) $(LIBIPT_LIBS) $(OPENCSD_LIBS) $(LINK_LIBBPF) \
	  $(GLFW_LIBS) $(GL_LIBS) -ldl -lpthread -o $@
	@echo "built $@ — the full app (GPL-2.0 as a whole; links the engines)"

$(BUILD)/asmtest-viewer: $(DESKTOP_RENDER_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(GLFW_LIBS) $(GL_LIBS) -o $@
	@echo "built $@ — the render-only viewer (engine-free; permissively distributable)"

ifeq ($(strip $(DESKTOP_MISSING)$(DESKTOP_ENGINE_MISSING)),)
desktop: $(BUILD)/asmtest-desktop
else
desktop:
	$(call DESKTOP_GUIDE,asmtest-desktop (full app),$(DESKTOP_MISSING) $(DESKTOP_ENGINE_MISSING))
endif

ifeq ($(strip $(DESKTOP_MISSING)),)
desktop-render: $(BUILD)/asmtest-viewer
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
	@echo "Both open a window, so they need a display (DISPLAY / WAYLAND_DISPLAY)."
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
                 $(BUILD)/desktop_test_slice $(BUILD)/desktop_test_nav \
                 $(BUILD)/desktop_test_diff $(BUILD)/desktop_test_canvas \
                 $(BUILD)/desktop_test_timeline \
                 $(BUILD)/desktop_test_slice_view \
                 $(BUILD)/desktop_test_diff_view \
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

# capview.o links NOTHING: it is a pure function of the status data a caller
# already probed, which is what makes the panel's two UI laws assertable on a
# container with no PT, no LBR and no AMD silicon.
$(BUILD)/desktop_test_capview: $(BUILD)/desktop/test/t/test_capview.o \
    $(BUILD)/desktop/test/src/capview.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# author_vm.o links NOTHING either: it maps engine STRUCTS, never calls engine
# functions, so the Author door's three rules are pinned without Keystone or
# Unicorn on the host.
$(BUILD)/desktop_test_author_vm: $(BUILD)/desktop/test/t/test_author_vm.o \
    $(BUILD)/desktop/test/src/author_vm.o
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
$(BUILD)/desktop_test_obs_topo: $(BUILD)/desktop/test/t/test_obs_topo.o \
    $(BUILD)/desktop/test/vw/topo.o $(BUILD)/desktop/test/vw/observer.o \
    $(BUILD)/desktop/test/src/nav.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_hotedges: \
    $(BUILD)/desktop/test/t/test_obs_hotedges.o \
    $(BUILD)/desktop/test/vw/hotedges.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_obs_tree: $(BUILD)/desktop/test/t/test_obs_tree.o \
    $(BUILD)/desktop/test/vw/tree.o $(BUILD)/desktop/test/vw/observer.o \
    $(DESKTOP_TEST_DOC)
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
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) \
    $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
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

$(BUILD)/desktop_test_nav: $(BUILD)/desktop/test/t/test_nav.o \
    $(BUILD)/desktop/test/src/nav.o $(DESKTOP_TEST_DOC)
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

$(BUILD)/desktop_test_diff_view: $(BUILD)/desktop/test/t/test_diff_view.o \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_data_readers: \
    $(BUILD)/desktop/test/t/test_data_readers.o $(DESKTOP_TEST_DA)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_completeness_view: \
    $(BUILD)/desktop/test/t/test_completeness_view.o $(DESKTOP_TEST_DA)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_null: $(BUILD)/desktop/test/t/test_null_render.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_recording: $(BUILD)/desktop/test/t/test_recording.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# shell.o now draws the replay views, so the shell test links the pure builders,
# their draw halves and the data readers. It still needs no GL and no engines:
# ImGui's null backend renders every one of those paths.
DESKTOP_TEST_SHELL_OBJ := $(BUILD)/desktop/test/ui/shell.o \
    $(BUILD)/desktop/test/ui/learn_door.o \
    $(BUILD)/desktop/test/ui/capability_panel.o \
    $(BUILD)/desktop/test/ui/inspect_door.o \
    $(DESKTOP_TEST_LIVE) \
    $(BUILD)/desktop/test/src/walkthrough.o \
    $(BUILD)/desktop/test/src/capview.o \
    $(BUILD)/desktop/test/src/author_vm.o \
    $(BUILD)/desktop/test/ui/author_door.o $(DESKTOP_TEST_DOC) \
    $(DESKTOP_TEST_VW) $(DESKTOP_TEST_AN) $(DESKTOP_TEST_DA) \
    $(DESKTOP_VIEW_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(DESKTOP_TEST_OBS) \
    $(DESKTOP_OBS_DRAW:%=$(BUILD)/desktop/test/vw/%.o) \
    $(DESKTOP_TEST_LOOM) \
    $(DESKTOP_LOOM_DRAW:%=$(BUILD)/desktop/test/lo/%.o) $(DESKTOP_TEST_IG)

$(BUILD)/desktop_test_shell: $(BUILD)/desktop/test/t/test_shell.o \
    $(DESKTOP_TEST_SHELL_OBJ) $(BUILD)/desktop/test/src/vm_compat.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_golden: $(BUILD)/desktop/test/t/test_golden.o \
    $(DESKTOP_TEST_SHELL_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

# The FULL-BUILD half of desktop-test: the Loom's fork engine (05 T5/T7) is the
# one desktop TU that links the engines, so its test needs unicorn + keystone.
# This is a host-capability gate, NOT a self-skip lane: `make docker-desktop`
# installs all three and runs it every time (CLAUDE.md — a test that can only
# ever self-skip is not a test), and the gate below prints why on a bare host.
DESKTOP_ENGINE_TESTS := $(BUILD)/desktop_test_loom_forks
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
    $(BUILD)/dataflow.o $(BUILD)/dataflow_operands.o $(BUILD)/dataflow_emu.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) \
	  $(CAPSTONE_LIBS) -o $@

DESKTOP_ALL_TESTS  = $(DESKTOP_TESTS)
DESKTOP_ENGINE_SAY = :
DESKTOP_REPLAY_SAY = :

# The two gates are separate because their dependencies are (see
# DESKTOP_REPLAY_MISSING above): a host with Unicorn + Capstone runs the PT
# replay even where the assembler is absent.
ifeq ($(strip $(DESKTOP_REPLAY_MISSING)),)
desktop-test: $(DESKTOP_REPLAY_TESTS)
DESKTOP_ALL_TESTS += $(DESKTOP_REPLAY_TESTS)
else
DESKTOP_REPLAY_SAY = echo "desktop-test: the PT-replay slice test needs:$(DESKTOP_REPLAY_MISSING) — not built here; 'make docker-desktop' installs them and runs it"
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
	@for t in $(DESKTOP_ALL_TESTS); do echo "== $$t =="; $$t || exit 1; done

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
$(BUILD)/desktop/test/t/test_slice_view.o \
$(BUILD)/desktop/test/t/test_diff.o \
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
