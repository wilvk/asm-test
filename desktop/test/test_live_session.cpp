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

#include "live/session.h"

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

int main(void) {
    test_state_machine();
    test_process_path();
    if (failures) {
        std::fprintf(stderr, "test_live_session: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_live_session: all checks passed\n");
    return 0;
}
