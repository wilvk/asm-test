// test_details_draw.cpp — the Process details pane renders in all of its real
// states without a live process: nothing selected, a full snapshot, a
// budget-truncated snapshot (both the "one module survived" and the "whole
// section skipped" shapes), a refusal, and the io/cpu absence-vs-zero
// branches. Drives ImGui's null backend.
//
// This plan's recurring failure mode (seven tasks running) is a first-draft
// test that passes against deliberately broken code — "it did not crash" is
// not evidence a refusal, a truncation flag or an absence-vs-zero distinction
// actually reached the screen. So most checks here read ImGui's own log of
// what it rendered (ImGui::LogToClipboard walks the widget tree emitting its
// literal text; the null backend's clipboard is a plain in-process buffer —
// see imgui.cpp's non-Win32/non-macOS Platform_*ClipboardTextFn_DefaultImpl),
// and assert on that RENDERED TEXT, never merely on absence of a crash.
//
// Text is NOT enough on its own, though: Task 7's review found a ScrollY
// table (the module list) that measured -175px of available height in the
// real docked shell and silently failed to open EVERY FRAME, while
// LogToClipboard happily logged all its rows anyway — the oracle cannot see
// whether a row ever had screen space, only whether its text was submitted.
// The last scenario below therefore reads the table's own ImGuiTable::
// OuterRect (a non-text signal `render()`'s `out_mods_table_h` reports) —
// BeginTable's own RETURN VALUE turned out not to be usable under the null
// backend (its early-return-when-clipped path does not reproduce there;
// see the comment on `render()`), but a table given no explicit outer_size
// still collapses to a ~1px floor there, which its rendered rect exposes.
//
// Every check below was verified BY MUTATION (see the task report for the
// full, current list and score) — reverting the fix under test flips this
// binary to FAIL, then the fix is restored before moving to the next one.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // ImGuiTable::OuterRect -- see render()

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
static size_t count_of(const std::string &hay, const std::string &needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}
// Presence is not enough when the SAME string is legitimately rendered
// somewhere else in the pane -- an attach_why that also fills every mode's
// why-not cell is exactly that case, and a presence check there is satisfied
// by the line ABOVE the table while the cells are all blank. Count instead.
static void want_at_least(const char *what, const std::string &hay,
                          const std::string &needle, size_t least) {
    size_t got = count_of(hay, needle);
    check(what, got >= least,
          "expected '" + needle + "' at least " + std::to_string(least) +
              " times, found " + std::to_string(got) + " in:\n" + hay);
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
//
// The window is a FIXED, modest size (not auto-resizing to content) --
// mirroring a real docked rail rather than an infinite canvas. That is
// deliberate: Task 7 review IMPORTANT 1 found that a `BeginTable(...,
// ImGuiTableFlags_ScrollY)` with no explicit outer_size claims ALL
// remaining vertical space, so a table drawn AFTER it can be starved to a
// negative available height and silently fail to open -- invisible to any
// assertion on LOGGED TEXT (LogToClipboard emits a row's text whether or
// not the table actually claimed screen space). `out_cursor_y`, when
// given, reports where the cursor ended up (window-local Y) after the
// whole pane drew -- a non-text, "did this actually occupy space" signal a
// broken ScrollY table cannot fake, because BeginTable returning false
// draws nothing and does not advance the cursor at all.
static std::string render(InspectState &s, ImGuiIO &io,
                          float *out_mods_table_h = nullptr,
                          float *out_threads_table_h = nullptr) {
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(360, 500), ImGuiCond_Always);
    ImGui::Begin("details-under-test");
    for (const char *label :
         {"Identity", "Code surface", "Containment", "Children"})
        ImGui::GetStateStorage()->SetInt(ImGui::GetID(label), 1);
    ImGui::LogToClipboard();
    draw_details_pane(s);
    ImGui::LogFinish();
    // GetID("mods") computed HERE, with "details-under-test" as the
    // current window and no other ID pushed since Begin(), is the exact
    // same id BeginTable("mods", ...) computed inside draw_details_pane --
    // the pane pushes no extra PushID scope before reaching it.
    if (out_mods_table_h) {
        ImGuiTable *tbl = ImGui::TableFindByID(ImGui::GetID("mods"));
        *out_mods_table_h = tbl ? tbl->OuterRect.GetHeight() : -1.0f;
    }
    // The threads table is the OTHER half of the same fix, and the one that
    // CAUSED it: it is drawn FIRST, so a missing outer_size there is what
    // starved the mods table below. Its own rect is the only signal that
    // says whether its explicit height is still there -- the mods check
    // alone cannot distinguish "threads is bounded" from "this window
    // happened to be tall enough for both".
    if (out_threads_table_h) {
        ImGuiTable *tbl = ImGui::TableFindByID(ImGui::GetID("threads"));
        *out_threads_table_h = tbl ? tbl->OuterRect.GetHeight() : -1.0f;
    }
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

// A budget-truncated gather that aborted BEFORE the code-surface section
// ever started (Task 7 review IMPORTANT 2): syms_total/jit_methods/
// anon_exec_bytes at their zero defaults AND modules empty. Distinct from
// make_truncated_snapshot() above, which has ONE module -- that scenario
// exercises the banner + the truncation lines, not this "the whole section
// was skipped" case, which needs modules genuinely empty to trigger the
// pane's code_unmeasured guard.
static ProcInfo make_budget_aborted_no_modules() {
    ProcInfo p;
    p.valid = true;
    p.pid = 8001;
    p.comm = "worker";
    p.state = 'R';
    p.runtime = "native";
    p.attachable = 1;
    p.attach_why = "same uid, no LSM blocking";
    p.syms_total = 0;
    p.jit_methods = 0;
    p.anon_exec_bytes = 0;
    p.modules.clear();
    // TRUE, matching what the producer actually emits for this exact case:
    // pi_read_code_and_modules' upfront budget guard (cli/asmspy_proc.c)
    // sets modules_truncated = 1 on the way out precisely because "the list
    // below never even started". A fixture with it false described a state
    // the producer cannot reach, and left the pane's "skip the table
    // entirely when there is nothing to show" guard untested -- both of the
    // avoid() checks in scenario 4b were vacuously true against it.
    p.modules_truncated = true;
    p.budget_exceeded = true;
    return p;
}

// Several threads AND several modules, budget_exceeded=false (so the
// code_unmeasured guard does not fire and the mods table is genuinely
// expected to render) -- built to prove Task 7 review IMPORTANT 1: a
// ScrollY table with no outer_size claims ALL remaining vertical space, so
// a table drawn AFTER it (here, "mods" after "threads") can be starved to
// a negative available height and BeginTable silently returns false. A
// text-only assertion cannot see this (LogToClipboard logs a row's text
// whether or not the row ever had screen space); this snapshot exists so
// the test can measure the window's rendered content height instead.
static ProcInfo make_wide_snapshot() {
    ProcInfo p;
    p.valid = true;
    p.pid = 8002;
    p.comm = "widerunner";
    p.state = 'R';
    p.runtime = "native";
    p.attachable = 1;
    p.attach_why = "same uid, no LSM blocking";
    for (int i = 0; i < 8; ++i) {
        PiThread t;
        t.tid = 8100 + i;
        t.comm = "widerunner";
        t.state = 'R';
        t.have_syscall = true;
        t.nr = 0;
        t.name = "read";
        p.threads.push_back(t);
    }
    p.n_threads = 8;
    p.syms_total = 12345;
    for (int i = 0; i < 8; ++i) {
        PiModule m;
        m.name = "libmod" + std::to_string(i) + ".so";
        m.size = 4096;
        m.syms = 10;
        m.exec = true;
        p.modules.push_back(m);
    }
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
        // Derived from the fixture, never hardcoded: these fixtures are
        // REGENERATED from the live producer whenever the wire changes (this
        // wave regenerated both, because they still carried a body the fixed
        // producer can no longer emit), and a literal pid turns every
        // regeneration into an unrelated test edit.
        const std::string pid_line =
            "pid " + std::to_string(s.details.shown.pid);
        avoid("no-selection hides a lingering valid snapshot's pid line", out,
              pid_line.c_str());
    }

    // --- 1b. the Connect pane's asmspy path reaches the runner -----------
    // InspectState::asmspy_path is fed from Settings (main.cpp) and edited in
    // both Connect panes (shell.cpp / inspect_door.cpp); ProcInfoRunner::
    // asmspy_path is what spawn() actually execs. Nothing outside the tests
    // connected the two, so the runner always fell through to
    // resolve_asmspy_path() and its own failure remedy -- "or set the path in
    // Connect" -- named a control that did not feed it. A whitebox check on
    // the copy, because the observable effect (a different binary being
    // exec'd) needs a live fork this draw test does not do. Deliberately on
    // the selected_pid <= 0 path: the copy must happen BEFORE the early
    // return, since procinfo_tick's own path-change backoff reset reads it on
    // the very next line.
    {
        InspectState s;
        s.selected_pid = 0;
        std::strncpy(s.asmspy_path, "/opt/custom/bin/asmspy",
                     sizeof(s.asmspy_path) - 1);
        render(s, io);
        check("the Connect asmspy path is copied into the details runner",
              s.details.asmspy_path == "/opt/custom/bin/asmspy",
              "runner asmspy_path = '" + s.details.asmspy_path +
                  "' -- the pane must assign InspectState::asmspy_path into "
                  "the runner, or Connect's field (and the remedy naming it) "
                  "feed nothing");
    }

    // --- 2. a full snapshot, injected straight into the runner ----------
    {
        InspectState s;
        ProcInfo full = load_fixture("procinfo_full.asmtrace");
        check("fixture parsed (full, case 2)", full.valid,
              "procinfo_full.asmtrace failed to parse");
        settle(s, 4242, full);
        std::string out = render(s, io);
        const std::string pid_line = "pid " + std::to_string(full.pid);
        want("header renders the real pid", out, pid_line.c_str());
        want("header renders comm", out, "zsh");
        avoid("no budget banner on an unexceeded gather", out,
              "gather budget ran out");
        avoid("no threads_truncated line when the flag is false", out,
              "of 1)"); // n_threads==1 in this fixture; nothing should
                       // ever read "(showing N of 1)" for a single row
        want("names verdict renders (uncaveated; budget not exceeded)", out,
             "will show names");
        want("attach word MAYBE for attachable==-1", out, "MAYBE");
        // `ok` is two-valued, the attach verdict is three-valued: a mode
        // that clears its own gates under attachable == -1 is "maybe", not
        // "yes". This fixture is exactly that state (Yama ptrace_scope=1,
        // the stock Debian/Ubuntu default), and it used to render nine
        // green "yes" cells directly under the MAYBE verdict line above,
        // with an empty "why not" column.
        want("mode cell renders maybe under an undetermined attach", out,
             "maybe");
        avoid("an undetermined mode never renders a confident yes", out, "yes");
        // Presence alone CANNOT see this cell: under attachable == -1 the
        // producer copies attach_why verbatim into every mode's why, so the
        // identical string is already on the verdict line above the table
        // and a find() is satisfied with all nine cells blank (measured:
        // reverting the cell to `m.ok ? "" : m.why` left the whole suite
        // green). Count instead -- once for the verdict line, plus once per
        // mode row.
        want_at_least("an undetermined mode carries the attach reason in its "
                      "OWN why-not cell, not only on the verdict line above",
                      out, "yama ptrace_scope", full.modes.size() + 1);
    }

    // --- 2b. the why-not cell renders the MODE's own why field ------------
    // `trace.why` and `modes[].why` are two separate wire fields, and the
    // cell must render the second. Against a real -1 body they hold the same
    // string (the producer copies one into the other), which is why the
    // count check above is the only thing that can see the cell there; here
    // they are made to differ so a single presence check names the field
    // directly. The schema permits any string in a mode's why -- and a
    // 90-char-truncated copy is already a different string whenever
    // attach_why is longer than that -- so this is a shape the wire allows,
    // not one it forbids.
    {
        InspectState s;
        ProcInfo p;
        p.valid = true;
        p.pid = 8300;
        p.comm = "undetermined";
        p.state = 'S';
        p.runtime = "native";
        p.attachable = -1;
        p.attach_why = "yama ptrace_scope=1 — only a descendant";
        {
            PiMode m;
            m.mode = "log";
            m.ok = true; // clears its own gates; the ATTACH is what is unknown
            m.why = "the attach itself is undetermined for this engine";
            p.modes.push_back(m);
        }
        settle(s, 8300, p);
        std::string out = render(s, io);
        want("the why-not cell renders the MODE's own why, not a re-print of "
             "trace.why",
             out, "undetermined for this engine");
        want("the verdict line still renders trace.why", out,
             "only a descendant");
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
        want("budget banner names the symbol/module totals", out,
             "symbol/module totals");
        want("budget banner names anonymous-executable bytes", out,
             "anonymous-executable bytes");
        // "exec bits" is dropped from the banner (Task 7 review IMPORTANT
        // 2): this pane never displays a module's exec bit, so naming it
        // as an affected field was simply wrong.
        avoid("budget banner no longer claims 'exec bits'", out, "exec bits");
        want("argv_truncated appends an ellipsis to the command line", out,
             "worker --flag …");
        want("threads_truncated shows the exact counts", out,
             "showing 3 of 500");
        // "the N highest-symbol modules" describes the 64-row CAP, which is
        // applied AFTER a full symbol-count ranking. This snapshot is a
        // BUDGET abort (budget_exceeded && modules_truncated): the merge
        // loop stopped mid-scan, nothing was ranked, and the survivor's
        // symbol count is 0 because it was never counted.
        want("a budget-cut module list says the ranking never ran", out,
             "cut mid-scan when the gather budget ran out");
        avoid("a budget-cut module list never claims a ranking", out,
              "highest-symbol");
        want("children_truncated states the list is short, not a cap of 1", out,
             "the scan stopped early, so there may be more");
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
        // The CONTRAST case for the "maybe" cell in scenario 2: this
        // snapshot's attachable is 1, so a mode that clears its gates is a
        // real, measured yes and must still render as one.
        want("a mode under a DEFINITE attach still renders yes", out, "yes");
        // The banner must name the two sections a budget abort can cut
        // that nothing else states: the fd walk (measured at 1017 ms
        // against a 1M-fd target, four times the whole budget) and the
        // /proc children scan.
        want("budget banner names the fd count", out, "fd count");
        want("budget banner names the children list", out, "children");
    }

    // --- 4c. a module list cut by the CAP, not the budget ------------------
    // The contrast case for scenario 4's budget-cut wording: modules_
    // truncated with budget_exceeded FALSE is the 64-row cap, which IS
    // applied after a full symbol-count ranking, so "the N highest-symbol
    // modules" is the true sentence there and must survive.
    {
        InspectState s;
        ProcInfo p = make_wide_snapshot();
        p.modules_truncated = true; // capped, not budget-cut
        settle(s, 6502, p);
        std::string out = render(s, io);
        want("a CAP-truncated module list does claim the ranking", out,
             "showing the 8 highest-symbol modules");
        avoid("a cap-truncated list never blames the budget", out,
              "cut mid-scan");
    }

    // --- 4d. an unreadable /proc/<pid>/maps is not a measured-zero code
    // surface. The symbol loader and the module scanner both read that file
    // first, so a target this user does not own produces the IDENTICAL
    // all-zero struct a genuinely code-free process would -- and the pane
    // rendered "0 symbols . 0 modules" plus a confident "will show raw
    // addresses" verdict for it. Reproduced against live targets: 4 of 684
    // pids landed in exactly this shape.
    {
        InspectState s;
        ProcInfo p = make_budget_aborted_no_modules();
        p.budget_exceeded = false;   // isolate this from the budget path
        p.modules_truncated = false; // ... and from the truncation lines
        p.maps_readable = false;
        settle(s, 6503, p);
        std::string out = render(s, io);
        want("an unreadable maps names ITS OWN reason, not the budget's", out,
             "maps could not be read");
        avoid("an unreadable maps never prints a confident count", out,
              "0 symbols");
        avoid("an unreadable maps withholds the raw-addresses verdict", out,
              "will show raw addresses");
        want("the withheld verdict says the measurement is absent", out,
             "absent measurement");
    }

    // --- 4e. namespace ids that could not be read are not "same namespaces"
    // ns_differs is computed from four readlink ids, each 0 when unreadable,
    // and 0 == 0 compares equal -- so all-four-zero fell into the else arm
    // and asserted a shared namespace about a process whose namespaces were
    // never read. Verified live against `docker run alpine`, which showed
    // that claim two lines above a contradicting docker-...scope cgroup.
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        p.ns_pid = p.ns_net = p.ns_mnt = p.ns_user = 0;
        p.ns_differs = false;
        p.cgroup = "/system.slice/docker-abc123.scope";
        settle(s, 6504, p);
        std::string out = render(s, io);
        want("unreadable namespace ids state so", out,
             "namespace ids unreadable");
        avoid("unreadable namespace ids never claim a shared namespace", out,
              "same namespaces as this app");
    }

    // --- 4f. a children scan the budget cut short is not "none" ------------
    // The /proc walk is budgeted per entry, and this pid's children sit
    // wherever inode order puts them -- so an abort before the first hit is
    // reachable, and it used to render "none" (a measurement) followed by
    // "capped at 0".
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.children.clear();
        p.children_truncated = true;
        settle(s, 6505, p);
        std::string out = render(s, io);
        avoid("a cut-short children scan never renders a measured 'none'", out,
              "none");
        avoid("a cut-short children scan never claims a cap of zero", out,
              "capped at 0");
        want("a cut-short children scan says the scan stopped early", out,
             "the /proc scan stopped early");
    }

    // --- 4b. budget_exceeded with the code-surface section fully skipped ---
    // (Task 7 review IMPORTANT 2). Distinct from scenario 4: there, ONE
    // module survives, so the pane's normal "%llu symbols . %zu modules"
    // line is the CORRECT thing to render (truncation, not full-skip).
    // Here modules is genuinely empty, matching pi_read_code_and_modules
    // being skipped outright because the budget was ALREADY exceeded on
    // entry -- reproduced end to end by the reviewer against the real
    // producer. "0 symbols . 0 modules" would assert a measurement that
    // was never taken; the pane must say so instead.
    {
        InspectState s;
        ProcInfo p = make_budget_aborted_no_modules();
        settle(s, 6501, p);
        std::string out = render(s, io);
        want("code-surface unmeasured line renders", out,
             "the gather budget ran out before this section was read");
        avoid("a budget-truncated FULL SKIP never prints a confident count",
              out, "0 symbols");
        avoid("no mods table header renders for a genuinely empty list", out,
              "highest-symbol modules");
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
        // The literal em dash glyph, not just the accompanying English --
        // a mutation could keep the words and drop the dash, or vice versa.
        want("cpu renders the actual em-dash glyph", out, "cpu  \xe2\x80\x94");
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

    // --- 5b/5c. io's readiness is gated on io_readable/io_have, NOT the
    // CPU-derived r.have (Task 7 review "cheap fix") -- neither branch was
    // exercised above: scenario 4 has io_readable=false throughout, so
    // "needs a second sample" and "measured bytes/sec" for io were both
    // untested.
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        p.io_readable = true; // readable, but no second sample YET
        ProcRates r;
        r.have = true; // cpu happens to be ready; io must not borrow this
        r.cpu_pct = 3.0;
        r.io_have = false;
        settle(s, 6003, p, r);
        std::string out = render(s, io);
        want("io em-dash when io_readable but not yet sampled twice", out,
             "io   \xe2\x80\x94 (needs a second sample)");
        avoid("io does not print a rate it never measured", out, "B/s");
    }
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        p.io_readable = true;
        ProcRates r;
        r.have = false; // cpu NOT ready; io must not be masked by this
        r.io_have = true;
        r.read_bps = 512.0;
        r.write_bps = 256.0;
        settle(s, 6004, p, r);
        std::string out = render(s, io);
        want("io renders measured bytes/sec once io_have is true", out,
             "512 B/s read");
        avoid("io is not gated on the CPU-derived have (cpu itself is "
              "correctly '-- (needs a second sample)' here; io must not be)",
              out, "io   \xe2\x80\x94");
    }
    {
        // fds' own em-dash (unreadable), for the same reason as cpu/io above
        // -- the literal glyph, not just the word.
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        settle(s, 6005, p);
        std::string out = render(s, io);
        want("fds renders the actual em-dash glyph", out, "fds  \xe2\x80\x94");
    }

    // --- 6b. an unreadable cwd states so, the same way exe already does ----
    // (Task 7 review "cheap fix"): both are the identical readlink(/proc/
    // <pid>/cwd or .../exe) failure, reachable on a same-uid non-dumpable
    // target (e.g. /usr/bin/ping) -- exe already said "(unreadable)" for
    // it; cwd rendered a blank line instead until now.
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        p.cwd.clear();
        settle(s, 6007, p);
        std::string out = render(s, io);
        want("cwd unreadable states so like exe already does", out,
             "cwd   (unreadable)");
    }

    // --- 7. an ssh host configured states the pid is LOCAL -----------------
    // (doors.h InspectState::ssh_host). Untested until now: every prior
    // scenario left ssh_host blank.
    {
        InspectState s;
        ProcInfo p = make_truncated_snapshot();
        p.budget_exceeded = false;
        settle(s, 6006, p);
        std::strncpy(s.ssh_host, "build-box.internal", sizeof(s.ssh_host) - 1);
        std::string out = render(s, io);
        want("ssh_host states the pid is local", out, "this pid is LOCAL");
        want("ssh_host names the capture host", out, "build-box.internal");
    }

    // --- 8. the mods table must actually occupy screen space (IMPORTANT 1)
    // A text-only assertion cannot catch a ScrollY table that renders at a
    // ~1px floor for lack of an explicit outer_size: LogToClipboard logs a
    // row's text whether or not that row ever had real screen space, which
    // is exactly how this bug shipped uncaught the first time. This reads
    // the "mods" ImGuiTable's own OuterRect straight out of ImGui's table
    // registry (imgui_internal.h, the same idiom test_layout.cpp/
    // test_shell.cpp already use for whitebox docking checks) -- a fixed
    // explicit outer_size (row_h * rows, independent of available space)
    // renders as a real, multi-row-tall rect; the bug's implicit (0,0)
    // outer_size collapses to CalcItemSize's ~1px floor once
    // GetContentRegionAvail() has gone negative, which the threads table
    // above it (8 rows, in a fixed 360x500 window) reliably drives it to.
    {
        InspectState s;
        ProcInfo p = make_wide_snapshot();
        settle(s, 7001, p);
        float mods_h = 0.0f, threads_h = 0.0f;
        std::string out = render(s, io, &mods_h, &threads_h);
        want("wide snapshot's module rows render their text", out,
             "libmod5.so");
        // The threads table's OWN explicit outer_size, asserted directly
        // rather than inferred from the mods table surviving. It is capped
        // at row_h * (min(rows, 8) + 1) -- 9 rows here -- so it must be
        // several rows tall AND must NOT have claimed the rest of the 500px
        // window, which is precisely what a ScrollY table with no
        // outer_size does (that is the bug, and it is the reason the mods
        // table below it measured -175px available).
        check("threads table's rendered rect is several rows tall",
              threads_h > 20.0f,
              "the \"threads\" ImGuiTable's OuterRect height = " +
                  std::to_string(threads_h) + "px");
        check("threads table does NOT claim all remaining window height "
              "(that is what starves the mods table below it)",
              threads_h < 300.0f,
              "the \"threads\" ImGuiTable's OuterRect height = " +
                  std::to_string(threads_h) +
                  "px in a 500px window -- 9 capped rows cannot be that "
                  "tall, so BeginTable(\"threads\", ...) most likely lost "
                  "its explicit outer_size and is claiming "
                  "GetContentRegionAvail()");
        check("mods table's OWN rendered rect is a real height, not the "
              "~1px floor a missing outer_size collapses to (non-text "
              "signal)",
              mods_h > 20.0f,
              "the \"mods\" ImGuiTable's OuterRect height = " +
                  std::to_string(mods_h) +
                  "px -- too short to be 8 real rows; BeginTable(\"mods\", "
                  "...) most likely has no explicit outer_size, so "
                  "CalcItemSize() clamped it to a ~1px floor once the "
                  "threads table above it drove GetContentRegionAvail() "
                  "negative");
    }

    ImGui::DestroyContext();

    if (failures) {
        std::fprintf(stderr, "test_details_draw: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_details_draw: all checks passed\n");
    return 0;
}
