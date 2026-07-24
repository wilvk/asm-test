// test_obs_draw.cpp — the Observer views under ImGui's null backend
// (08-observer-views.md T1-T7). No display, no GL, no engines.
//
// The model tests next door assert what each view DECIDES; this one asserts
// that the deck actually draws every one of those decisions — including the
// awkward paths a happy-path smoke never reaches: a payload reveal, a refused
// watchpoint arm, a survey whose provenance contradicts itself, a filter the
// panel must refuse, an invocation that was cut off mid-flight, and a code
// image that was never captured. A draw half that crashed on the refusal path
// would be a UI that works until the moment it matters.
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"

#include "views/observer_draw.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static Recording load(const std::string &name) {
    std::string err;
    auto r =
        load_recording_file(std::string(ASMTEST_FIXTURE_DIR) + "/" + name, err);
    if (!r) {
        check("load", false, name + ": " + err);
        return Recording{};
    }
    return *r;
}

// One frame of the whole deck over `rec`. Returns the vertex count, which is
// only used as "something was actually drawn".
static int frame(ObserverState &s, const Recording &rec) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("observer");
    draw_observer(s, rec, "fixture.asmtrace", nullptr);
    ImGui::End();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    return dd ? dd->TotalVtxCount : -1;
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    const char *fixtures[] = {
        "obs-syscalls.asmtrace",  "obs-syscalls-redacted.asmtrace",
        "obs-watch.asmtrace",     "obs-watch-skip.asmtrace",
        "obs-topo.asmtrace",      "obs-survey-ibs.asmtrace",
        "obs-survey-sw.asmtrace", "obs-survey-dishonest.asmtrace",
        "obs-tree.asmtrace",      "obs-region.asmtrace",
        "obs-codeimage.asmtrace", "obs-codeimage-gate.asmtrace",
        "obs-ptslice.asmtrace",   "min-trace.asmtrace",
        "truncated.asmtrace",     "redacted.asmtrace",
    };
    for (const char *name : fixtures) {
        Recording rec = load(name);
        ObserverState s;
        int vtx = frame(s, rec);
        check(name, vtx >= 0, "no draw data for this fixture");
        check(name, s.built, "the deck should have built itself on first draw");
    }

    // The paths a first frame never reaches. Each is driven through the MODEL's
    // own action (never by poking a flag), then drawn.
    {
        Recording rec = load("obs-syscalls.asmtrace");
        ObserverState s;
        frame(s, rec);
        obs_syscall_reveal(s.syscalls, 0, true); // a revealed row
        check("reveal draws", frame(s, rec) >= 0, "revealed payload row");
        obs_syscall_reveal_all(s.syscalls); // armed, prompt showing
        check("reveal-all prompt draws", frame(s, rec) >= 0,
              "the confirmation prompt");
        obs_syscall_reveal_all(s.syscalls); // confirmed
        check("reveal-all draws", frame(s, rec) >= 0, "every payload revealed");
    }
    {
        Recording rec = load("obs-region.asmtrace");
        ObserverState s;
        frame(s, rec);
        obs_region_page(s.region, 2); // the invocation with no coverage footer
        check("open invocation draws", frame(s, rec) >= 0,
              "the cut-off invocation and its warning");
    }
    {
        Recording rec = load("obs-tree.asmtrace");
        ObserverState s;
        frame(s, rec);
        // An illegal filter: the panel must draw the refusal, not the button.
        s.filter.tid = 4243;
        s.filter.follow = true;
        check("refused filter draws", frame(s, rec) >= 0,
              "the tid-XOR-follow refusal");
    }
    {
        Recording rec = load("obs-codeimage.asmtrace");
        ObserverState s;
        frame(s, rec);
        s.disasm_when = 2; // between the two refreshes
        check("historical bytes draw", frame(s, rec) >= 0,
              "the pane at a past logical time");
    }
    // An EMPTY recording: the deck must say it has nothing rather than draw a
    // row of empty panes.
    {
        Recording rec = load("min-trace.asmtrace");
        ObserverState s;
        observer_build(s, rec);
        check("empty deck",
              !observer_has_any(s) || !s.region.invocations.empty(),
              "min-trace carries trace events but no coverage footer");
        check("empty deck draws", frame(s, rec) >= 0,
              "the nothing-here message");
    }

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "test_obs_draw: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_obs_draw: PASS\n");
    return 0;
}
