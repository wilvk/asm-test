// test_inspect.cpp — the Inspect door's two decisions (07-serve-live-host T5).
//
// Both are pure, and both are the kind of rule that is wrong in a way nothing
// else would catch:
//
//  - ATTACHABILITY has to report the fact that DOMINATES. Every combination is
//    checked here, including the ones this host cannot produce (ptrace_scope=3,
//    a 32-bit tracee, an already-traced target), because the failure mode is
//    not "it crashes" — it is telling an operator to raise a Yama scope when
//    the real problem is that no privilege can help.
//
//  - EVIDENCE LABELLING has to say residency is weaker. There is no crash and
//    no wrong number if it does not; the interface just quietly implies the
//    front door measured something it did not.
//
// The end-to-end half drives the real session host against fake_serve.sh's two
// sampler paths, so the `pick` events are parsed off a real pipe.
#include <cstdio>
#include <string>
#include <vector>

#include <signal.h>   // kill / SIGKILL — reap the busy child of the activity test
#include <sys/wait.h> // waitpid
#include <time.h>
#include <unistd.h>

#include "live/inspect.h"
#include "live/session.h"
#include "ui/progress.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;
using nlohmann::json;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// attachability
// ---------------------------------------------------------------------------
static void test_attach() {
    // The happy paths.
    {
        AttachFacts f; // same uid, Yama absent
        AttachVerdict v = attach_verdict(f);
        check("att/plain", v.verdict == Attach::Yes,
              "same-uid, no Yama -> Yes");
        check("att/plain-why", has(v.why, "not enforced"),
              "an absent Yama must be reported as NOT ENFORCING, not unknown");
    }
    {
        AttachFacts f;
        f.yama_scope = 0;
        check("att/scope0", attach_verdict(f).verdict == Attach::Yes,
              "scope 0 + same uid -> Yes");
    }
    {
        AttachFacts f;
        f.yama_scope = 1;
        AttachVerdict v = attach_verdict(f);
        // Whether the target opted in is NOT readable from outside, so the only
        // truthful answer is Unknown — a confident Yes here fails at attach.
        check("att/scope1-unknown", v.verdict == Attach::Unknown,
              "scope 1 without a known opt-in is Unknown, not Yes");
        check("att/scope1-why", has(v.why, "PR_SET_PTRACER"),
              "the reason must name the opt-in mechanism");
        check("att/scope1-remedy", !v.remedy.empty(),
              "scope 1 has a remedy and must offer it");
        f.target_opted_in = true;
        check("att/scope1-optin", attach_verdict(f).verdict == Attach::Yes,
              "scope 1 + a known opt-in -> Yes");
    }

    // The refusals, each of which sends an operator somewhere different.
    {
        AttachFacts f;
        f.yama_scope = 2;
        AttachVerdict v = attach_verdict(f);
        check("att/scope2", v.verdict == Attach::No, "scope 2 -> No");
        check("att/scope2-remedy", has(v.remedy, "CAP_SYS_PTRACE"),
              "scope 2's remedy is the capability");
    }
    {
        AttachFacts f;
        f.yama_scope = 3;
        AttachVerdict v = attach_verdict(f);
        check("att/scope3", v.verdict == Attach::No, "scope 3 -> No");
        // The distinguishing fact: scope 3 is one-way. Offering a remedy that
        // cannot work is worse than offering none.
        check("att/scope3-oneway", has(v.remedy, "reboot"),
              "scope 3 must say it cannot be lowered without a reboot");
    }
    {
        AttachFacts f;
        f.same_uid = false;
        AttachVerdict v = attach_verdict(f);
        check("att/uid", v.verdict == Attach::No, "different uid -> No");
        check("att/uid-remedy", has(v.remedy, "CAP_SYS_PTRACE"),
              "a uid refusal must name what would fix it");
    }
    {
        AttachFacts f;
        f.tracer_pid = 991;
        AttachVerdict v = attach_verdict(f);
        check("att/traced", v.verdict == Attach::No,
              "an already-traced target -> No");
        check("att/traced-names-pid", has(v.why, "991"),
              "the refusal must name the tracer holding it");
    }
    {
        AttachFacts f;
        f.elf_class = 32;
        AttachVerdict v = attach_verdict(f);
        check("att/i386", v.verdict == Attach::No, "a 32-bit tracee -> No");
        // The load-bearing part: this is NOT a permission problem, so the
        // remedy must not suggest a privilege.
        check("att/i386-not-perm", !has(v.remedy, "CAP_SYS_PTRACE"),
              "no privilege fixes a 32-bit tracee; the remedy must not imply "
              "one");
        check("att/i386-why", has(v.why, "nonsense") || has(v.why, "x86-64"),
              "the reason must say WHY it is refused rather than attempted");
    }
    {
        AttachFacts f;
        f.is_kthread = true;
        check("att/kthread", attach_verdict(f).verdict == Attach::No,
              "a kernel thread -> No");
    }
    {
        AttachFacts f;
        f.is_self = true;
        check("att/self", attach_verdict(f).verdict == Attach::No,
              "our own process -> No");
    }

    // ---- DOMINANCE: which fact wins when several hold ----------------------
    {
        // CAP overrides uid and Yama — but NOT a 32-bit tracee or an existing
        // tracer, which no privilege can fix.
        AttachFacts f;
        f.have_cap_sys_ptrace = true;
        f.same_uid = false;
        f.yama_scope = 2;
        check("dom/cap-wins", attach_verdict(f).verdict == Attach::Yes,
              "CAP_SYS_PTRACE overrides uid and ptrace_scope");

        f.elf_class = 32;
        AttachVerdict v = attach_verdict(f);
        check("dom/i386-beats-cap", v.verdict == Attach::No,
              "no capability makes a 32-bit tracee traceable");
        check("dom/i386-reported", has(v.why, "32-bit"),
              "the dominating fact must be the one reported");

        AttachFacts g;
        g.have_cap_sys_ptrace = true;
        g.tracer_pid = 7;
        check("dom/traced-beats-cap", attach_verdict(g).verdict == Attach::No,
              "no capability gives a tracee a second tracer");
    }
    {
        // A uid mismatch AND scope 2: scope 2 is the one to report, because
        // becoming the right user would still not be enough.
        AttachFacts f;
        f.same_uid = false;
        f.yama_scope = 2;
        check("dom/scope2-beats-uid", has(attach_verdict(f).why, "scope=2"),
              "scope 2 dominates a uid mismatch — fixing the uid would not "
              "help");
    }

    // Every verdict must carry a reason. A row that cannot say why is the
    // thing this door exists to eliminate.
    {
        const int scopes[] = {-1, 0, 1, 2, 3};
        for (int sc : scopes)
            for (int uid = 0; uid < 2; uid++)
                for (int cap = 0; cap < 2; cap++)
                    for (int cls : {0, 32, 64}) {
                        AttachFacts f;
                        f.yama_scope = sc;
                        f.same_uid = uid != 0;
                        f.have_cap_sys_ptrace = cap != 0;
                        f.elf_class = cls;
                        AttachVerdict v = attach_verdict(f);
                        check("att/always-explains", !v.why.empty(),
                              "a verdict with no reason is not a verdict");
                    }
    }

    // The real lister, on whatever host is running this. Two shapes, and which
    // one applies is a property of the platform, not a thing to skip over: on
    // Linux the /proc walk must find us and refuse us; on a host with no /proc
    // the emptiness must come WITH a reason, because an unexplained empty list
    // is the one output that reads as a measurement it never made.
    {
        std::vector<ProcRow> rows = list_processes();
        const char *why_not = local_inspect_unavailable();
        if (*why_not) {
            check("list/absence-explained", rows.empty(),
                  "a host with no local lister must return no rows");
            // Verbatim-reason law, same as the capability panel's greyed rows:
            // it has to name what is missing AND where to go instead, or it
            // sends a reader to fix the wrong thing.
            const std::string w(why_not);
            check("list/absence-names-proc", w.find("/proc") != std::string::npos,
                  "the reason must name what is absent");
            check("list/absence-offers-remote", w.find("ssh") != std::string::npos,
                  "the reason must name the path that does work");
        } else {
            check("list/nonempty", !rows.empty(), "/proc listed no processes");
            bool found_self = false;
            for (const ProcRow &r : rows)
                if (r.pid == (long)::getpid()) {
                    found_self = true;
                    check("list/self-refused", r.verdict.verdict == Attach::No,
                          "the lister must refuse our own pid");
                }
            check("list/found-self", found_self,
                  "the lister did not include this process");
            for (const ProcRow &r : rows)
                check("list/all-explained", !r.verdict.why.empty(),
                      "every row must carry a reason");
        }
    }

    // 2026-08-06 plan, Task 1: the probe must not invent facts.
    //
    // `readlink("/proc/<pid>/exe")` fails with EACCES for every process we do
    // not own -- 453 of 588 on the box this was measured on, INCLUDING pid 1.
    // Reporting those as kernel threads is a confidently wrong sentence on 77%
    // of the picker, and it wins over the uid reason because attach_verdict
    // checks is_kthread first.
    {
        AttachFacts f;
        f.is_kthread = false;
        f.same_uid = false;
        AttachVerdict v = attach_verdict(f);
        check("probe/other-user-not-kthread",
              has(v.why, "uid") || has(v.remedy, "CAP_SYS_PTRACE"),
              "a process owned by another user must be refused for its UID, "
              "not described as having no user-space address space");
    }
    {
        // The other invented fact: target_opted_in is what turns "maybe" into
        // "yes", and 12 of 15 live Firefox processes DO opt in (their crash
        // reporter calls PR_SET_PTRACER). Nothing outside this test ever set it.
        AttachFacts f;
        f.yama_scope = 1;
        f.target_opted_in = true;
        check("probe/optin-is-yes", attach_verdict(f).verdict == Attach::Yes,
              "a target that opted in is attachable under scope 1 -- this is "
              "the branch that makes a browser's content processes reachable");
    }
    {
        // And the probe itself. SELF is always attachable-by-us in the sense the
        // probe measures (we can open our own /proc/self/mem), so it is the one
        // pid whose answer is knowable without privilege or a victim.
        AttachFacts self = probe_attach((long)::getpid(), read_yama_scope(),
                                        (long)::geteuid(), false);
        check("probe/self-not-kthread", !self.is_kthread,
              "our own process has an address space; a readlink failure must "
              "never be reported as 'kernel thread'");
        check(
            "probe/self-opted-in-measured", self.target_opted_in,
            "probe_attach must MEASURE attachability (open /proc/<pid>/mem, "
            "which runs the kernel's own ptrace_may_access) rather than leave "
            "the field at its default -- unset, every row renders 'maybe'");
    }
}

// ---------------------------------------------------------------------------
// the activity sample — list_processes(sample_cpu), the "activity" sort
// ---------------------------------------------------------------------------
static void test_activity() {
    // No /proc: the sampled path is a no-op over an empty list (the emptiness is
    // already asserted, with its reason, in test_attach). Nothing to measure.
    if (*local_inspect_unavailable())
        return;

    // The gate earns its name: the cheap default samples nothing, so every row's
    // cpu is 0. A nonzero here would mean the pid / comm / attach sorts are
    // silently paying the ~150ms window they exist to avoid.
    std::vector<ProcRow> plain = list_processes(false);
    bool any_cpu = false;
    for (const ProcRow &r : plain)
        if (r.cpu != 0)
            any_cpu = true;
    check("activity/unsampled-is-cheap", !any_cpu,
          "the unsampled list must not carry a CPU sample");

    // The measure itself: a child pegged at 100% across the sample window MUST
    // show nonzero CPU jiffies — the exact quantity the activity sort ranks on.
    // Forked, not threaded, so the test needs no -lpthread.
    pid_t busy = ::fork();
    if (busy == 0) {
        volatile unsigned long x = 0;
        for (;;)
            x++; // spin; the parent SIGKILLs us right after the sample
    }
    check("activity/spawn", busy > 0, "could not fork a busy child");
    if (busy > 0) {
        std::vector<ProcRow> hot = list_processes(true);
        ::kill(busy, SIGKILL);
        int st = 0;
        ::waitpid(busy, &st, 0);
        unsigned long long child_cpu = 0;
        bool found = false;
        for (const ProcRow &r : hot)
            if (r.pid == (long)busy) {
                found = true;
                child_cpu = r.cpu;
            }
        check("activity/busy-child-listed", found,
              "the sampled list dropped a live child pid");
        check("activity/busy-child-active", child_cpu > 0,
              "a pegged child must show CPU jiffies over the sample window");
    }
}

// ---------------------------------------------------------------------------
// the --auto evidence labels
// ---------------------------------------------------------------------------
static void test_evidence() {
    {
        json body = json::parse(
            R"({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"entry","func":"entered_often","base":100,"len":96,"weight":184,"sites":2,"attempt":1,"of":1}})");
        AutoPick p;
        check("ev/parse-entry", parse_auto_pick(body, &p), "entry pick parses");
        check("ev/entry-strong", !pick_is_weak_evidence(p),
              "an entry pick is NOT weak evidence");
        std::string l = pick_evidence_label(p);
        check("ev/entry-label", has(l, "ENTERED"),
              "the entry label must say it observed an entry");
        check("ev/entry-counts", has(l, "184") && has(l, "2"),
              "the entry label should carry its measured counts");
        check("ev/entry-names-sampler", has(l, "ibs-op"),
              "the label names WHICH sampler ran (the rule is host-shaped)");
        check("ev/entry-no-walk", pick_walk_note(p).empty(),
              "a first attempt has no walk note");
    }
    {
        json body = json::parse(
            R"({"state":"pick","mode":"auto","pick":{"sampler":"sw-clock","evidence":"residency","func":"grind_forever","base":100,"len":320,"weight":97,"sites":11,"attempt":1,"of":3}})");
        AutoPick p;
        check("ev/parse-res", parse_auto_pick(body, &p),
              "residency pick parses");
        // THE assertion this whole file exists for.
        check("ev/res-weak", pick_is_weak_evidence(p),
              "residency MUST be flagged as weaker evidence");
        std::string l = pick_evidence_label(p);
        check("ev/res-labelled", has(l, "WEAKER EVIDENCE"),
              "the residency label must say so in words");
        check("ev/res-consequence",
              has(l, "never fire") || has(l, "re-entered"),
              "the label must state the CONSEQUENCE, not just the caveat — a "
              "warning the reader must already understand is not a warning");
        check("ev/res-names-sampler", has(l, "sw-clock"),
              "the residency label names its sampler too (host-shaped rule)");
    }
    {
        // The walk: attempt 2 means a previous candidate was refused, and that
        // refusal is information the user is owed.
        json body = json::parse(
            R"({"state":"pick","mode":"auto","pick":{"sampler":"sw-clock","evidence":"residency","func":"entered_often","base":420,"len":96,"weight":41,"sites":4,"attempt":2,"of":3}})");
        AutoPick p;
        check("ev/parse-walk", parse_auto_pick(body, &p), "walk pick parses");
        std::string n = pick_walk_note(p);
        check("ev/walk-note", has(n, "candidate 2 of 3"),
              "the walk note must say which candidate this is");
        check("ev/walk-fidelity", has(n, "not a fact about the target"),
              "the walk note must not let a refusal read as a finding");
    }
    {
        // Non-pick session events, and a pick with no evidence field, must be
        // refused rather than half-parsed into a confident label.
        AutoPick p;
        check("ev/not-a-pick",
              !parse_auto_pick(
                  json::parse(R"({"state":"started","mode":"auto"})"), &p),
              "a started event is not a pick");
        check(
            "ev/no-evidence",
            !parse_auto_pick(
                json::parse(
                    R"({"state":"pick","pick":{"sampler":"sw-clock","func":"x"}})"),
                &p),
            "a pick with no evidence field cannot be presented faithfully");
    }
    {
        // 39 T3: an IDLE-WINDOW retry marker rides the pick channel with the
        // sentinel func "(idle window)". It must render as a faithful "re-sampling"
        // note, NOT as a region pick claiming an entry/residency observation.
        // Custom raw-string delimiter: the JSON's "(idle window)" contains a `)"`
        // sequence that would close a plain R"(...)" early. Evidence "idle" is the
        // truthful wire value — the window observed NOTHING, so it is not entry.
        json body = json::parse(
            R"J({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"idle","func":"(idle window)","base":0,"len":0,"weight":0,"sites":0,"attempt":2,"of":3}})J");
        AutoPick p;
        check("ev/parse-idle", parse_auto_pick(body, &p),
              "an idle-window marker parses off the pick channel");
        check("ev/is-idle-window", pick_is_idle_window(p),
              "the evidence:\"idle\" marker is recognised, not treated as a pick");
        std::string l = pick_evidence_label(p);
        check("ev/idle-labelled",
              has(l, "idle sample window 2 of 3") && has(l, "re-sampling"),
              "the idle label says which window and that it is re-sampling");
        check("ev/idle-not-entered",
              !has(l, "ENTERED") && !has(l, "EXECUTING"),
              "an idle window must NOT claim it observed an entry or execution");
        check("ev/idle-no-walk-note", pick_walk_note(p).empty(),
              "an idle window carries no candidate-walk note (its attempt/of is "
              "the WINDOW retry, not a candidate ordinal)");
        check("ev/idle-no-region", pick_region_spec(p).empty(),
              "an idle window picked NOTHING — handing its zero base/len to a "
              "scoped leg would single-step address 0");
    }
    {
        // The AUTO-LED SWEEP's hand-off. `auto` samples for its own region;
        // `trace` cannot. So a sweep that leads with auto must carry the picked
        // region to the leg behind it, and this is the rule that says what it
        // carries: the EXACT base+len the sampler measured, spelled the way
        // parse_region_spec reads it back.
        //
        // base+len rather than the func NAME on purpose: a name is re-resolved
        // against the symbol table by the serve host, and a duplicated static
        // symbol (firefox carries thousands) would resolve to a DIFFERENT
        // function than the one the sampler actually watched.
        json body = json::parse(
            R"({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"entry","func":"entered_often","base":4198400,"len":96,"weight":184,"sites":2,"attempt":1,"of":1}})");
        AutoPick p;
        check("ev/region/parse", parse_auto_pick(body, &p), "the pick parses");
        const std::string spec = pick_region_spec(p);
        check("ev/region/spec", spec == "0x401000:96",
              "the spec must be the pick's own base+len in the grammar "
              "parse_region_spec reads (0xADDR:LEN): got \"" +
                  spec + "\"");

        // A pick with no extent is not a region. The sampler emits len 0 for a
        // symbol it could not size, and a zero-length region is one the host
        // would refuse — the sweep must stop and say so, not send it.
        AutoPick z = p;
        z.len = 0;
        check(
            "ev/region/zero-len-is-no-region", pick_region_spec(z).empty(),
            "a zero-length pick is not a region a scoped leg can single-step");
        AutoPick nb = p;
        nb.base = 0;
        check("ev/region/zero-base-is-no-region", pick_region_spec(nb).empty(),
              "a pick with no base is not a region");
    }
}

// ---------------------------------------------------------------------------
// end to end: the picks come off a real pipe
// ---------------------------------------------------------------------------
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

static std::vector<AutoPick> picks_of(const LiveSession &s) {
    std::vector<AutoPick> v;
    for (const LiveNote &n : s.notes()) {
        AutoPick p;
        if (n.kind == "session" && parse_auto_pick(n.body, &p))
            v.push_back(p);
    }
    return v;
}

static void test_front_door() {
    const std::string fake =
        std::string(ASMTEST_FIXTURE_DIR) + "/fake_serve.sh";
    {
        // The IBS path: one pick, strong evidence, no walk.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("door/ibs-start", false, "spawn failed: " + err);
            return;
        }
        s.send(R"({"cmd":"start","mode":"auto","pid":4242,"sampler":"ibs"})");
        bool ok = pump_until(s, [](const LiveSession &x) {
            return x.status().sessions_ended == 1;
        });
        check("door/ibs-ran", ok, "the auto session should complete");
        std::vector<AutoPick> p = picks_of(s);
        check("door/ibs-one-pick", p.size() == 1,
              "the entry path picks once and does not walk");
        check("door/ibs-strong", !p.empty() && !pick_is_weak_evidence(p[0]),
              "an ibs-op pick is entry evidence");
        check("door/ibs-func", !p.empty() && p[0].func == "entered_often",
              "the pick's function should be carried through");
        s.shutdown();
    }
    {
        // The portable path: two picks, both weak, the second carrying a walk
        // note. That the walk is VISIBLE is the property being tested.
        LiveSession s;
        LiveSession::Spec spec;
        spec.asmspy_path = fake;
        std::string err;
        if (!s.start(spec, err)) {
            check("door/sw-start", false, "spawn failed: " + err);
            return;
        }
        s.send(R"({"cmd":"start","mode":"auto","pid":4242,"sampler":"sw"})");
        bool ok = pump_until(s, [](const LiveSession &x) {
            return x.status().sessions_ended == 1;
        });
        check("door/sw-ran", ok, "the auto session should complete");
        std::vector<AutoPick> p = picks_of(s);
        check("door/sw-walk", p.size() == 2,
              "the residency path should surface BOTH candidates it tried");
        if (p.size() == 2) {
            check("door/sw-both-weak",
                  pick_is_weak_evidence(p[0]) && pick_is_weak_evidence(p[1]),
                  "every residency pick is weak evidence, including the one "
                  "that finally worked");
            check("door/sw-walk-note", !pick_walk_note(p[1]).empty(),
                  "the second candidate must carry a walk note");
            check("door/sw-first-nowalk", pick_walk_note(p[0]).empty(),
                  "the first candidate must not");
            check("door/sw-order",
                  p[0].func == "grind_forever" && p[1].func == "entered_often",
                  "the walk order must be preserved: the residency winner "
                  "first, then the callee it moved on to");
        }
        // The capture itself still landed — a walk is not a failure.
        check("door/sw-captured",
              s.recordings().size() == 1 && !s.recordings()[0].torn,
              "the walk should end in a real, closed capture");
        s.shutdown();
    }
}

// The faithful progress decision (14 T3): a real fraction ONLY with a real
// total; torn / unbounded gets the indeterminate bar, never a fabricated %.
static void test_progress() {
    check("prog/idle-hidden",
          progress_mode(false, true, 100) == ProgressMode::Hidden,
          "nothing in flight -> no bar");
    check("prog/unbounded-indeterminate",
          progress_mode(true, false, 0) == ProgressMode::Indeterminate,
          "an unbounded live session has no real total");
    check("prog/torn-indeterminate",
          progress_mode(true, false, 500) == ProgressMode::Indeterminate,
          "a torn recording (has_total=false) is never a percentage");
    check("prog/footered-determinate",
          progress_mode(true, true, 200) == ProgressMode::Determinate,
          "an end footer gives a real total");
    check("prog/zero-total-not-determinate",
          progress_mode(true, true, 0) == ProgressMode::Indeterminate,
          "has_total but total==0 cannot form a fraction");
    check("prog/fraction", progress_fraction(50, 200) == 0.25f, "50/200");
    check("prog/fraction-clamped-high", progress_fraction(300, 200) == 1.0f,
          "over-100% is clamped, never shown");
    check("prog/fraction-zero-total", progress_fraction(5, 0) == 0.0f,
          "no divide-by-zero");
}

// Live-session toasts (16 T1): a state TRANSITION raises the right toasts, an
// unchanged state raises none (no re-toast spam), a skip is Info not Error, and
// only an exact save carries an "Open in Loom" (open_path) button.
static void test_toasts() {
    FeedbackInputs a, b;
    check("toast/none-when-unchanged", live_session_toasts(a, b).empty(),
          "no transition should raise no toast");
    b.status.last_err = "ptrace: Operation not permitted";
    std::vector<SessionToast> t1 = live_session_toasts(a, b);
    check("toast/refusal-is-one-error",
          t1.size() == 1 && t1[0].kind == ToastKind::Error &&
              has(t1[0].text, "refused"),
          "a new refusal should raise one Error toast");
    check("toast/no-repeat", live_session_toasts(b, b).empty(),
          "an unchanged refusal must not re-toast every frame");
    FeedbackInputs c = b;
    c.status.sessions_ended = 1;
    c.status.last_stop_reason = "exited(0)";
    std::vector<SessionToast> t2 = live_session_toasts(b, c);
    check("toast/ended-is-success",
          t2.size() == 1 && t2[0].kind == ToastKind::Success,
          "a completed session should raise one Success toast");
    FeedbackInputs d = c;
    d.status.skip_code = 42;
    d.status.skip_reason = "no PT silicon";
    std::vector<SessionToast> t3 = live_session_toasts(c, d);
    check("toast/skip-is-info", t3.size() == 1 && t3[0].kind == ToastKind::Info,
          "a skip means success-with-nothing-to-report — Info, not Error");

    // An exact save carries an "Open in Loom" (open_path) button; a statistical
    // save does not (there is no Loom for a non-exact capture).
    FeedbackInputs e = d;
    e.saved_ok = true;
    e.saved_path = "capture.asmtrace";
    std::vector<SessionToast> t4 = live_session_toasts(d, e);
    check("toast/exact-save-has-open-button",
          t4.size() == 1 && t4[0].kind == ToastKind::Success &&
              t4[0].open_path == "capture.asmtrace",
          "an exact save should offer an Open-in-Loom button");
    check("toast/save-no-repeat", live_session_toasts(e, e).empty(),
          "a save must not re-toast until a NEW path is written");
    FeedbackInputs f = e;
    f.saved_statistical = true;
    f.saved_path = "sampled.asmtrace";
    std::vector<SessionToast> t5 = live_session_toasts(e, f);
    check("toast/statistical-save-has-no-button",
          t5.size() == 1 && t5[0].open_path.empty(),
          "a statistical save must NOT offer a Loom button (there is no Loom)");

    // A failed save (saved_ok stays false, only the status line changes) is an
    // Error toast, not a phantom success.
    FeedbackInputs g;
    g.save_status = "save failed: Permission denied";
    std::vector<SessionToast> t6 = live_session_toasts(FeedbackInputs{}, g);
    check("toast/failed-save-is-error",
          t6.size() == 1 && t6[0].kind == ToastKind::Error,
          "a failed save should raise an Error toast, not a Success");
}

// The split "paused" state (23 T3, F23): operator pause and budget block are
// DISTINCT model states with disjoint action sets, and a Queue never auto-swaps.
static void test_patchbay() {
    // Free: nothing paused, the jack is available.
    check("patch/free", patch_mode(false, true) == PatchMode::Free,
          "no pause + allowed -> Free");
    // Operator pause -> its ONLY recovery is Resume.
    PatchMode op = patch_mode(true, true);
    check("patch/operator", op == PatchMode::OperatorPaused,
          "the operator paused -> OperatorPaused, distinct from a budget block");
    PatchActions oa = patch_actions(op);
    check("patch/operator-actions",
          oa.resume && !oa.swap && !oa.queue && !oa.cancel,
          "an operator pause offers ONLY Resume");
    // Budget block -> Swap / Queue / Cancel, and NOT Resume.
    PatchMode bl = patch_mode(false, false);
    check("patch/budget", bl == PatchMode::BudgetBlocked,
          "the jack is held -> BudgetBlocked, distinct from an operator pause");
    PatchActions ba = patch_actions(bl);
    check("patch/budget-actions",
          !ba.resume && ba.swap && ba.queue && ba.cancel,
          "a budget block offers Swap / Queue / Cancel — never a bare Resume");
    // The two states are never the same: an operator pause is not a budget block
    // even if the jack also happens to be held (operator pause dominates the
    // label the user reads — they paused it themselves).
    check("patch/distinct", patch_mode(true, false) == PatchMode::OperatorPaused,
          "an operator pause reads as its own state, never the budget block");
}

// The lifecycle button gating (R4): only a button that can ACT at the current
// stage is enabled, so a greyed button never fires a command the serve loop would
// refuse. Pins every stage of live_controls().
static void test_controls() {
    // Idle: nothing to start (no host/target), nothing running -> all greyed.
    LiveControls idle = live_controls(false, false, false, false);
    check("controls/idle",
          !idle.start && !idle.stop && !idle.pause && !idle.resume,
          "with nothing running and no valid target, every button is greyed");
    // A valid target, nothing running yet -> only Start.
    LiveControls ready = live_controls(true, false, false, false);
    check("controls/ready",
          ready.start && !ready.stop && !ready.pause && !ready.resume,
          "a valid target enables Start only");
    // Running -> Stop + Pause; not Resume (nothing to resume).
    LiveControls run = live_controls(true, true, false, false);
    check("controls/running", run.stop && run.pause && !run.resume,
          "a running capture can be Stopped or Paused, not Resumed");
    // Operator-paused -> Stop + Resume; NOT Pause (already paused).
    LiveControls paused = live_controls(false, false, true, false);
    check("controls/paused", paused.stop && paused.resume && !paused.pause,
          "a paused capture can be Stopped or Resumed, never re-Paused");
    // Running AND operator-paused (the pause landed while it streams): Pause stays
    // greyed (already paused), Resume enabled — the two are never both offered.
    LiveControls both = live_controls(false, true, true, false);
    check("controls/running-paused", !both.pause && both.resume && both.stop,
          "a running-but-paused capture offers Resume, never a second Pause");
    // Believed-active but not yet 'running' in the status -> Stop still enabled.
    LiveControls active = live_controls(false, false, false, true);
    check("controls/active-can-stop", active.stop,
          "a believed-live capture is Stoppable even before the status flips");
}

// The scoped-region parser (the dataflow/trace region input). "0xADDR:LEN" yields
// base+len; a bare name (including a C++ `ns::func`) is the func form; anything
// malformed falls back to the name form. A wrong split here sends garbage to the
// tracer, so the shapes are pinned exactly.
static void test_region() {
    uint64_t base = 123, len = 456;
    check("region/hex-dec",
          parse_region_spec("0x401000:64", &base, &len) && base == 0x401000 &&
              len == 64,
          "0x401000:64 -> base 0x401000, len 64");
    base = len = 0;
    check("region/hex-hex",
          parse_region_spec("0x1000:0x20", &base, &len) && base == 0x1000 &&
              len == 0x20,
          "0x1000:0x20 -> base 0x1000, len 32");
    base = len = 0;
    check("region/dec-dec",
          parse_region_spec("4096:16", &base, &len) && base == 4096 &&
              len == 16,
          "4096:16 -> base 4096, len 16");
    base = 9, len = 9;
    check("region/name",
          !parse_region_spec("hotfn", &base, &len) && base == 0 && len == 0,
          "a bare name is the func form (false; base/len cleared)");
    check("region/cpp-name", !parse_region_spec("ns::func", nullptr, nullptr),
          "ns::func must stay a name, not be split as base:len");
    check("region/zero-len", !parse_region_spec("0x1000:0", nullptr, nullptr),
          "a zero length is not a region -> name form");
    check("region/junk", !parse_region_spec("0x1000:64x", nullptr, nullptr),
          "trailing junk in the length must not parse as a region");
    check("region/empty", !parse_region_spec("", nullptr, nullptr),
          "an empty spec is the (invalid) name form");
}

// ---------------------------------------------------------------------------
// remedy_command — the copy-pasteable terminal fix a remedy names, or "" when no
// single command clears the gate. Derived from the SAME remedy prose the UI
// shows, so a command is offered iff a universal one-liner exists.
// ---------------------------------------------------------------------------
static void test_remedy_command() {
    // The attach gate the operator actually hits: scope 1 -> the yama sysctl,
    // taken from the real remedy string (not a hand-written duplicate).
    {
        AttachFacts f;
        f.yama_scope = 1;
        std::string cmd = remedy_command(attach_verdict(f).remedy);
        check("cmd/scope1-exact",
              cmd == "sudo sysctl -w kernel.yama.ptrace_scope=0", cmd);
    }
    // The sampling/hwtrace gate, from a verbatim IBS skip reason.
    check("cmd/paranoid-exact",
          remedy_command("IBS is present but perf is locked down — needs "
                         "perf_event_paranoid<=2 or CAP_PERFMON") ==
              "sudo sysctl -w kernel.perf_event_paranoid=2",
          "a perf_event_paranoid reason yields the sysctl command");
    // The protocol-mismatch fix, from the end-placard prose.
    check("cmd/protocol-make-cli",
          remedy_command("Fix: rebuild `build/asmspy` (`make cli`); Disconnect "
                         "+ reconnect.") == "make cli",
          "a protocol-mismatch fix yields `make cli`");

    // The gates with NO single-command fix return "" — matching the REMEDY, not
    // the why: scope 3's why names ptrace_scope=3, but its remedy says reboot.
    {
        AttachFacts f;
        f.yama_scope = 3;
        check("cmd/scope3-none",
              remedy_command(attach_verdict(f).remedy).empty(),
              "scope 3 needs a reboot — no command, despite its why naming "
              "ptrace_scope=3");
    }
    {
        AttachFacts f;
        f.yama_scope = 2;
        check("cmd/scope2-none",
              remedy_command(attach_verdict(f).remedy).empty(),
              "scope 2 needs a privileged relaunch — no universal one-liner");
    }
    {
        AttachFacts f;
        f.elf_class = 32;
        check("cmd/i386-none", remedy_command(attach_verdict(f).remedy).empty(),
              "an i386 ABI mismatch has no terminal fix");
    }
    check("cmd/benign-none", remedy_command("capturing 42 steps").empty(),
          "an ordinary status names no gate and yields no command");
}

// ---------------------------------------------------------------------------
// proc_tree_layout — the Processes pane's lineage.
//
// The whole point of this function is what it does with rows the pane is NOT
// drawing, so every check below is about the interaction between the snapshot
// and the visibility mask. A layout that only ever sees an all-visible mask is
// a sorted list with extra steps; the cases that matter are the ones where the
// gate or the filter has taken an ancestor away, and those are exactly the ones
// a live /proc will not reproduce on demand.
// ---------------------------------------------------------------------------
namespace {

// pid/ppid/comm is all the layout reads. Nothing here needs a real process.
ProcRow pr(long pid, long ppid, const char *comm) {
    ProcRow r;
    r.pid = pid;
    r.ppid = ppid;
    r.comm = comm;
    return r;
}

// systemd(1) -> gnome-shell(1130) -> firefox(2201) -> two content children,
// plus NetworkManager(842) as a second child of pid 1 and an orphan(3000)
// whose parent is not in the snapshot at all.
std::vector<ProcRow> tree_rows() {
    return {
        pr(1, 0, "systemd"),           pr(842, 1, "NetworkManager"),
        pr(1130, 1, "gnome-shell"),    pr(2201, 1130, "firefox"),
        pr(2260, 2201, "Web Content"), pr(2281, 2201, "RDD Process"),
        pr(3000, 2999, "orphan"),
    };
}

// The emitted (pid, depth) pairs, as "pid@depth pid@depth …" — one string is a
// far better failure message than six index comparisons.
std::string shape(const std::vector<ProcRow> &rows,
                  const std::vector<ProcTreeRow> &t) {
    std::string o;
    for (const ProcTreeRow &n : t)
        o += (o.empty() ? "" : " ") + std::to_string(rows[n.index].pid) + "@" +
             std::to_string(n.depth);
    return o;
}

// Find the emitted PROCESS row for a pid; nullptr when it was not emitted.
const ProcTreeRow *find(const std::vector<ProcRow> &rows,
                        const std::vector<ProcTreeRow> &t, long pid) {
    for (const ProcTreeRow &n : t)
        if (n.kind == ProcNode::Process && rows[n.index].pid == pid)
            return &n;
    return nullptr;
}

// "nothing is collapsed", for the lineage checks that predate collapse and
// pin shapes it does not affect. show_all_processes rather than an explicit
// pid set because those cases carry no threads, and a thread group is the one
// thing this flag deliberately leaves shut.
ProcTreeExpansion open_all() {
    ProcTreeExpansion e;
    e.show_all_processes = true;
    return e;
}

// Open exactly these node ids (pid for a process, -pid for its threads group).
ProcTreeExpansion open_of(std::initializer_list<long> ids) {
    ProcTreeExpansion e;
    for (long id : ids)
        e.open.insert(id);
    return e;
}

// A process carrying `n` tasks, leader first — the shape read_threads()
// produces, without needing a live process to produce it.
ProcRow with_threads(long pid, long ppid, const char *comm, int n) {
    ProcRow r = pr(pid, ppid, comm);
    for (int i = 0; i < n; ++i) {
        ProcThread t;
        t.tid = i == 0 ? pid : pid + 1000 + i;
        t.comm = i == 0 ? comm : (std::string(comm) + "-w") + std::to_string(i);
        t.state = 'S';
        r.threads.push_back(t);
    }
    return r;
}

// The emitted rows as "kind:id@depth …", which makes a wrong SHAPE readable
// in the failure message instead of six index comparisons.
std::string kshape(const std::vector<ProcRow> &rows,
                   const std::vector<ProcTreeRow> &t) {
    std::string o;
    for (const ProcTreeRow &n : t) {
        o += o.empty() ? "" : " ";
        if (n.kind == ProcNode::Process)
            o += "p" + std::to_string(rows[n.index].pid);
        else if (n.kind == ProcNode::ThreadGroup)
            o += "g" + std::to_string(rows[n.index].pid);
        else
            o += "t" + std::to_string(n.tid);
        o += "@" + std::to_string(n.depth);
    }
    return o;
}

} // namespace

static void test_proc_tree() {
    const std::vector<ProcRow> rows = tree_rows();
    const std::vector<char> all(rows.size(), 1);

    // 1. Everything visible: pre-order, siblings by pid, depth = real ancestry.
    {
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, open_all());
        check("tree/order",
              shape(rows, t) == "1@0 842@1 1130@1 2201@2 2260@3 2281@3 3000@0",
              "parent-then-children, siblings by pid: got " + shape(rows, t));
        check("tree/emits-all", t.size() == rows.size(),
              "every visible row is emitted exactly once");
    }

    // 2. Descending flips the sibling ORDER without changing the SHAPE — the
    // depths are identical, only each group's order reverses.
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, open_all(), true);
        check("tree/descending",
              shape(rows, t) == "3000@0 1@0 1130@1 2201@2 2281@3 2260@3 842@1",
              "descending reverses each sibling group only: got " +
                  shape(rows, t));
    }

    // 3. THE CASE THIS FUNCTION EXISTS FOR. Hide every ancestor of the two
    // content processes (a "firefox"-less filter, or an attachability gate that
    // dropped them) and the children must KEEP depth 3. Re-rooting them to 0
    // would render them flush left, claiming a top-level lineage that was never
    // measured — which is the defect, not a cosmetic one.
    {
        std::vector<char> vis(rows.size(), 0);
        vis[4] = vis[5] = 1; // 2260, 2281 only
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, vis, open_all());
        check("tree/hidden-ancestors-keep-depth",
              shape(rows, t) == "2260@3 2281@3",
              "a hidden ancestor must not re-root its child: got " +
                  shape(rows, t));
        const ProcTreeRow *n = find(rows, t, 2260);
        check("tree/hidden-parent-reported",
              n && n->parent == ProcParent::Hidden,
              "the parent is in the snapshot but not drawn -> Hidden, so the "
              "pane can say so rather than leave an unexplained indent");
    }

    // 4. The four ProcParent cases are four different facts, and the one that
    // must never collapse into "Hidden" is a ppid naming a process this walk
    // does not carry — a parent that exited mid-scan, or one outside our pid
    // namespace. Nothing can un-hide that one, so the remedy differs.
    {
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, open_all());
        const ProcTreeRow *root = find(rows, t, 1);
        const ProcTreeRow *kid = find(rows, t, 2201);
        const ProcTreeRow *orph = find(rows, t, 3000);
        check("tree/parent-none", root && root->parent == ProcParent::None,
              "ppid 0 is a real root, not an unknown parent");
        check("tree/parent-shown", kid && kid->parent == ProcParent::Shown,
              "a drawn parent reads Shown");
        check("tree/parent-unknown",
              orph && orph->parent == ProcParent::Unknown,
              "a ppid absent from the snapshot is Unknown, NOT Hidden — no "
              "filter change can bring it back");
        check("tree/orphan-is-root", orph && orph->depth == 0,
              "a parent this snapshot does not carry leaves nothing to nest "
              "under, so depth 0 is measured, not assumed");
    }

    // 5. last_sibling is computed over the DRAWN siblings. Hide the youngest
    // child and the └ has to move up to the one above it, or the tree draws a
    // branch continuing into a row that is not there.
    {
        std::vector<char> vis(rows.size(), 1);
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, vis, open_all());
        const ProcTreeRow *a = find(rows, t, 2260);
        const ProcTreeRow *b = find(rows, t, 2281);
        check("tree/last-sibling-full",
              a && b && !a->last_sibling && b->last_sibling,
              "with both drawn, the └ belongs to the later pid");
        vis[5] = 0; // hide 2281, the youngest
        t = proc_tree_layout(rows, vis, open_all());
        a = find(rows, t, 2260);
        check("tree/last-sibling-follows-filter", a && a->last_sibling,
              "hiding the youngest sibling must move the └ up, not leave a ├ "
              "pointing at nothing");
    }

    // 6. A short/empty mask under-draws rather than leaking rows the gate meant
    // to withhold — an empty result is the safe answer to a caller bug.
    {
        check("tree/empty-mask",
              proc_tree_layout(rows, std::vector<char>(), open_all()).empty(),
              "no mask means nothing visible, never everything visible");
        check("tree/empty-rows",
              proc_tree_layout(std::vector<ProcRow>(), std::vector<char>(),
                               open_all())
                  .empty(),
              "an empty snapshot lays out empty");
    }

    // 7. A CYCLE. Impossible in a live /proc, reachable in a snapshot read pid
    // by pid across a recycled pid. Verified by mutation: with the cut removed
    // both rows DISAPPEAR (each has a parent, so neither is a root, and the
    // pre-order walk never reaches either) — a table whose job is to list what
    // is running, silently not listing two of them.
    {
        std::vector<ProcRow> cyc = {pr(10, 11, "a"), pr(11, 10, "b")};
        std::vector<ProcTreeRow> t =
            proc_tree_layout(cyc, std::vector<char>(2, 1), open_all());
        check(
            "tree/cycle-still-listed", t.size() == 2,
            "a cycle is cut to roots — neither row may vanish from the table");
        check("tree/cycle-depth",
              t.size() == 2 && t[0].depth == 0 && t[1].depth == 0,
              "every member of a cut cycle is a root");
        // The ppid still names a real row in the table, so the parent COLUMN
        // stays truthful even though the nesting gave up.
        check("tree/cycle-parent-still-named",
              t.size() == 2 && t[0].parent == ProcParent::Shown,
              "a cycle-cut row's ppid is still a drawn row: say so");
    }

    // 8. Self-parenthood (pid == ppid) is the degenerate one-node cycle, and it
    // must not nest a row under itself.
    {
        std::vector<ProcRow> self = {pr(7, 7, "self")};
        std::vector<ProcTreeRow> t =
            proc_tree_layout(self, std::vector<char>(1, 1), open_all());
        check("tree/self-parent",
              t.size() == 1 && t[0].depth == 0 &&
                  t[0].parent == ProcParent::None,
              "a row whose ppid is its own pid has nothing above it");
    }

    // 9. The layout must not assume the input is pid-sorted: list_processes
    // sorts, but nothing in the signature promises it, and a caller-built
    // vector need not.
    {
        std::vector<ProcRow> shuffled = {pr(2201, 1130, "firefox"),
                                         pr(1, 0, "systemd"),
                                         pr(1130, 1, "gnome-shell")};
        std::vector<ProcTreeRow> t =
            proc_tree_layout(shuffled, std::vector<char>(3, 1), open_all());
        check("tree/unsorted-input", shape(shuffled, t) == "1@0 1130@1 2201@2",
              "the order is the layout's, not the input's: got " +
                  shape(shuffled, t));
    }
}

// ---------------------------------------------------------------------------
// proc_tree_layout — COLLAPSE and THREADS.
//
// Collapse is the half that decides what an operator can even reach, so the
// cases below are the ones where "closed" and "not there" could be confused:
// an ancestor the gate removed has no expander to click, so treating it as
// closed would strand its whole subtree behind a control that is not on
// screen. Threads are the half where a tid could leak into a pid — every
// emitted thread row is checked to carry its LEADER in ::pid.
// ---------------------------------------------------------------------------
static void test_proc_tree_collapse() {
    const std::vector<ProcRow> rows = tree_rows();
    const std::vector<char> all(rows.size(), 1);

    // 1. Nothing open: only the rows with no visible ancestor. This is the
    //    measured cost of the collapsed default — on a real 604-process
    //    desktop it is 2 rows, because everything descends from pid 1.
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, ProcTreeExpansion{});
        check("collapse/closed-shows-roots", kshape(rows, t) == "p1@0 p3000@0",
              "fully collapsed shows only what has nothing above it: got " +
                  kshape(rows, t));
    }

    // 2. Opening one node reveals ITS children and no further.
    {
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, open_of({1}));
        check("collapse/open-one-level",
              kshape(rows, t) == "p1@0 p842@1 p1130@1 p3000@0",
              "opening pid 1 reveals its children only: got " +
                  kshape(rows, t));
        const ProcTreeRow *n = find(rows, t, 1130);
        check("collapse/expandable-flagged", n && n->expandable && !n->expanded,
              "gnome-shell has a child, so it offers an expander and is shut");
    }

    // 3. THE RULE COLLAPSE TURNS ON. Hide gnome-shell (1130) with the gate and
    //    open only pid 1: firefox must appear at its TRUE depth 2, because the
    //    ancestor between them is not on screen and so has no expander to
    //    click. Treating an invisible ancestor as closed strands the subtree
    //    behind a control that does not exist.
    {
        std::vector<char> vis(rows.size(), 1);
        vis[2] = 0; // gnome-shell
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, vis, open_of({1}));
        check("collapse/invisible-ancestor-does-not-block",
              kshape(rows, t) == "p1@0 p842@1 p2201@2 p3000@0",
              "an ancestor the gate removed cannot hold its subtree shut: "
              "got " +
                  kshape(rows, t));
    }

    // 4. A VISIBLE closed ancestor does block — the other half of rule 3, or
    //    the flag would simply mean "never collapse anything".
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, open_of({1, 1130}));
        check("collapse/visible-closed-blocks",
              kshape(rows, t) == "p1@0 p842@1 p1130@1 p2201@2 p3000@0",
              "firefox is open's child but firefox itself is shut, so its own "
              "children stay hidden: got " +
                  kshape(rows, t));
    }

    // 5. An expander must not open onto nothing. A leaf offers none, and
    //    neither does a process whose entire subtree the gate removed.
    {
        std::vector<char> vis(rows.size(), 1);
        vis[4] = vis[5] = 0; // both content processes
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, vis, open_of({1, 1130}));
        const ProcTreeRow *ff = find(rows, t, 2201);
        check("collapse/no-expander-onto-nothing", ff && !ff->expandable,
              "firefox's only children are hidden, so an expander there would "
              "open onto an empty subtree");
        const ProcTreeRow *nm = find(rows, t, 842);
        check("collapse/leaf-has-no-expander", nm && !nm->expandable,
              "a childless, single-threaded process offers no expander");
    }

    // 6. show_all_processes (what the filter sets) bypasses process collapse.
    {
        ProcTreeExpansion e;
        e.show_all_processes = true;
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, e);
        check("collapse/filter-bypasses",
              kshape(rows, t) ==
                  "p1@0 p842@1 p1130@1 p2201@2 p2260@3 p2281@3 p3000@0",
              "a filter must not return nothing merely because the tree was "
              "shut: got " +
                  kshape(rows, t));
    }
}

static void test_proc_tree_threads() {
    // firefox has 3 tasks; NetworkManager has exactly ONE — which is what
    // read_threads() returns for a single-threaded process, and is NOT the
    // same as the empty vector an unreadable task dir gives. A fixture with
    // zero threads here would let "group when threads is non-empty" pass,
    // since it is only wrong at size 1 (mutation-verified).
    std::vector<ProcRow> rows = {pr(1, 0, "systemd"),
                                 with_threads(842, 1, "NetworkManager", 1),
                                 with_threads(2201, 1, "firefox", 3)};
    const std::vector<char> all(rows.size(), 1);

    // 1. A threads group appears only when the process is OPEN, and sits
    //    directly under it — before the child processes, so it stays next to
    //    the row it describes rather than pages below a deep subtree.
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, open_of({1, 2201}));
        check("threads/group-under-its-process",
              kshape(rows, t) == "p1@0 p842@1 p2201@1 g2201@2",
              "an open multi-threaded process gets a group row: got " +
                  kshape(rows, t));
    }

    // 2. Single-threaded processes get NO group — "1 thread" under every row
    //    is noise, and that one thread is the process already on screen.
    {
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, open_all());
        for (const ProcTreeRow &n : t)
            check(
                "threads/no-group-for-single",
                !(n.kind == ProcNode::ThreadGroup && rows[n.index].pid == 842),
                "NetworkManager is single-threaded and must offer no group");
    }

    // 3. Opening the GROUP reveals the tasks, leader first.
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, open_of({1, 2201, -2201}));
        check("threads/expand-group",
              kshape(rows, t) ==
                  "p1@0 p842@1 p2201@1 g2201@2 t2201@3 t3202@3 t3203@3",
              "the open group lists its tasks, leader first: got " +
                  kshape(rows, t));
    }

    // 4. THE ONE THAT MATTERS. Every thread row targets the thread-group
    //    LEADER, never its own tid. asmspy's target is a thread group, and
    //    `--info <tid>` answers with identity.pid set to the tid — so a tid
    //    reaching ::pid would render a thread as a process duplicating its
    //    own, and would hand the engine a non-leader to seize.
    {
        std::vector<ProcTreeRow> t =
            proc_tree_layout(rows, all, open_of({1, 2201, -2201}));
        int threads_seen = 0;
        for (const ProcTreeRow &n : t) {
            if (n.kind != ProcNode::Thread)
                continue;
            threads_seen++;
            check("threads/targets-the-leader", n.pid == 2201,
                  "a thread row must target its LEADER (2201), got " +
                      std::to_string(n.pid));
            check("threads/tid-kept-for-display", n.tid != 0,
                  "the tid is still carried, for the row to show");
        }
        check("threads/all-three-seen", threads_seen == 3,
              "expected 3 thread rows, got " + std::to_string(threads_seen));
        // The non-leader tids must not be mistaken for the leader.
        const ProcTreeRow *g = nullptr;
        for (const ProcTreeRow &n : t)
            if (n.kind == ProcNode::ThreadGroup)
                g = &n;
        check("threads/group-targets-leader", g && g->pid == 2201,
              "the group row targets the process too");
    }

    // 5. A filter (show_all_processes) reveals processes but NOT threads: the
    //    query matched pid/comm/cmdline, so answering it with a hundred task
    //    rows would answer a different question than the one typed.
    {
        ProcTreeExpansion e;
        e.show_all_processes = true;
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, e);
        for (const ProcTreeRow &n : t)
            check("threads/filter-does-not-dump-threads",
                  n.kind == ProcNode::Process,
                  "show_all_processes must not open thread groups");
    }

    // 6. A multi-threaded LEAF is still expandable — its group is the thing to
    //    reveal, even with no child process.
    {
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, open_of({1}));
        const ProcTreeRow *ff = find(rows, t, 2201);
        check("threads/multithreaded-leaf-expandable", ff && ff->expandable,
              "a childless process with 3 tasks still has a group to open");
    }

    // 7. proc_tree_all_nodes covers both id kinds — that is what makes
    //    "Expand all" total rather than "expand all the processes".
    {
        std::vector<long> ids = proc_tree_all_nodes(rows, all);
        bool has_proc = false, has_group = false;
        for (long id : ids) {
            if (id == 2201)
                has_proc = true;
            if (id == -2201)
                has_group = true;
        }
        check("threads/all-nodes-covers-both", has_proc && has_group,
              "Expand all must open thread groups too, or it is not 'all'");
        // Feeding it straight back in must leave nothing shut.
        ProcTreeExpansion e;
        for (long id : ids)
            e.open.insert(id);
        std::vector<ProcTreeRow> t = proc_tree_layout(rows, all, e);
        check("threads/all-nodes-opens-everything",
              kshape(rows, t) ==
                  "p1@0 p842@1 p2201@1 g2201@2 t2201@3 t3202@3 t3203@3",
              "Expand all leaves nothing collapsed: got " + kshape(rows, t));
    }
}

int main(void) {
    test_attach();
    test_activity();
    test_proc_tree();
    test_proc_tree_collapse();
    test_proc_tree_threads();
    test_remedy_command();
    test_evidence();
    test_front_door();
    test_progress();
    test_toasts();
    test_patchbay();
    test_controls();
    test_region();
    if (failures) {
        std::fprintf(stderr, "test_inspect: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_inspect: all checks passed\n");
    return 0;
}
