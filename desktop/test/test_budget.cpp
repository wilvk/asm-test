// test_budget.cpp — the D6 concurrency budget, every mode PAIR
// (07-serve-live-host.md T4).
//
// This is the doc's own "cheapest highest-value test", and the reason is worth
// stating: the budget is a claim about what the kernel will allow, but the
// thing that can actually be WRONG is the table — one mode mis-marked as free
// and the UI cheerfully fires a second attach that cannot succeed. The table is
// pure, so all 100 pairs are checked here on any host, and the live lanes are
// then only responsible for the wiring.
//
// It links budget.o and NOTHING else.
#include <cstdio>
#include <string>
#include <vector>

#include "live/budget.h"

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main(void) {
    const std::vector<LiveMode> &modes = all_modes();
    check("modes/count", modes.size() == 10,
          "the protocol defines 10 modes; got " + std::to_string(modes.size()));

    // The ONE free view, stated positively so a future mode marked free by
    // accident fails here rather than in production.
    int free_count = 0;
    for (LiveMode m : modes)
        if (!mode_uses_ptrace(m)) {
            free_count++;
            check("free/is-sample", mode_name(m) == std::string("sample"),
                  std::string("only `sample` reads out of band; `") +
                      mode_name(m) + "` must not be marked free");
        }
    check("free/count", free_count == 1,
          "exactly one mode is out of band; got " + std::to_string(free_count));

    // auto ENDS UP holding the jack even though it starts by sampling. A mode
    // is judged by the jack it holds, not by how it begins.
    LiveMode autom;
    check("auto/known", mode_from_name("auto", &autom), "auto should parse");
    check("auto/holds-jack", mode_uses_ptrace(autom),
          "auto captures with the data-flow engine, so it takes the jack");

    // A scoped REGION is needed by EXACTLY trace + dataflow (they single-step ONE
    // region, so the serve host demands a func/base+len). `auto` finds its own, so
    // it needs none — the door must NOT block Start on a region for auto — and
    // every whole-process mode needs none either. Stated positively so a future
    // scoped mode marked region-free (or a whole-process mode marked needing one)
    // fails here rather than as a rejected `start` in production.
    int region_count = 0;
    for (LiveMode m : modes)
        if (mode_needs_region(m)) {
            region_count++;
            std::string n = mode_name(m);
            check("region/is-scoped", n == "trace" || n == "dataflow",
                  std::string("only trace/dataflow are scoped; `") + n +
                      "` must not be marked as needing a region");
        }
    check("region/count", region_count == 2,
          "exactly trace + dataflow need a region; got " +
              std::to_string(region_count));
    check("region/auto-free", !mode_needs_region(autom),
          "auto finds its own region via the sampler — it must need none");

    // Every mode names its structural reason — a refusal with no reason is a
    // dead end rather than a next step.
    for (LiveMode m : modes) {
        check("reason/nonempty", mode_jack_reason(m)[0] != '\0',
              std::string(mode_name(m)) + " has no jack reason");
        LiveMode rt;
        check("name/roundtrip", mode_from_name(mode_name(m), &rt) && rt == m,
              std::string("name round-trip failed for ") + mode_name(m));
    }
    check("name/unknown-refused", !mode_from_name("nonesuch", nullptr),
          "an unknown mode name must be refused, not guessed");

    // ---- every ordered pair ------------------------------------------------
    int pairs = 0, allowed = 0, refused = 0;
    for (LiveMode active : modes) {
        for (LiveMode want : modes) {
            pairs++;
            std::vector<LiveMode> live{active};
            BudgetDecision d = budget_can_start(want, live);
            // THE rule: refused exactly when both want and the active view
            // need the jack. Everything else is allowed.
            bool expect_refused =
                mode_uses_ptrace(want) && mode_uses_ptrace(active);
            check("pair/decision", d.allowed == !expect_refused,
                  std::string("start ") + mode_name(want) + " while " +
                      mode_name(active) + " runs: got " +
                      (d.allowed ? "allowed" : "refused") + ", want " +
                      (expect_refused ? "refused" : "allowed"));
            if (d.allowed) {
                allowed++;
                check("pair/allowed-quiet", d.reason.empty(),
                      "an allowed decision must carry no refusal prose");
                check("pair/allowed-nolabel", budget_blocked_label(d).empty(),
                      "an allowed decision must render no blocked label");
            } else {
                refused++;
                check("pair/blocker", d.blocker == active,
                      std::string("the refusal must name ") +
                          mode_name(active) + " as the blocker");
                // The refusal has to be actionable: it names the holder AND
                // the mode that was refused, so the UI can offer a swap.
                check("pair/names-both",
                      d.reason.find(mode_name(active)) != std::string::npos &&
                          d.reason.find(mode_name(want)) != std::string::npos,
                      "the refusal must name both the holder and the refused "
                      "mode");
                // The blocked label reads BLOCKED (not "paused") and names the
                // holder (23 T3, F23): the bare word "paused" used to name both
                // this budget block and an operator pause.
                check("pair/label-blocked",
                      budget_blocked_label(d).find("BLOCKED") !=
                          std::string::npos,
                      "the blocked label must read BLOCKED, never paused");
                check("pair/label-names-holder",
                      budget_blocked_label(d).find(mode_name(active)) !=
                          std::string::npos,
                      "the blocked label must name the jack holder");
                check("pair/label-not-paused",
                      budget_blocked_label(d).find("paused") ==
                          std::string::npos,
                      "the blocked label must not say 'paused' — that is the "
                      "operator-pause state");
            }
        }
    }
    check("pairs/exhaustive", pairs == 100,
          "expected 100 ordered pairs, walked " + std::to_string(pairs));
    // 9 ptrace modes x 9 = 81 refusals; the rest allowed. Asserting the split
    // stops a table that trivially allows (or refuses) everything from passing
    // the per-pair check by agreeing with a broken expectation.
    check("pairs/refused-count", refused == 81,
          "expected 81 refusals, got " + std::to_string(refused));
    check("pairs/allowed-count", allowed == 19,
          "expected 19 allowed, got " + std::to_string(allowed));

    // ---- the two facts the doc calls out by name ---------------------------
    // procs and watch ARE consumers. They are the two most easily forgotten:
    // procs looks like a listing, watch like a breakpoint.
    LiveMode procs, watch, sample, log;
    mode_from_name("procs", &procs);
    mode_from_name("watch", &watch);
    mode_from_name("sample", &sample);
    mode_from_name("log", &log);
    check("procs/consumer", mode_uses_ptrace(procs),
          "procs SEIZEs the descendant tree — it is a consumer");
    check("watch/consumer", mode_uses_ptrace(watch),
          "watch SEIZEs every thread to arm debug registers — it is a "
          "consumer");
    check("procs/scope-is-tree",
          std::string(mode_jack_reason(procs)).find("TREE") !=
              std::string::npos,
          "procs' reason must state the jack is per-TREE, not per-process");

    // A free view neither blocks nor is blocked, in both directions.
    check("sample/never-blocks", budget_can_start(log, {sample}).allowed,
          "a running sample must not block a ptrace view");
    check("sample/never-blocked", budget_can_start(sample, {log}).allowed,
          "a running ptrace view must not block sample");
    check("sample/pairs-with-itself",
          budget_can_start(sample, {sample}).allowed,
          "two out-of-band views can coexist");

    // Nothing active: everything starts.
    for (LiveMode m : modes)
        check("empty/allows", budget_can_start(m, {}).allowed,
              std::string("an idle target must allow ") + mode_name(m));

    // Multiple actives: the FIRST ptrace holder is named, and a free view in
    // the list does not mask it.
    check("multi/blocked", !budget_can_start(log, {sample, watch}).allowed,
          "a ptrace holder anywhere in the active set must block");
    check("multi/names-holder",
          budget_can_start(log, {sample, watch}).blocker == watch,
          "the named blocker must be the ptrace holder, not the free view");

    // ---- 18-T5: the perturbation gate + least-perturbing default -----------
    // The pure model behind the pre-commit confirm (F22). mode_uses_ptrace is
    // what gates whether a Start needs the confirm; mode_perturb_warning is the
    // consequence sentence; the arm64 clause names the detach-fatal kill hazard;
    // and the default resolves to the least-perturbing substrate the host has.
    {
        LiveMode sample, log, stream, dataflow;
        mode_from_name("sample", &sample);
        mode_from_name("log", &log);
        mode_from_name("stream", &stream);
        mode_from_name("dataflow", &dataflow);

        // A perturbing (single-step / ptrace) mode needs the confirm; `sample`
        // (out of band) does not — its warning is empty.
        check("perturb/sample-no-warning",
              mode_perturb_warning(sample, "x86_64").empty(),
              "sample reads out of band — it needs no perturb confirm");
        for (LiveMode m : modes) {
            if (!mode_uses_ptrace(m))
                continue;
            check("perturb/ptrace-has-warning",
                  !mode_perturb_warning(m, "x86_64").empty(),
                  std::string(mode_name(m)) +
                      " single-steps — it must state the consequence");
        }

        // The generic (non-arm64) sentence states the page-dirty / timing cost
        // and steers toward IBS/PT; it does NOT claim the arm64 kill.
        std::string generic = mode_perturb_warning(dataflow, "x86_64");
        check("perturb/generic-page-dirty",
              generic.find("DIRTIES") != std::string::npos &&
                  generic.find("timing") != std::string::npos,
              generic);
        check("perturb/generic-steers-to-ibs-pt",
              generic.find("IBS/PT") != std::string::npos, generic);
        check("perturb/generic-no-arm64-claim",
              generic.find("arm64") == std::string::npos,
              "the x86 sentence must not claim the arm64 termination hazard");

        // The arm64 sentence names the blocking-syscall termination that detach
        // cannot undo — the recorded aarch64 detach-fatal hazard. Both spellings.
        for (const char *arch : {"aarch64", "arm64"}) {
            std::string a = mode_perturb_warning(stream, arch);
            check("perturb/arm64-kill",
                  a.find("TERMINATE") != std::string::npos &&
                      a.find("detach") != std::string::npos,
                  std::string(arch) + ": " + a);
        }

        // The hazard predicate: true only for a ptrace mode ON arm64.
        check("hazard/ptrace-on-arm64",
              mode_arm64_blocking_hazard(stream, "aarch64"),
              "a single-step mode on arm64 carries the blocking-syscall hazard");
        check("hazard/sample-on-arm64-clear",
              !mode_arm64_blocking_hazard(sample, "aarch64"),
              "an out-of-band mode has no single-step hazard");
        check("hazard/ptrace-on-x86-clear",
              !mode_arm64_blocking_hazard(stream, "x86_64"),
              "x86 has a writable trap flag — no detach-fatal hazard");

        // The least-perturbing default: sample where the host has IBS, else the
        // lightest ptrace mode (log — it does not single-step).
        check("default/sample-when-available-unprobed",
              budget_least_perturbing(true, false, false) == sample,
              "with IBS and an unprobed host, the default is the sampler -- "
              "an unprobed host blocks nothing (see mode_host_blocked)");
        check("default/sample-when-available-and-perf-ok",
              budget_least_perturbing(true, true, true) == sample,
              "with IBS and a MEASURED working perf, the default is the "
              "out-of-band sampler");
        check("default/log-otherwise",
              budget_least_perturbing(false, false, false) == log,
              "without IBS the default is the lightest ptrace mode (log)");
        // 2026-08-06 plan, Task 4 fix (coordinator review, Finding 2): THE
        // regression this task exists to close. IBS hardware present
        // (sample_available) is a SUBSTRATE fact and says nothing about
        // PERMISSION -- a host can have the silicon and still MEASURE every
        // perf_event_open refused (this box does). Defaulting to `sample`
        // there would fire and record ZERO events on the very first Start.
        check("default/log-when-sample-would-be-host-blocked",
              budget_least_perturbing(true, true, false) == log,
              "IBS hardware present does not mean perf_event_open works -- "
              "the default must fall back to a mode that actually captures");
        check("default/log-does-not-single-step", mode_uses_ptrace(log),
              "log holds the jack (streams syscalls) but does not single-step");
    }

    // ---- 23-T3: the Queue path fires only on a genuinely free jack ----------
    // A queued want must NOT start while a ptrace blocker is active, and MUST be
    // ready the moment the jack frees — never an auto-swap (the one-jack
    // invariant is never bypassed).
    {
        LiveMode watch, log, sample;
        mode_from_name("watch", &watch);
        mode_from_name("log", &log);
        mode_from_name("sample", &sample);

        // Blocked: another ptrace view holds the jack -> the queued want waits.
        check("queue/not-ready-while-blocked",
              !budget_queue_ready(log, {watch}),
              "a queued want must not fire while a ptrace blocker is active");
        // The blocker stops (active empties) -> the queued want is ready.
        check("queue/ready-when-free", budget_queue_ready(log, {}),
              "a queued want is ready the moment the jack frees");
        // A free-view blocker never blocks the queue (it holds no jack).
        check("queue/free-blocker-ok", budget_queue_ready(log, {sample}),
              "an out-of-band view holds no jack, so it never blocks a queue");
        // The queue-ready predicate is EXACTLY budget_can_start's allow — it can
        // never bypass the budget (no auto-swap masquerading as a queue).
        for (LiveMode active : modes)
            for (LiveMode want : modes)
                check("queue/matches-budget",
                      budget_queue_ready(want, {active}) ==
                          budget_can_start(want, {active}).allowed,
                      "the queue must fire on exactly the budget's allow — never "
                      "a hidden swap");
    }

    // ---- the picker's reading order + the per-mode visualizations (R11) -----
    // patch_bay_order() is a UI-only permutation of all_modes() that leads with
    // `auto`; the wire order (all_modes) is unchanged. mode_visualizations names
    // what each mode's capture can fill, and must stay consistent with the
    // presence rules — most importantly, the statistical `sample` never offers the
    // exact Loom / Scrubber.
    {
        const std::vector<LiveMode> &order = patch_bay_order();
        check("order/auto-first", !order.empty() && order.front() == autom,
              "the picker must lead with auto");
        check("order/same-size", order.size() == modes.size(),
              "the picker order is a permutation of all_modes — same size");
        // Every wire mode appears exactly once (a true permutation, nothing lost).
        for (LiveMode m : modes) {
            int n = 0;
            for (LiveMode o : order)
                if (o == m)
                    n++;
            check("order/permutation",
                  n == 1, std::string("mode `") + mode_name(m) +
                              "` must appear exactly once in the picker order");
        }

        // Every mode advertises at least one visualization — EXCEPT `stream`
        // and `graph` (2026-08-07 plan, Task 2: `graph` was the row nobody
        // re-checked when sample/log/trace/stream lost this same promise in
        // 2026-08-05), whose events no view in this build reads (there is no
        // by_kind("stream") or by_kind("graph") consumer anywhere in
        // desktop/src, and observer_has_any does not count them). An empty
        // list is that fact stated, and the picker renders it as a sentence.
        // Pinning WHICH modes may be empty is a stronger contract than "all
        // non-empty": it fails both if a third mode goes empty and if stream
        // or graph regains a phantom entry that nothing reads.
        for (LiveMode m : modes) {
            const bool empty = mode_visualizations(m).empty();
            const std::string name = mode_name(m);
            const bool may_be_empty = name == "stream" || name == "graph";
            check("viz/empty-only-stream-or-graph", empty == may_be_empty,
                  std::string("mode `") + mode_name(m) + "` " +
                      (empty ? "names no visualization, but only `stream`/"
                               "`graph` may — every other mode fills something"
                             : "names a visualization, but `stream`/`graph` "
                               "events have no reader in this build"));
        }

        // A statistical sampler cannot offer an EXACT Loom or Scrubber (it never
        // single-steps) — the one mapping that would be a lie if it drifted.
        LiveMode sample;
        mode_from_name("sample", &sample);
        for (const char *v : mode_visualizations(sample)) {
            check("viz/sample-no-exact-loom",
                  std::string(v).find("Loom") == std::string::npos,
                  "the statistical sampler must not advertise the exact Loom");
            check("viz/sample-no-scrubber",
                  std::string(v).find("Scrubber") == std::string::npos,
                  "the statistical sampler must not advertise the Scrubber");
        }
        // auto is the fullest exact deck — it DOES offer the Loom and Scrubber.
        bool auto_loom = false, auto_scrub = false;
        for (const char *v : mode_visualizations(autom)) {
            if (std::string(v).find("Loom") != std::string::npos)
                auto_loom = true;
            if (std::string(v).find("Scrubber") != std::string::npos)
                auto_scrub = true;
        }
        check("viz/auto-has-loom-and-scrubber", auto_loom && auto_scrub,
              "auto captures exact per-step values — it offers Loom + Scrubber");
    }

    // ---- the advertise-vs-fill contract -------------------------------------
    //
    // mode_visualizations is a CLAIM about what another TU records. Nothing
    // links it to the producer, so it drifts silently and a tooltip promises a
    // tab that never appears. These checks pin each row against the event kinds
    // the mode actually emits (cli/asmspy.c's serve_* sinks), naming the
    // producer site in the failure text so a future edit knows where to look.
    {
        auto viz_has = [](LiveMode m, const char *want) {
            for (const char *v : mode_visualizations(m))
                if (std::string(v) == want)
                    return true;
            return false;
        };

        // A mode fills the 3D overview iff the serve host arms a codeimage for
        // it — ViewId::Scene3D presence is !regions_from_codeimage(r).empty()
        // (ui/view_presence.cpp). asmspy.c arms it for tree/trace/dataflow/auto
        // only (asmspy.c:4039, :4062, :4067, :4155).
        const struct {
            const char *name;
            bool codeimage;
        } kCodeimage[] = {
            {"tree", true},   {"trace", true},  {"dataflow", true},
            {"auto", true},   {"log", false},   {"stream", false},
            {"graph", false}, {"procs", false}, {"sample", false},
            {"watch", false},
        };
        for (const auto &row : kCodeimage) {
            LiveMode m;
            check("viz/3d/mode-name", mode_from_name(row.name, &m),
                  std::string("unknown mode name in this test: ") + row.name);
            if (!mode_from_name(row.name, &m))
                continue;
            check("viz/3d-vs-codeimage",
                  viz_has(m, "3D overview") == row.codeimage,
                  std::string(row.name) +
                      (row.codeimage
                           ? " arms a codeimage (asmspy.c serve_codeimage_arm)"
                             " so it fills the 3D overview, but its row does"
                             " not say so"
                           : " arms no codeimage, so the 3D overview tab can"
                             " never appear — its row must not promise it"));
        }

        // The Slice and the Timeline are both built from df_step (doc/streams.cpp
        // decodes Streams::df from df_step/df_edge only), so only the two
        // dataflow-bearing modes may name them.
        const char *kDfOnly[] = {"Slice", "Timeline"};
        const struct {
            const char *name;
            bool df;
        } kDataflow[] = {
            {"dataflow", true}, {"auto", true},   {"log", false},
            {"stream", false},  {"trace", false}, {"tree", false},
            {"graph", false},   {"procs", false}, {"sample", false},
            {"watch", false},
        };
        for (const auto &row : kDataflow) {
            LiveMode m;
            if (!mode_from_name(row.name, &m))
                continue;
            for (const char *v : kDfOnly)
                check("viz/df-vs-dfstep", viz_has(m, v) == row.df,
                      std::string(row.name) + "/" + v + ": emits " +
                          (row.df ? "" : "no ") +
                          "df_step, so it must " + (row.df ? "" : "not ") +
                          "advertise \"" + v + "\"");
        }
    }

    // ---- the substrate sweep (59 T1) ---------------------------------------
    //
    // Each 3D substrate comes from a different engine and the host runs ONE at
    // a time, so no single capture fills more than one. The sweep runs them
    // back to back — and is only worth running into a session recording,
    // because the desktop's live tab shows exactly one Recording.
    {
        const std::vector<LiveMode> &legs = sweep_legs(true);
        check("sweep/legs-count", legs.size() == 3,
              "one leg per substrate a LIVE capture can fill (tree, trace, "
              "dataflow); got " + std::to_string(legs.size()));
        bool tree = false, trace = false, df = false, diverge = false;
        for (LiveMode m : legs) {
            const std::string n = mode_name(m);
            tree = tree || n == "tree";
            trace = trace || n == "trace";
            df = df || n == "dataflow";
        }
        check("sweep/legs-are-the-three-engines", tree && trace && df,
              "the ribbon needs `call` (tree), the invocation stack needs a "
              "`coverage` block set (trace), the prism needs wide register "
              "writes (dataflow) — three engines, three legs");
        (void)diverge;

        // EVERY leg of EVERY shape must arm a codeimage, or the address plane
        // never places and the pane the sweep exists for cannot host the scene
        // it captured. Same for the jack: every leg is a ptrace mode, which is
        // exactly why they must run in SEQUENCE.
        for (bool have_region : {true, false})
            for (LiveMode m : sweep_legs(have_region)) {
                bool has3d = false;
                for (const char *v : mode_visualizations(m))
                    has3d = has3d || std::string(v) == "3D overview";
                check(
                    "sweep/every-leg-places-the-plane", has3d,
                    std::string("leg `") + mode_name(m) +
                        "` does not fill the 3D overview, so it cannot belong "
                        "to a sweep whose whole point is that pane");
                check("sweep/legs-need-the-jack", mode_uses_ptrace(m),
                      std::string("leg `") + mode_name(m) +
                          "` does not hold the ptrace jack, so a sweep would "
                          "not need to serialise it");
            }
    }
    {
        // The AUTO-LED shape. `auto` is the one capture mode that needs no
        // region — it SAMPLES for one — and it is the front door the patch bay
        // offers first. From it, three of the five substrates were unreachable:
        // an `auto` capture records `codeimage` + `df_step` and nothing else, so
        // the invocation stack (`coverage`) and the module excursion ribbon
        // (`call`) stayed disabled, and the sweep that would have filled them
        // refused to start without a hand-named region — which an `auto`
        // operator by construction does not have.
        //
        // So with no region named the sweep LEADS with `auto`: that leg is the
        // dataflow engine (the serve host runs asmspy_engine_dataflow for
        // SM_AUTO), and the region it picks is what the scoped leg behind it
        // single-steps.
        const std::vector<LiveMode> &led = sweep_legs(false);
        check("sweep/auto/leads-with-auto",
              !led.empty() && std::string(mode_name(led[0])) == "auto",
              "the auto leg must run FIRST — it is what samples for the region "
              "the scoped leg behind it single-steps");
        bool has_tree = false, has_trace = false;
        for (LiveMode m : led) {
            const std::string n = mode_name(m);
            has_tree = has_tree || n == "tree";
            has_trace = has_trace || n == "trace";
        }
        check("sweep/auto/covers-ribbon-and-stack", has_tree && has_trace,
              "the ribbon still needs `call` (tree) and the invocation stack "
              "still needs a `coverage` block set (trace) — leading with auto "
              "replaces the dataflow leg, not those two");
        check(
            "sweep/auto/no-second-dataflow-leg",
            led.size() == 3 && std::string(mode_name(led[1])) != "dataflow" &&
                std::string(mode_name(led[2])) != "dataflow",
            "`auto` IS the dataflow engine with a picker in front of it, so a "
            "separate dataflow leg would re-capture the same substrate at the "
            "cost of a whole extra leg");

        // The ordering invariant that makes the hand-off possible at all: the
        // region does not exist until the auto leg's `pick` lands, so every leg
        // that needs one must come AFTER it.
        size_t auto_at = led.size();
        for (size_t i = 0; i < led.size(); i++)
            if (led[i] == LiveMode::Auto)
                auto_at = i;
        for (size_t i = 0; i < led.size(); i++)
            if (mode_needs_region(led[i]))
                check("sweep/auto/scoped-leg-runs-after-the-pick", i > auto_at,
                      std::string("leg `") + mode_name(led[i]) +
                          "` single-steps ONE region, but it is placed before "
                          "the auto leg that picks it — the host would refuse "
                          "it with no region at all");
    }
    {
        // The refusals, in the order an operator hits them.
        check(
            "sweep/blocked-no-host",
            sweep_blocked(false, false, false, false, false).find("connect") !=
                std::string::npos,
            "with no host the reason must say to connect");
        check("sweep/blocked-no-pid",
              sweep_blocked(true, false, false, false, false).find("process") !=
                  std::string::npos,
              "with no pid the reason must say to select one");
        // THE load-bearing refusal: without --record each leg lands in its own
        // Recording and the live tab shows one, so a sweep would cost three
        // captures and change nothing the pane can show.
        check("sweep/blocked-no-recording",
              sweep_blocked(true, true, false, false, false)
                      .find("record the whole session") != std::string::npos,
              "a sweep without a session recording gains NOTHING — the reason "
              "must say so, and say where to turn it on");
        // NOT a refusal any more, and this is the whole point: an un-named
        // region used to block the sweep, which is what left every substrate
        // but the address plane unreachable from `auto`.
        check(
            "sweep/no-region-is-not-a-refusal",
            sweep_blocked(true, true, true, false, false).empty(),
            "an un-named region must NOT block a sweep — the auto leg samples "
            "for one, and refusing here is what made the invocation stack and "
            "the module ribbon unreachable from the `auto` front door");
        check(
            "sweep/blocked-jack-held",
            sweep_blocked(true, true, true, false, true).find("ptrace jack") !=
                std::string::npos,
            "a sweep may not bypass the one-jack rule");
        check("sweep/blocked-already",
              !sweep_blocked(true, true, true, true, false).empty(),
              "a second sweep must not start on top of a running one");
        check("sweep/ready",
              sweep_blocked(true, true, true, false, false).empty(),
              "host + pid + recording, jack free, no sweep running is "
              "everything a sweep needs");
        // No refusal may be blank — the same D7 rule the rest of this file pins.
        const std::string all[] = {
            sweep_blocked(false, false, false, false, false),
            sweep_blocked(true, false, false, false, false),
            sweep_blocked(true, true, false, false, false),
            sweep_blocked(true, true, true, true, false),
            sweep_blocked(true, true, true, false, true),
        };
        for (const std::string &r : all)
            check("sweep/every-refusal-names-a-fix", r.size() > 20,
                  "a refusal must name the thing to fix, never a bare "
                  "'unavailable': got \"" + r + "\"");
    }
    {
        // The standing line under the button. It is derived from sweep_legs, so
        // the two shapes can never describe each other — and the auto-led one
        // must say that the FIRST leg is what finds the region, or the operator
        // reads an empty region box as a bug.
        const std::string named = sweep_plan(true, 400);
        const std::string led = sweep_plan(false, 400);
        check("sweep/plan/named-names-its-legs",
              named.find("tree") != std::string::npos &&
                  named.find("trace") != std::string::npos &&
                  named.find("dataflow") != std::string::npos,
              "a plan that does not name its legs cannot be checked against "
              "what runs: got \"" +
                  named + "\"");
        check("sweep/plan/named-names-the-bound",
              named.find("400") != std::string::npos,
              "every leg is bounded, and an operator who does not know the "
              "bound cannot tell a finished leg from a stalled one");
        // Tied to the LEG ORDER, not to the prose: the explanatory tail names
        // `auto` too, so a plan that merely mentions it would pass while
        // printing the wrong sequence.
        check("sweep/plan/auto-leads", led.rfind("auto ->", 0) == 0,
              "the auto-led plan must OPEN with the leg that leads: got \"" +
                  led + "\"");
        check("sweep/plan/auto-says-where-the-region-comes-from",
              led.find("sample") != std::string::npos ||
                  led.find("pick") != std::string::npos,
              "with no region named the plan must say the first leg FINDS one, "
              "not leave the empty region box unexplained: got \"" +
                  led + "\"");
        check("sweep/plan/shapes-differ", named != led,
              "two different leg lists must not print the same plan");
    }

    // 2026-08-06 plan, Task 12 (M13): the completion note is scored against
    // what LANDED, decoded from real recordings — never a fixed four-substrate
    // claim keyed only on the leg count. sweep_result_note is the pure scorer;
    // the door (test_shell) drives the decode from s.session.recordings().
    {
        const std::string marker = "Not this time";

        // Split a note into its "claims landed" half and its "names what did
        // not, and why" half. A note with nothing absent has no marker at
        // all, and the whole string is the landed half.
        auto landed_half = [&](const std::string &note) {
            size_t p = note.find(marker);
            return p == std::string::npos ? note : note.substr(0, p);
        };
        auto absent_half = [&](const std::string &note) {
            size_t p = note.find(marker);
            return p == std::string::npos ? std::string() : note.substr(p);
        };

        // All four landed: nothing absent, every label claimed, and the
        // measured defect's own routine name must not leak in when it is not
        // needed.
        {
            std::string note =
                sweep_result_note(true, true, true, true, "blend_tile");
            check("note/all-landed/no-absent-marker",
                  note.find(marker) == std::string::npos,
                  "every substrate present must produce no absent clause: "
                  "got \"" +
                      note + "\"");
            for (const char *label :
                 {"address plane", "invocation stack",
                  "module excursion ribbon", "SIMD lane prism"})
                check("note/all-landed/names-it",
                      note.find(label) != std::string::npos,
                      std::string("a fully-landed sweep must name `") + label +
                          "`: got \"" + note + "\"");
        }

        // THE measured defect, reproduced directly: identical capture (plane/
        // stack/ribbon all true — every leg completed and wrote its events),
        // prism false because the picked routine is scalar. The note must
        // still claim the three real substrates, must NOT claim the prism,
        // and must name the ROUTINE — not the capture — as the reason.
        {
            std::string note =
                sweep_result_note(true, true, true, false, "entered_often");
            std::string landed = landed_half(note);
            std::string absent = absent_half(note);
            check("note/prism-absent/still-claims-plane",
                  landed.find("address plane") != std::string::npos,
                  "a leg that DID write codeimage must still be named landed: "
                  "got \"" +
                      note + "\"");
            check("note/prism-absent/still-claims-stack",
                  landed.find("invocation stack") != std::string::npos,
                  "got \"" + note + "\"");
            check("note/prism-absent/still-claims-ribbon",
                  landed.find("module excursion ribbon") != std::string::npos,
                  "got \"" + note + "\"");
            check("note/prism-absent/does-not-claim-it",
                  landed.find("SIMD lane prism") == std::string::npos,
                  "the prism must not be named among what landed when it did "
                  "not: got \"" +
                      note + "\"");
            check("note/prism-absent/names-it-somewhere",
                  absent.find("SIMD lane prism") != std::string::npos,
                  "the absence must still be SAID, with a reason, never "
                  "silently dropped: got \"" +
                      note + "\"");
            check("note/prism-absent/names-the-routine",
                  absent.find("entered_often") != std::string::npos,
                  "M13: the reason must name the PICKED ROUTINE, not the "
                  "capture — got \"" +
                      note + "\"");
            check("note/prism-absent/does-not-blame-the-capture",
                  absent.find("capture failed") == std::string::npos &&
                      absent.find("capture did not") == std::string::npos,
                  "the capture completed fine; only the routine's own "
                  "instructions decide the prism — got \"" +
                      note + "\"");
        }

        // Every substrate is independently absent-able with its own reason —
        // not just the prism. Sweeping one flag false at a time over an
        // otherwise-all-true note must move exactly that label from the
        // landed half to the absent half, with SOME reason attached (never a
        // bare omission).
        {
            struct Case {
                const char *label;
                std::string note;
            };
            Case cases[] = {
                {"address plane",
                 sweep_result_note(false, true, true, true, "blend_tile")},
                {"invocation stack",
                 sweep_result_note(true, false, true, true, "blend_tile")},
                {"module excursion ribbon",
                 sweep_result_note(true, true, false, true, "blend_tile")},
            };
            for (const Case &c : cases) {
                std::string landed = landed_half(c.note);
                std::string absent = absent_half(c.note);
                check(
                    (std::string("note/one-absent/") + c.label).c_str(),
                    landed.find(c.label) == std::string::npos &&
                        absent.find(c.label) != std::string::npos,
                    std::string("`") + c.label +
                        "` must move to the absent half, named with a reason, "
                        "when its own flag is false: got \"" +
                        c.note + "\"");
                // The other three stay claimed — one false flag must not
                // erase the substrates that DID land.
                for (const char *other :
                     {"address plane", "invocation stack",
                      "module excursion ribbon", "SIMD lane prism"}) {
                    if (other == std::string(c.label))
                        continue;
                    check("note/one-absent/others-still-landed",
                          landed.find(other) != std::string::npos,
                          std::string("`") + other +
                              "` was true and must stay in the landed half "
                              "even though `" +
                              c.label + "` was false: got \"" + c.note + "\"");
                }
            }
        }

        // Nothing landed at all: every substrate absent, each with its own
        // reason, and the note must not claim a single one of them.
        {
            std::string note =
                sweep_result_note(false, false, false, false, "sort_batch");
            std::string landed = landed_half(note);
            for (const char *label :
                 {"address plane", "invocation stack",
                  "module excursion ribbon", "SIMD lane prism"})
                check("note/none-landed/claims-nothing",
                      landed.find(label) == std::string::npos,
                      std::string("`") + label +
                          "` is false and must not be claimed landed: got \"" +
                          note + "\"");
        }

        // An empty `picked` (a hand-typed base+len region carries no symbol)
        // must fall back to plain prose rather than printing empty backticks.
        {
            std::string note = sweep_result_note(true, true, true, false, "");
            check("note/prism-absent/empty-picked-no-dangling-backticks",
                  note.find("``") == std::string::npos,
                  "an unknown routine name must fall back to prose, not empty "
                  "backticks: got \"" +
                      note + "\"");
        }
    }

    // 2026-08-06 plan, Task 4, NARROWED by the final review (finding 2): a mode
    // the host cannot run is refused BEFORE Start -- and after Tasks 5 and 7
    // that is `sample` ALONE. `auto`'s sampler chain falls through IBS ->
    // sw-clock -> a perf-free ptrace region picker, so a perf refusal says
    // nothing about it; every other mode is pure ptrace and runs with perf
    // locked down, which is measured, not assumed.
    {
        const std::string why = "perf_event_open refused (Permission denied)";
        LiveMode a, s, d, t, l;
        mode_from_name("auto", &a);
        mode_from_name("sample", &s);
        mode_from_name("dataflow", &d);
        mode_from_name("trace", &t);
        mode_from_name("log", &l);

        // THE REGRESSION THIS FILE EXISTS TO CATCH NOW. The inverse of what it
        // pinned before: the branch taught `auto` to capture with no perf at
        // all, and blocking it here greyed the radio, Start, Swap, Queue and
        // the auto-led sweep on exactly the host that capture was built for --
        // while the same capture by hand succeeded, in a transcript this
        // branch's own docs print as evidence.
        check("host/auto-NOT-blocked-when-perf-refused",
              mode_host_blocked(a, true, false, why).empty(),
              "`auto` must NOT be refused on a perf-locked-down host: its "
              "sampler chain ends in a perf-free ptrace region picker, which "
              "is the whole point of the 2026-08-06 branch");
        check("host/sample-blocked",
              !mode_host_blocked(s, true, false, why).empty(),
              "`sample` IS the out-of-band sampler, and it has no fallback");
        // NOT find("perf_event_open") -- the fallback literal in
        // mode_host_blocked contains that word too, so that check passed even
        // when the reason was dropped (the review called it a clean tautology).
        // Assert the caller's OWN string survives.
        check("host/sample-reason-carries-the-measured-text",
              mode_host_blocked(s, true, false, why).find(why) !=
                  std::string::npos,
              "the refusal must carry the producer's own reason verbatim -- it "
              "is the only text that names the fix");
        // And it must point at the path that still works, or a blocked
        // operator reads a dead end.
        check("host/sample-reason-names-the-working-path",
              mode_host_blocked(s, true, false, why).find("`auto`") !=
                  std::string::npos,
              "the one sentence a blocked operator reads must name the mode "
              "that DOES capture here");

        for (LiveMode m : {a, d, t, l})
            check("host/non-sample-modes-unaffected",
                  mode_host_blocked(m, true, false, why).empty(),
                  std::string("`") + mode_name(m) +
                      "` needs no perf_event_open; refusing it would hide a "
                      "path that captures on a stock host");

        check("host/perf-ok-blocks-nothing",
              mode_host_blocked(s, true, true, "").empty(),
              "a host whose perf opens blocks nothing");
        // THE rule that keeps this honest: only a MEASURED refusal may grey a
        // control. An unprobed host is a note somewhere else, never a disabled
        // radio -- a control greyed on a guess reads as a broken build.
        check("host/unprobed-blocks-nothing",
              mode_host_blocked(s, false, false, why).empty(),
              "an unprobed host must block nothing: we have not asked, and "
              "CAP_PERFMON on the asmspy binary overrides the sysctl anyway");
    }

    // 2026-08-07 plan, Task 2: `graph` advertised a facet with no consumer.
    // grep finds no by_kind("graph") reader anywhere in desktop/src —
    // views/graph_nav.cpp is navigation over an existing model, not a reader
    // of the wire kind. `stream` already returns {} for exactly this reason
    // (budget.cpp's own comment); `graph` was the row nobody re-checked.
    {
        LiveMode g;
        check("viz/graph/known", mode_from_name("graph", &g), "graph parses");
        check("viz/graph/promises-nothing", mode_visualizations(g).empty(),
              "a `graph` capture records `graph` events and nothing in this "
              "build reads them, so naming a view here is the same "
              "promise-what-you-cannot-fill defect the other rows lost");

        // The control, so this cannot be made green by emptying every row:
        // the modes that DO fill something must still say so.
        LiveMode t, a;
        mode_from_name("tree", &t);
        mode_from_name("auto", &a);
        check("viz/tree/still-promises", !mode_visualizations(t).empty(),
              "`tree` fills the Observer's call-tree facet and the 3D "
              "overview — emptying it would hide a real capability");
        check("viz/auto/still-promises", !mode_visualizations(a).empty(),
              "`auto` fills the whole exact deck");
    }

    if (failures) {
        std::fprintf(stderr, "test_budget: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_budget: all checks passed (%d mode pairs)\n", pairs);
    return 0;
}
