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

    // --- 54 T3: Event::seq, not `index` -----------------------------------
    // The fixture's first event is "session started" (seq 0), so the four
    // syscalls land at seq 1..4 — ascending, matching the source events, and
    // NOT 0-based like `index`, which is exactly the point of carrying seq
    // separately.
    vt::check("seq present", v.seq_present,
              "a recording with real stream positions read as seq_present=false");
    vt::eq("seq[0] follows the session-started event", v.rows[0].seq,
           uint64_t{1});
    vt::eq("seq[1]", v.rows[1].seq, uint64_t{2});
    vt::eq("seq[2]", v.rows[2].seq, uint64_t{3});
    vt::eq("seq[3]", v.rows[3].seq, uint64_t{4});

    // A recording whose only syscall is the very first event in the stream
    // (seq 0) is indistinguishable, row-by-row, from one that never carried a
    // meaningful seq at all — seq_present is what tells the two apart.
    {
        Recording zero;
        zero.arch = "x86_64";
        Event e;
        e.kind = "syscall";
        e.body["line"] = "close(3) = 0";
        e.seq = 0;
        zero.by_kind["syscall"].push_back(e);
        SyscallView zv = obs_syscalls_build(zero);
        vt::eq("all-zero-seq rows", zv.rows.size(), size_t{1});
        vt::eq("all-zero-seq row carries seq 0", zv.rows[0].seq, uint64_t{0});
        vt::check("all-zero-seq recording sets seq_present=false",
                  !zv.seq_present,
                  "a single seq-0 syscall must not read as a real stream "
                  "position");
    }

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

    // The selectable copy view's line source (14 T6): one payload-free line per
    // row, and NEVER the reveal-gated payload — even after a reveal.
    std::vector<std::string> lines = obs_syscall_copy_lines(v);
    vt::eq("copy lines: one per row", lines.size(), v.rows.size());
    vt::check("copy line 0 is <idx>  <line>",
              lines[0].rfind("0  ", 0) == 0 &&
                  lines[0].find(v.rows[0].line) != std::string::npos,
              "line 0 was: " + lines[0]);
    obs_syscall_reveal(rv, 0, true);
    for (const std::string &ln : obs_syscall_copy_lines(rv)) {
        vt::check("copy lines never leak payload (1)", !leaks(ln, kSecret1), ln);
        vt::check("copy lines never leak payload (2)", !leaks(ln, kSecret2), ln);
    }

    // Client-side name filter (16 T2): order-preserving, case-insensitive, and it
    // never touches the model. Built by hand so the assertions do not depend on
    // the fixture's syscall names.
    SyscallView fv;
    SyscallRow r0;
    r0.line = "openat(AT_FDCWD, <path>) = 3";
    SyscallRow r1;
    r1.line = "read(3, <64 bytes>) = 64";
    SyscallRow r2;
    r2.line = "openat(AT_FDCWD, <path>) = 4";
    SyscallRow r3;
    r3.line = "close(3) = 0";
    fv.rows = {r0, r1, r2, r3};

    std::vector<size_t> all = obs_syscall_filter_indices(fv, "");
    vt::check("filter: empty query = all rows, in order",
              all.size() == 4 && all[0] == 0 && all[1] == 1 && all[2] == 2 &&
                  all[3] == 3,
              "empty query must restore the full set");
    std::vector<size_t> op = obs_syscall_filter_indices(fv, "openat");
    vt::check("filter: narrows to matches, ORIGINAL indices ascending",
              op.size() == 2 && op[0] == 0 && op[1] == 2,
              "the two openat rows are #0 and #2, in execution order");
    std::vector<size_t> up = obs_syscall_filter_indices(fv, "OPENAT");
    vt::check("filter: case-insensitive", up.size() == 2 && up[0] == 0 &&
                                              up[1] == 2,
              "OPENAT must match openat");
    vt::eq("filter: whitespace-only query = all",
           obs_syscall_filter_indices(fv, "   ").size(), size_t{4});
    vt::eq("filter: surrounding whitespace trimmed",
           obs_syscall_filter_indices(fv, "  close  ").size(), size_t{1});
    vt::eq("filter: no match = empty",
           obs_syscall_filter_indices(fv, "mmap").size(), size_t{0});
    vt::check("filter: model untouched",
              fv.rows.size() == 4 &&
                  fv.rows[0].line == "openat(AT_FDCWD, <path>) = 3",
              "the filter must never mutate the model");

    // The filter matches the payload-FREE `line` only — never the reveal-gated
    // payload, so filtering can never surface withheld content.
    SyscallRow rp;
    rp.line = "write(1, <8 bytes>) = 8";
    rp.has_payload = true;
    rp.payload = "topsecret";
    fv.rows.push_back(rp);
    vt::eq("filter: matches line only, never payload",
           obs_syscall_filter_indices(fv, "topsecret").size(), size_t{0});

    return vt::report("test_obs_syscalls");
}
