// budget.cpp — the D6 concurrency budget, table-driven from engine facts.
// See budget.h for why this is a pure module.
#include "live/budget.h"

namespace asmdesk {

namespace {

struct Row {
    LiveMode mode;
    const char *name;
    bool ptrace; // occupies the target's single ptrace jack
    const char *why;
};

// One row per mode. `why` is the MEASURED structural reason — what the engine
// actually does — because "unavailable" sends an operator nowhere while "the
// topology view has SEIZEd this process tree" sends them to the stop button.
const Row kRows[] = {
    {LiveMode::Log, "log", true,
     "SEIZEs every thread of the target to stream its syscalls"},
    {LiveMode::Stream, "stream", true,
     "SEIZEs every thread and single-steps it"},
    {LiveMode::Trace, "trace", true,
     "SEIZEs every thread and plants an entry breakpoint in the region"},
    {LiveMode::Dataflow, "dataflow", true,
     "SEIZEs every thread and single-steps the region for operand values"},
    {LiveMode::Tree, "tree", true,
     "SEIZEs every thread and single-steps it to track call depth"},
    {LiveMode::Graph, "graph", true,
     "SEIZEs every thread and single-steps it to build the call graph"},
    // procs is the widest consumer of the three shapes: it follows FORK/VFORK
    // as well as CLONE, so the jack it takes is the whole descendant tree's,
    // not just this process's. That is why the budget is per TREE.
    {LiveMode::Procs, "procs", true,
     "SEIZEs the whole descendant process TREE (it follows forks, not just "
     "threads)"},
    // The two free slots. Out of band through perf: nothing is attached and the
    // target runs unperturbed, which is the entire point of these views.
    {LiveMode::Sample, "sample", false,
     "reads AMD IBS-Op samples OUT OF BAND (no ptrace, no single-step) — it "
     "runs alongside any other view"},
    {LiveMode::Watch, "watch", true,
     "SEIZEs every thread to arm a hardware debug register in each"},
    // auto samples out of band to PICK a region and then runs the data-flow
    // engine on it. The sampling half is free; the capture half is not, and a
    // mode is judged by the jack it ends up holding.
    {LiveMode::Auto, "auto", true,
     "samples out of band to pick a region, then SEIZEs every thread to "
     "capture it"},
};

const Row *row_for(LiveMode m) {
    for (const Row &r : kRows)
        if (r.mode == m)
            return &r;
    return &kRows[0]; // unreachable: every enumerator has a row
}

} // namespace

const char *mode_name(LiveMode m) { return row_for(m)->name; }

bool mode_from_name(const std::string &name, LiveMode *out) {
    for (const Row &r : kRows)
        if (name == r.name) {
            if (out)
                *out = r.mode;
            return true;
        }
    return false;
}

const std::vector<LiveMode> &all_modes() {
    static const std::vector<LiveMode> v = [] {
        std::vector<LiveMode> m;
        for (const Row &r : kRows)
            m.push_back(r.mode);
        return m;
    }();
    return v;
}

const std::vector<LiveMode> &patch_bay_order() {
    // `auto` first (39-auto-capture-reliability.md): the fullest, self-scoping
    // choice an un-named target should reach for. Then the two scoped single-step
    // engines (dataflow / trace), then the whole-process streamers and the
    // out-of-band samplers. A UI-only permutation of all_modes() — the wire order
    // (kRows) is unchanged.
    static const std::vector<LiveMode> v = {
        LiveMode::Auto,  LiveMode::Dataflow, LiveMode::Trace,
        LiveMode::Log,   LiveMode::Stream,   LiveMode::Tree,
        LiveMode::Graph, LiveMode::Procs,    LiveMode::Sample,
        LiveMode::Watch,
    };
    return v;
}

std::vector<const char *> mode_visualizations(LiveMode m) {
    // The view names a mode's capture can fill, kept in step with
    // view_presence.cpp: the Slice needs df_step; the Loom + Scrubber need EXACT
    // per-step values (so a statistical sampler can never offer them); the 3D
    // overview needs codeimage regions; the Observer deck holds the syscall / tree
    // / graph / watch / hot-edge facets.
    switch (m) {
    case LiveMode::Auto:
        // dataflow-with-a-picker at full detail: the whole exact deck.
        return {"Slice", "Loom", "Scrubber", "Timeline", "3D overview",
                "Observer"};
    case LiveMode::Dataflow:
        return {"Slice", "Loom", "Scrubber", "Timeline", "3D overview"};
    case LiveMode::Trace:
        // `trace`/`coverage` fill the Canvas (dt_canvas_build reads the region
        // snapshot) and — via the serve host's code image — the 3D overview's
        // address plane and invocation stack. They carry no df_step, so the
        // OPERAND Timeline is not among them: that tab is always present and
        // would simply render zero rows.
        return {"Canvas", "3D overview", "Observer (codeimage)"};
    case LiveMode::Log:
        return {"Observer (syscalls)"};
    case LiveMode::Stream:
        // Deliberately EMPTY, and the one mode allowed to be (test_budget pins
        // which). A `stream` capture records `stream` events, and nothing in
        // this build reads them: there is no by_kind("stream") consumer in
        // desktop/src, and observer_has_any does not count them. Naming a view
        // here would be the same promise-what-you-cannot-fill defect the other
        // rows just lost.
        return {};
    case LiveMode::Tree:
        // A tree session has no region of its own, but the serve host arms a
        // code image over the executable's text precisely so this pane can host
        // the module-excursion ribbon (cli/asmspy.c:4031-4040). Saying only
        // "Observer" here is what closed that pane.
        return {"Observer (call tree)", "3D overview"};
    case LiveMode::Graph:
        // Deliberately EMPTY, and the SECOND mode allowed to be (test_budget
        // pins which). A `graph` capture records `graph` events, and nothing
        // in this build reads them: there is no by_kind("graph") consumer in
        // desktop/src — views/graph_nav.cpp navigates an existing model, it
        // does not decode the wire kind. This row promised "Observer (call
        // graph)" until 2026-08-07; that facet does not exist. Naming one here
        // is the promise-what-you-cannot-fill defect the sample/log/trace/
        // stream rows lost in the 2026-08-05 plan.
        return {};
    case LiveMode::Procs:
        return {"Observer (process tree)"};
    case LiveMode::Sample:
        // Out-of-band statistical: hot edges, and NEVER an exact Loom /
        // Scrubber (it does not single-step). Nor the 3D overview: the
        // SM_SAMPLE serve branch arms no code image, so that tab — presence-
        // gated on codeimage regions — never appears for a sample capture.
        return {"Observer (hot edges)"};
    case LiveMode::Watch:
        return {"Observer (watchpoints)"};
    }
    return {};
}

const std::vector<LiveMode> &sweep_legs(bool have_region) {
    static const std::vector<LiveMode> named = {LiveMode::Tree, LiveMode::Trace,
                                                LiveMode::Dataflow};
    // Auto FIRST: it is the leg that samples for the region, and `trace` behind
    // it cannot start without one. `tree` sits in the middle because it needs no
    // region at all — so a sweep whose auto leg picked nothing still lands the
    // module ribbon before it has to stop.
    static const std::vector<LiveMode> led = {LiveMode::Auto, LiveMode::Tree,
                                              LiveMode::Trace};
    return have_region ? named : led;
}

std::string sweep_plan(bool have_region, long max_cap) {
    std::string legs;
    for (LiveMode m : sweep_legs(have_region))
        legs += (legs.empty() ? "" : " -> ") + std::string(mode_name(m));
    // doc 68 T3: the unit, and the consequence. This line said "N events each"
    // until 2026-08-08, and both halves of that were misleading. `max` is the
    // engine's own count of the thing the leg produces, which for the two
    // single-step legs — the ones that fill the Timeline, Loom and Scrubber — is
    // STEPS: a `--dataflow --max=400` capture measures exactly 400 `df_step` and
    // 1628 events, so "400 events" understates it about fourfold. And a leg that
    // stops at its cap writes `truncated: true` into the footer, which every
    // downstream banner then reports; an operator who was not told the sweep
    // caps its legs reads that as a fault in the capture.
    legs += ", " + std::to_string(max_cap) +
            " steps each (the single-step legs) — a leg that reaches its cap "
            "stops there and its recording is flagged TRUNCATED";
    if (have_region)
        return legs;
    // The empty region box is not a missing setting here — it is what selects
    // this shape. Say so, and say which leg fills it in.
    return legs +
           " — no region named, so the `auto` leg samples for one and the "
           "`trace` leg single-steps whatever it picks";
}

std::string sweep_blocked(bool host_started, bool have_pid, bool recording,
                          bool already_sweeping, bool jack_held) {
    if (!host_started)
        return "connect to a serve host first — a sweep drives the same "
               "protocol the patch bay does";
    if (!have_pid)
        return "select a process first: a sweep captures one target";
    if (!recording)
        return "turn on \"record the whole session to one file\" in Connect: "
               "each leg is its own Recording and the live tab shows one, so "
               "only that file carries them together — which is what lets the "
               "3D pane offer more than one substrate";
    if (already_sweeping)
        return "a sweep is already running";
    if (jack_held)
        return "stop the running capture first — the target has one ptrace "
               "jack and a sweep needs it for each leg in turn";
    return "";
}

namespace {
// Join with ", " between interior items and " and " before the last — plain
// English, not a bare bullet dump, because this reads as prose in the door.
std::string join_and(const std::vector<std::string> &items) {
    std::string s;
    for (size_t i = 0; i < items.size(); i++) {
        if (i == 0)
            s = items[i];
        else if (i + 1 == items.size())
            s += " and " + items[i];
        else
            s += ", " + items[i];
    }
    return s;
}
} // namespace

std::string sweep_result_note(bool plane, bool stack, bool ribbon, bool prism,
                              const std::string &picked) {
    // The prism's OWN reason names the routine (M13): the other three are
    // absent only when a leg's events never arrived on the wire — a fact
    // about the capture, so they keep the event-kind reason. See budget.h for
    // the measurement (8-11 writes over blend_tile, 0 over entered_often,
    // byte-identical flags) that is why the prism cannot share that wording.
    const std::string prism_who =
        picked.empty() ? "the routine it captured" : "`" + picked + "`";
    struct Row {
        bool present;
        const char *label;
        std::string absent_reason;
    };
    const Row rows[] = {
        {plane, "the address plane", "no `codeimage` events landed"},
        {stack, "the invocation stack", "no `coverage` block set landed"},
        {ribbon, "the module excursion ribbon", "no `call` events landed"},
        {prism, "the SIMD lane prism",
         prism_who + " writes no vector registers — the capture ran fine, the "
                     "routine just has nothing wide to record"},
    };

    std::vector<std::string> landed, absent;
    for (const Row &r : rows) {
        if (r.present)
            landed.push_back(r.label);
        else
            absent.push_back(std::string(r.label) + " (" + r.absent_reason +
                             ")");
    }

    std::string msg = "sweep complete — ";
    msg += landed.empty() ? "every leg ran, but no substrate landed this run"
                          : join_and(landed) + " landed";
    msg += ".";
    if (!absent.empty())
        // "Not this time" is the load-bearing marker test_budget splits on:
        // never claim a substrate past this point.
        msg += " Not this time: " + join_and(absent) + ".";
    msg += " Disconnect, then File > Open the recorded file to see what "
           "landed together. The divergence worldline needs a SECOND "
           "recording — sweep again and attach it with `d`.";
    return msg;
}

bool mode_uses_ptrace(LiveMode m) { return row_for(m)->ptrace; }

bool mode_needs_region(LiveMode m) {
    // trace and dataflow single-step ONE scoped region, so the serve host demands
    // a base+len or a func name (serve_resolve_region, cli/asmspy.c) — starting
    // one without a region is rejected. `auto` is dataflow-with-a-picker: it finds
    // its own region via the sampler, so it needs none; every whole-process mode
    // (log/stream/tree/graph/procs/sample/watch) traces the whole task.
    return m == LiveMode::Trace || m == LiveMode::Dataflow;
}

const char *mode_jack_reason(LiveMode m) { return row_for(m)->why; }

std::string mode_host_blocked(LiveMode want, bool perf_probed, bool perf_ok,
                              const std::string &perf_reason) {
    // Only a MEASURED refusal may grey a control (see the header). Not asking
    // is not a refusal, and CAP_PERFMON on the asmspy binary overrides the
    // sysctl -- so an unprobed host, or a host we could not probe, blocks
    // nothing and says so elsewhere.
    if (!perf_probed || perf_ok)
        return std::string();
    // `sample` ONLY, and the narrowing is the point (2026-08-06 final review,
    // finding 2). `auto` was blocked here too, and that was TRUE when this
    // predicate was written at Task 4 and FALSE from Task 5 onward: the sampler
    // chain now falls through IBS -> sw-clock -> the perf-free ptrace picker
    // (cli/asmspy_ptracesample.c), which issues no perf_event_open at all, and
    // Task 7 wired it as the last rung of `auto`. The GUI sends no `sampler`
    // key, so serve_parse_start leaves SAMPLER_AUTO and the chain reaches that
    // rung — MEASURED on a stock Ubuntu host at perf_event_paranoid=4, which is
    // the host this whole branch exists to support, and whose successful
    // transcript docs/getting-started/host-setup.md rung 1 prints.
    //
    // So blocking `auto` greyed the radio, Start, Swap, Queue and the auto-led
    // substrate sweep on exactly the host where the capture works, and sent the
    // operator to the pre-branch workaround. `sample` IS the out-of-band
    // sampler; it has no fallback, so it is the one mode a measured refusal
    // still speaks to.
    if (want != LiveMode::Sample)
        return std::string();
    return std::string("`sample` IS the out-of-band sampler: ") +
           (perf_reason.empty() ? std::string("perf_event_open was refused")
                                : perf_reason) +
           ". `auto` still works here — its sampler chain ends in a perf-free "
           "ptrace region picker — and `dataflow`/`trace` over a named region "
           "are pure ptrace.";
}

namespace {
// aarch64 goes by several spellings on the wire (uname `aarch64`, some
// producers `arm64`); match either so the kill-hazard clause is not silently
// dropped on a host that spells it the other way.
bool is_arm64(const std::string &arch) {
    return arch == "aarch64" || arch == "arm64" || arch == "aarch64_be";
}
} // namespace

std::string mode_perturb_warning(LiveMode m, const std::string &arch) {
    if (!mode_uses_ptrace(m))
        return std::string(); // out of band: nothing to perturb, no confirm
    std::string s = "single-step DIRTIES the traced page (it plants an int3 / "
                    "sets the trap flag) and PERTURBS the target's timing";
    if (is_arm64(arch))
        s += "; on arm64 it can TERMINATE a target blocked in a syscall, and "
             "detach CANNOT undo it (the thread dies ~300ms after DETACH) — "
             "prefer IBS/PT if the host supports them";
    else
        s += " — prefer the out-of-band IBS/PT samplers where the host supports "
             "them";
    s += ". This mode " + std::string(mode_jack_reason(m)) + ".";
    return s;
}

bool mode_arm64_blocking_hazard(LiveMode m, const std::string &arch) {
    return mode_uses_ptrace(m) && is_arm64(arch);
}

LiveMode budget_least_perturbing(bool sample_available, bool perf_probed,
                                 bool perf_ok) {
    // `sample` reads out of band (mode_uses_ptrace==false) and perturbs nothing;
    // it is the least-perturbing substrate when the host has AMD IBS. Otherwise
    // the lightest ptrace mode: `log` streams syscalls without single-stepping,
    // so it dirties no page, unlike `stream`/`trace`/`dataflow`/`tree`/`graph`.
    //
    // Task 4 fix (Finding 2): `sample_available` alone is not enough — reuse
    // mode_host_blocked (the same predicate the picker greys a mode with) to
    // ask whether THIS host's measured perf verdict would actually refuse
    // `sample`. `perf_reason` is passed empty: only emptiness of the return is
    // read here, never its text, so the real reason is never needed.
    const bool sample_host_blocked =
        !mode_host_blocked(LiveMode::Sample, perf_probed, perf_ok, "").empty();
    return (sample_available && !sample_host_blocked) ? LiveMode::Sample
                                                      : LiveMode::Log;
}

BudgetDecision budget_can_start(LiveMode want,
                                const std::vector<LiveMode> &active) {
    BudgetDecision d;
    // A free view is always allowed — including alongside another free view,
    // and including alongside a ptrace view. Nothing to contend for.
    if (!mode_uses_ptrace(want))
        return d;

    for (LiveMode a : active) {
        if (!mode_uses_ptrace(a))
            continue; // a free view blocks nothing
        d.allowed = false;
        d.blocker = a;
        d.reason = std::string("the target already has a live ") +
                   mode_name(a) + " view, which " + mode_jack_reason(a) +
                   ". A target has one tracer, so " + mode_name(want) +
                   " cannot attach until it stops.";
        return d;
    }
    return d;
}

std::string budget_blocked_label(const BudgetDecision &d) {
    if (d.allowed)
        return std::string();
    // "BLOCKED — jack held by <session>" (23-graded-truth-layer.md T3, F23): the
    // word "paused" used to name BOTH this budget preemption and an operator
    // pause, whose recoveries are disjoint. This reads BLOCKED, never paused; the
    // caller appends "on <target>" from the selected pid.
    return std::string("BLOCKED — jack held by ") + mode_name(d.blocker);
}

bool budget_queue_ready(LiveMode queued, const std::vector<LiveMode> &active) {
    // A queued want may FIRE only when the jack is genuinely free — never an
    // auto-swap. This is exactly budget_can_start's allow, named for the intent
    // so the queue path cannot bypass the one-jack invariant.
    return budget_can_start(queued, active).allowed;
}

} // namespace asmdesk
