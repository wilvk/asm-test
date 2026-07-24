// test_recording.cpp — the .asmtrace loader's reject rules + honesty accounting,
// one case per rule (03-desktop-shell.md T3). The D7 laws are executable here,
// not prose: each dishonesty fixture produces its asserted outcome (a hard error,
// or a flag surfaced into the model). Runs on any host (no GL, no engines).
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/workspace.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

static void fail(const char *what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
    failures++;
}
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static std::string fx(const char *name) {
    return std::string(ASMTEST_FIXTURE_DIR) + "/" + name;
}
// Load a fixture that MUST succeed; records a failure and returns a default
// Recording if it does not (so the caller's later asserts still run predictably).
static Recording must_load(const char *what, const char *name) {
    std::string err;
    auto rec = load_recording_file(fx(name), err);
    if (!rec) {
        fail(what, std::string("expected load OK, got error: ") + err);
        return Recording{};
    }
    return *rec;
}
// Assert a fixture is REJECTED, optionally requiring a substring in the message.
static void must_reject(const char *what, const char *name,
                        const char *needle) {
    std::string err;
    auto rec = load_recording_file(fx(name), err);
    if (rec) {
        fail(what, "expected reject, but load succeeded");
        return;
    }
    if (needle && err.find(needle) == std::string::npos)
        fail(what, std::string("message '") + err + "' lacks '" + needle + "'");
}

int main() {
    // load_minimal_groups_by_kind
    {
        Recording r = must_load("minimal", "min-trace.asmtrace");
        check("minimal/version", r.version == 1, "version != 1");
        check("minimal/backend", r.provenance.backend == "emu-l0", "backend");
        check("minimal/producer", r.producer.name == "asmtrace_record",
              "producer");
        check("minimal/arch", r.arch == "x86_64", "arch");
        check("minimal/groups", r.by_kind["trace"].size() == 2,
              "trace count != 2");
        check("minimal/clean", !r.truncated() && !r.dropped(),
              "should be clean");
        check("minimal/has_end", r.has_end && !r.torn, "should have an end");
        // declared_events (footer's own count) == the reader's own count
        check("minimal/count", r.declared_events == r.event_count(),
              "declared_events != event_count");
        check("minimal/count2", r.event_count() == 2, "event_count != 2");
    }
    // disasm_optional_passthrough (D10): present on one trace, absent on the other
    {
        Recording r = must_load("disasm", "min-trace.asmtrace");
        const auto &tr = r.by_kind["trace"];
        check("disasm/present",
              tr.size() == 2 &&
                  tr[0].body.value("disasm", "") == "mov eax, edi",
              "first trace disasm");
        check("disasm/absent", tr.size() == 2 && !tr[1].body.contains("disasm"),
              "second trace should have no disasm");
    }
    // reject_newer_major — by name (mentions both majors)
    must_reject("newer-major", "newer-major.asmtrace", "99");
    // reject_missing_provenance
    must_reject("missing-provenance", "missing-provenance.asmtrace",
                "provenance");
    // unknown_kind_kept_and_counted + unknown_field_ignored
    {
        Recording r = must_load("unknown", "unknown-kind.asmtrace");
        check("unknown/kept", r.by_kind["future_widget"].size() == 1,
              "unknown kept");
        check("unknown/counted", r.unknown_kinds == 1, "unknown_kinds != 1");
        // a known kind carrying an unknown field still loads; the field survives
        // in body (ignored, not an error)
        check("unknown/known-not-counted", r.by_kind.count("note") == 1,
              "note should be present and known");
        check("unknown/field-survives",
              r.by_kind["note"][0].body.contains("surprise"),
              "unknown field should be preserved in body");
    }
    // serve_lifecycle_kinds_are_known (07-serve-live-host.md T1)
    // One session's slice of a --serve stream. The point of the fixture is that
    // the three lifecycle kinds are DEFINED, not reserved: a client reading a
    // live session must not report its own bracketing as unreadable noise.
    {
        Recording r = must_load("serve", "serve-session.asmtrace");
        check("serve/unknown-kinds-zero", r.unknown_kinds == 0,
              "session/cmd/err must be KNOWN kinds, not unknown ones");
        check("serve/session-events", r.by_kind["session"].size() == 3,
              "expected 3 session events");
        check("serve/cmd-echo", r.by_kind["cmd"].size() == 2,
              "expected the accepted-command echoes (start + stop)");
        check("serve/err-kept", r.by_kind["err"].size() == 1,
              "expected the refusal event");
        // The mode's own events ride between the lifecycle lines untouched —
        // protocol law 1: a session's events ARE record mode's events.
        check("serve/mode-events", r.by_kind["syscall"].size() == 2,
              "the log mode's syscall events should load normally");
        check("serve/payload-split",
              r.by_kind["syscall"][0].body.value("line", "").find("<path>") !=
                  std::string::npos,
              "the payload-free line must survive the split");
        // A refusal does not end a session, and a skip is a SUCCESS with
        // nothing to report — neither may read as a load failure.
        check("serve/err-reason",
              !r.by_kind["err"][0].body.value("reason", "").empty(),
              "err must name the rule that refused the command");
        check("serve/skip-code",
              r.by_kind["session"][2].body["skip"].value("code", 0) == 2,
              "the skip event must carry the positive engine code");
        check("serve/not-torn", !r.torn, "the session slice closed cleanly");
        // THE client rule (protocol law 1): the `end` footer counts RECORDING
        // events, and cmd/err are control lines that land mid-session — so a
        // client reconciles the footer only AFTER dropping the serve-only
        // kinds. Checking it unfiltered would check the wrong contract and
        // would read a healthy stream as a corrupt recording.
        {
            uint64_t recorded = 0;
            for (const auto &kv : r.by_kind)
                if (kv.first != "session" && kv.first != "cmd" &&
                    kv.first != "err" && kv.first != "note")
                    recorded += kv.second.size();
            check("serve/footer-reconciles", recorded == r.declared_events,
                  "end.events must equal the non-control event count");
            // ...and the control lines really are inside the slice, or the
            // check above is vacuous.
            check("serve/control-lines-inside",
                  r.by_kind.count("err") == 1 && r.by_kind.count("cmd") == 1,
                  "the fixture must carry control lines inside the slice");
        }
    }
    // truncation_surfaces_on_recording
    {
        Recording r = must_load("truncated", "truncated.asmtrace");
        check("truncated/flag", r.end_truncated, "end_truncated not set");
        check("truncated/truncated()", r.truncated(),
              "truncated() should be true");
    }
    // drops_surface + statistical
    {
        Recording r = must_load("dropped", "dropped.asmtrace");
        check("dropped/lost", r.drops_lost == 12345, "drops_lost != 12345");
        check("dropped/throttled", r.drops_throttled, "throttled not set");
        check("dropped/dropped()", r.dropped(), "dropped() should be true");
        check("dropped/statistical", r.statistical(),
              "statistical() should be true");
    }
    // redacted_flag_survives
    {
        Recording r = must_load("redacted", "redacted.asmtrace");
        check("redacted/flag", r.provenance.redacted, "redacted flag lost");
    }
    // torn_tail: an unparseable FINAL line is kept as torn (load succeeds)
    {
        Recording r = must_load("torn-tail", "torn-tail.asmtrace");
        check("torn-tail/torn", r.torn, "torn should be set");
        check("torn-tail/no-end", !r.has_end, "should have no end");
        check("torn-tail/truncated()", r.truncated(), "torn ⇒ truncated()");
        // the complete lines before the tear are still there
        check("torn-tail/kept", r.by_kind["syscall"].size() == 1,
              "the one complete syscall line should survive");
    }
    // reject_kindless_event — with a line number (built inline)
    {
        std::string s = "{\"asmtrace\":1,\"provenance\":{\"backend\":\"x\","
                        "\"exact\":true}}\n"
                        "{\"k\":\"note\",\"text\":\"ok\"}\n"
                        "{\"text\":\"this event has no k\"}\n"
                        "{\"k\":\"end\",\"events\":1}\n";
        std::istringstream in(s);
        std::string err;
        auto r = load_recording(in, err);
        check("kindless/reject", !r, "kindless event should be rejected");
        check("kindless/lineno", err.find("line 3") != std::string::npos,
              std::string("message should name line 3: ") + err);
    }
    // unparseable NON-final line is a hard error (not a torn tail)
    {
        std::string s = "{\"asmtrace\":1,\"provenance\":{\"backend\":\"x\","
                        "\"exact\":true}}\n"
                        "{ this is not json\n"
                        "{\"k\":\"end\",\"events\":0}\n";
        std::istringstream in(s);
        std::string err;
        auto r = load_recording(in, err);
        check("midline/reject", !r, "an unparseable middle line should error");
        check("midline/lineno", err.find("line 2") != std::string::npos,
              std::string("message should name line 2: ") + err);
    }
    // torn_missing_end: valid events but no end -> torn (not an error)
    {
        std::string s = "{\"asmtrace\":1,\"provenance\":{\"backend\":\"x\","
                        "\"exact\":true}}\n"
                        "{\"k\":\"note\",\"text\":\"complete line, but the "
                        "stream has no end\"}\n";
        std::istringstream in(s);
        std::string err;
        auto r = load_recording(in, err);
        check("missing-end/loads", (bool)r,
              "a stream without end should still load");
        check("missing-end/torn", r && r->torn, "no end ⇒ torn");
    }
    // reject missing/non-bool provenance.exact (never defaulted)
    {
        std::string s = "{\"asmtrace\":1,\"provenance\":{\"backend\":\"x\"}}\n"
                        "{\"k\":\"end\",\"events\":0}\n";
        std::istringstream in(s);
        std::string err;
        auto r = load_recording(in, err);
        check("no-exact/reject", !r,
              "missing provenance.exact should be rejected");
        check("no-exact/msg", err.find("exact") != std::string::npos,
              std::string("message should mention exact: ") + err);
    }
    // workspace_opens_a_set (plan D3)
    {
        Workspace ws;
        std::string err;
        int a = ws.open(fx("min-trace.asmtrace"), err);
        int b = ws.open(fx("truncated.asmtrace"), err);
        check("workspace/indices", a == 0 && b == 1,
              "open should return indices");
        check("workspace/size", ws.recordings.size() == 2, "set size != 2");
        int bad = ws.open(fx("does-not-exist.asmtrace"), err);
        check("workspace/bad", bad == -1 && !err.empty(),
              "opening a missing file should return -1 with err set");
        check("workspace/unchanged", ws.recordings.size() == 2,
              "a failed open must not grow the set");
        ws.close(0);
        check("workspace/close", ws.recordings.size() == 1,
              "close should shrink");
    }

    if (failures) {
        std::fprintf(stderr, "test_recording: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_recording: PASS\n");
    return 0;
}
