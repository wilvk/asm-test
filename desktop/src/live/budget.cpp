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

const char *mode_jack_reason(LiveMode m) { return row_for(m)->why; }

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
    return std::string("paused — another live view holds the tracer (") +
           mode_name(d.blocker) + ")";
}

} // namespace asmdesk
