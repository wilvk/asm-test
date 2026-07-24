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

# --- object lists ------------------------------------------------------------
# asmtest-desktop / asmtest-viewer: imgui core + glfw/opengl3 backends + src/
# (vm_compat + main) + doc/ + ui/. The app additionally links the Author-tier
# engine objects and their libs -> GPL-2.0 as a whole (D4); the viewer links NONE
# of them and stays permissive.
desktop_app_objs = \
  $(addprefix $(BUILD)/desktop/$(1)/ig/,$(addsuffix .o,$(DESKTOP_IMGUI_CORE))) \
  $(addprefix $(BUILD)/desktop/$(1)/igb/,$(addsuffix .o,$(DESKTOP_IMGUI_BACK))) \
  $(BUILD)/desktop/$(1)/src/vm_compat.o $(BUILD)/desktop/$(1)/src/main.o \
  $(BUILD)/desktop/$(1)/doc/recording.o $(BUILD)/desktop/$(1)/doc/workspace.o \
  $(BUILD)/desktop/$(1)/ui/shell.o
DESKTOP_APP_OBJ    := $(call desktop_app_objs,app)
DESKTOP_RENDER_OBJ := $(call desktop_app_objs,render)
# The Author-tier engine objects (emu/assemble link unicorn/keystone, disasm
# links capstone) — they carry the GPL engine linkage that makes the app GPL-2.0
# as a whole (D4) and are self-contained (dataflow.o is the pure L0/L1/L2 sink,
# as mk/cli.mk's test_view links it alone). NOTE: $(FRAMEWORK_OBJS) is
# deliberately NOT here — asmtest.o is the test-runner and defines its own main()
# (src/asmtest.c), which would collide with the desktop's main.cpp; the desktop
# is not a test binary and needs no runner.
DESKTOP_ENGINE_OBJ := $(BUILD)/emu.o $(BUILD)/trace.o \
                      $(BUILD)/disasm.o $(BUILD)/assemble.o $(BUILD)/dataflow.o

# test-tree shared objects (no backends, no main, no engines).
DESKTOP_TEST_IG  := $(addprefix $(BUILD)/desktop/test/ig/,$(addsuffix .o,$(DESKTOP_IMGUI_CORE)))
DESKTOP_TEST_DOC := $(BUILD)/desktop/test/doc/recording.o $(BUILD)/desktop/test/doc/workspace.o

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
DESKTOP_ENGINE_MISSING :=
ifneq ($(shell pkg-config --exists unicorn 2>/dev/null && echo ok),ok)
DESKTOP_ENGINE_MISSING += libunicorn-dev
endif
ifneq ($(shell pkg-config --exists capstone 2>/dev/null && echo ok),ok)
DESKTOP_ENGINE_MISSING += libcapstone-dev
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

.PHONY: desktop desktop-render desktop-test desktop-fmt desktop-fmt-check docker-desktop

$(BUILD)/asmtest-desktop: $(DESKTOP_APP_OBJ) $(DESKTOP_ENGINE_OBJ)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(UNICORN_LIBS) $(KEYSTONE_LIBS) \
	  $(CAPSTONE_LIBS) $(GLFW_LIBS) $(GL_LIBS) -o $@
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

# desktop-test: the null-backend headless tests. No GLFW, no GL, no engines — the
# gate above never applies here (a test that could only self-skip is not a test,
# CLAUDE.md). vm_compat.o compiling in the test tree IS the regression test that
# keeps the reused asmspy headers C++-clean (03-desktop-shell.md T5).
DESKTOP_TESTS := $(BUILD)/desktop_test_null $(BUILD)/desktop_test_recording \
                 $(BUILD)/desktop_test_shell $(BUILD)/desktop_test_golden

$(BUILD)/desktop_test_null: $(BUILD)/desktop/test/t/test_null_render.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_recording: $(BUILD)/desktop/test/t/test_recording.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_shell: $(BUILD)/desktop/test/t/test_shell.o \
    $(BUILD)/desktop/test/ui/shell.o $(DESKTOP_TEST_DOC) \
    $(BUILD)/desktop/test/src/vm_compat.o $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

$(BUILD)/desktop_test_golden: $(BUILD)/desktop/test/t/test_golden.o \
    $(BUILD)/desktop/test/ui/shell.o $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@

desktop-test: $(DESKTOP_TESTS)
	@for t in $(DESKTOP_TESTS); do echo "== $$t =="; $$t || exit 1; done

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
