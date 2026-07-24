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

#include <time.h>
#include <unistd.h>

#include "live/inspect.h"
#include "live/session.h"

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
        // honest answer is Unknown — a confident Yes here fails at attach.
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

    // The real /proc lister runs on this host: it must find us, and must
    // refuse to attach to us.
    {
        std::vector<ProcRow> rows = list_processes();
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
        check("ev/walk-honest", has(n, "not a fact about the target"),
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
            "a pick with no evidence field cannot be presented honestly");
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

int main(void) {
    test_attach();
    test_evidence();
    test_front_door();
    if (failures) {
        std::fprintf(stderr, "test_inspect: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_inspect: all checks passed\n");
    return 0;
}
