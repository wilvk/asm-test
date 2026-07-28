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

LiveMode budget_least_perturbing(bool sample_available) {
    // `sample` reads out of band (mode_uses_ptrace==false) and perturbs nothing;
    // it is the least-perturbing substrate when the host has AMD IBS. Otherwise
    // the lightest ptrace mode: `log` streams syscalls without single-stepping,
    // so it dirties no page, unlike `stream`/`trace`/`dataflow`/`tree`/`graph`.
    return sample_available ? LiveMode::Sample : LiveMode::Log;
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
