// budget.h — the concurrency budget as interaction physics
// (07-serve-live-host.md T4; plan decision D6).
//
// A target has ONE ptrace jack. Not as a policy, but as a fact about the
// kernel: a tracee has exactly one tracer, and asmspy's ptrace views SEIZE
// either every thread of the target (`log`, `stream`, `tree`, `graph`,
// `watch`) or every task in its whole descendant TREE (`procs`). Two of them
// at once is not "slower", it is impossible — the second attach fails.
//
// Two views are FREE, and for the same structural reason inverted: the AMD
// IBS-Op hot-edge sampler and the software-clock survey read out of band
// through perf, attach nothing, and perturb nothing. They can run alongside a
// ptrace view, and that is the whole reason they exist.
//
// This header is the decision, and nothing else: pure functions over an enum,
// no ImGui, no I/O, no session objects. That is what makes every mode PAIR
// assertable on a machine with nothing to attach to — the table is the part
// that can be wrong, and a rule verified only against a live target would be
// verified almost nowhere.
#ifndef ASMDESK_LIVE_BUDGET_H
#define ASMDESK_LIVE_BUDGET_H

#include <string>
#include <vector>

namespace asmdesk {

// The protocol's modes (asmtrace-schema.md, "Serve protocol"), in wire order.
enum class LiveMode {
    Log,
    Stream,
    Trace,
    Dataflow,
    Tree,
    Graph,
    Procs,
    Sample,
    Watch,
    Auto,
};

// The wire name, and the reverse. `mode_from_name` returns false on an unknown
// name rather than picking one — a mode is not a thing to guess at.
const char *mode_name(LiveMode m);
bool mode_from_name(const std::string &name, LiveMode *out);

// Every mode, for exhaustive iteration (the budget test walks all pairs).
const std::vector<LiveMode> &all_modes();

// Does this mode occupy the target's single ptrace jack?
bool mode_uses_ptrace(LiveMode m);

// Why it does (or does not) — the measured structural reason, for the UI to
// show instead of a bare "unavailable". Never empty.
const char *mode_jack_reason(LiveMode m);

// --- perturbation gate (18-breach-stops.md T5, F22) -------------------------
// Arming a perturbing single-step mode on a LIVE target dirties the traced page,
// perturbs its timing, and on arm64 can TERMINATE a target blocked in a syscall
// with no way for detach to undo it (the recorded aarch64 detach-fatal hazard).
// So the arm gets a pre-commit confirm stating the concrete consequence — the
// same two-step posture the syscall reveal-all uses. These are the pure model
// behind that gate, so test_budget pins them on a host with nothing to attach to.

// The consequence sentence to show before arming `m`. Empty for a non-perturbing
// (out-of-band) mode — `sample` needs no confirm. `arch` keys the arm64 clause:
// on aarch64 the sentence names the blocking-syscall termination hazard that
// detach cannot undo; elsewhere it states the page-dirty / timing cost and
// steers toward IBS/PT. Never empty for a ptrace mode.
std::string mode_perturb_warning(LiveMode m, const std::string &arch);

// Does arming `m` against a target on `arch` carry the arm64 blocking-syscall
// KILL hazard (a single-stepped thread inside a blocking syscall survives DETACH
// on arm64 and dies ~300ms later, SPSR.SS)? True only for a ptrace mode on
// aarch64 — the exact case draw_patch_bay greys/annotates.
bool mode_arm64_blocking_hazard(LiveMode m, const std::string &arch);

// The least-perturbing capture substrate the host supports, for the picker's
// default (F22): out-of-band `sample` (AMD IBS) where the capability probe
// reports it available, otherwise the lightest ptrace mode (`log` — it streams
// syscalls without single-stepping, unlike `stream`/`trace`/`dataflow`). A pure
// choice over the resolved capability model, so test_budget pins it.
LiveMode budget_least_perturbing(bool sample_available);

struct BudgetDecision {
    bool allowed = true;
    // Set when !allowed: the mode already holding the jack, and prose naming
    // both it and the rule. The UI shows this verbatim; a refusal that does not
    // say WHO is blocking is a dead end rather than a next step.
    LiveMode blocker = LiveMode::Log;
    std::string reason;
};

// May `want` start while `active` are running? `active` is whatever the client
// believes is live on this target tree.
//
// This is the CLIENT-side gate. The serve loop refuses a second concurrent
// start too (protocol), but that refusal is a backstop: the point of deciding
// here is that the UI can render the jack as occupied and offer a swap, rather
// than letting the user fire a command that comes back as an error.
BudgetDecision budget_can_start(LiveMode want,
                                const std::vector<LiveMode> &active);

// The UI string for an occupied jack — "paused — another live view holds the
// tracer", naming the holder. Empty when nothing blocks.
std::string budget_blocked_label(const BudgetDecision &d);

} // namespace asmdesk
#endif // ASMDESK_LIVE_BUDGET_H
