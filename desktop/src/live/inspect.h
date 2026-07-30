// inspect.h — the Inspect door's two decisions (07-serve-live-host.md T5).
//
// The door's promise is "pick a process, and see WHY NOT when you cannot". Two
// things in it can actually be wrong, and both are pure functions here:
//
//  1. ATTACHABILITY. Whether a target can be traced is a conjunction of facts
//     that live in four different places — the caller's uid, Yama's
//     ptrace_scope, CAP_SYS_PTRACE, whether something already traces it, and
//     the tracee's ELF class — and each one sends an operator somewhere
//     different. A row that just says "cannot attach" has thrown away the only
//     useful part. So the verdict carries WHY and, where one exists, the
//     REMEDY; and it is computed from a fact struct, so every combination is
//     testable on a machine where none of them hold.
//
//  2. EVIDENCE LABELLING for `mode:"auto"`. The front door picks a region for
//     you, and the two samplers behind it do NOT produce the same grade of
//     evidence: an IBS-Op entry edge is a direct observation of the event the
//     capture waits for; a software-clock residency sample is not. Showing
//     them identically would be the interface lying, so the weaker one is
//     labelled — and that labelling is a function, not a convention someone
//     has to remember at each call site.
//
// Pure: no ImGui, no session object, no engine. The /proc reader below is the
// only part that touches the machine, and it is deliberately the thin part.
#ifndef ASMDESK_LIVE_INSPECT_H
#define ASMDESK_LIVE_INSPECT_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "live/session.h" // LiveStatus, for the toast-transition helper (16 T1)

namespace asmdesk {

// Non-modal toast feedback for live-session events (16-live-feedback-and-
// filtering.md T1). `live_session_toasts` decides which toasts a state
// TRANSITION should raise — a new refusal, a session end, a fatal, a new skip,
// a completed save — as a neutral list the shell maps onto ImGuiNotify. Pure +
// tested; toasts SUPPLEMENT the in-pane refusal banners, they never replace
// them (D7). Deciding here (not in the draw code) is what lets the null-backend
// test drive the queue as a MODEL rather than reading pixels.
enum class ToastKind { Info, Success, Warning, Error };
struct SessionToast {
    ToastKind kind = ToastKind::Info;
    std::string text;
    // Non-empty -> the shell renders an "Open in Loom" button on the toast that
    // feeds this path back through InspectState::open_request (the same door the
    // panes use). Only an exact, saved .asmtrace earns one (a statistical
    // capture has no Loom, so no button).
    std::string open_path;
};

// Everything the shell knows each frame that a toast could be raised from: the
// live status plus the save outcome (which lives in InspectState, not
// LiveStatus). Bundling them means ONE pure function and ONE remembered
// prev-value drive every toast, and the test constructs states directly.
struct FeedbackInputs {
    LiveStatus status;
    bool saved_ok = false;          // a save succeeded
    bool saved_statistical = false; // ...but the capture was non-exact (no Loom)
    std::string saved_path;         // the file the last successful save wrote
    std::string save_status;        // the last save result, verbatim (error too)
};
std::vector<SessionToast> live_session_toasts(const FeedbackInputs &prev,
                                              const FeedbackInputs &cur);

// ---------------------------------------------------------------------------
// the patch bay's two distinct states (23-graded-truth-layer.md T3, F23)
// ---------------------------------------------------------------------------
//
// The bare word "paused" named TWO states with disjoint recoveries: an OPERATOR
// pause (whose only recovery is Resume) and a budget PREEMPTION (another view
// holds the ptrace jack — Swap / Queue / Cancel). Neither recovery was offered
// at the state. These pure functions split them so the distinction + the right
// action set are asserted with no live target — the states are model-checked, the
// click flow can then use the doc-17 interaction lane.

enum class PatchMode {
    Free,           // nothing blocks and the operator has not paused
    OperatorPaused, // the operator hit Pause — recovery is Resume, nothing else
    BudgetBlocked,  // the jack is held by another view — Swap / Queue / Cancel
};

// Classify the patch bay. Operator pause and budget block are DISTINCT and never
// conflated: an operator pause is a deliberate hold the user can Resume; a budget
// block is a fact about the kernel's one-tracer rule. `budget_allowed` is
// budget_can_start(...).allowed (kept a bool so this needs no budget link).
inline PatchMode patch_mode(bool operator_paused, bool budget_allowed) {
    if (operator_paused)
        return PatchMode::OperatorPaused;
    if (!budget_allowed)
        return PatchMode::BudgetBlocked;
    return PatchMode::Free;
}

// The action set each state offers — the whole point of splitting them is that
// the recoveries do not overlap. Operator pause offers ONLY Resume; a budget
// block offers Swap / Queue / Cancel and NEVER an auto-swap.
struct PatchActions {
    bool resume = false; // operator pause -> Resume
    bool swap = false;   // budget block -> Swap (a named two-step confirm)
    bool queue = false;  // budget block -> Queue (a cancellable chip)
    bool cancel = false; // budget block -> Cancel
};
inline PatchActions patch_actions(PatchMode m) {
    PatchActions a;
    if (m == PatchMode::OperatorPaused)
        a.resume = true;
    else if (m == PatchMode::BudgetBlocked)
        a.swap = a.queue = a.cancel = true;
    return a;
}

// Which lifecycle buttons can ACT at the current stage (R4): Start needs a valid
// target (can_start); Stop needs a live-or-paused capture; Pause needs a
// running-and-not-already-paused one; Resume needs an operator pause. A button
// that cannot act is greyed rather than firing a command the serve loop would just
// refuse — no dead levers. Pure, so test_inspect pins every stage.
struct LiveControls {
    bool start = false;
    bool stop = false;
    bool pause = false;
    bool resume = false;
};
inline LiveControls live_controls(bool can_start, bool running,
                                  bool operator_paused, bool have_active) {
    LiveControls c;
    c.start = can_start;
    c.stop = running || have_active || operator_paused;
    c.pause = running && !operator_paused;
    c.resume = operator_paused;
    return c;
}

// ---------------------------------------------------------------------------
// 1. attachability
// ---------------------------------------------------------------------------

// Everything the verdict depends on, gathered from /proc (see probe_attach).
struct AttachFacts {
    bool same_uid = true;             // the target's uid == ours
    bool have_cap_sys_ptrace = false; // we hold CAP_SYS_PTRACE (or are root)
    // /proc/sys/kernel/yama/ptrace_scope. -1 = the file is absent, which means
    // the Yama LSM is not enforcing at all — NOT "unknown, assume the worst".
    int yama_scope = -1;
    bool target_opted_in = false; // the target called PR_SET_PTRACER
    long tracer_pid = 0;          // /proc/<pid>/status TracerPid (0 = none)
    int elf_class = 64;           // 32 = an i386 tracee; 0 = unreadable
    bool is_kthread = false;      // no mm: a kernel thread
    bool is_self = false;         // our own pid
};

enum class Attach {
    Yes,     // the attach should succeed
    No,      // it will not, and we know why
    Unknown, // it may; the deciding fact cannot be read from outside
};

struct AttachVerdict {
    Attach verdict = Attach::Unknown;
    std::string why;    // the measured reason. Never empty.
    std::string remedy; // what would change the answer; "" when nothing can.
};

// The whole decision, pure. Order matters: the facts are not independent, and
// the one that DOMINATES has to be reported, or the operator fixes the wrong
// thing (raising a Yama scope when the real problem is a 32-bit tracee).
AttachVerdict attach_verdict(const AttachFacts &f);

// The exact terminal command that clears the gate named in `advice` — a why /
// remedy / reason / skip / fix string the app already shows — or "" when no
// single command fixes it (a reboot for ptrace_scope=3, a privileged relaunch
// for CAP_*, an ABI mismatch for i386, a kernel rebuild, or hardware). Pure, and
// matched on the stable tokens the prose already carries, so it stays ONE source
// of truth with the remedy text: the empty cases keep rendering as prose with no
// invented one-liner. Every UI surface that shows a remedy runs this to decide
// whether to also offer a copy-pasteable command (and the Log echoes it too).
std::string remedy_command(const std::string &advice);

// One row of the process list.
struct ProcRow {
    long pid = 0;
    long uid = 0;
    std::string comm;
    std::string cmdline;
    AttachFacts facts;
    AttachVerdict verdict;
    // CPU jiffies (utime + stime) used during the sample window when the list is
    // built with sample_cpu — the same measure cli/asmspy_proc.c's
    // ASMSPY_SORT_ACTIVE ranks on. 0 (and never sampled) on the cheap default
    // list, so the "activity" column shows it only when it was actually measured.
    unsigned long long cpu = 0;
};

// Read /proc/sys/kernel/yama/ptrace_scope; -1 when absent (Yama not enforcing).
int read_yama_scope();

// Gather the per-target facts for `pid`. Best effort: unreadable fields keep
// their defaults, and `verdict` reports Unknown rather than guessing.
AttachFacts probe_attach(long pid, int yama_scope, long our_uid, bool have_cap);

// The whole list, client-side (D9: the desktop reads /proc itself; it does not
// need a tracer to enumerate processes). Sorted by pid.
//
// sample_cpu ranks activity: it snapshots each process's CPU jiffies, sleeps a
// short FIXED window (~150ms), then re-snapshots, filling ProcRow::cpu with the
// delta — most-active processes carry the largest values. A fixed window makes
// the raw delta comparable across rows with no wall-clock normalization, and a
// process idle during the window reads 0 ("active now", not "ever active"). It
// is off by default because that sample is not free (the call briefly sleeps);
// the Processes pane only asks for it when the "activity" column is the sort.
std::vector<ProcRow> list_processes(bool sample_cpu = false);

// Parse a scoped-region spec for trace/dataflow's serve `start` params. A spec of
// the form "0xADDR:LEN" (base and len each 0x-hex or decimal, len > 0) yields
// base+len — fills *base/*len and returns true. Anything else is a FUNCTION NAME:
// returns false with *base=*len=0, and the caller sends {"func": spec}. Pure and
// allocation-free, so test_inspect drives it headlessly. A C++ symbol containing
// `::` stays a name (its left half is not a number).
bool parse_region_spec(const std::string &spec, uint64_t *base, uint64_t *len);

// Why LOCAL inspection has no body on this host, or "" where it has one.
//
// A /proc-less host makes list_processes() return an empty vector, and an empty
// process list is indistinguishable from "nothing is running" — which is never
// true. The caller must therefore be able to tell the two apart, so the absence
// gets a reason of its own rather than being inferred from a count of zero.
// Non-empty is not an error state: remote capture (`ssh <host> asmspy --serve`)
// is the supported path there and is unaffected.
const char *local_inspect_unavailable();

// ---------------------------------------------------------------------------
// 2. the --auto front door's evidence
// ---------------------------------------------------------------------------

// One `session state:"pick"` event (asmtrace-schema.md, Serve protocol).
struct AutoPick {
    std::string sampler;  // "ibs-op" | "sw-clock"
    std::string evidence; // "entry" | "residency"
    std::string func;
    uint64_t base = 0, len = 0;
    uint64_t weight = 0;
    unsigned sites = 0;
    int attempt = 0, of = 0;
};

// Parse a `session` event body. Returns false unless it is a `state:"pick"`
// carrying a `pick` object — a caller must not have to pre-filter.
bool parse_auto_pick(const nlohmann::json &session_body, AutoPick *out);

// Is this pick's evidence WEAKER than the event the capture actually waits
// for? True for residency. This is the predicate the UI must not skip.
bool pick_is_weak_evidence(const AutoPick &p);

// Is this "pick" actually an IDLE-WINDOW retry marker (39 T3)? The serve loop
// reuses the pick channel to report each empty sample window — the sampler ran
// and nothing qualified — with the sentinel func "(idle window)". It is NOT a
// region pick: rendering it through the entry/residency label would claim the
// capture observed something it did not. The pane shows the faithful retry note
// instead.
bool pick_is_idle_window(const AutoPick &p);

// The label to show beside the pick. For residency it states the weakness and
// its consequence in the same breath — a caveat the user has to already
// understand is not a caveat.
std::string pick_evidence_label(const AutoPick &p);

// When `attempt` > 1 the server walked past a candidate that was never seen
// entering. That refusal is information and must be shown, not smoothed over
// by silently presenting the replacement. "" for a first attempt.
std::string pick_walk_note(const AutoPick &p);

} // namespace asmdesk
#endif // ASMDESK_LIVE_INSPECT_H
