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
#include <cerrno>
#include <chrono> // procinfo_tick timing — see the "must never block" test
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <csignal> // ::kill(pid, 0), SIGALRM pressure below
#include <sys/time.h> // setitimer — SIGALRM pressure below
#include <unistd.h> // usleep — see the drain loop below

#include "doc/recording.h"
#include "live/procinfo.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

// EINTR pressure for the I3 test below: a signal handler installed WITHOUT
// SA_RESTART interrupts a blocking waitpid() mid-syscall, exactly as a real
// GUI process's own signal plumbing (e.g. a SIGALRM-based timer elsewhere in
// the app) can. A waitpid that does not retry on EINTR loses the handle and
// leaves the zombie behind permanently — this is what proves it does.
static volatile std::sig_atomic_t g_alarm_hits = 0;
static void alarm_noop(int) { g_alarm_hits++; }

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
    // The 50 is SPLIT across utime and stime rather than loaded entirely
    // onto utime: cpu_pct sums both, and with q.stime == p.stime the stime
    // term contributed exactly 0, so an implementation that dropped stime
    // from the sum altogether produced the same 50% and passed. Split
    // 30 + 20, and dropping either term now lands at 30% or 20%.
    ProcInfo q = p;
    q.ts_ns = p.ts_ns + 1000000000ull;
    q.utime = p.utime + 30;
    q.stime = p.stime + 20;
    q.clk_tck = 100;
    q.io_read_bytes = p.io_read_bytes + 2048;
    ProcRates r1 = procinfo_rates(p, q);
    check("rate available", r1.have, "two snapshots must yield a rate");
    check("cpu 50%", r1.cpu_pct > 49.0 && r1.cpu_pct < 51.0,
          "cpu_pct = " + std::to_string(r1.cpu_pct));
    check("io_have available", r1.io_have,
          "two io_readable snapshots must yield io_have");
    check("read 2048 B/s", r1.read_bps > 2040.0 && r1.read_bps < 2056.0,
          "read_bps = " + std::to_string(r1.read_bps));

    // Task 7 review "cheap fix": io's readiness must be INDEPENDENT of the
    // CPU-only gates (clk_tck==0, utime/stime went backwards) — a target
    // with a genuinely measurable io rate but an unrelated CPU-side hiccup
    // must not have that io rate masked by the CPU gate.
    ProcInfo q_no_tick = q;
    q_no_tick.clk_tck = 0;
    ProcRates r_no_tick = procinfo_rates(p, q_no_tick);
    check("no cpu rate without a tick rate", !r_no_tick.have,
          "clk_tck==0 must not yield a cpu rate");
    check("io_have survives a missing tick rate", r_no_tick.io_have,
          "io's readiness must not be gated on the CPU tick rate");
    check("read_bps survives a missing tick rate",
          r_no_tick.read_bps > 2040.0 && r_no_tick.read_bps < 2056.0,
          "read_bps = " + std::to_string(r_no_tick.read_bps));

    // Fixed, known jiffy values rather than deriving from the real
    // capture's utime/stime (which may legitimately be 0 on an idle
    // process, and p.utime - 1 would then underflow the unsigned counter
    // instead of going "backwards").
    ProcInfo base_back = p;
    base_back.utime = 1000;
    base_back.stime = 1000;
    ProcInfo q_cpu_back = q;
    q_cpu_back.utime = 900; // 900+900 < 1000+1000: counters "went backwards"
    q_cpu_back.stime = 900;
    ProcRates r_cpu_back = procinfo_rates(base_back, q_cpu_back);
    check("no cpu rate when counters go backwards", !r_cpu_back.have,
          "utime/stime going backwards must not yield a cpu rate");
    check("io_have survives cpu counters going backwards", r_cpu_back.io_have,
          "io's readiness must not be gated on the cpu jiffy counters");

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

    // Task 7 review IMPORTANT 4: a budget-truncated gather can leave
    // syms_total/jit_methods at zero (a full section skip) or a partial
    // undercount (a mid-loop bail) WITHOUT that being a genuine "no
    // symbols" fact. procinfo_names_verdict must state the uncertainty
    // rather than manufacture a confident verdict out of the truncation
    // itself — checked directly against the shared function (this is where
    // Task 5/7's review found the caveat belongs, not pane-local).
    ProcInfo truncated_zero = p;
    truncated_zero.budget_exceeded = true;
    truncated_zero.syms_total = 0;
    truncated_zero.jit_methods = 0;
    std::string vz = procinfo_names_verdict(truncated_zero);
    check("budget_exceeded verdict withholds 'no symbols'",
          vz.find("no symbols and no JIT map") == std::string::npos,
          "verdict '" + vz +
              "' asserted 'no symbols' from a budget-truncated zero");
    check("budget_exceeded verdict states the uncertainty",
          vz.find("undercount") != std::string::npos,
          "verdict '" + vz + "' does not say the counts may be undercounts");

    ProcInfo truncated_nonzero = p; // syms_total > 0 from the real capture
    truncated_nonzero.budget_exceeded = true;
    std::string vn = procinfo_names_verdict(truncated_nonzero);
    check("budget_exceeded caveats even a nonzero syms_total",
          vn.find("undercount") != std::string::npos,
          "verdict '" + vn +
              "' did not caveat a nonzero-but-truncated count");

    // The three branches below the budget caveat had NO coverage at all:
    // the p-fixture only ever exercises the "syms_total > 0, no JIT, no
    // anon-exec" path, so a mutation deleting any of the others was
    // invisible.
    {
        // 1. named == 0 -> the raw-addresses conclusion. This is the one
        //    branch that draws a CONCLUSION from an absence, so it is the
        //    one that has to be reachable only when the absence is real.
        ProcInfo none = p;
        none.syms_total = 0;
        none.jit_methods = 0;
        std::string v = procinfo_names_verdict(none);
        check("a genuinely symbol-free target does get the raw-addresses "
              "verdict",
              v.find("raw addresses") != std::string::npos,
              "verdict '" + v + "'");

        // 2. the JIT arm: count AND source, both of which the confident
        //    sentence is built from.
        ProcInfo jit = p;
        jit.jit_methods = 1402;
        jit.jit_source = "perf-map";
        std::string vj = procinfo_names_verdict(jit);
        check("the JIT arm states the method count",
              vj.find("1402") != std::string::npos, "verdict '" + vj + "'");
        check("the JIT arm names the map's source",
              vj.find("perf-map") != std::string::npos, "verdict '" + vj + "'");

        // 3. anon-exec WITHOUT a JIT map: executable memory no symtab
        //    covers, and no jitdump/perf-map to explain it -- the "names,
        //    except here" caveat. Deliberately paired with its negative:
        //    the same bytes WITH a JIT map are explained, and must not
        //    produce the caveat.
        ProcInfo anon = p;
        anon.jit_methods = 0;
        anon.jit_source.clear();
        anon.anon_exec_bytes = 12582912; // 12 MB
        std::string va = procinfo_names_verdict(anon);
        check("unexplained anonymous executable memory is called out",
              va.find("anonymous executable memory") != std::string::npos,
              "verdict '" + va + "'");
        check("the anon-exec caveat states its size in KB",
              va.find("12288") != std::string::npos, "verdict '" + va + "'");
        ProcInfo anon_jit = anon;
        anon_jit.jit_methods = 5;
        anon_jit.jit_source = "jitdump";
        check("anon-exec explained BY a JIT map is not called out again",
              procinfo_names_verdict(anon_jit).find(
                  "anonymous executable memory") == std::string::npos,
              "verdict '" + procinfo_names_verdict(anon_jit) + "'");
    }

    // 4. An unreadable /proc/<pid>/maps: every number this verdict is built
    //    from comes from that file, so zero means "never read", not "not
    //    there". This must WIN over both the budget caveat and the
    //    raw-addresses conclusion, which is why it is the first check in
    //    the function.
    {
        ProcInfo nomaps = p;
        nomaps.maps_readable = false;
        nomaps.syms_total = 0;
        nomaps.jit_methods = 0;
        std::string v = procinfo_names_verdict(nomaps);
        check("an unreadable maps withholds the raw-addresses conclusion",
              v.find("raw addresses") == std::string::npos,
              "verdict '" + v + "'");
        check("an unreadable maps names /proc/<pid>/maps as the reason",
              v.find("/maps could not be read") != std::string::npos,
              "verdict '" + v + "'");
        nomaps.budget_exceeded = true;
        check("the unreadable-maps reason DOMINATES the budget caveat",
              procinfo_names_verdict(nomaps).find("/maps could not be read") !=
                  std::string::npos,
              "verdict '" + procinfo_names_verdict(nomaps) + "'");
    }

    // The wire flag itself decodes (the fixtures carry it true), so a
    // decode that hardcoded false would fail here rather than silently
    // withholding every verdict in the app.
    check("maps_readable decodes from the wire", p.maps_readable,
          "the fixture emits code.maps_readable:true");

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

    // --- the utf-8 sanitization fixture (finding, gui-process-details Task
    // 4b) ---------------------------------------------------------------
    // comm/argv/exe/cwd/module path/the header's cmd are arbitrary KERNEL
    // bytes with no encoding guarantee. Before this fix, a single raw
    // invalid byte in any of them made nlohmann reject the WHOLE recording
    // (json::parse's is_discarded()), not just the field carrying it, so
    // load_recording never got far enough to populate ANY of ProcInfo --
    // the Process details pane could not render such a process at all.
    // This fixture's cwd carries the field AS THE PRODUCER NOW EMITS IT:
    // sanitized to U+FFFD (the replacement character) rather than the raw
    // invalid byte it replaced.
    ProcInfo u = load("procinfo_utf8.asmtrace");
    check("utf8-sanitized fixture parses", u.valid, u.parse_error);
    check("sanitized cwd carries U+FFFD, not the invalid byte it replaced",
          u.cwd.find("\xef\xbf\xbd") != std::string::npos,
          "cwd = " + u.cwd);

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

    // --- the runner: a pure state machine over an injected clock ------
    // Every timing rule here is a real hazard: without the debounce, arrowing
    // down a process table spawns one subprocess PER ROW; without the cache
    // key including start_ticks, a reused pid serves another process's card.
    {
        ProcInfoRunner run;
        run.asmspy_path = std::string(ASMTEST_FIXTURE_DIR) +
                          "/fake_asmspy_info.sh";
        // fake_asmspy_info.sh always echoes procinfo_full.asmtrace regardless
        // of the pid argument, and the pid-mismatch guard below discards any
        // parse whose identity.pid disagrees with the pid we asked about — so
        // the "stable" target this test settles on and probes has to BE that
        // fixture's own real pid, not an arbitrary constant, or every parse
        // would be discarded as a mismatch and nothing would ever arrive.
        const long real_pid = load("procinfo_full.asmtrace").pid;

        // Selecting arms the debounce; it must NOT spawn on the same tick.
        procinfo_tick(run, 100, 0.0);
        check("no spawn on select", run.spawns == 0,
              "spawned immediately — arrowing a table would fork per row");
        procinfo_tick(run, 100, 0.10);
        check("no spawn before 250ms", run.spawns == 0, "debounce too short");

        // Moving the selection before expiry re-arms rather than spawning.
        procinfo_tick(run, real_pid, 0.20);
        procinfo_tick(run, real_pid, 0.40);
        check("re-armed by a new selection", run.spawns == 0,
              "a moving selection must re-arm, not spawn");

        // Stable past 250ms -> exactly one spawn.
        procinfo_tick(run, real_pid, 0.46);
        check("spawned after the debounce", run.spawns == 1,
              "no spawn after 250ms of a stable selection");
        procinfo_tick(run, real_pid, 0.50);
        check("no double spawn", run.spawns == 1, "spawned twice");

        // Drain the child, then the result is current. `now_s` here is
        // simulated and advances instantly; the fork/exec/read path underneath
        // is REAL and needs actual wall-clock ticks to be scheduled and run.
        // With no delay at all, this loop can spin through all 200 simulated
        // iterations (2s of simulated time) in far under a millisecond of
        // real time — faster than the kernel can even schedule the forked
        // child — so every read() sees EAGAIN forever and the loop times out
        // having never seen a byte. This one-line real sleep is test
        // scaffolding only: procinfo_tick itself still never sleeps or blocks.
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            procinfo_tick(run, real_pid, 0.50 + 0.01 * i);
            ::usleep(1000);
        }
        check("result arrived", procinfo_current(run).valid,
              procinfo_current(run).parse_error);

        // A cached pid re-renders with NO new spawn. The dwell on pid=100
        // stays UNDER the 250ms debounce on purpose: pid=100 was never
        // successfully probed (we moved off it before its own debounce fired,
        // back at the top of this test), so if this dwell crossed debounce it
        // would trigger its OWN legitimate first-time probe attempt — a real,
        // separate spawn this section isn't testing for. The one spawn this
        // cap does tolerate is the OTHER legitimate case, spelled out in
        // procinfo_tick's switch block: "a cache hit renders immediately; a
        // refresh may still follow" — real_pid's own last success is long
        // enough ago by 3.90 that its refresh timer is due.
        const int after = run.spawns;
        procinfo_tick(run, 100, 3.0);
        procinfo_tick(run, 100, 3.10);
        procinfo_tick(run, real_pid, 3.60);
        procinfo_tick(run, real_pid, 3.90);
        check("cache hit is instant", procinfo_current(run).valid,
              "a cached pid did not render");
        check("cache avoids a respawn", run.spawns <= after + 1,
              "a cached selection respawned");

        // Settle any straggling in-flight child from the cache-hit section
        // above (a refresh may legitimately have just spawned one) before
        // testing the visible gate in isolation — otherwise a residual
        // idle==false could mask THIS gate regardless of whether `visible`
        // is even checked, and the mutation this guards (dropping `visible`
        // from the spawn condition) would silently survive.
        double settle_t = 3.90;
        for (int i = 0; i < 300 && run.child_pid != 0; i++) {
            settle_t += 0.01;
            procinfo_tick(run, real_pid, settle_t);
            ::usleep(1000);
        }
        check("settle: no child left in flight before the visible-gate test",
              run.child_pid == 0,
              "child_pid=" + std::to_string(run.child_pid) +
                  " still in flight — the visible-gate check below cannot "
                  "isolate its own condition");

        // The refresh timer only fires while visible.
        run.visible = false;
        const int before_hidden = run.spawns;
        procinfo_tick(run, real_pid, 10.0);
        check("hidden pane does not poll", run.spawns == before_hidden,
              "a hidden pane kept spawning");
    }

    // --- 2026-08-06 final review, finding 8: `host` — the PROBING BINARY'S own
    // perf verdict, not the machine's and not this process's.
    //
    // perf permission travels with FILE CAPABILITIES, which land on execve. The
    // setup ladder's rung 2 is `setcap cap_perfmon+ep` on the asmspy inode, so
    // after it `asmspy --sample` works while the desktop's own perf_event_open
    // still fails — and the desktop used to grey `sample` on its own failure,
    // advising the operator to grant a capability they had already granted. The
    // verdict that may block therefore has to come over this wire.
    {
        ProcInfo h = load("procinfo_host_perf.asmtrace");
        check("host/parsed", h.valid, h.parse_error);
        check("host/probed", h.host_probed,
              "a recording carrying host.perf_ok must decode as PROBED — "
              "without this the desktop falls back to its own process's "
              "syscall, which measures the wrong binary");
        check("host/verdict", !h.host_perf_ok,
              "the fixture's measured verdict is a refusal and must survive "
              "the decode");
        check("host/why-carried",
              h.host_perf_why.find("asmspy binary itself") != std::string::npos,
              "the binary's own reason must cross verbatim — it is the text "
              "the mode refusal embeds, and the one that names the remedy: " +
                  h.host_perf_why);
        // ABSENCE IS NOT REFUSAL. The identical fixture WITHOUT the key (they
        // differ in nothing else) must leave host_probed false, so an older
        // asmspy — or any producer that never emitted it — blocks nothing.
        ProcInfo n = load("procinfo_full.asmtrace");
        check("host/absent-is-not-probed", n.valid && !n.host_probed,
              "a producer that emits no host object has no opinion; treating "
              "its silence as a refusal would grey a mode nobody measured");
    }

    // --- and the runner LATCHES it, because it is a fact about the binary
    // rather than about the target: `shown` is blanked on every selection
    // change, and a verdict that blinked out with it would make the mode gate
    // flicker. It IS dropped when asmspy_path changes — a different inode has
    // different file capabilities, which is the whole mechanism.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_host_perf.sh";
        const long fpid = load("procinfo_host_perf.asmtrace").pid;
        procinfo_tick(run, fpid, 0.0);
        for (int i = 0; i < 200 && !run.host_perf_probed; i++) {
            procinfo_tick(run, fpid, 0.30 + 0.01 * i);
            ::usleep(1000);
        }
        check("host/runner-latched", run.host_perf_probed && !run.host_perf_ok,
              "the runner must keep the binary's verdict: " +
                  procinfo_status(run));
        // A selection change blanks `shown`; the verdict must survive it.
        procinfo_tick(run, fpid + 1, 5.0);
        check("host/survives-a-selection-change",
              run.host_perf_probed && !run.host_perf_ok &&
                  !procinfo_current(run).valid,
              "the target changed, the BINARY did not — a verdict that blinks "
              "out with the snapshot makes the mode gate flicker for no "
              "reason the operator can see");
        // A path change is a different binary: back to "not asked".
        run.asmspy_path = "/some/other/asmspy";
        procinfo_tick(run, fpid + 1, 6.0);
        check("host/dropped-on-a-path-change", !run.host_perf_probed,
              "CAP_PERFMON is granted per inode, so a verdict about the old "
              "binary says nothing about the new one");
    }

    // A child that never exits is killed and SAYS it timed out.
    {
        ProcInfoRunner run;
        run.asmspy_path = "/bin/sleep"; // never emits, never exits in time
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // spawn
        procinfo_tick(run, 100, 3.00); // past the 2s deadline
        check("timeout is stated", procinfo_status(run).find("timed out") !=
                                       std::string::npos,
              "a hung probe must say so: " + procinfo_status(run));
    }

    // --- the pid-mismatch guard: a snapshot for a DIFFERENT pid than the one
    // probed must be discarded, never rendered as if it belonged to the pid
    // actually selected. fake_asmspy_mismatch.sh always reports pid 42 no
    // matter what --info argument it is given (mirroring how a stale child's
    // leftover output could, in principle, still be sitting in the pipe under
    // a pid that no longer matches what the runner asked about).
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_mismatch.sh";
        procinfo_tick(run, 999, 0.0);
        procinfo_tick(run, 999, 0.30); // spawn

        bool ever_valid = false;
        for (int i = 0; i < 200; i++) {
            procinfo_tick(run, 999, 0.30 + 0.01 * i);
            ::usleep(1000);
            if (procinfo_current(run).valid) {
                ever_valid = true;
                break;
            }
            if (procinfo_status(run).find("ignored a snapshot") !=
                std::string::npos)
                break;
        }
        check("a pid-mismatched snapshot is never shown", !ever_valid,
              "a snapshot for pid 42 was rendered while viewing pid 999");
        check("the mismatch names both pids in the status",
              procinfo_status(run).find("ignored a snapshot") !=
                  std::string::npos,
              "status did not name the mismatch: " + procinfo_status(run));
    }

    // --- the cache key is (pid, start_ticks), not pid alone: a pid REUSE
    // must land in its OWN cache slot rather than overwrite the previous
    // process's entry. cache_put() has internal linkage (it is not part of
    // the public API), so this drives it through two REAL successful parses
    // of the same pid at two different start_ticks and inspects the runner's
    // public `cache` vector directly.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // spawn
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            procinfo_tick(run, 100, 0.30 + 0.01 * i);
            ::usleep(1000);
        }
        check("pid-reuse setup: the first process parsed and cached",
              procinfo_current(run).valid && run.cache.size() == 1,
              "first probe did not land in the cache — valid=" +
                  std::to_string(procinfo_current(run).valid) +
                  " cache.size()=" + std::to_string(run.cache.size()));
        const uint64_t first_start_ticks = procinfo_current(run).start_ticks;

        // The SAME pid is probed again, but the process behind it is now a
        // DIFFERENT one (a different start_ticks) — a pid reuse. Poking
        // last_ok_at directly forces the refresh due on the next tick without
        // waiting out a real refresh_s of simulated time; that timer is
        // already covered elsewhere, this test is only about the cache key.
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_b.sh";
        run.last_ok_at = -1;
        for (int i = 0;
             i < 200 && procinfo_current(run).start_ticks == first_start_ticks;
             i++) {
            procinfo_tick(run, 100, 1.0 + 0.01 * i);
            ::usleep(1000);
        }
        check("pid reuse: the shown snapshot moved to the new process",
              procinfo_current(run).start_ticks != first_start_ticks,
              "start_ticks stayed " + std::to_string(first_start_ticks) +
                  " after a pid reuse");
        check("pid reuse: a SECOND, distinct cache entry — not an overwrite",
              run.cache.size() == 2,
              "cache.size()=" + std::to_string(run.cache.size()) +
                  " — a key that ignores start_ticks collapses a reused pid "
                  "into the previous process's slot");
        const uint64_t second_start_ticks = procinfo_current(run).start_ticks;

        // I1 (behaviour, not storage): the cache now holds BOTH the dead
        // process's entry (first_start_ticks) and the live one
        // (second_start_ticks). Switching away and back is a cache HIT — no
        // time is given for a fresh probe to complete — so whichever entry
        // the lookup resolves to IS what the pane renders. A lookup that
        // scans front-to-back and stops at the first pid match would always
        // resolve to the OLDER entry (inserted first), rendering the dead
        // process's card under a "cached" label: exactly the bug the
        // compound key exists to prevent, restored by the lookup.
        procinfo_tick(run, 777, 5.0);  // switch away (a miss; cache is pid=100-only)
        procinfo_tick(run, 100, 5.01); // and back — a cache hit
        check("cache lookup on re-selection resolves to the NEWEST matching "
              "entry, not the oldest",
              procinfo_current(run).valid &&
                  procinfo_current(run).start_ticks == second_start_ticks,
              "start_ticks=" + std::to_string(procinfo_current(run).start_ticks) +
                  " — expected the live process's " +
                  std::to_string(second_start_ticks) + ", not the dead " +
                  std::to_string(first_start_ticks));
    }

    // --- the in-flight child is killed on EVERY selection change, not only
    // at the deadline. fake_asmspy_hang.sh genuinely sleeps regardless of its
    // arguments (invoking /bin/sleep directly with the runner's fixed argv
    // does NOT hang — GNU coreutils rejects "--info" and exits in about a
    // millisecond, too fast to tell "killed by the switch" apart from
    // "exited on its own"), so this proves the switch itself reaped it.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // past debounce -> spawn
        check("child spawned for the switch-kill test", run.child_pid != 0,
              "no child was spawned to test the switch-kill against");
        const int old_child_pid = run.child_pid;

        procinfo_tick(run, 200, 0.31); // switch away, well before any deadline
        check("in-flight child is reaped synchronously on selection change",
              run.child_pid == 0 && run.child_fd == -1,
              "child_pid=" + std::to_string(run.child_pid) +
                  " child_fd=" + std::to_string(run.child_fd) +
                  " right after switching selection — the old probe was not "
                  "reaped");
        int rc = ::kill(old_child_pid, 0);
        check("the killed child is actually gone at the OS level",
              rc != 0 && errno == ESRCH,
              "kill(old_child_pid, 0) = " + std::to_string(rc) +
                  " errno=" + std::to_string(errno) +
                  " — the process (or its zombie) is still around");
    }

    // --- C1: a brand-new selection must not wait behind a DIFFERENT pid's
    // refresh clock. last_ok_at is per-target now (reset to -1 on every
    // selection change); before that fix, a fresh pid's own first probe
    // waited up to refresh_s (2s) rather than just the 250ms debounce,
    // because `due` was measured against whichever OTHER pid last
    // succeeded — a fork+exec that should follow arrowing to a new row
    // within 250ms instead silently waited up to 2 full seconds.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        procinfo_tick(run, 100, 0.0);
        double t = 0.30;
        procinfo_tick(run, 100, t); // spawn
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t = 0.30 + 0.01 * i;
            procinfo_tick(run, 100, t);
            ::usleep(1000);
        }
        check("C1 setup: pid 100 succeeded", procinfo_current(run).valid,
              "setup for the C1 test never got a successful read");

        const int after = run.spawns;
        // Switch to a NEVER-before-seen pid shortly (well under refresh_s)
        // after the pid above succeeded, and give it only its own debounce.
        procinfo_tick(run, 555, t + 0.10);
        procinfo_tick(run, 555, t + 0.40); // +0.30s -> past the 250ms debounce
        check("a fresh pid is probed on ITS OWN debounce, not blocked behind "
              "another pid's refresh timer",
              run.spawns == after + 1,
              "spawns=" + std::to_string(run.spawns) + " (expected " +
                  std::to_string(after + 1) +
                  ") — a fresh selection waited on someone else's last_ok_at");
    }

    // --- I2: a cache HIT's freshness must reflect THAT ENTRY's own read
    // time, not the runner's global last_ok_at — which, right after a
    // selection change lands on a cache hit, is whatever OTHER pid most
    // recently succeeded. Before the fix, a 5-second-stale cached card could
    // report "read 0.0s ago" simply because some unrelated pid happened to
    // have just succeeded — "measured zero" and "not yet measurable" are
    // different claims, and this was the model saying the wrong one.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        procinfo_tick(run, 100, 0.0);
        double t = 0.30;
        procinfo_tick(run, 100, t); // spawn
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t = 0.30 + 0.01 * i;
            procinfo_tick(run, 100, t);
            ::usleep(1000);
        }
        check("I2 setup: pid 100 cached", procinfo_current(run).valid,
              "setup for the I2 test never got a successful read");
        const double cached_at = t; // pid 100's OWN read time

        // A DIFFERENT pid succeeds much later, moving the GLOBAL last_ok_at
        // far forward — the exact scenario the bug conflated.
        const long real_pid = load("procinfo_full.asmtrace").pid;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_info.sh";
        double t2 = cached_at + 5.30;
        procinfo_tick(run, real_pid, cached_at + 5.0);
        procinfo_tick(run, real_pid, t2); // spawn
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t2 = cached_at + 5.30 + 0.01 * i;
            procinfo_tick(run, real_pid, t2);
            ::usleep(1000);
        }
        check("I2 setup: a different pid succeeded later",
              procinfo_current(run).valid,
              "setup for the I2 test's second success never landed");

        // Switch BACK to the cached pid 100 immediately — a cache hit; no
        // time is given for a fresh probe of it to complete.
        const double back_at = t2 + 0.05;
        procinfo_tick(run, 100, back_at);
        const double expected_age = back_at - cached_at;
        const double bug_age = back_at - t2; // what the OLD code would show

        const std::string st = procinfo_status(run);
        const double shown_age =
            st.rfind("read ", 0) == 0 ? std::atof(st.c_str() + 5) : -1.0;
        check("cache-hit freshness reflects the ENTRY's own read time, not a "
              "different pid's",
              shown_age > expected_age - 0.5 && shown_age < expected_age + 0.5,
              "status='" + st + "' shown_age=" + std::to_string(shown_age) +
                  " expected~" + std::to_string(expected_age) +
                  " (the bug would show ~" + std::to_string(bug_age) + ")");
    }

    // --- a pid that VANISHED reports the spec's named outcome ------------
    // `asmspy --info` writes its "no such process" line to stderr (which
    // this runner sends to /dev/null) and exits 3 —
    // ASMSPY_INFO_EXIT_NO_SUCH_PID, a code of its own precisely so this arm
    // can tell it apart. Reported as "asmspy produced no output (exit 1)"
    // before, which blames the probe for a race that is the ROUTINE
    // consequence of browsing a live process list: the target exits between
    // the Processes table listing it and the debounce firing.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_vanished.sh";
        procinfo_tick(run, 4242, 0.0);
        procinfo_tick(run, 4242, 0.30); // spawn
        for (int i = 0; i < 200 && run.child_pid != 0; i++) {
            procinfo_tick(run, 4242, 0.30 + 0.01 * i);
            ::usleep(1000);
        }
        const std::string st = procinfo_status(run);
        check("a vanished pid is reported as having exited, naming the pid",
              st.find("pid 4242 exited") != std::string::npos, "status: " + st);
        check("a vanished pid never blames asmspy for producing no output",
              st.find("produced no output") == std::string::npos,
              "status: " + st);
    }

    // --- a spawn that never happened is still a FAILURE -------------------
    // spawn() returns false in three places — nothing resolved to run,
    // pipe2 failed, fork failed — and every one of them leaves a stated
    // reason and no snapshot, i.e. a failed probe. It was not COUNTED as
    // one, so the backoff never grew and the runner retried at FRAME RATE
    // for as long as the condition lasted: fork() failing under EAGAIN
    // answered with a fork attempt every frame. Note that an asmspy_path
    // pointing at a nonexistent FILE does not reach this path at all — the
    // fork succeeds and the CHILD's execvp fails, which the empty-buffer
    // arm already handles; the first return is the only one reachable
    // without breaking the process's fd or pid limits, so it is the one
    // driven here: an empty $PATH and a cwd with no ./build/asmspy make
    // resolve_asmspy_path() genuinely find nothing. Both are restored
    // immediately, before any other check runs.
    {
        char cwd[4096] = "";
        const char *pathenv = ::getenv("PATH");
        const std::string saved_path = pathenv ? pathenv : "";
        const bool have_cwd = ::getcwd(cwd, sizeof cwd) != nullptr;
        ::setenv("PATH", "", 1);
        const bool moved = ::chdir("/") == 0;

        ProcInfoRunner run; // asmspy_path stays "" -> resolve, and find none
        procinfo_tick(run, 777, 0.0);
        double t = 0.30;
        for (int i = 0; i < 300; i++) {
            procinfo_tick(run, 777, t);
            t += 0.05;
        }
        const std::string st = procinfo_status(run);
        const int fails = run.fail_count;
        const int spawns = run.spawns;

        ::setenv("PATH", saved_path.c_str(), 1);
        if (have_cwd && moved && ::chdir(cwd) != 0)
            check("restored the cwd after the no-asmspy test", false,
                  "chdir back to the test's own cwd failed");

        check("setup: resolve_asmspy_path found nothing to run",
              spawns == 0 && st.find("no asmspy found") != std::string::npos,
              "spawns=" + std::to_string(spawns) + " status: " + st);
        // 300 frames of 0.05s = ~15s. Uncounted, EVERY one of those frames
        // re-enters spawn(); counted, the 0.5/1/2/4/8/10s backoff admits a
        // handful. fail_count is the observable either way, since spawn()
        // never got far enough to increment `spawns`.
        check("a spawn that could not even start still backs off",
              fails > 0 && fails <= 12,
              "fail_count=" + std::to_string(fails) +
                  " over ~15s of a persistent, un-spawnable condition — "
                  "uncounted, this is one retry per frame (300)");
    }

    // --- C2: a target that fails EVERY time must back off, not respawn at
    // frame rate forever. fake_asmspy_mismatch.sh always "fails" (a pid
    // mismatch) from this runner's perspective, so every attempt is a
    // failure and, before the fix, `due` (gated only on the success clock)
    // stayed true forever — a fork+exec every single idle frame.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_mismatch.sh";
        procinfo_tick(run, 999, 0.0);
        double t = 0.30;
        procinfo_tick(run, 999, t); // first spawn attempt
        for (int i = 0; i < 300; i++) {
            t += 0.05;
            procinfo_tick(run, 999, t);
            ::usleep(1000);
        }
        // 300 * 0.05s = 15s of simulated failures. Without backoff, every
        // idle tick refires (measured elsewhere: ~281 spawns in 300 frames
        // on this exact scenario); with backoff (0.5s/1s/2s/4s/8s/10s...,
        // capped) at most a handful of retries fit in 15s.
        check("repeated failures back off instead of respawning every frame",
              run.spawns <= 8,
              "spawns=" + std::to_string(run.spawns) +
                  " over ~15s of simulated failures — expected a handful, "
                  "not one per frame");
    }

    // --- I5: r.buf must not grow without bound against a flooding or
    // wrong-binary child (asmspy_path is user-settable in Connect). Pin
    // max_buf_bytes tiny so the cap trips on the very first read rather than
    // needing a real multi-MB payload to prove the mechanism exists.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_info.sh";
        run.max_buf_bytes = 32; // the real fixture is several KB
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // spawn
        for (int i = 0; i < 200 && run.child_pid != 0; i++) {
            procinfo_tick(run, 100, 0.30 + 0.01 * i);
            ::usleep(1000);
        }
        check("over-cap output never becomes a shown snapshot",
              !procinfo_current(run).valid,
              "a snapshot arrived despite exceeding max_buf_bytes");
        check("the over-cap status names the cap, not a generic parse error",
              procinfo_status(run).find("without finishing") !=
                  std::string::npos,
              "status: " + procinfo_status(run));
        check("the over-cap child is killed and reaped, not left running",
              run.child_pid == 0 && run.child_fd == -1,
              "child_pid=" + std::to_string(run.child_pid) +
                  " child_fd=" + std::to_string(run.child_fd));
        // clear() alone does not release a string's allocation -- a single
        // flood would otherwise permanently pin max_buf_bytes of heap even
        // after the child is long gone. capacity() is the observable proxy:
        // it should be nowhere near what a 32-byte-triggered flood grew to.
        check("the buffer's capacity is released after an over-cap trip, "
              "not just its logical size",
              // 1024, not 4096. The threshold must sit far below the
              // FIXTURE's size, not just below it: procinfo_full.asmtrace
              // (what the fake asmspy echoes) is ~4 KB, so the buggy
              // capacity lands within a few dozen bytes of 4096 and which
              // side it falls on is decided by the fixture, not by the bug.
              // Measured all three ways: at 4081 bytes (the size before this
              // wave) the bug PASSED a `< 4096` check -- a test that could
              // not fail against the very thing it names; at 4102 (the same
              // fixture hand-patched with one more key) it failed by six
              // bytes; regenerated, it is back under. The cap under test
              // here is 32 bytes and the drain path SWAPS the string rather
              // than clear()ing it, so a correct runner leaves a capacity in
              // the tens of bytes -- 1024 states that property instead of
              // grazing the payload size.
              run.buf.capacity() < 1024,
              "buf.capacity()=" + std::to_string(run.buf.capacity()) +
                  " -- clear() without releasing the allocation pins it");
    }

    // --- true LRU: touching an existing cache key moves it to the back,
    // not just updating it in place. cache_put has internal linkage, so
    // this drives it through TWO real successful parses of the SAME key
    // (a re-probe of an already-cached pid) and inspects the runner's
    // public `cache` vector's ORDER directly -- no eviction (kCacheCap
    // entries) needed to observe reordering.
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        procinfo_tick(run, 100, 0.0);
        double t = 0.30;
        procinfo_tick(run, 100, t);
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t = 0.30 + 0.01 * i;
            procinfo_tick(run, 100, t);
            ::usleep(1000);
        }
        check("LRU setup: pid 100 cached first", run.cache.size() == 1,
              "cache.size()=" + std::to_string(run.cache.size()));

        const long real_pid = load("procinfo_full.asmtrace").pid;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_info.sh";
        procinfo_tick(run, real_pid, t + 0.10);
        t += 0.40;
        procinfo_tick(run, real_pid, t);
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t += 0.01;
            procinfo_tick(run, real_pid, t);
            ::usleep(1000);
        }
        check("LRU setup: a second, distinct entry cached",
              run.cache.size() == 2,
              "cache.size()=" + std::to_string(run.cache.size()));
        check("LRU setup: insertion order is oldest-first",
              run.cache[0].first.first == 100 &&
                  run.cache[1].first.first == real_pid,
              "cache[0].pid=" + std::to_string(run.cache[0].first.first) +
                  " cache[1].pid=" + std::to_string(run.cache[1].first.first));

        // Re-probe pid 100 (the SAME key: pid AND start_ticks) again — true
        // LRU must move it to the BACK, not merely update it where it sits.
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        procinfo_tick(run, 100, t + 0.10);
        t += 0.40;
        run.last_ok_at = -1; // force due without waiting refresh_s
        procinfo_tick(run, 100, t);
        for (int i = 0; i < 200 && run.cache.size() == 2 &&
                        run.cache[1].first.first == real_pid;
             i++) {
            t += 0.01;
            procinfo_tick(run, 100, t);
            ::usleep(1000);
        }
        check("LRU touch: still exactly two entries (updated, not appended)",
              run.cache.size() == 2,
              "cache.size()=" + std::to_string(run.cache.size()));
        check("LRU touch: the re-probed key moved to the BACK (true LRU, "
              "not FIFO)",
              run.cache[0].first.first == real_pid &&
                  run.cache[1].first.first == 100,
              "cache[0].pid=" + std::to_string(run.cache[0].first.first) +
                  " cache[1].pid=" + std::to_string(run.cache[1].first.first) +
                  " — expected the touched pid=100 entry at the back");
    }

    // --- cache eviction at kCacheCap: the 33rd distinct entry must evict
    // the OLDEST (front). Nothing else in this suite ever creates a 33rd
    // entry, so the "Bounded LRU" claim was otherwise unverified.
    // fake_asmspy_cache_fill.sh keeps pid fixed at 100 and reads its
    // start_ticks from FAKE_START_TICKS, synthesizing 33 distinct keys
    // without 33 fixture files.
    {
        ProcInfoRunner run;
        run.asmspy_path = std::string(ASMTEST_FIXTURE_DIR) +
                          "/fake_asmspy_cache_fill.sh";
        double t = 0.0;
        procinfo_tick(run, 100, t);
        for (int n = 1; n <= 33; n++) {
            ::setenv("FAKE_START_TICKS", std::to_string(n).c_str(), 1);
            t += 0.30;
            run.last_ok_at = -1;    // force due
            run.next_retry_at = -1; // and bypass any backoff, every cycle
            procinfo_tick(run, 100, t);
            const uint64_t before = procinfo_current(run).start_ticks;
            for (int i = 0;
                 i < 200 && procinfo_current(run).start_ticks == before; i++) {
                t += 0.01;
                procinfo_tick(run, 100, t);
                ::usleep(1000);
            }
        }
        ::unsetenv("FAKE_START_TICKS");

        check("cache stays bounded at kCacheCap after 33 distinct entries",
              run.cache.size() == 32,
              "cache.size()=" + std::to_string(run.cache.size()));
        bool has_first = false, has_last = false;
        for (auto &e : run.cache) {
            if (e.first.second == 1)
                has_first = true; // the very FIRST (oldest) entry
            if (e.first.second == 33)
                has_last = true; // the 33rd (newest) entry
        }
        check("the oldest entry (start_ticks=1) was evicted", !has_first,
              "start_ticks=1 is still present — eviction did not drop the "
              "front");
        check("the newest entry (start_ticks=33) is present", has_last,
              "start_ticks=33 is missing — the 33rd probe did not land in "
              "the cache");
    }

    // --- a path fix in Connect must not still wait out the OLD path's
    // backoff: an operator who corrects asmspy_path after a run of
    // failures is saying "try again now."
    {
        ProcInfoRunner run;
        // fake_asmspy_mismatch.sh always reports pid 42, so selecting pid
        // 100 (NOT 42) makes every attempt a mismatch failure — and pid 100
        // is deliberately never changed below, so the ONLY way the backoff
        // could reset is the path-change detection itself, not the
        // pre-existing (and separately tested) selection-change reset.
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_mismatch.sh";
        double t = 0.0;
        procinfo_tick(run, 100, t);
        t = 0.30;
        procinfo_tick(run, 100, t); // first failure
        for (int i = 0; i < 100 && run.fail_count < 3; i++) {
            t += 0.05;
            procinfo_tick(run, 100, t); // SAME pid=100 throughout
            ::usleep(1000);
        }
        check("path-fix setup: a real run of failures accumulated",
              run.fail_count >= 3,
              "fail_count=" + std::to_string(run.fail_count) +
                  " — setup never accumulated enough failures");
        const double backoff_next_retry = run.next_retry_at;
        check("path-fix setup: a real backoff is pending",
              backoff_next_retry > t,
              "next_retry_at=" + std::to_string(backoff_next_retry) +
                  " is not in the future at t=" + std::to_string(t));

        // Fix the path WITHOUT touching the selection (still pid 100) — this
        // must reset the backoff on its own; fake_asmspy_pidreuse_a.sh
        // reports pid 100 for real, so a probe now succeeds instead of
        // mismatching.
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_pidreuse_a.sh";
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++) {
            t += 0.01;
            procinfo_tick(run, 100, t); // still pid 100, no switch
            ::usleep(1000);
        }
        check("a path fix in Connect resets the backoff instead of waiting "
              "out the old one",
              procinfo_current(run).valid,
              "no successful read arrived promptly after the path fix — "
              "status: " + procinfo_status(run));
    }

    // --- reap() must ALSO release r.buf's capacity, not just its logical
    // size — the drain-completion site's own release has its own test
    // above; this is the OTHER call site (a selection change away from a
    // partially-buffered child, or destruction).
    {
        ProcInfoRunner run;
        run.asmspy_path =
            std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // spawn; genuinely hangs
        check("reap-capacity setup: a child is in flight", run.child_pid != 0,
              "no child was spawned to test reap()'s buffer release");
        run.buf.assign(8192, 'x'); // simulate a partially-buffered read
        check("reap-capacity setup: the buffer actually grew",
              run.buf.capacity() >= 8192,
              "buf.capacity()=" + std::to_string(run.buf.capacity()));
        procinfo_tick(run, 200, 0.31); // switch away -> reap()
        check("reap() releases r.buf's capacity, not just its logical size",
              run.buf.capacity() < 4096,
              "buf.capacity()=" + std::to_string(run.buf.capacity()) +
                  " — clear() without releasing the allocation pins it");
    }

    // --- I3: waitpid must retry across EINTR, or a zombie is left behind
    // permanently. Installs a real SIGALRM handler WITHOUT SA_RESTART and
    // fires it aggressively across many real kill+reap cycles, so any
    // blocking ::waitpid in this file has a genuine chance of being
    // interrupted mid-syscall — exactly what a GUI process's own signal
    // plumbing elsewhere (this app's own SIGALRM-based teardown included)
    // can do to it for real.
    //
    // The reap TARGET matters: draining to EOF before waitpid (the normal
    // completion path) means the child is usually ALREADY a zombie by the
    // time waitpid is called, so there is barely any window to interrupt.
    // SIGKILL-then-waitpid is different — the kernel needs real (if brief)
    // time after the signal to actually tear the process down — so this
    // drives reap()'s kill+wait specifically, by spawning a GENUINELY
    // hanging child (fake_asmspy_hang.sh) and switching selection away
    // before it could ever finish on its own, every single cycle.
    {
        struct sigaction sa {};
        sa.sa_handler = alarm_noop;
        sa.sa_flags = 0; // deliberately NOT SA_RESTART
        sigemptyset(&sa.sa_mask);
        struct sigaction old_sa {};
        ::sigaction(SIGALRM, &sa, &old_sa);

        itimerval it{};
        it.it_interval.tv_usec = 200; // ~5 kHz — aggressive on purpose
        it.it_value.tv_usec = 200;
        itimerval old_it{};
        ::setitimer(ITIMER_REAL, &it, &old_it);

        std::vector<int> spawned;
        {
            // (a) reap()'s kill+wait, via rapid selection switches away from
            // a still-hanging child.
            ProcInfoRunner run;
            run.asmspy_path =
                std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
            double t = 0.0;
            long pid = 100;
            procinfo_tick(run, pid, t);
            for (int cycle = 0; cycle < 200; cycle++) {
                t += 0.30; // past debounce -> spawn a real, hanging child
                procinfo_tick(run, pid, t);
                if (run.child_pid != 0)
                    spawned.push_back(run.child_pid);
                // Alternate the selected pid every cycle: switching away
                // from a still-hanging child is what calls reap()'s
                // kill+waitpid on a process that is DEFINITELY still alive.
                pid = (pid == 100) ? 200 : 100;
                t += 0.001;
                procinfo_tick(run, pid, t);
            }
        } // ~ProcInfoRunner's own reap() also runs under this pressure here
        {
            // (b) the DRAIN block's OWN kill+wait (a different call site
            // than reap()'s), via the deadline firing while the selection
            // stays put — no switch involved, so reap() is never entered.
            //
            // Every timeout here is a FAILURE (C2), which backs off the next
            // spawn attempt (1s/2s/4s/8s/10s, capped) — left alone, that
            // throttles this loop from 200 real exercises of this call site
            // down to about 8 over its ~64s of simulated time (confirmed by
            // review measurement), silently gutting the pressure this test
            // exists to apply. Resetting next_retry_at before each cycle's
            // spawn keeps this loop's 200 a real count, not a nominal one —
            // the backoff timer itself already has its own dedicated,
            // unpressured test elsewhere.
            ProcInfoRunner run2;
            run2.asmspy_path =
                std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
            run2.deadline_s = 0.01; // fire on the very next tick after spawn
            double t = 0.0;
            procinfo_tick(run2, 100, t);
            for (int cycle = 0; cycle < 200; cycle++) {
                run2.next_retry_at = -1; // bypass C2's backoff for this cycle
                t += 0.30; // past debounce -> spawn
                procinfo_tick(run2, 100, t);
                if (run2.child_pid != 0)
                    spawned.push_back(run2.child_pid);
                t += 0.02; // now overdue: the SAME tick's drain block kills
                procinfo_tick(run2, 100, t); // and waits, still selected
            }
        }

        ::setitimer(ITIMER_REAL, &old_it, nullptr);
        ::sigaction(SIGALRM, &old_sa, nullptr);

        check("SIGALRM pressure actually applied (sanity)", g_alarm_hits > 0,
              "no SIGALRM fired — this test did not actually apply pressure");
        int leaked = 0;
        for (int pid : spawned)
            if (::kill(pid, 0) == 0)
                leaked++;
        check("no zombies survive waitpid under EINTR pressure", leaked == 0,
              std::to_string(leaked) + "/" + std::to_string(spawned.size()) +
                  " spawned children are still present after the run");
    }

    // --- timing: procinfo_tick must never block the frame loop, across
    // every adversarial path that can complete/abandon a child. The round-3
    // review's sharpest finding: nothing in this suite measured tick
    // duration at all, so "non-blocking" — the runner's whole reason for
    // existing — was asserted in comments and nowhere else. It also found
    // that the drain-completion site's waitpid, left unkilled on eof/ioerr,
    // could block a tick for a still-alive child's entire remaining life
    // (measured at 4000.4ms against a `sleep 4` fixture) — this is the
    // direct regression test for that fix (procinfo.cpp's kill is now
    // unconditional before waitpid_retry there).
    {
        // Generous on purpose: real syscalls under a loaded shared host can
        // jitter a few ms, but the failure mode this guards is measured in
        // SECONDS (4000ms), three orders of magnitude above this bound.
        constexpr double kTickBoundMs = 50.0;
        auto time_tick = [](ProcInfoRunner &r, long pid, double now_s) {
            auto t0 = std::chrono::steady_clock::now();
            procinfo_tick(r, pid, now_s);
            auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(t1 - t0).count();
        };

        // (a) a hung child hitting ITS OWN deadline — the drain-completion
        // block's kill+wait call site.
        {
            ProcInfoRunner run;
            run.asmspy_path =
                std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
            run.deadline_s = 0.01;
            double t = 0.0;
            procinfo_tick(run, 100, t);
            t += 0.30;
            procinfo_tick(run, 100, t); // spawn
            t += 0.02;                 // now overdue
            double ms = time_tick(run, 100, t); // the kill+wait tick itself
            check("hung-child deadline tick stays fast", ms < kTickBoundMs,
                  "took " + std::to_string(ms) + "ms");
        }

        // (b) a selection change away from an in-flight, genuinely hanging
        // child — reap()'s kill+wait call site.
        {
            ProcInfoRunner run;
            run.asmspy_path =
                std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_hang.sh";
            double t = 0.0;
            procinfo_tick(run, 100, t);
            t += 0.30;
            procinfo_tick(run, 100, t); // spawn
            t += 0.01;
            double ms = time_tick(run, 200, t); // the switch -> reap() tick
            check("selection-change-away-from-hang tick stays fast",
                  ms < kTickBoundMs, "took " + std::to_string(ms) + "ms");
        }

        // (c) an over-cap flood — already killed unconditionally before this
        // review round; confirming it stays fast under a real timer too.
        {
            ProcInfoRunner run;
            run.asmspy_path =
                std::string(ASMTEST_FIXTURE_DIR) + "/fake_asmspy_info.sh";
            run.max_buf_bytes = 32;
            double t = 0.0;
            procinfo_tick(run, 100, t);
            t += 0.30;
            procinfo_tick(run, 100, t); // spawn
            double worst = 0.0;
            for (int i = 0; i < 200 && run.child_pid != 0; i++) {
                t += 0.01;
                double ms = time_tick(run, 100, t);
                if (ms > worst)
                    worst = ms;
                ::usleep(1000);
            }
            check("over-cap flood ticks stay fast", worst < kTickBoundMs,
                  "worst tick took " + std::to_string(worst) + "ms");
        }

        // (d) THE CRITICAL CASE: a child that closes stdout (a real EOF) but
        // keeps running for seconds afterward. Before the fix, the
        // drain-completion block sent no kill on eof, so waitpid blocked
        // this tick until the child exited on its own — measured at
        // 4000.4ms against this exact fixture's `sleep 4`.
        {
            ProcInfoRunner run;
            run.asmspy_path = std::string(ASMTEST_FIXTURE_DIR) +
                              "/fake_asmspy_closes_stdout.sh";
            double t = 0.0;
            procinfo_tick(run, 100, t);
            t += 0.30;
            procinfo_tick(run, 100, t); // spawn
            double worst = 0.0;
            for (int i = 0; i < 300 && run.child_pid != 0; i++) {
                t += 0.01;
                double ms = time_tick(run, 100, t);
                if (ms > worst)
                    worst = ms;
                ::usleep(1000);
            }
            check("closes-stdout-but-lingers: the child is eventually reaped",
                  run.child_pid == 0,
                  "child_pid=" + std::to_string(run.child_pid) +
                      " still set after the loop — the fixture never got "
                      "cleaned up");
            check("closes-stdout-but-lingers tick stays fast (the round-3 "
                  "critical fix: kill is unconditional before waitpid)",
                  worst < kTickBoundMs,
                  "worst tick took " + std::to_string(worst) +
                      "ms — this is the exact scenario measured at ~4000ms "
                      "before the fix");
        }
    }

    if (failures) {
        std::fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_procinfo: all checks passed\n");
    return 0;
}
