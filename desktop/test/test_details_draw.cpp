// test_details_draw.cpp — the Process details pane renders in all of its real
// states without a live process: nothing selected, a full snapshot, a
// budget-truncated snapshot, and a refusal. Drives ImGui's null backend.
//
// This plan's recurring failure mode (seven tasks running) is a first-draft
// test that passes against deliberately broken code — "it did not crash" is
// not evidence a refusal, a truncation flag or an absence-vs-zero distinction
// actually reached the screen. So every check here reads ImGui's own log of
// what it rendered (ImGui::LogToClipboard walks the widget tree emitting its
// literal text; the null backend's clipboard is a plain in-process buffer —
// see imgui.cpp's non-Win32/non-macOS Platform_*ClipboardTextFn_DefaultImpl),
// and asserts on that RENDERED TEXT, never merely on absence of a crash. Every
// check below was verified BY MUTATION: a one-line change to details_pane.cpp
// (banner removed, have==false printing "0%", a mode's `why` dropped, a
// `*_truncated` flag not surfaced, the selected_pid<=0 guard deleted) was
// rebuilt and confirmed to flip this test to FAIL before being reverted.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#include "imgui.h"

#include "doc/recording.h"
#include "live/procinfo.h"
#include "ui/doors.h"

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
// A substring-presence assertion reads better inline than a bool + a
// hand-written why every time; `hay` is always the full captured render.
static void want(const char *what, const std::string &hay, const char *needle) {
    check(what, hay.find(needle) != std::string::npos,
          std::string("expected to find '") + needle + "' in:\n" + hay);
}
static void avoid(const char *what, const std::string &hay, const char *needle) {
    check(what, hay.find(needle) == std::string::npos,
          std::string("did NOT expect to find '") + needle + "' in:\n" + hay);
}

static ProcInfo load_fixture(const char *name) {
    std::ifstream f(std::string(ASMTEST_FIXTURE_DIR) + "/" + name);
    std::string err;
    std::optional<Recording> r = load_recording(f, err);
    if (!r)
        return ProcInfo{};
    return procinfo_parse(*r);
}

// Pins the runner's want_pid to `pid` and a settled last_ok_at, so
// draw_details_pane's OWN procinfo_tick call (its first line — the brief's
// verbatim body) treats this as an ALREADY-SETTLED selection and does not
// reset `shown` back to blank before the render below reads it. This is
// exactly the state a real session is in from frame 2 of a settled read
// onward: procinfo_tick only resets `shown` when selected_pid CHANGES.
static void settle(InspectState &s, long pid, const ProcInfo &snapshot,
                   const ProcRates &rates = ProcRates{}) {
    s.selected_pid = pid;
    s.details.want_pid = pid;
    s.details.want_since = -1; // not "just changed" -> tick will not respawn
    s.details.shown = snapshot;
    s.details.rates = rates;
    s.details.status = "attach-free (no ptrace)";
    s.details.last_ok_at = 1000.0;
    s.details.last_tick_s = 1000.0;
}

// Render draw_details_pane(s) once into a fresh frame and return every string
// ImGui actually rendered. Force the four collapsed-by-default sections
// (Identity / Code surface / Containment / Children) open first, via the
// PUBLIC ImGuiStorage API keyed the same way CollapsingHeader keys its own
// persisted open/closed bool — no imgui_internal.h, no test-only hook in
// production code.
static std::string render(InspectState &s, ImGuiIO &io) {
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("details-under-test");
    for (const char *label :
         {"Identity", "Code surface", "Containment", "Children"})
        ImGui::GetStateStorage()->SetInt(ImGui::GetID(label), 1);
    ImGui::LogToClipboard();
    draw_details_pane(s);
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    if (ImGui::GetDrawData() == nullptr) {
        std::fprintf(stderr, "test_details_draw: FAIL: null draw data\n");
        failures++;
    }
    const char *clip = ImGui::GetClipboardText();
    return clip ? clip : "";
}

// A hand-built snapshot exercising what NO checked-in fixture combines: a
// budget-truncated gather (every *_truncated flag AND budget_exceeded set),
// one thread of each `in`-column preference (syscall name, wchan-only,
// why-only), unreadable io/fds, and a non-zero JIT/anon-exec code surface.
static ProcInfo make_truncated_snapshot() {
    ProcInfo p;
    p.valid = true;
    p.pid = 9001;
    p.ppid = 1;
    p.uid = 1000;
    p.user = "will";
    p.comm = "worker";
    p.state = 'R';
    p.runtime = "native";
    p.argv = {"worker", "--flag"};
    p.argv_truncated = true;
    p.exe = "/usr/bin/worker";
    p.cwd = "/home/will";
    p.elapsed_s = 12.0;

    p.n_threads = 500;
    {
        PiThread t;
        t.tid = 9002;
        t.comm = "worker";
        t.state = 'R';
        t.have_syscall = true;
        t.nr = 0;
        t.name = "read";
        t.pc_sym = "main+0x20";
        p.threads.push_back(t);
    }
    {
        PiThread t;
        t.tid = 9003;
        t.comm = "worker";
        t.state = 'S';
        t.wchan = "futex_wait_queue_me";
        p.threads.push_back(t);
    }
    {
        PiThread t;
        t.tid = 9004;
        t.comm = "worker";
        t.state = 'S';
        t.why = "needs ptrace permission (Yama ptrace_scope / uid)";
        p.threads.push_back(t);
    }
    p.threads_truncated = true;

    p.syms_total = 42;
    p.jit_methods = 7;
    p.jit_source = "perf-map";
    p.anon_exec_bytes = 4096;
    {
        PiModule m;
        m.name = "libfoo.so";
        m.size = 0;
        m.syms = 0;
        m.exec = false;
        p.modules.push_back(m);
    }
    p.modules_truncated = true;

    p.attachable = 1;
    p.attach_why = "same uid, no LSM blocking";
    {
        PiMode m;
        m.mode = "log";
        m.ok = true;
        p.modes.push_back(m);
    }
    {
        PiMode m;
        m.mode = "stream";
        m.ok = false;
        m.why = "stream needs a live host";
        p.modes.push_back(m);
    }

    p.cgroup = "/user.slice";
    p.seccomp = 2; // filtered

    {
        PiChild c;
        c.pid = 9100;
        c.comm = "child1";
        p.children.push_back(c);
    }
    p.children_truncated = true;

    p.io_readable = false;
    p.fds_readable = false;
    p.budget_exceeded = true;
    return p;
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // --- 1. nothing selected -------------------------------------------
    // A stale VALID snapshot sitting in the runner (want_pid already matches
    // selected_pid=0, so procinfo_tick's change-branch does not blank it —
    // this is the precondition that actually exercises the pane's OWN
    // `selected_pid <= 0` guard rather than piggy-backing on tick's reset)
    // must not leak into the render. This is the mutation the brief calls
    // out by name: "the pane rendering a snapshot when selected_pid <= 0".
    {
        InspectState s;
        s.details.shown = load_fixture("procinfo_full.asmtrace");
        check("fixture parsed (full)", s.details.shown.valid,
              "procinfo_full.asmtrace failed to parse");
        s.selected_pid = 0;
        std::string out = render(s, io);
        want("no-selection hint shown", out, "pick a process");
        avoid("no-selection hides a lingering valid snapshot's comm", out,
              "zsh");
        avoid("no-selection hides a lingering valid snapshot's pid line", out,
              "pid 2346208");
    }

    // --- 2. a full snapshot, injected straight into the runner ----------
    {
        InspectState s;
        ProcInfo full = load_fixture("procinfo_full.asmtrace");
        check("fixture parsed (full, case 2)", full.valid,
              "procinfo_full.asmtrace failed to parse");
        settle(s, 4242, full);
        std::string out = render(s, io);
        want("header renders the real pid", out, "pid 2346208");
        want("header renders comm", out, "zsh");
        avoid("no budget banner on an unexceeded gather", out,
              "gather budget ran out");
        avoid("no threads_truncated line when the flag is false", out,
              "of 1)"); // n_threads==1 in this fixture; nothing should
                       // ever read "(showing N of 1)" for a single row
        want("names verdict renders (uncaveated; budget not exceeded)", out,
             "will show names");
        want("attach word MAYBE for attachable==-1", out, "MAYBE");
        want("mode ok renders yes", out, "yes");
    }

    // --- 3. the refusal fixture ------------------------------------------
    {
        InspectState s;
        ProcInfo refused = load_fixture("procinfo_refused.asmtrace");
        check("fixture parsed (refused)", refused.valid,
              "procinfo_refused.asmtrace failed to parse");
        settle(s, 4343, refused);
        s.details.status = "timed out after 2.0s — the probe was killed";
        std::string out = render(s, io);
        want("refusal status renders", out, "timed out after 2.0s");
        want("attach word NO for attachable==0", out, "NO");
        want("attach_why renders", out,
             "target uid 0 (root) differs from ours");
        // Every mode was refused in this fixture, each with its OWN why — a
        // refusal without ITS reason is exactly the failure this pane exists
        // to prevent (the brief's "a refused mode's why not shown" check).
        want("a mode's why renders", out,
             "mode log needs an attach, which is refused");
        want("threads_truncated states showing N of the kernel total", out,
             "showing 2 of 200");
        want("a syscall-resolved thread renders its name", out, "read");
        want("its pc_sym renders alongside", out, "main+0x10");
        want("a syscall-unreadable thread renders wchan, not blank", out,
             "sigsuspend");
    }

    // --- 4. budget_exceeded + every *_truncated flag, hand-built --------
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        settle(s, 5555, p);
        std::string out = render(s, io);
        want("budget banner renders and names the affected fields", out,
             "gather budget ran out");
        want("budget banner names module sizes", out, "module sizes");
        want("argv_truncated appends an ellipsis to the command line", out,
             "worker --flag …");
        want("threads_truncated shows the exact counts", out,
             "showing 3 of 500");
        want("modules_truncated states the cut list, not a silent drop", out,
             "showing the 1 highest-symbol modules");
        want("children_truncated states its cap", out, "capped at 1");
        // budget_exceeded must caveat the names verdict rather than assert
        // it (Task 5 review addition): syms_total/jit_methods can themselves
        // be undercounts of a truncated gather.
        want("names verdict is CAVEATED when budget_exceeded", out,
             "may be undercounts");
        avoid("the confident names-verdict sentence is withheld", out,
              "will show names");
        want("a wchan-only thread renders its wchan", out,
             "futex_wait_queue_me");
        want("a why-only thread renders its reason (no syscall, no wchan)",
             out, "needs ptrace permission");
        want("io unreadable states the credential remedy", out,
             "needs matching creds");
        want("fds unreadable states so, not a bare 0", out, "unreadable");
    }

    // --- 5. have==false renders an em dash, NEVER "0%" -------------------
    // Paired with its contrast case (have=true, cpu_pct=0.0) so a mutation
    // that ignores `have` and always prints a percentage is caught: the
    // not-yet-measurable case must NOT contain "0.0%", and the measured-zero
    // case must.
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false; // isolate the rates check from the banner
        ProcRates none; // have=false by default
        settle(s, 6001, p, none);
        std::string out = render(s, io);
        want("cpu em-dash when not yet measurable", out, "needs a second sample");
        avoid("cpu never prints 0% for a first snapshot", out, "0.0%");
    }
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        ProcRates zero;
        zero.have = true;
        zero.cpu_pct = 0.0;
        settle(s, 6002, p, zero);
        std::string out = render(s, io);
        want("cpu DOES print 0.0% once a rate is genuinely measured", out,
             "0.0%");
    }

    ImGui::DestroyContext();

    if (failures) {
        std::fprintf(stderr, "test_details_draw: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_details_draw: all checks passed\n");
    return 0;
}
