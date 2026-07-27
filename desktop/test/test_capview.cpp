// test_capview.cpp — the capability panel's two UI laws
// (docs/internal/gui/06-doors-and-learning.md T6).
//
// Every input is SYNTHETIC, which is the point: the panel's rules must be
// assertable on a host with no Intel PT, no CoreSight, no AMD silicon and no
// perf permission — i.e. on every CI container. A test that could only run on
// the right machine would test nothing on any other.
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

#include "capview.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static asmtest_hwtrace_status_t mk(int avail, int code, int stage, int paranoid,
                                   const char *reason) {
    asmtest_hwtrace_status_t s;
    std::memset(&s, 0, sizeof s);
    s.available = avail;
    s.code = code;
    s.stage = stage;
    s.perf_event_paranoid = paranoid;
    std::snprintf(s.reason, sizeof s.reason, "%s", reason);
    return s;
}

int main() {
    // A believable Linux-x86-64 host: single-step works, PT is present but
    // perf refuses, LBR is the wrong vendor, CoreSight is the wrong ISA.
    const char *pt_reason =
        "perf_event_open(intel_pt) failed: EACCES — perf_event_paranoid=4 "
        "blocks unprivileged tracing (needs CAP_PERFMON or paranoid<=2)";
    asmtest_hwtrace_status_t st[4] = {
        mk(0, ASMTEST_HW_EPERM, ASMTEST_HW_STAGE_PROBE, 4, pt_reason),
        mk(0, ASMTEST_HW_EUNAVAIL, ASMTEST_HW_STAGE_CPU, INT_MIN,
           "ARM CoreSight is an AArch64 facility; this host is x86_64"),
        mk(0, ASMTEST_HW_EUNAVAIL, ASMTEST_HW_STAGE_CPU, 4,
           "AMD LBR needs a Zen 4+ AMD CPU; this host is GenuineIntel"),
        mk(1, ASMTEST_HW_OK, ASMTEST_HW_STAGE_OK, 4, ""),
    };

    asmtest_trace_choice_t cascade[3];
    std::memset(cascade, 0, sizeof cascade);
    cascade[0].tier = ASMTEST_TIER_HWTRACE;
    cascade[0].backend = ASMTEST_HWTRACE_SINGLESTEP;
    cascade[0].fidelity = ASMTEST_FIDELITY_NATIVE;
    cascade[1].tier = ASMTEST_TIER_DYNAMORIO;
    cascade[1].fidelity = ASMTEST_FIDELITY_NATIVE;
    cascade[2].tier = ASMTEST_TIER_EMULATOR;
    cascade[2].fidelity = ASMTEST_FIDELITY_VIRTUAL;

    const char *ibs_sub = "not AMD";
    const char *ibs_cap = "";

    auto rows = capview_build(cascade, 3, st, 0, ibs_sub, ibs_cap, false);
    std::string d = capview_dump(rows);

    // --- UI LAW 1: a greyed row always shows its machine reason ------------
    for (const cap_row &r : rows) {
        if (r.kind != cap_kind::backend || r.available)
            continue;
        check("greyed backend " + r.label + " carries a reason",
              !r.reason.empty(), d);
    }
    {
        const cap_row *pt = nullptr;
        for (const cap_row &r : rows)
            if (r.kind == cap_kind::backend && r.label == "Intel PT")
                pt = &r;
        check("the PT row exists", pt != nullptr, d);
        if (pt != nullptr) {
            check("the EPERM row's reason is VERBATIM, not paraphrased",
                  pt->reason == pt_reason, pt->reason);
            check("...and it names the stage",
                  pt->chip.find("stage: probe") != std::string::npos, pt->chip);
            check("...and the paranoid level",
                  pt->chip.find("perf_event_paranoid=4") != std::string::npos,
                  pt->chip);
            check("...and EPERM is distinguished from EUNAVAIL",
                  pt->code == ASMTEST_HW_EPERM, std::to_string(pt->code));
        }
    }
    {
        const cap_row *lbr = nullptr;
        for (const cap_row &r : rows)
            if (r.kind == cap_kind::backend && r.label == "AMD LBR")
                lbr = &r;
        check("the wrong-vendor row is EUNAVAIL, not EPERM",
              lbr != nullptr && lbr->code == ASMTEST_HW_EUNAVAIL, d);
    }
    {
        const cap_row *ss = nullptr;
        for (const cap_row &r : rows)
            if (r.kind == cap_kind::backend && r.label == "single-step (TF)")
                ss = &r;
        check("an available backend is not greyed",
              ss != nullptr && ss->available, d);
        check("...and carries no reason (there is nothing to explain)",
              ss != nullptr && ss->reason.empty(), ss ? ss->reason : "");
    }

    // --- IBS: BOTH reasons, labelled, never collapsed ---------------------
    {
        const cap_row *ibs = nullptr;
        for (const cap_row &r : rows)
            if (r.kind == cap_kind::ibs)
                ibs = &r;
        check("the IBS row exists", ibs != nullptr, d);
        if (ibs != nullptr) {
            check("it labels the SUBSTRATE reason",
                  ibs->reason.find("substrate: not AMD") != std::string::npos,
                  ibs->reason);
            check("it labels the LAST CAPTURE reason separately",
                  ibs->reason.find("last capture:") != std::string::npos,
                  ibs->reason);
            check("the two are not collapsed into one line",
                  ibs->reason.find('\n') != std::string::npos, ibs->reason);
            check("a statistical row carries the sampled chip",
                  ibs->chip == std::string(kCapStatisticalChip), ibs->chip);
        }
    }

    // --- STATISTICAL cascade rows carry the chip too ----------------------
    {
        asmtest_trace_choice_t stat[1];
        std::memset(stat, 0, sizeof stat);
        stat[0].tier = ASMTEST_TIER_HWTRACE;
        stat[0].backend = ASMTEST_HWTRACE_AMD_LBR;
        stat[0].fidelity = ASMTEST_FIDELITY_STATISTICAL;
        auto sr = capview_build(stat, 1, st, 1, "", "", false);
        check("a statistical cascade row is chipped",
              sr[0].statistical &&
                  sr[0].chip == std::string(kCapStatisticalChip),
              sr[0].chip);
    }

    // --- UI LAW 2: never auto-fall back across the fidelity line ----------
    {
        auto empty = capview_build(nullptr, 0, st, 0, ibs_sub, ibs_cap, true);
        std::string ed = capview_dump(empty);
        const cap_row *ref = nullptr;
        for (const cap_row &r : empty)
            if (r.kind == cap_kind::refusal)
                ref = &r;
        check("an empty native-only cascade yields the refusal row",
              ref != nullptr, ed);
        if (ref != nullptr)
            check("the refusal copy is verbatim",
                  ref->reason == std::string(kCapNativeOnlyEmpty), ref->reason);
        check("the refusal names EUNAVAIL",
              ref != nullptr &&
                  ref->reason.find("EUNAVAIL") != std::string::npos,
              ed);
        check("the refusal offers the crossing as an EXPLICIT choice",
              ref != nullptr &&
                  ref->reason.find("explicit choice") != std::string::npos,
              ed);

        // No refusal row when the cascade is non-empty, or when native-only is
        // off — the refusal must be an answer, never decoration.
        auto full = capview_build(cascade, 3, st, 0, ibs_sub, ibs_cap, true);
        for (const cap_row &r : full)
            check("a non-empty cascade has no refusal row",
                  r.kind != cap_kind::refusal, capview_dump(full));
        auto off = capview_build(nullptr, 0, st, 0, ibs_sub, ibs_cap, false);
        for (const cap_row &r : off)
            check("native-only OFF has no refusal row",
                  r.kind != cap_kind::refusal, capview_dump(off));
    }

    // --- the fidelity line ------------------------------------------------
    {
        bool emu_below = false, native_above = true;
        for (const cap_row &r : rows) {
            if (r.kind == cap_kind::emulator)
                emu_below = r.below_fidelity_line;
            if (r.kind == cap_kind::cascade &&
                r.fidelity == ASMTEST_FIDELITY_NATIVE && r.below_fidelity_line)
                native_above = false;
        }
        check("the emulator sits BELOW the native→virtual line", emu_below, d);
        check("a native row never sits below it", native_above, d);
        check("the rule's label is verbatim",
              std::string(kCapFidelityLine) == "native → virtual fidelity line",
              kCapFidelityLine);
    }

    // --- 18-T4: positives-first summary + the remedy map ------------------
    // (a) A believable bare host (this fixture: PT/CoreSight/LBR/IBS all
    // unavailable, single-step + emulator available). The summary must LEAD with
    // what works, state the Learn/Author floor, and demote the negatives.
    {
        std::string sum = capview_summary(rows);
        check("summary/leads-with-can",
              sum.find("This host:") == 0 &&
                  sum.find("available") != std::string::npos,
              sum);
        check("summary/names-single-step",
              sum.find("single-step (TF)") != std::string::npos, sum);
        check("summary/demotes-negatives",
              sum.find("details below") != std::string::npos, sum);
        check("summary/states-learn-author-floor",
              sum.find(kCapLearnAuthorFloor) != std::string::npos, sum);
    }
    // (b) A fully capable host still leads with positives, and with nothing
    // unavailable it does not fabricate a "details below".
    {
        asmtest_hwtrace_status_t all_ok[4] = {
            mk(1, ASMTEST_HW_OK, ASMTEST_HW_STAGE_OK, 0, ""),
            mk(1, ASMTEST_HW_OK, ASMTEST_HW_STAGE_OK, 0, ""),
            mk(1, ASMTEST_HW_OK, ASMTEST_HW_STAGE_OK, 0, ""),
            mk(1, ASMTEST_HW_OK, ASMTEST_HW_STAGE_OK, 0, ""),
        };
        auto caps = capview_build(cascade, 3, all_ok, 1, "", "", false);
        std::string sum = capview_summary(caps);
        check("summary/capable-leads-positive",
              sum.find("available") != std::string::npos &&
                  sum.find("no capture backend") == std::string::npos,
              sum);
        check("summary/capable-floor-still-stated",
              sum.find(kCapLearnAuthorFloor) != std::string::npos, sum);
    }

    // (c) The remedy map: a recognised reason yields the matching remedy REUSED
    // from the inspect_door attach_verdict map; an unrecognised one yields none,
    // and the verbatim reason still stands.
    {
        const cap_row *pt = nullptr, *lbr = nullptr, *ss = nullptr;
        for (const cap_row &r : rows) {
            if (r.kind != cap_kind::backend)
                continue;
            if (r.label == "Intel PT")
                pt = &r;
            if (r.label == "AMD LBR")
                lbr = &r;
            if (r.label == "single-step (TF)")
                ss = &r;
        }
        check("remedy/paranoid",
              pt != nullptr &&
                  capview_remedy(*pt).find("perf_event_paranoid") !=
                      std::string::npos,
              "a perf_event_paranoid refusal must offer the sysctl/CAP_PERFMON "
              "remedy");
        check("remedy/wrong-vendor-none",
              lbr != nullptr && capview_remedy(*lbr).empty(),
              "a wrong-vendor LBR carries a paranoid level but no paranoid "
              "remedy — lowering the sysctl would not help");
        check("remedy/available-none",
              ss != nullptr && capview_remedy(*ss).empty(),
              "a working backend has nothing to remedy");

        // The ptrace family REUSES attach_verdict, so the capability panel and
        // the Inspect door give one answer. Synthetic reasons for each.
        cap_row yama;
        yama.kind = cap_kind::backend;
        yama.available = false;
        yama.reason = "blocked by Yama ptrace_scope=2";
        check("remedy/yama-reuses-attach-map",
              capview_remedy(yama).find("CAP_SYS_PTRACE") != std::string::npos,
              "a ptrace_scope refusal must reuse attach_verdict's CAP remedy");

        cap_row i386;
        i386.kind = cap_kind::backend;
        i386.available = false;
        i386.reason = "the target is a 32-bit (i386) process";
        check("remedy/i386-reuses-attach-map",
              capview_remedy(i386).find("64-bit") != std::string::npos,
              "an i386 refusal must reuse attach_verdict's 64-bit remedy");

        // No verbatim reason is ever dropped: every negative backend row still
        // carries its machine reason (UI LAW 1 survives the restructure, D7).
        for (const cap_row &r : rows)
            if (r.kind == cap_kind::backend && !r.available)
                check("remedy/verbatim-reason-kept", !r.reason.empty(),
                      "a demoted negative must keep its verbatim reason");
    }

    // --- determinism ------------------------------------------------------
    check("two builds are byte-identical",
          capview_dump(
              capview_build(cascade, 3, st, 0, ibs_sub, ibs_cap, false)) == d,
          "");

    if (failures) {
        std::fprintf(stderr, "%d capview check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_capview: all checks passed\n");
    return 0;
}
