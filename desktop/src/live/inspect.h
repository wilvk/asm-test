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

namespace asmdesk {

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

// One row of the process list.
struct ProcRow {
    long pid = 0;
    long uid = 0;
    std::string comm;
    std::string cmdline;
    AttachFacts facts;
    AttachVerdict verdict;
};

// Read /proc/sys/kernel/yama/ptrace_scope; -1 when absent (Yama not enforcing).
int read_yama_scope();

// Gather the per-target facts for `pid`. Best effort: unreadable fields keep
// their defaults, and `verdict` reports Unknown rather than guessing.
AttachFacts probe_attach(long pid, int yama_scope, long our_uid, bool have_cap);

// The whole list, client-side (D9: the desktop reads /proc itself; it does not
// need a tracer to enumerate processes). Sorted by pid.
std::vector<ProcRow> list_processes();

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
