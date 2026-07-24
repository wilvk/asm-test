// test_obs_syscalls.cpp — the syscall stream view (08-observer-views.md T1).
//
// The load-bearing check is negative and deliberately blunt: build the view the
// way the UI builds it, dump everything it would render, and assert the secret
// bytes from the fixture appear NOWHERE. A per-field assertion would pass just
// as happily while a tooltip, a summary line or a debug field leaked the
// payload — and default-redaction is exactly the property that fails silently.
#include "views/syscalls.h"

#include "view_test.h"

using namespace asmdesk;

// The two payload strings in obs-syscalls.asmtrace. Nothing the view renders by
// default may contain either.
static const char *kSecret1 = "/etc/shadow";
static const char *kSecret2 = "hunter2!";

static bool leaks(const std::string &dump, const char *secret) {
    return dump.find(secret) != std::string::npos;
}

int main() {
    Recording rec = vt::load_fixture("obs-syscalls.asmtrace");
    SyscallView v = obs_syscalls_build(rec);

    vt::eq("rows", v.rows.size(), size_t{4});
    vt::check("follow echoed", v.follow,
              "the started params echo said follow:true");
    vt::eq("tid absent by default", v.rows[0].tid, -1L);
    vt::eq("tid when present", v.rows[3].tid, 4243L);
    vt::check("payload-free line kept",
              v.rows[0].line.find("<path>") != std::string::npos,
              "the schema's payload-free rendering is what `line` carries");

    // --- default state: nothing revealed ---------------------------------
    std::string dump = obs_syscalls_dump(v);
    vt::check("no payload bytes by default", !leaks(dump, kSecret1),
              "the default dump contained the openat path");
    vt::check("no payload bytes by default (2)", !leaks(dump, kSecret2),
              "the default dump contained the read buffer");
    vt::check("hidden cell says so",
              obs_syscall_payload_cell(v, 0).find("hidden") !=
                  std::string::npos,
              "a hidden payload must say it is hidden, not render blank");
    vt::check("byte count survives redaction",
              obs_syscall_payload_cell(v, 0).find("11 bytes") !=
                  std::string::npos,
              "the length is structure (the line already carries it)");
    vt::eq("no payload at all", obs_syscall_payload_cell(v, 2),
           std::string("(no payload)"));

    // --- per-row reveal ---------------------------------------------------
    obs_syscall_reveal(v, 0, true);
    vt::eq("revealed row shows content", obs_syscall_payload_cell(v, 0),
           std::string(kSecret1));
    vt::check("reveal is PER ROW",
              !leaks(obs_syscall_payload_cell(v, 1), kSecret2),
              "revealing row 0 revealed row 1 as well");
    obs_syscall_reveal(v, 0, false);
    vt::check("un-reveal works", !leaks(obs_syscalls_dump(v), kSecret1),
              "a hidden row came back revealed");

    // --- session-wide reveal needs a second confirmation ------------------
    vt::check("first reveal-all only arms", !obs_syscall_reveal_all(v),
              "the first call performed the reveal instead of arming it");
    vt::check("still hidden while armed",
              !leaks(obs_syscalls_dump(v), kSecret1),
              "arming the confirmation already revealed the payloads");
    vt::check("armed flag set", v.reveal_all_armed,
              "the prompt would not show");
    vt::check("prompt names the consequence",
              obs_syscall_reveal_all_prompt(v).find("3 syscall") !=
                  std::string::npos,
              "the prompt must say how many payloads it is about to show");
    vt::check("second reveal-all performs it", obs_syscall_reveal_all(v),
              "the confirmed reveal did not happen");
    vt::check("now revealed", leaks(obs_syscalls_dump(v), kSecret1),
              "a confirmed reveal-all still hid the payloads");
    // Any other interaction disarms: a confirmation that survives an unrelated
    // click is a confirmation the next click can trip over.
    SyscallView v2 = obs_syscalls_build(rec);
    obs_syscall_reveal_all(v2);
    obs_syscall_reveal(v2, 1, true);
    vt::check("an unrelated action disarms", !v2.reveal_all_armed,
              "the armed confirmation survived a per-row reveal");

    // --- redaction AT RECORD TIME is a different fact ----------------------
    Recording red = vt::load_fixture("obs-syscalls-redacted.asmtrace");
    SyscallView rv = obs_syscalls_build(red);
    vt::check("record-time redaction seen", rv.record_redacted,
              "provenance.redacted did not reach the view");
    vt::eq("cell distinguishes the two absences",
           obs_syscall_payload_cell(rv, 0),
           std::string("(redacted at record time — not in this recording)"));
    obs_syscall_reveal(rv, 0, true);
    vt::eq("reveal cannot conjure absent bytes",
           obs_syscall_payload_cell(rv, 0),
           std::string("(redacted at record time — not in this recording)"));
    vt::check("truncation banner", !rv.chrome.banner.empty(),
              "a truncated + dropped recording must carry a banner");

    vt::golden("obs-syscalls.txt", obs_syscalls_dump(obs_syscalls_build(rec)));
    vt::golden("obs-syscalls-redacted.txt", obs_syscalls_dump(rv));
    return vt::report("test_obs_syscalls");
}
