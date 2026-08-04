// test_procinfo.cpp — the Process details model (the details pane's half that
// needs no subprocess and no ImGui).
//
// Three things here are wrong in ways nothing else would catch:
//
//  - RATES. The wire carries RAW jiffies and a monotonic stamp, never a rate,
//    so %CPU exists only as a difference between two snapshots. A first
//    snapshot must therefore report NO rate rather than 0% — "measured zero"
//    and "not yet measurable" are different claims, and one of them is a lie.
//
//  - ABSENCE. A thread with no readable syscall omits the object and carries
//    syscall_why. A model that folded that into an empty string would render
//    a blank cell, which reads as "doing nothing" — never true.
//
//  - HEX STRINGS. pc/sp/base/args cross the wire as strings precisely so a
//    64-bit pointer is not rounded through a double. Parsing them as numbers
//    passes every small-value test and corrupts every real address.
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>

#include "doc/recording.h"
#include "live/procinfo.h"

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

// The loader is `std::optional<Recording> load_recording(std::istream &in,
// std::string &err)` (doc/recording.h:133) — it reports a load failure as an
// empty optional plus a reason, so a fixture that fails to load must surface
// THAT reason rather than a generic parse error two layers up.
static ProcInfo load(const char *name) {
    std::ifstream f(std::string(ASMTEST_FIXTURE_DIR) + "/" + name);
    std::string err;
    std::optional<Recording> r = load_recording(f, err);
    if (!r) {
        ProcInfo bad;
        bad.parse_error = std::string("load_recording failed: ") + err;
        return bad;
    }
    return procinfo_parse(*r);
}

int main() {
    ProcInfo p = load("procinfo_full.asmtrace");

    check("parsed", p.valid, p.parse_error);
    check("pid", p.pid > 0, "no pid");
    check("comm", !p.comm.empty(), "no comm");
    check("argv", !p.argv.empty(), "no argv");
    check("threads", !p.threads.empty(), "no threads");
    check("clk_tck", p.clk_tck > 0, "no tick rate");
    check("ts_ns", p.ts_ns > 0, "no timestamp");
    check("modes listed", p.modes.size() >= 9, "not every mode reported");

    // A refused mode ALWAYS carries its reason.
    for (const PiMode &m : p.modes)
        check("refused mode states why", m.ok || !m.why.empty(),
              "mode " + m.mode + " refused with an empty why");

    // Absence is preserved as absence, with a reason.
    for (const PiThread &t : p.threads)
        check("absent syscall states why", t.have_syscall || !t.why.empty(),
              "a thread with no syscall and no why");

    // Hex strings survive as full 64-bit values.
    {
        ProcInfo h = p;
        h.threads.clear();
        PiThread t;
        t.have_syscall = true;
        t.pc = procinfo_parse_hex("0xdeadbeefcafef00d");
        check("64-bit pc survives", t.pc == 0xdeadbeefcafef00dull,
              "hex parsed lossily — a double would have rounded this");
    }

    // A module's `base` crosses the wire as a hex STRING too, and this checks
    // the REAL decode path (procinfo_parse), not a hand-built struct: any real
    // module base is a large address, so a hex-string-parsed-as-decimal
    // mutation (which yields exactly 0 for every "0x..."-prefixed string) is
    // caught here even though the exact ASLR value is not.
    check("module base is a real address",
          !p.modules.empty() && p.modules[0].base > 0xFFFFull,
          "module base = " +
              std::to_string(p.modules.empty()
                                  ? 0ull
                                  : (unsigned long long)p.modules[0].base));

    // --- rates: the first snapshot has NO rate -----------------------
    ProcRates r0 = procinfo_rates(ProcInfo{}, p);
    check("no rate from one snapshot", !r0.have,
          "a single snapshot cannot yield a rate — 0% would be a lie");


    // A second snapshot one second later, 50 jiffies busier at 100 Hz = 50%.
    ProcInfo q = p;
    q.ts_ns = p.ts_ns + 1000000000ull;
    q.utime = p.utime + 50;
    q.clk_tck = 100;
    q.io_read_bytes = p.io_read_bytes + 2048;
    ProcRates r1 = procinfo_rates(p, q);
    check("rate available", r1.have, "two snapshots must yield a rate");
    check("cpu 50%", r1.cpu_pct > 49.0 && r1.cpu_pct < 51.0,
          "cpu_pct = " + std::to_string(r1.cpu_pct));
    check("read 2048 B/s", r1.read_bps > 2040.0 && r1.read_bps < 2056.0,
          "read_bps = " + std::to_string(r1.read_bps));

    // An explicitly-invalid snapshot must never seed a rate even when its
    // pid/start_ticks MATCH cur — this isolates the `valid` guard from the
    // pid/start_ticks guard right below, which would otherwise mask its
    // removal (both prev=ProcInfo{} above AND an invalid same-pid prev hit
    // some guard, but only this one proves THIS specific guard is load-
    // bearing).
    ProcInfo invalid_same_pid = p;
    invalid_same_pid.valid = false;
    check("no rate from an explicitly-invalid snapshot",
          !procinfo_rates(invalid_same_pid, q).have,
          "an invalid snapshot must not seed a rate even with a matching pid");

    // A snapshot of a DIFFERENT process must never produce a rate: the
    // counters are unrelated and the difference is meaningless.
    ProcInfo other = q;
    other.pid = p.pid + 1;
    check("no cross-pid rate", !procinfo_rates(p, other).have,
          "rates derived across two different pids");

    // A REUSED pid: same pid, but the kernel's start_ticks differs (the old
    // process at this pid exited and a new, unrelated one got the same
    // number) — the counters belong to two different processes and must
    // never be diffed into a rate.
    ProcInfo reused_pid = q;
    reused_pid.start_ticks = p.start_ticks + 1;
    check("no reused-pid rate", !procinfo_rates(p, reused_pid).have,
          "rates derived across a reused pid (same pid, different "
          "start_ticks)");

    // Backwards time (a cached snapshot replayed) yields no rate.
    ProcInfo back = p;
    back.ts_ns = p.ts_ns - 1000;
    check("no backwards rate", !procinfo_rates(p, back).have,
          "a negative interval produced a rate");

    // --- the names verdict -------------------------------------------
    check("names verdict non-empty", !procinfo_names_verdict(p).empty(),
          "no verdict");
    // Non-emptiness alone survives a verdict that ignores syms_total
    // entirely (it would still return SOME non-empty sentence) — check the
    // real count shows up in it.
    check("names verdict reflects syms_total",
          procinfo_names_verdict(p).find(std::to_string(p.syms_total)) !=
              std::string::npos,
          "verdict '" + procinfo_names_verdict(p) +
              "' does not mention syms_total=" + std::to_string(p.syms_total));

    // Sections with no coverage above: runtime (the wire key is "static", not
    // "static_linked" — a rename mismatch would silently leave this false-by-
    // default, indistinguishable from a real statically-linked target unless
    // checked against a target KNOWN to be dynamic), containment (the wire key
    // is "differs", decoded into ns_differs — same rename risk), and children
    // (asmspy always has at least the asmspy helper itself as a child here).
    check("runtime decodes (elf_class/pie)", p.elf_class == 64 && p.pie,
          "elf_class=" + std::to_string(p.elf_class) +
              " pie=" + std::to_string(p.pie));
    check("runtime 'static' key maps to static_linked", !p.static_linked,
          "static_linked should be false for this dynamically-linked target");
    check("containment 'differs' key maps to ns_differs", !p.ns_differs,
          "ns_differs should be false (same namespaces as us)");
    check("containment cgroup decodes", !p.cgroup.empty(), "empty cgroup");
    check("rss_kb decodes", p.rss_kb > 0, "rss_kb should be nonzero");
    check("children decode", !p.children.empty() && p.children[0].pid > 0,
          "expected at least one child with a real pid");

    // The code-surface section (syms_total/jit_methods/jit_source/
    // anon_exec_bytes) has no check downstream that depends on its exact
    // value — procinfo_names_verdict() returns a non-empty string either
    // way — so it needs its own check, or a section-drop mutation there
    // is invisible.
    check("syms_total decodes", p.syms_total > 0, "syms_total should be nonzero");

    // The three truncation/budget flags below (modules_truncated,
    // children_truncated, budget_exceeded) parallel threads_truncated (which
    // IS exercised, via the refused fixture) but were otherwise unchecked: a
    // decode bug that hardcodes any of them would be invisible. The real
    // capture is small enough that none of its caps were hit.
    check("modules_truncated decodes (false here)", !p.modules_truncated,
          "modules_truncated should be false for this small capture");
    check("children_truncated/budget_exceeded decode (false here)",
          !p.children_truncated && !p.budget_exceeded,
          "both should be false for this small, fast capture");

    // --- the refused fixture ------------------------------------------
    ProcInfo x = load("procinfo_refused.asmtrace");
    check("refused parses", x.valid, x.parse_error);
    check("refused verdict", x.attachable == 0, "expected attachable 0");
    check("refused why", !x.attach_why.empty(), "empty why on a refusal");
    check("refused remedy", !x.attach_remedy.empty(),
          "empty remedy on a refusal (this fixture always sets one)");
    check("refused truncation stated", x.threads_truncated,
          "fixture sets threads_truncated");
    for (const PiMode &m : x.modes) {
        check("all modes refused", !m.ok, "mode " + m.mode + " unexpectedly ok");
        // A refused mode ALWAYS carries its reason — same absence-with-a-
        // reason rule as a thread's syscall_why, and the p-fixture loop above
        // never actually exercises this arm (every mode there is ok=true).
        check("refused mode states why (refused fixture)", !m.why.empty(),
              "mode " + m.mode + " refused with an empty why");
    }

    // n_threads is the KERNEL's own count; `threads` is the capped rows. The
    // fixture sets them to DIFFERENT values on purpose — collapsing them (a
    // model that read threads.size() for both) would make a 200-thread
    // process silently claim it has 2.
    check("n_threads stays the kernel's count, not threads.size()",
          x.n_threads == 200 && x.threads.size() == 2,
          "n_threads=" + std::to_string(x.n_threads) +
              " threads.size()=" + std::to_string(x.threads.size()));

    // The refused fixture hand-authors ONE thread with a real `syscall`
    // object (procinfo_full's own capture never observes one under this
    // sandbox's ptrace_scope) so the have_syscall=true branch — nr/name/
    // args/pc/sp/pc_sym, all through procinfo_parse_hex — is exercised
    // against known values, not just the hand-built struct above.
    {
        const PiThread *sc = nullptr;
        for (const PiThread &t : x.threads)
            if (t.have_syscall)
                sc = &t;
        check("refused fixture carries a syscall thread", sc != nullptr,
              "no thread with have_syscall==true in procinfo_refused.asmtrace");
        if (sc) {
            check("syscall nr/name decode", sc->nr == 0 && sc->name == "read",
                  "nr=" + std::to_string(sc->nr) + " name=" + sc->name);
            check("syscall args decode as hex",
                  sc->args.size() == 6 && sc->args[0] == 0x3ull &&
                      sc->args[1] == 0x7ffe12340000ull,
                  "args[0]/[1] wrong — a decimal misparse of a 0x-prefixed "
                  "string always yields 0");
            check("syscall pc decodes as a full 64-bit hex value",
                  sc->pc == 0x400000000010ull,
                  "pc = " + std::to_string(sc->pc));
            check("syscall sp decodes as a full 64-bit hex value",
                  sc->sp == 0x7ffe12340ff8ull, "sp = " + std::to_string(sc->sp));
            check("syscall pc_sym decode", sc->pc_sym == "main+0x10",
                  "pc_sym = " + sc->pc_sym);
        }
    }

    // --- a non-procinfo recording is a stated failure, not a blank ----
    Recording empty;
    ProcInfo none = procinfo_parse(empty);
    check("empty is invalid", !none.valid, "an empty recording parsed as valid");
    check("empty says why", !none.parse_error.empty(),
          "invalid without a reason");

    if (failures) {
        std::fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_procinfo: all checks passed\n");
    return 0;
}
