// test_live_session.cpp — the desktop's live capture host (07-serve-live-host
// T3), tested twice over:
//
//  1. the STATE MACHINE, fed lines directly. This is where the interesting
//     rules live — a session's events become a Recording, an `err` does not end
//     a session, a skip is a success, a stream that stops without its `end`
//     footer is TORN — and none of them need a subprocess to check.
//  2. the PROCESS PATH, against desktop/test/fixtures/fake_serve.sh: fork,
//     exec, non-blocking pipe reads, line framing, and reaping. A fake host is
//     what makes this runnable on any machine (no ptrace, no permissions, no
//     victim), and it is also the only way to test the awkward shapes on
//     demand rather than by luck.
//
// No GL, no engines, no ncurses — it links session.o + the doc model and
// nothing else, which is also the standing proof that hosting a live session
// costs the render-only viewer no engine dependency (D9).
#include <cstdio>
#include <string>
#include <vector>

#include <time.h>

#include "live/end_state.h"
#include "live/session.h"
#include "views/tree.h"

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

static const char *kHeader =
    R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},)"
    R"("provenance":{"backend":"ptrace-syscalls","exact":true,"trust":"exact"},"arch":"x86_64"})";

// ---------------------------------------------------------------------------
// 1. the state machine
// ---------------------------------------------------------------------------
static void test_state_machine() {
    {
        // A complete session: lifecycle brackets, header, events, end.
        LiveSession s;
        s.feed_line(R"({"k":"cmd","cmd":"start","mode":"log"})");
        s.feed_line(
            R"({"k":"session","state":"started","mode":"log","pid":4242,"params":{"max":2}})");
        check("sm/running", s.status().state == LiveState::Running,
              "started should put the host in Running");
        check("sm/mode", s.status().mode == "log", "mode should be log");
        check("sm/pid", s.status().pid == 4242, "pid should be echoed");

        s.feed_line(kHeader);
        check("sm/growing", s.growing() != nullptr,
              "a header should open a growing recording");
        s.feed_line(R"({"k":"syscall","line":"openat(...) = 3"})");
        s.feed_line(R"({"k":"syscall","line":"write(...) = 14"})");
        check("sm/grows",
              s.growing() && s.growing()->by_kind.count("syscall") &&
                  s.growing()->by_kind.at("syscall").size() == 2,
              "two syscall events should have landed in the growing recording");

        // An `err` mid-session: surfaced, and NOT session-ending. This is the
        // rule most easily got wrong, because "error" reads like "stop".
        s.feed_line(
            R"({"k":"err","reason":"a session is already running","cmd":"start"})");
        check("sm/err-surfaced",
              s.status().last_err.find("already running") != std::string::npos,
              "the refusal reason must be surfaced");
        check("sm/err-not-terminal", s.status().state == LiveState::Running,
              "an err must NOT end the session");
        check("sm/err-not-an-event",
              s.growing() && s.growing()->by_kind.count("err") == 0,
              "a control line must not be recorded as a session event");

        s.feed_line(
            R"({"k":"end","events":2,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        s.feed_line(
            R"({"k":"session","state":"stopped","mode":"log","events":2,"reason":"stop"})");
        check("sm/idle-after-stop", s.status().state == LiveState::Idle,
              "the jack must be free again after a session stops");
        check("sm/one-recording", s.recordings().size() == 1,
              "the finished session should be one completed recording");
        check("sm/not-torn", !s.recordings()[0].torn,
              "a session closed by its end footer is NOT torn");
        check("sm/footer-reconciles",
              s.recordings()[0].declared_events ==
                  s.recordings()[0].event_count(),
              "end.events must match the recorded events (control lines are "
              "not recorded)");
        check("sm/no-malformed", s.malformed_lines() == 0,
              "nothing here is malformed");
    }
    {
        // A skip is a SUCCESS with nothing to report — never an error.
        LiveSession s;
        s.feed_line(
            R"({"k":"session","state":"started","mode":"sample","pid":7,"params":{}})");
        s.feed_line(
            R"({"k":"session","state":"skip","mode":"sample","skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}})");
        check("sm/skip-code", s.status().skip_code == 2,
              "the positive engine skip code must be kept");
        check("sm/skip-reason",
              s.status().skip_reason.find("AMD") != std::string::npos,
              "the MEASURED reason must be kept verbatim");
        check("sm/skip-not-err", s.status().last_err.empty(),
              "a skip must not be recorded as an error");
        check("sm/skip-idle", s.status().state == LiveState::Idle,
              "a skipped session still frees the jack");
    }
    {
        // The skip must land on the RECORDING, not only on LiveStatus. Status
        // is per-session and the next Start overwrites it; the recording is
        // what every view predicate is handed. Without this, `auto` on a host
        // with perf locked down reaches the panes as an ordinary empty capture,
        // and the "unavailable views" placard recites which events each view
        // would have needed while nothing says the capture was refused.
        LiveSession s;
        s.feed_line(
            R"({"k":"session","state":"started","mode":"auto","pid":7,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(
            R"({"k":"end","events":0,"truncated":false,"skip":{"code":2,"reason":"perf_event_open refused (EACCES): needs perf_event_paranoid<=2 or CAP_PERFMON"}})");
        check("sm/skip-rec-closed", s.recordings().size() == 1,
              "the skipped capture is still a completed recording");
        if (s.recordings().size() == 1) {
            const Recording &r = s.recordings().front();
            check("sm/skip-rec-flagged", r.skipped,
                  "a live capture the producer refused must reach the views as "
                  "SKIPPED, not as an ordinary empty recording");
            check("sm/skip-rec-reason",
                  r.skip_reason.find("perf_event_paranoid") !=
                      std::string::npos,
                  "the reason names the REMEDY and must survive onto the "
                  "recording verbatim");
            check("sm/skip-rec-not-torn", !r.torn,
                  "the producer wrote its footer and said why — that is a "
                  "clean end, not a torn capture");
        }
    }
    {
        // Torn: a stream that stops with a recording still open.
        LiveSession s;
        s.feed_line(
            R"({"k":"session","state":"started","mode":"stream","pid":9,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"stream","text":"work+0x4 push rbp"})");
        s.mark_eof();
        check("sm/torn-count", s.recordings().size() == 1,
              "the open recording should be closed out at EOF");
        check("sm/torn-flag", !s.recordings().empty() && s.recordings()[0].torn,
              "a recording with no end footer is TORN");
        check("sm/torn-truncated",
              !s.recordings().empty() && s.recordings()[0].truncated(),
              "torn implies truncated()");
        check("sm/torn-reason", s.status().last_stop_reason == "host-eof",
              "the UI needs to know the host died rather than the mode ended");
    }
    {
        // Malformed lines are COUNTED, never fatal: a live host is not a file.
        LiveSession s;
        s.feed_line("this is not json");
        s.feed_line(R"({"no_k":true})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"syscall","line":"ok"})");
        check("sm/malformed-counted", s.malformed_lines() == 2,
              "both bad lines should be counted");
        check("sm/malformed-not-fatal",
              s.growing() && s.growing()->by_kind.count("syscall") == 1,
              "a bad line must not discard the session that follows it");
    }
    {
        // The header's reject rules apply ON THE WIRE exactly as in a file: a
        // newer major must be refused by name, not best-efforted.
        LiveSession s;
        s.feed_line(
            R"({"asmtrace":99,"container":"ndjson","provenance":{"backend":"x","exact":true,"trust":"exact"},"arch":"x86_64"})");
        check("sm/newer-major-refused", s.growing() == nullptr,
              "a newer major must not open a recording");
        check("sm/newer-major-named",
              s.status().last_err.find("99") != std::string::npos,
              "the refusal must name the major it refused");
        // ...and a header with no provenance is not a recording at all (D7).
        LiveSession t;
        t.feed_line(R"({"asmtrace":1,"container":"ndjson","arch":"x86_64"})");
        check("sm/no-provenance-refused", t.growing() == nullptr,
              "a header without provenance must be refused");
    }
    {
        // Two sessions back to back: each gets its own recording, and the
        // second's header does not append to the first.
        LiveSession s;
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"syscall","line":"a"})");
        s.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"stream","text":"b"})");
        s.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        check("sm/two-recordings", s.recordings().size() == 2,
              "two headers + two footers = two recordings");
        check("sm/no-bleed",
              s.recordings().size() == 2 &&
                  s.recordings()[0].by_kind.count("stream") == 0 &&
                  s.recordings()[1].by_kind.count("syscall") == 0,
              "events must not bleed between sessions");
    }
    {
        // reset() (Disconnect): a session with real state is returned to its
        // just-constructed shape, and a reconnect that reuses the object does
        // NOT carry the old recording, notes, or malformed count into the new
        // session. This is the difference between a clean Connect and one still
        // showing the last host's capture.
        LiveSession s;
        s.feed_line("garbage-not-json"); // bumps malformed_
        s.feed_line(
            R"({"k":"session","state":"started","mode":"log","pid":4242,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"syscall","line":"openat(...) = 3"})");
        s.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        s.feed_line(
            R"({"k":"session","state":"stopped","mode":"log","events":1,"reason":"stop"})");
        check("reset/pre",
              s.recordings().size() == 1 && s.malformed_lines() == 1,
              "the session should have one recording and one malformed line "
              "before reset");

        s.reset();
        check("reset/no-recordings", s.recordings().empty(),
              "reset must drop every completed recording");
        check("reset/no-notes", s.notes().empty(),
              "reset must drop every lifecycle note");
        check("reset/no-growing", s.growing() == nullptr,
              "reset must leave no recording open");
        check("reset/no-malformed", s.malformed_lines() == 0,
              "reset must zero the malformed counter");
        check("reset/idle", s.status().state == LiveState::Idle,
              "a reset session is Idle, as if just constructed");
        check("reset/no-pid", s.status().pid == 0 && s.status().mode.empty(),
              "reset must forget the last session's pid and mode");

        // A fresh session over the reset object stands alone.
        s.feed_line(
            R"({"k":"session","state":"started","mode":"stream","pid":77,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"stream","text":"b"})");
        s.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        s.feed_line(
            R"({"k":"session","state":"stopped","mode":"stream","events":1,"reason":"stop"})");
        check(
            "reset/reconnect-one", s.recordings().size() == 1,
            "the post-reset session is the only recording — the pre-reset one "
            "did not survive");
        check("reset/reconnect-clean",
              s.recordings().size() == 1 &&
                  s.recordings()[0].by_kind.count("syscall") == 0 &&
                  s.recordings()[0].by_kind.count("stream") == 1,
              "the new recording carries only its own events");
    }
}

// ---------------------------------------------------------------------------
// 2. the process path, against the fake serve host
// ---------------------------------------------------------------------------

// Pump until `want()` holds or the budget runs out. The host is a real
// subprocess, so the test must be time-based — but every wait is bounded and a
// timeout is a FAILURE, never a skip.
static bool pump_until(LiveSession &s, bool (*want)(const LiveSession &),
                       int max_ms = 5000) {
    for (int i = 0; i < max_ms / 5; i++) {
        s.poll();
        if (want(s))
            return true;
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    s.poll();
    return want(s);
}

static void test_process_path() {
    const std::string fake =
        std::string(ASMTEST_FIXTURE_DIR) + "/fake_serve.sh";

    {
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("proc/start", false, "could not spawn the fake host: " + err);
            return;
        }
        check("proc/command",
              s.status().command.find("--serve") != std::string::npos,
              "the spawned command should carry --serve");

        s.send_start("log", 4242);
        bool ok = pump_until(s, [](const LiveSession &x) {
            return x.growing() && x.growing()->by_kind.count("syscall") &&
                   x.growing()->by_kind.at("syscall").size() == 2;
        });
        check("proc/events", ok,
              "the two canned syscall events should arrive over the pipe");
        check("proc/running", s.status().state == LiveState::Running,
              "the session should be Running after its started event");
        check("proc/err-surfaced",
              s.status().last_err.find("already running") != std::string::npos,
              "the mid-session refusal should be surfaced");

        s.send_stop();
        ok = pump_until(
            s, [](const LiveSession &x) { return x.recordings().size() == 1; });
        check("proc/closed", ok, "stop should close the recording");
        check("proc/clean", !s.recordings().empty() && !s.recordings()[0].torn,
              "a stopped session's recording is not torn");
        check("proc/idle", s.status().state == LiveState::Idle,
              "the jack should be free after stop");

        s.shutdown();
        check("proc/reaped", s.status().host_exited,
              "the host should have been reaped");
    }
    {
        // A host that dies mid-session leaves a TORN recording, and the UI must
        // be able to say so. This is the case a real crashed/killed asmspy
        // produces, and the fixture makes it reproducible.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("proc/torn-start", false, "could not spawn: " + err);
            return;
        }
        s.send_start("torn", 4242);
        bool ok = pump_until(s, [](const LiveSession &x) {
            return x.status().host_exited && x.recordings().size() == 1;
        });
        check("proc/torn-seen", ok, "the host should exit mid-session");
        check("proc/torn-flag",
              !s.recordings().empty() && s.recordings()[0].torn,
              "the abandoned recording must be marked TORN");
        check(
            "proc/torn-has-event",
            !s.recordings().empty() &&
                s.recordings()[0].by_kind.count("stream") == 1,
            "what DID arrive is still kept — torn means incomplete, not void");
    }
    {
        // docs/internal/archive/gui/45-launch-and-window-target.md T3: `launch` sends
        // no pid — the host (fake_serve.sh here, asmspy for real) "forks" one
        // and names it only in the `started` reply. Proves send_launch's wire
        // shape (the fixture greps argv[0] back out and echoes it) and that
        // LiveStatus::pid resolves from that reply exactly like an attach's,
        // with no separate plumbing needed in LiveSession itself.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("proc/launch-start", false, "could not spawn: " + err);
            return;
        }
        s.send_launch("log", {"/path/to/victim", "--flag"}, "");
        bool ok = pump_until(s, [](const LiveSession &x) {
            return x.status().state == LiveState::Running;
        });
        check("proc/launch-running", ok,
              "the launch's started event should arrive");
        check("proc/launch-pid", s.status().pid == 9999,
              "LiveStatus::pid must resolve from the started event's \"pid\" "
              "— the launch itself sent none");
        ok = pump_until(s, [](const LiveSession &x) {
            return x.growing() && x.growing()->by_kind.count("syscall");
        });
        check("proc/launch-events", ok,
              "the launched target's syscalls should arrive over the pipe "
              "exactly like an attach's");
        // The fixture greps argv[0] back out of the launch command and echoes
        // it into the started event's (non-schema, test-only) "launch_argv0"
        // — proof the actual argv reached the wire, not just SOME launch
        // command. LiveNote::body is the raw parsed JSON of a session/cmd/err
        // line, so it is readable here without Recording growing a new field.
        bool argv_echoed = false;
        for (const LiveNote &n : s.notes())
            if (n.body.value("launch_argv0", std::string()) ==
                "/path/to/victim")
                argv_echoed = true;
        check("proc/launch-argv", argv_echoed,
              "the sent argv[0] must reach the wire (fake_serve.sh's echo)");
        s.shutdown();
    }
    {
        // The TREE FILTER ROUND TRIP (08-observer-views.md T5): the client
        // builds the start command, the host echoes the EFFECTIVE parameters,
        // and the view reads them back off the wire. The point is that the
        // filter the UI shows as running is the server's answer, not the
        // client's own request replayed to itself.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("proc/tree-start", false, "could not spawn: " + err);
            return;
        }
        TreeFilter f;
        f.depth = 3;
        f.focus = "work";
        f.module = "spy_victim";
        check("proc/tree-filter-legal", obs_tree_filter_error(f).empty(),
              "this filter is legal and must not be refused client-side");
        s.send(obs_tree_start_command(f, 4242));
        bool ok = pump_until(
            s, [](const LiveSession &x) { return x.recordings().size() == 1; });
        check("proc/tree-session", ok, "the tree session should complete");
        if (!s.recordings().empty()) {
            // A live host keeps the lifecycle OUT of the recording (it is not a
            // recording event), so the view is handed it separately — the same
            // views, over the same facts, from either source.
            ObsLifecycle lc;
            for (const LiveNote &n : s.notes())
                if (n.kind == "session")
                    lc.sessions.push_back(n.body);
            TreeView v = obs_tree_build(s.recordings()[0], &lc);
            check("proc/tree-rows", v.rows.size() == 2,
                  "both call events should have arrived");
            check("proc/tree-echo", v.have_effective,
                  "the started event's params echo must reach the view");
            check("proc/tree-depth", v.effective.depth == 3,
                  "depth should round-trip through the session");
            check("proc/tree-focus", v.effective.focus == "work",
                  "focus should round-trip through the session");
            check("proc/tree-module", v.effective.module == "spy_victim",
                  "module should round-trip through the session");
        }
        s.shutdown();
    }
    {
        // A host that cannot be executed is Failed, and says so — distinct from
        // a host that ran and stopped.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = "/nonexistent/definitely-not-asmspy";
        std::string err;
        s.start(spec, err); // fork succeeds; the exec inside it does not
        pump_until(
            s,
            [](const LiveSession &x) {
                return x.status().state == LiveState::Failed;
            },
            3000);
        check("proc/exec-fail", s.status().state == LiveState::Failed,
              "an unexecutable host must end up Failed, not Ended");
        check("proc/exec-fail-named", !s.status().fatal.empty(),
              "the failure must name what could not be run");
    }
}

// The persistent, cause-distinguished session-end placard (23 T2, F20): the
// single collapsed `Ended` is fanned into its four causes, each derivable from
// the state the machine already keeps. Pure end_cause() — no subprocess needed.
static void test_end_state() {
    // StoppedClean: a full session brackets, header, event, `end`, stopped.
    {
        LiveSession s;
        s.feed_line(
            R"({"k":"session","state":"started","mode":"log","pid":7,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"syscall","line":"openat(...) = 3"})");
        s.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        s.feed_line(
            R"({"k":"session","state":"stopped","mode":"log","events":1,"reason":"stop"})");
        EndCause c = end_cause(end_facts_of(s));
        check("end/clean", c == EndCause::StoppedClean,
              "a fully bracketed session is a clean stop");
        check("end/clean-neutral", !end_cause_is_integrity(c),
              "a clean stop is a neutral placard, not integrity");
        check("end/clean-no-fix", end_cause_fix(c).empty(),
              "a clean stop needs no fix line");
    }
    // TornEof: a stream that stops with a recording still open (EOF mid-capture),
    // the host status clean/absent.
    {
        LiveSession s;
        s.feed_line(
            R"({"k":"session","state":"started","mode":"stream","pid":9,"params":{}})");
        s.feed_line(kHeader);
        s.feed_line(R"({"k":"stream","text":"work+0x4 push rbp"})");
        s.mark_eof();
        EndCause c = end_cause(end_facts_of(s));
        check("end/eof", c == EndCause::TornEof,
              "an EOF mid-capture with a clean host status is a torn EOF");
        check("end/eof-integrity", end_cause_is_integrity(c),
              "a torn tail is untrusted -> integrity");
    }
    // ProtocolMismatch: a usage-banner host — malformed (non-JSON) output, no
    // header ever, nothing captured. The overwhelmingly common cause.
    {
        LiveSession s;
        s.feed_line("usage: asmspy [--serve] [options]");
        s.feed_line("  -h, --help   show this help and exit");
        s.mark_eof();
        EndFacts f = end_facts_of(s);
        check("end/mismatch-facts", f.malformed_lines == 2 && !f.any_recording,
              "a usage banner is malformed lines with no recording");
        EndCause c = end_cause(f);
        check("end/mismatch", c == EndCause::ProtocolMismatch,
              "malformed output + no header = a non-serve asmspy");
        std::string fix = end_cause_fix(c);
        check("end/mismatch-fix-make-cli",
              fix.find("make cli") != std::string::npos,
              "the fix must name `make cli`");
        check("end/mismatch-fix-reconnect",
              fix.find("reconnect") != std::string::npos,
              "the fix must name Disconnect + reconnect");
        check("end/mismatch-integrity", end_cause_is_integrity(c),
              "a protocol mismatch is loud (integrity)");
    }
    // TornHostGone: a non-clean host exit (a crash) leaving a torn recording.
    // host_status is set by reap() (a subprocess), so this cause is asserted by
    // constructing the facts — end_cause is pure.
    {
        EndFacts f;
        f.host_exited = true;
        f.host_status = 139; // SIGSEGV-shaped: a crash, not a clean exit
        f.any_recording = true;
        f.torn_recording = true;
        EndCause c = end_cause(f);
        check("end/host-gone", c == EndCause::TornHostGone,
              "a non-clean host exit with a torn tail is a crash");
        check("end/host-gone-integrity", end_cause_is_integrity(c),
              "a crash mid-capture is integrity");
    }
    // HostFailedNoData: the host exited with an ERROR before any recording began
    // (a refused ssh, a bad flag) — no data, non-zero status, no usage banner.
    // Must NOT collapse to a clean stop.
    {
        EndFacts f;
        f.host_exited = true;
        f.host_status = 255 << 8; // ssh-shaped: WEXITSTATUS 255, a failed connect
        f.malformed_lines = 0;
        f.any_recording = false;
        f.torn_recording = false;
        EndCause c = end_cause(f);
        check("end/host-failed", c == EndCause::HostFailedNoData,
              "a non-zero host exit with no recording is a host failure");
        check("end/host-failed-not-clean", c != EndCause::StoppedClean,
              "a failed host must never read as a clean stop");
        check("end/host-failed-integrity", end_cause_is_integrity(c),
              "a host that failed to start is loud (integrity)");
        check("end/host-failed-has-fix", !end_cause_fix(c).empty(),
              "the host-failure placard offers a mechanical fix");
    }
    // A host that ran and exited CLEANLY (status 0) with no recording is still a
    // clean stop, not a failure — the discriminator is the non-zero status.
    {
        EndFacts f;
        f.host_exited = true;
        f.host_status = 0;
        f.any_recording = false;
        EndCause c = end_cause(f);
        check("end/clean-no-data", c == EndCause::StoppedClean,
              "a clean host exit with no data is a clean stop, not a failure");
    }
    // The five placards are DISTINCT strings (the user can tell them apart).
    {
        std::string a = end_cause_message(EndCause::StoppedClean);
        std::string b = end_cause_message(EndCause::TornHostGone);
        std::string d = end_cause_message(EndCause::TornEof);
        std::string e = end_cause_message(EndCause::ProtocolMismatch);
        std::string g = end_cause_message(EndCause::HostFailedNoData);
        check("end/distinct",
              a != b && a != d && a != e && a != g && b != d && b != e &&
                  b != g && d != e && d != g && e != g,
              "each cause must render a distinct placard string");
        check("end/mismatch-nothing-captured",
              e.find("Nothing was captured") != std::string::npos,
              "the protocol-mismatch placard must state nothing was captured");
        check("end/host-failed-not-clean-text",
              g.find("not a clean stop") != std::string::npos,
              "the host-failure placard must disclaim a clean stop");
    }
}

// The spawn argv (59 T1 / plan Task 8). A merged multi-mode recording is the
// only artifact that can carry `call` + `trace`/`coverage` + wide
// `df_step.ops` together, because this class keeps each capture as its own
// Recording and the Save pane serialises exactly one. `asmspy --serve
// --record=<f>` already produces such a file; the desktop simply had no way to
// ask for one.
static void test_host_argv(void) {
    auto joined = [](const std::vector<std::string> &v) {
        std::string o;
        for (size_t i = 0; i < v.size(); i++)
            o += (i ? " " : "") + v[i];
        return o;
    };

    // No record path: byte-identical to what every existing lane expects.
    {
        LiveSession::Spec sp;
        const std::string a = joined(LiveSession::host_argv(sp, "/bin/asmspy"));
        check("argv/local-plain", a == "/bin/asmspy --serve",
              "an empty record_path must leave the argv exactly as it was: "
              "got \"" + a + "\"");
    }
    {
        LiveSession::Spec sp;
        sp.ssh_host = "box";
        const std::string a = joined(LiveSession::host_argv(sp, "asmspy"));
        check("argv/ssh-plain", a == "ssh -T box asmspy --serve",
              "the ssh argv must be unchanged when nothing is recorded: got \"" +
                  a + "\"");
    }

    // With one: --record rides after --serve.
    {
        LiveSession::Spec sp;
        sp.record_path = "/tmp/s.asmtrace";
        const std::string a = joined(LiveSession::host_argv(sp, "/bin/asmspy"));
        check("argv/local-record",
              a == "/bin/asmspy --serve --record=/tmp/s.asmtrace",
              "a record_path must reach the host as --record=<path>: got \"" +
                  a + "\"");
    }
}

int main(void) {
    test_state_machine();
    test_process_path();
    test_end_state();
    test_host_argv();
    if (failures) {
        std::fprintf(stderr, "test_live_session: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_live_session: all checks passed\n");
    return 0;
}
