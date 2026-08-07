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

// Every mode, for exhaustive iteration (the budget test walks all pairs). This
// is the WIRE order (asmtrace-schema.md); do not reorder it — the budget walk and
// the protocol both depend on it. For the picker's reading order, use
// patch_bay_order() instead.
const std::vector<LiveMode> &all_modes();

// The mode picker's READING order (39-auto-capture-reliability.md framing): `auto`
// leads, because it is the one choice an un-named target should reach for first
// (it finds its own region and captures at full detail), then the two scoped
// single-step engines, then the whole-process and out-of-band modes. A UI-only
// ordering over the SAME set as all_modes() — distinct so the wire order stays
// fixed. test_budget pins that this is a permutation of all_modes() with Auto first.
const std::vector<LiveMode> &patch_bay_order();

// The visualizations a mode's capture can fill, as short view names — what the
// picker advertises so a choice is not a leap of faith ("pick auto → you get the
// Slice, Loom, Scrubber, Timeline and 3D overview"). Derived from WHAT each mode
// records, kept consistent with ui/view_presence.cpp's presence rules: a
// statistical sampler (`sample`) never single-steps, so it offers no exact Loom /
// Scrubber. Pure and allocation-light, so test_budget pins the mapping.
std::vector<const char *> mode_visualizations(LiveMode m);

// Does this mode occupy the target's single ptrace jack?
bool mode_uses_ptrace(LiveMode m);

// Does this mode need a scoped REGION (a func name or base+len)? trace/dataflow
// single-step ONE region, so the serve host requires one (serve_resolve_region);
// `auto` finds its own via the sampler and every whole-process mode needs none.
// The door shows a region input only for these, and gates Start on it being set.
bool mode_needs_region(LiveMode m);

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
//
// 2026-08-06 plan, Task 4 fix (coordinator review, Finding 2): `sample_available`
// is a SUBSTRATE fact (CPUID/sysfs — IBS hardware exists) and says nothing about
// PERMISSION — the exact gap CapState::perf_ok exists to close (a host can have
// IBS silicon and still refuse every perf_event_open, measured on this box). So
// `perf_probed`/`perf_ok` are threaded through here too, via mode_host_blocked,
// rather than defaulting to a mode that would record zero events on the very
// first Start.
LiveMode budget_least_perturbing(bool sample_available, bool perf_probed,
                                 bool perf_ok);

// --- the substrate sweep (59 T1) --------------------------------------------
//
// Each of the 3D pane's substrates comes from a DIFFERENT engine, and the serve
// host runs one engine at a time, so no single capture can fill more than one:
//
//   tree      -> the module excursion ribbon (`call` events)
//   trace     -> the invocation stack (`trace` + a `coverage` block set)
//   dataflow  -> the SIMD lane prism (wide `df_step.ops` register writes)
//
// The address plane rides along — all three arm a `codeimage`.
//
// The DIVERGENCE worldline is deliberately absent and cannot be added. It
// compares two RECORDINGS, so no sequence of captures within one session can
// produce it; run a sweep twice and attach the second file as B.
//
// TWO SHAPES, chosen by whether the operator named a region:
//
//   have_region   tree -> trace -> dataflow    (each scoped leg steps THAT region)
//   !have_region  auto -> tree  -> trace       (the auto leg SAMPLES for one)
//
// The auto-led shape is what makes the pane reachable from the `auto` front
// door. An `auto` capture records `codeimage` + `df_step` and nothing else, so
// its recording offers the address plane (and the lane prism, when the picked
// region writes wide registers) with the invocation stack and the module
// excursion ribbon disabled — and the sweep that would have filled them refused
// to start without a hand-named region, which an `auto` operator by
// construction does not have. `auto` IS the dataflow engine with a picker in
// front of it (the serve host runs asmspy_engine_dataflow for SM_AUTO), so it
// replaces the dataflow leg rather than adding to it, and the region it picks is
// what the `trace` leg behind it single-steps. Every leg that needs a region
// therefore runs AFTER the auto leg — the region does not exist until its `pick`
// lands.
//
// Pure, and here rather than in the ImGui door, for the same reason the budget
// table is: the part that can be WRONG is the rule, and a rule verified only
// against a live target would be verified almost nowhere.
const std::vector<LiveMode> &sweep_legs(bool have_region);

// The standing one-line description of what a sweep will do, derived from
// sweep_legs so the two can never drift apart. The auto-led form must say that
// the FIRST leg is what finds the region — otherwise an operator reads the
// empty region box as the reason the button will fail.
std::string sweep_plan(bool have_region, long max_events);

// Why a sweep cannot start, given the facts the door holds; "" when it can.
// Every reason names the thing to fix rather than a bare refusal.
//
// `recording` is the load-bearing one. The desktop's live tab shows exactly ONE
// Recording, so without a session-level `--record` each leg lands in its own
// Recording and the pane can still only show one substrate — the sweep would
// cost three captures and change nothing.
//
// There is deliberately NO have_region parameter. An un-named region used to
// refuse here, and that refusal is exactly what left the invocation stack and
// the module ribbon unreachable from `auto`: the sweep now leads with the one
// mode that finds its own region. A sweep whose auto leg then picks nothing
// stops at the first scoped leg and says so — a fact measured on the wire, not
// guessed at before it runs.
std::string sweep_blocked(bool host_started, bool have_pid, bool recording,
                          bool already_sweeping, bool jack_held);

// The sweep's COMPLETION note (2026-08-06 plan, Task 12; M13).
//
// The note this replaced fired on `sweep_at >= legs.size()` alone and named
// all four substrates unconditionally — "every leg captured" is a fact about
// the LOOP, not about what those legs wrote. The SIMD lane prism in
// particular is data-dependent on the picked routine's OWN instructions, not
// on the capture succeeding: measured with byte-identical capture flags,
// 8-11 prism writes over scenes_victim's blend_tile, 0 over auto_victim's
// entered_often, 0 over scenes_victim's sort_batch. A note that promises a
// prism a scalar routine was never going to write sends the operator hunting
// for a scene that cannot exist — and every leg still ran cleanly.
//
// Score against what LANDED, not against the leg count. The caller (the
// door) decodes the four flags from s.session.recordings() — every completed
// leg, of either sweep shape — and this stays the pure half: booleans in,
// prose out, so test_budget pins the wording with no session, no engine, no
// live target at all.
//
// `plane`/`stack`/`ribbon`/`prism` mirror the four 3D-pane substrates (59
// T1): the address plane (`codeimage`), the invocation stack (a `coverage`
// block set), the module excursion ribbon (`call` events) and the SIMD lane
// prism (wide `df_step.ops` writes). An absent plane/stack/ribbon means a
// leg's own events never arrived — a capture fact, named with the event kind
// it needed. An absent prism is the ONE substrate this function will not
// blame on the capture: `picked` is the routine the sweep actually stepped
// (the auto leg's pick, or the operator's own named function), and the
// reason names IT ("`<picked>` writes no vector registers"), because a
// completed capture over a scalar routine is not a defect. `picked` may be
// "" (a hand-named base+len region carries no symbol) and the reason then
// falls back to "the routine it captured" rather than printing empty
// backticks.
std::string sweep_result_note(bool plane, bool stack, bool ribbon, bool prism,
                              const std::string &picked);

// --- the host-capability gate (2026-08-06 plan, Task 4) ---------------------
//
// why `want` cannot run on THIS host, or "" when nothing is known to stop it.
// Pure over the probed facts, so test_budget pins it against no live host at
// all.
//
// `perf_probed` is what separates "measured refusal" from "not asked": only a
// MEASURED refusal may grey a mode. An unprobed host yields "" -- a note
// elsewhere, never a disabled control, because a control greyed on a guess is
// indistinguishable from a broken build.
std::string mode_host_blocked(LiveMode want, bool perf_probed, bool perf_ok,
                              const std::string &perf_reason);

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

// The UI string for an occupied jack — "BLOCKED — jack held by <session>",
// naming the holder (23-graded-truth-layer.md T3, F23). It reads BLOCKED, never
// "paused": the bare word "paused" used to name BOTH this budget preemption and
// an operator pause, and their recoveries are disjoint. The caller appends "on
// <target>". Empty when nothing blocks.
std::string budget_blocked_label(const BudgetDecision &d);

// May a QUEUED want start now (23-graded-truth-layer.md T3)? True exactly when
// the jack is genuinely free (budget_can_start allows). The Queue path polls this
// and fires only when it flips true — never an auto-swap, so the one-ptrace-jack
// invariant is never bypassed.
bool budget_queue_ready(LiveMode queued, const std::vector<LiveMode> &active);

} // namespace asmdesk
#endif // ASMDESK_LIVE_BUDGET_H
