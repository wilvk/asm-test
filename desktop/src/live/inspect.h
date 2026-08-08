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
#include <unordered_set> // ProcTreeExpansion::open — the tree's open node ids
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
    // The target called PR_SET_PTRACER (or is our descendant) -- MEASURED by
    // probe_attach via open("/proc/<pid>/mem", O_RDONLY), which runs the
    // kernel's own ptrace_may_access(PTRACE_MODE_ATTACH_FSCREDS), the same
    // check PTRACE_ATTACH runs, and changes no state (NOT PTRACE_SEIZE: a
    // SEIZE probe cannot detach without a prior ptrace-stop and leaves the
    // target frozen). This is the fact that turns a scope-1 "maybe" into a
    // "yes" -- 12 of 15 live Firefox processes pass it, unprivileged.
    bool target_opted_in = false;
    long tracer_pid = 0; // /proc/<pid>/status TracerPid (0 = none)
    int elf_class = 64;  // 32 = an i386 tracee; 0 = unreadable
    // No mm: a kernel thread. Discriminated from an unreadable process (a
    // readlink("/proc/<pid>/exe") failure, which EACCES far more often than a
    // real kernel thread) via /proc/<pid>/status's VmSize field, which every
    // process with an address space carries and every kernel thread omits.
    bool is_kthread = false;
    bool is_self = false; // our own pid
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

// One thread (task) of a process.
//
// Deliberately NOT a ProcRow, and deliberately carrying no attach verdict of
// its own. ptrace_may_access is evaluated per task, but every task in a thread
// group shares the credentials and dumpability that check reads, so a
// per-thread verdict would be its process's answer repeated N times — and the
// pane would be inviting a click on a target asmspy cannot take (see
// ProcTreeRow::pid: selecting a thread targets its thread GROUP). What a
// thread does carry is what actually differs between the tasks of one process:
// what it is called, what state it is in, and how much CPU it used.
struct ProcThread {
    long tid = 0;
    std::string comm;
    char state = '?';
    // Same measure and the same sample window as ProcRow::cpu, and 0 (never
    // sampled) on the cheap default list for the same reason.
    unsigned long long cpu = 0;
};

// One row of the process list.
struct ProcRow {
    long pid = 0;
    // /proc/<pid>/status PPid. 0 for the one real root (pid 1's parent, and
    // every kernel thread's in a namespace that hides it), and 0 as well when
    // status was unreadable — the two are not distinguished here because
    // nothing downstream can act on the difference: both mean "this snapshot
    // gives this row no parent to nest under" (proc_tree_layout's
    // ProcParent::None). What must NOT be conflated is a ppid we DID read
    // naming a process this snapshot does not carry, which is
    // ProcParent::Unknown and a different fact entirely.
    long ppid = 0;
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
    // Every task under /proc/<pid>/task, LEADER FIRST then ascending by tid —
    // the same order cli/asmspy_tidsort.h gives the details pane, so the two
    // never disagree about which row is the leader. Empty when the task dir
    // could not be read (an exit mid-scan, or a permission denial), which is
    // not the same as "one thread": size 1 is a measured single-threaded
    // process. The pane only offers a threads group when this exceeds 1.
    std::vector<ProcThread> threads;
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

// --- the process tree -------------------------------------------------------
//
// A /proc snapshot carries lineage that a flat pid-sorted table throws away:
// `firefox` and its eight content children read as nine unrelated rows. This
// is the pure half of putting it back — the Processes pane's draw code owns
// the glyphs and nothing else.
//
// The rule that shapes the whole thing: DEPTH IS COMPUTED OVER THE WHOLE
// SNAPSHOT, never over the rows that survived the pane's attachability gate
// and type-to-narrow filter. Re-rooting a child whose parent was filtered out
// would render it flush left, which claims it is a top-level process — a
// lineage this pane never measured, and one of the two the operator is
// looking at the table to learn. So a hidden parent keeps its child's depth
// and is REPORTED as hidden, for the pane to say so in the row.

// What is known about a visible row's parent. Four facts, not a bool: the
// remedy an operator reaches for differs for each, and collapsing them was
// the failure this enum exists to prevent.
enum class ProcParent {
    None,    // ppid 0 (or the row is its own parent): nothing to nest under
    Shown,   // the parent is in this snapshot AND in the table
    Hidden,  // the parent is in this snapshot, but the gate or filter cut it
    Unknown, // ppid names no row in this snapshot — the parent exited between
             // the readdir and the read, or lives outside our pid namespace
};

// What a drawn row IS. A process list that shows threads has three kinds of
// row, and flattening them into one would put a tid where a pid goes — the
// single mistake this whole area has to avoid (see ProcTreeRow::pid).
enum class ProcNode {
    Process,     // a thread-group leader: the only kind that is a real target
    ThreadGroup, // the synthetic "N threads" row a multi-threaded process gets
    Thread,      // one task under that group
};

struct ProcTreeRow {
    ProcNode kind = ProcNode::Process;
    size_t index = 0; // into the `rows` handed in — the owning PROCESS, always
    // Which of rows[index].threads this is; meaningful only for Thread.
    size_t thread_at = 0;
    int depth = 0; // ancestors within the snapshot; 0 = nothing above it
    ProcParent parent = ProcParent::None;
    // The last of its parent's children that this table will actually DRAW —
    // which is what the └ vs ├ connector means. Computed over the VISIBLE
    // siblings, not the snapshot's: a filter that hides the youngest sibling
    // has to move the └ up to the one above it, or the tree renders a branch
    // continuing into nothing.
    bool last_sibling = true;
    // Does this row have anything under it to reveal? A Process is expandable
    // when it has a drawable child process or a threads group; a ThreadGroup
    // always is; a Thread never is.
    bool expandable = false;
    bool expanded = false;
    // The id to toggle in ProcTreeExpansion, and the id the pane keys its
    // widget on. Processes use their pid; a ThreadGroup uses -pid, so the two
    // can never collide in one set.
    long node_id = 0;
    // WHAT SELECTING THIS ROW TARGETS — always a thread-group leader, never a
    // tid. asmspy's target is a thread GROUP: seize_threads() SEIZEs the pid
    // it is handed and then walks /proc/<pid>/task, and /proc/<tid>/task lists
    // the whole group, so handing it a tid traces the same process with its
    // notion of the leader wrong. `asmspy --info <tid>` likewise answers with
    // identity.pid set to the TID, which would render a thread as a process
    // duplicating its own parent. So a Thread row carries its LEADER's pid
    // here and the pane says so in the row; nothing downstream ever sees a tid.
    long pid = 0;
    // The tid, for display only. 0 unless kind == Thread.
    long tid = 0;
};

// Which nodes are open. Empty = everything collapsed, which is the default the
// pane starts at.
struct ProcTreeExpansion {
    // node_id values (pid for a Process, -pid for a ThreadGroup).
    std::unordered_set<long> open;
    // Ignore process collapse entirely: every VISIBLE process is emitted at
    // its true depth. The type-to-narrow filter sets this, because a search
    // that returned nothing because the tree happened to be collapsed is a
    // search that lied about the snapshot.
    //
    // It deliberately does NOT open thread groups. The filter matches on
    // pid/comm/cmdline — process facts — so honouring it by dumping a hundred
    // thread rows nobody asked for would answer a different question than the
    // one typed.
    bool show_all_processes = false;
};

// The visible rows, in parent-then-children order, each at its true depth.
//
// `visible[i]` is non-zero for a PROCESS the pane will draw; a short or empty
// vector reads as "none visible" rather than silently defaulting to shown.
// `descending` orders siblings (and roots) by descending pid, to match a
// descending pid sort — the tree SHAPE is identical either way, only the
// order within each sibling group flips.
//
// COLLAPSE IS EVALUATED OVER THE VISIBLE ANCESTORS ONLY. A row is emitted when
// it passes `visible` and no visible ancestor above it is closed. An ancestor
// the gate or the filter removed cannot block anything, because there is no
// row on screen to click open — treating it as closed would strand its whole
// subtree behind a control that does not exist.
//
// Every row of `rows` is consulted for the shape; only the visible ones are
// returned. Cycles (impossible in a live /proc, reachable in a snapshot read
// pid-by-pid across a reused pid) are cut to roots rather than walked.
std::vector<ProcTreeRow> proc_tree_layout(const std::vector<ProcRow> &rows,
                                          const std::vector<char> &visible,
                                          const ProcTreeExpansion &exp,
                                          bool descending = false);

// Every node_id the tree could open, for the pane's "Expand all". Returned
// rather than built by the caller so the two can never disagree about what a
// node id is — and so "expand all" cannot miss a kind added later.
std::vector<long> proc_tree_all_nodes(const std::vector<ProcRow> &rows,
                                      const std::vector<char> &visible);

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

// The region spec a sweep's SCOPED legs inherit from an `auto` pick — the exact
// base+len the sampler measured, spelled in parse_region_spec's grammar
// ("0xADDR:LEN"). Empty for an idle-window marker or a pick with no extent: a
// sweep must stop and say so rather than hand a scoped leg address 0.
//
// base+len rather than the func NAME on purpose. A name is re-resolved against
// the symbol table by the serve host, and a duplicated static symbol (any large
// C++ target carries thousands) would resolve to a DIFFERENT function than the
// one the sampler actually watched.
std::string pick_region_spec(const AutoPick &p);

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
