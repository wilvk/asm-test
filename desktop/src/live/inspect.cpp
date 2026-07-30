// inspect.cpp — the Inspect door's decisions. See inspect.h for why these are
// pure functions rather than UI code (07-serve-live-host.md T5).
#include "live/inspect.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h> // nanosleep — the fixed activity-sample window
#include <unistd.h>

namespace asmdesk {

using nlohmann::json;

bool parse_region_spec(const std::string &spec, uint64_t *base, uint64_t *len) {
    if (base)
        *base = 0;
    if (len)
        *len = 0;
    std::string::size_type colon = spec.find(':');
    if (colon == std::string::npos)
        return false; // a bare name (or a bare address, which lacks a length)
    // Both halves must be numbers (0x-hex or decimal) for a base+len spec; a name
    // like `ns::func` has a non-numeric left half and stays a name.
    auto as_num = [](const std::string &t, uint64_t *out) -> bool {
        if (t.empty())
            return false;
        errno = 0;
        char *end = nullptr;
        unsigned long long v = std::strtoull(t.c_str(), &end, 0);
        if (end == t.c_str() || *end != '\0' || errno != 0)
            return false;
        if (out)
            *out = static_cast<uint64_t>(v);
        return true;
    };
    uint64_t b = 0, l = 0;
    if (!as_num(spec.substr(0, colon), &b) ||
        !as_num(spec.substr(colon + 1), &l) || l == 0)
        return false; // not a valid base+len -> treat the whole thing as a name
    if (base)
        *base = b;
    if (len)
        *len = l;
    return true;
}

// ---------------------------------------------------------------------------
// attachability
// ---------------------------------------------------------------------------

AttachVerdict attach_verdict(const AttachFacts &f) {
    AttachVerdict v;

    // The order below is the point of this function. Each of these facts is
    // sufficient on its own, and the FIRST one that holds is the one an
    // operator has to act on — reporting a later one would send them to fix
    // something that would not change the answer.

    if (f.is_self) {
        v.verdict = Attach::No;
        v.why = "this is the viewer's own process — tracing it would trace the "
                "thing doing the tracing";
        return v;
    }
    if (f.is_kthread) {
        v.verdict = Attach::No;
        v.why = "kernel thread: it has no user-space address space, so there "
                "is no instruction stream to trace";
        return v;
    }
    if (f.elf_class == 32) {
        // ASMSPY_ETRACEE_I386, refused PRE-attach. It is not a permission
        // problem and no privilege fixes it: the engines read rip/orig_rax
        // through the x86-64 ABI and decode against the x86-64 syscall table,
        // so on an i386 task they would produce confident nonsense.
        v.verdict = Attach::No;
        v.why = "32-bit (i386) process: asmspy decodes against the x86-64 "
                "syscall table and register layout, so tracing this would "
                "produce confident nonsense rather than an error";
        v.remedy = "trace a 64-bit process";
        return v;
    }
    if (f.tracer_pid != 0) {
        v.verdict = Attach::No;
        v.why = "already traced by pid " + std::to_string(f.tracer_pid) +
                " — a tracee has exactly one tracer";
        v.remedy = "stop the other tracer (a debugger, strace, or an earlier "
                   "session) first";
        return v;
    }

    // CAP_SYS_PTRACE overrides both Yama and the uid check, so it is tested
    // before either — otherwise a privileged viewer would report a uid problem
    // that does not apply to it.
    if (f.have_cap_sys_ptrace) {
        v.verdict = Attach::Yes;
        v.why = "CAP_SYS_PTRACE is held, which overrides the uid and "
                "ptrace_scope restrictions";
        return v;
    }

    if (f.yama_scope >= 3) {
        v.verdict = Attach::No;
        v.why = "ptrace_scope=3: attach is disabled kernel-wide";
        v.remedy = "nothing here can change it — scope 3 is one-way and needs "
                   "a reboot to lower";
        return v;
    }
    if (f.yama_scope == 2) {
        v.verdict = Attach::No;
        v.why = "ptrace_scope=2: only a process holding CAP_SYS_PTRACE may "
                "attach to anything";
        v.remedy = "run the viewer with CAP_SYS_PTRACE (in Docker: "
                   "--cap-add=SYS_PTRACE)";
        return v;
    }
    if (!f.same_uid) {
        v.verdict = Attach::No;
        v.why = "the target runs as a different user, and we do not hold "
                "CAP_SYS_PTRACE";
        v.remedy = "run the viewer as that user, or with CAP_SYS_PTRACE";
        return v;
    }
    if (f.yama_scope == 1) {
        // Scope 1 permits a DESCENDANT, or a target that opted in via
        // PR_SET_PTRACER. Whether we are a descendant is knowable, but whether
        // the target opted in is not readable from outside — so a truthful
        // Unknown beats a confident Yes that fails at attach.
        if (f.target_opted_in) {
            v.verdict = Attach::Yes;
            v.why = "ptrace_scope=1, and the target opted in via "
                    "PR_SET_PTRACER";
            return v;
        }
        v.verdict = Attach::Unknown;
        v.why = "ptrace_scope=1 allows attaching only to a descendant, or to a "
                "target that opted in with PR_SET_PTRACER — and whether it did "
                "cannot be read from outside the process";
        v.remedy = "set /proc/sys/kernel/yama/ptrace_scope to 0, or run the "
                   "viewer with CAP_SYS_PTRACE, to remove the doubt";
        return v;
    }

    // scope 0, or Yama absent entirely (-1). Same-uid attach is permitted.
    v.verdict = Attach::Yes;
    v.why = f.yama_scope < 0
                ? "same user, and the Yama ptrace_scope restriction is not "
                  "enforced on this host"
                : "same user, and ptrace_scope=0";
    return v;
}

std::string remedy_command(const std::string &advice) {
    auto has = [&](const char *n) {
        return advice.find(n) != std::string::npos;
    };
    // Ordered most-specific first, and matched on the sysctl KEY / tool name the
    // remedy prose already spells — so a command is offered only where a single,
    // universal, argv-independent line actually clears the gate:
    //  - perf_event_paranoid: the sampling / hwtrace / IBS gate (auto, --sample).
    //  - ptrace_scope: the attach gate. NB match the remedy, not the why — the
    //    scope-3 refusal names "ptrace_scope=3" in its why but its remedy says
    //    "reboot", so running this over the REMEDY correctly yields "" there.
    //  - make cli: the protocol-mismatch fix (stale build/asmspy).
    // Everything else — a CAP_* relaunch, a uid change, an i386 ABI mismatch, a
    // kernel rebuild, hardware — has no genuine one-liner, so it returns "" and
    // the caller keeps showing the prose remedy alone.
    if (has("perf_event_paranoid"))
        return "sudo sysctl -w kernel.perf_event_paranoid=2";
    if (has("ptrace_scope"))
        return "sudo sysctl -w kernel.yama.ptrace_scope=0";
    if (has("make cli"))
        return "make cli";
    return "";
}

// ---------------------------------------------------------------------------
// /proc
// ---------------------------------------------------------------------------

namespace {

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// One "Field:\tvalue" line out of /proc/<pid>/status.
std::string status_field(const std::string &status, const char *key) {
    std::string needle = std::string("\n") + key + ":";
    size_t at = status.compare(0, strlen(key) + 1, std::string(key) + ":") == 0
                    ? 0
                    : status.find(needle);
    if (at == std::string::npos)
        return std::string();
    if (at != 0)
        at++; // step over the '\n'
    size_t colon = status.find(':', at);
    if (colon == std::string::npos)
        return std::string();
    size_t eol = status.find('\n', colon);
    std::string v = status.substr(colon + 1, eol - colon - 1);
    size_t b = v.find_first_not_of(" \t");
    if (b == std::string::npos)
        return std::string();
    return v.substr(b);
}

bool all_digits(const char *s) {
    if (!*s)
        return false;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

// Total CPU jiffies (utime + stime) of a pid, from /proc/<pid>/stat — the same
// field pair cli/asmspy_proc.c's proc_cpu() reads for ASMSPY_SORT_ACTIVE. The
// comm field can hold spaces and parens, so parse everything AFTER the last ')'.
unsigned long long proc_cpu(const std::string &pid) {
    const std::string stat = read_file("/proc/" + pid + "/stat");
    const size_t rp = stat.rfind(')');
    if (rp == std::string::npos)
        return 0;
    unsigned long ut = 0, st = 0;
    // after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt
    // cmajflt utime stime ...  (utime = field 14, stime = field 15)
    if (std::sscanf(stat.c_str() + rp + 1,
                    " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut,
                    &st) == 2)
        return (unsigned long long)ut + st;
    return 0;
}

} // namespace

int read_yama_scope() {
    std::string s = read_file("/proc/sys/kernel/yama/ptrace_scope");
    if (s.empty())
        return -1; // the LSM is not present: not "unknown", not enforcing
    return std::atoi(s.c_str());
}

AttachFacts probe_attach(long pid, int yama_scope, long our_uid,
                         bool have_cap) {
    AttachFacts f;
    const std::string base = "/proc/" + std::to_string(pid);
    f.yama_scope = yama_scope;
    f.have_cap_sys_ptrace = have_cap;
    f.is_self = (pid == (long)::getpid());

    const std::string status = read_file(base + "/status");
    if (!status.empty()) {
        std::string uid = status_field(status, "Uid");
        if (!uid.empty())
            f.same_uid = (std::atol(uid.c_str()) == our_uid);
        std::string tracer = status_field(status, "TracerPid");
        if (!tracer.empty())
            f.tracer_pid = std::atol(tracer.c_str());
    }

    // A kernel thread has no exe link (no mm). readlink failing for any other
    // reason also leaves us unable to read the ELF, so the class stays 0 =
    // unknown rather than being assumed 64.
    char buf[512];
    ssize_t n = ::readlink((base + "/exe").c_str(), buf, sizeof buf - 1);
    if (n <= 0) {
        f.is_kthread = true;
        f.elf_class = 0;
    } else {
        buf[n] = '\0';
        // EI_CLASS is byte 4 of the ELF header: 1 = 32-bit, 2 = 64-bit. Read
        // it through /proc rather than the path, which may be deleted.
        std::ifstream elf(base + "/exe", std::ios::binary);
        unsigned char hdr[5] = {0, 0, 0, 0, 0};
        if (elf && elf.read((char *)hdr, 5) &&
            std::memcmp(hdr, "\177ELF", 4) == 0)
            f.elf_class = hdr[4] == 1 ? 32 : 64;
        else
            f.elf_class = 0;
    }
    return f;
}

const char *local_inspect_unavailable() {
#ifdef __linux__
    return "";
#else
    // Not a TODO. The engines this door drives are ptrace(2) + process_vm_readv
    // + /proc on Linux (libasmspy.h's preamble), so even a perfect macOS/BSD
    // process lister would enumerate targets that can never be attached — a
    // table of confident Yes verdicts none of which hold. Saying so is the
    // correct output; `ssh <linux-host> asmspy --serve` is the path that works.
    return "local process inspection needs /proc, which this host does not "
           "have. The tracer engines are Linux-only besides, so a list here "
           "would offer targets that could never be attached. Capture against "
           "a Linux host over ssh instead — same code path, same views.";
#endif
}

std::vector<ProcRow> list_processes(bool sample_cpu) {
    std::vector<ProcRow> rows;
    const int scope = read_yama_scope();
    const long our_uid = (long)::geteuid();
    const bool have_cap = (our_uid == 0); // root always has it; a finer probe
                                          // would need libcap, which is not a
                                          // dependency this tree carries
    DIR *d = ::opendir("/proc");
    if (!d)
        return rows;
    struct dirent *e;
    while ((e = ::readdir(d)) != nullptr) {
        if (!all_digits(e->d_name))
            continue;
        ProcRow r;
        r.pid = std::atol(e->d_name);
        const std::string base = "/proc/" + std::string(e->d_name);
        r.comm = read_file(base + "/comm");
        while (!r.comm.empty() &&
               (r.comm.back() == '\n' || r.comm.back() == ' '))
            r.comm.pop_back();
        std::string cmd = read_file(base + "/cmdline");
        for (char &c : cmd)
            if (c == '\0')
                c = ' ';
        while (!cmd.empty() && cmd.back() == ' ')
            cmd.pop_back();
        r.cmdline = cmd;
        const std::string status = read_file(base + "/status");
        std::string uid = status_field(status, "Uid");
        r.uid = uid.empty() ? -1 : std::atol(uid.c_str());
        r.facts = probe_attach(r.pid, scope, our_uid, have_cap);
        r.verdict = attach_verdict(r.facts);
        // First CPU snapshot; the delta over the window below becomes ::cpu. Only
        // the activity sort asks for this, so an unsampled list never reads /stat
        // and never sleeps.
        if (sample_cpu)
            r.cpu = proc_cpu(e->d_name);
        rows.push_back(std::move(r));
    }
    ::closedir(d);

    // Second snapshot after a short FIXED window -> per-process CPU jiffies used
    // during it, exactly as cli/asmspy_proc.c's ASMSPY_SORT_ACTIVE measures. The
    // model stays pid-sorted (below); the Processes pane's "activity" column
    // reorders view indices to rank by ::cpu (D4/D7: never reorder the model).
    if (sample_cpu && !rows.empty()) {
        struct timespec ts = {0, 150L * 1000 * 1000};
        ::nanosleep(&ts, nullptr);
        for (ProcRow &r : rows) {
            const unsigned long long c1 = proc_cpu(std::to_string(r.pid));
            r.cpu = (c1 > r.cpu) ? c1 - r.cpu : 0;
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const ProcRow &a, const ProcRow &b) { return a.pid < b.pid; });
    return rows;
}

// ---------------------------------------------------------------------------
// the --auto front door's evidence
// ---------------------------------------------------------------------------

bool parse_auto_pick(const json &b, AutoPick *out) {
    if (!b.is_object() || b.value("state", "") != "pick")
        return false;
    if (!b.contains("pick") || !b["pick"].is_object())
        return false;
    const json &p = b["pick"];
    AutoPick a;
    a.sampler = p.value("sampler", "");
    a.evidence = p.value("evidence", "");
    a.func = p.value("func", "?");
    a.base = p.value("base", (uint64_t)0);
    a.len = p.value("len", (uint64_t)0);
    a.weight = p.value("weight", (uint64_t)0);
    a.sites = p.value("sites", 0u);
    a.attempt = p.value("attempt", 0);
    a.of = p.value("of", 0);
    // `evidence` is what the label turns on, so a pick without one is not a
    // pick we know how to present faithfully.
    if (a.evidence.empty())
        return false;
    if (out)
        *out = a;
    return true;
}

bool pick_is_weak_evidence(const AutoPick &p) {
    // Anything that is not a direct observation of an ENTRY is weaker than the
    // event the capture waits for. Written as "not entry" rather than
    // "== residency" so a future third sampler is weak until it proves
    // otherwise, which is the safe direction for this particular claim.
    return p.evidence != "entry";
}

bool pick_is_idle_window(const AutoPick &p) {
    // The serve loop reuses the pick channel for an empty sample window (39 T3):
    // evidence "idle" (it observed nothing this window — NOT an entry/residency
    // claim) with the sentinel func "(idle window)". Keyed on the genuine evidence
    // value; the func is a secondary tell.
    return p.evidence == "idle" || p.func == "(idle window)";
}

std::string pick_evidence_label(const AutoPick &p) {
    if (pick_is_idle_window(p)) {
        // An empty window is a RETRY, not a verdict: the sampler ran and nothing
        // qualified this time, so the capture re-samples. Rendering it as an
        // entry/residency pick would claim an observation that did not happen.
        return "idle sample window " + std::to_string(p.attempt) + " of " +
               std::to_string(p.of) +
               " — nothing qualified as a region this window; re-sampling (an "
               "empty window is a retry, not a verdict)";
    }
    // Name the SAMPLER (39 T6): which one ran is host-shaped — an AMD box takes
    // the IBS-Op entry path, everywhere else the software-clock residency path —
    // and two operators on different hosts get different SELECTION RULES. Saying
    // which is what stops the pane from silently presenting them as the same.
    std::string smp = p.sampler.empty() ? std::string() : " [" + p.sampler + "]";
    if (!pick_is_weak_evidence(p)) {
        std::string s = "entry evidence" + smp + ": " + p.func +
                        " was observed being ENTERED — the same event the "
                        "capture waits for";
        if (p.weight)
            s += " (" + std::to_string(p.weight) + " entry samples";
        if (p.weight && p.sites)
            s += " from " + std::to_string(p.sites) + " call sites";
        if (p.weight)
            s += ")";
        return s;
    }
    std::string s = "WEAKER EVIDENCE — residency" + smp + ", not entry: " +
                    p.func + " was observed EXECUTING";
    if (p.weight)
        s += " (" + std::to_string(p.weight) + " residency samples)";
    s += ". That is a different claim: a function entered once and never "
         "re-entered ranks top here, and the capture's entry breakpoint would "
         "never fire on it.";
    return s;
}

std::string pick_walk_note(const AutoPick &p) {
    // An idle-window marker carries the WINDOW retry in attempt/of, not a
    // candidate ordinal — its whole story is in pick_evidence_label, and the
    // "candidate N of M, not seen entering" wording would be wrong for it.
    if (pick_is_idle_window(p))
        return std::string();
    if (p.attempt <= 1)
        return std::string();
    return "candidate " + std::to_string(p.attempt) + " of " +
           std::to_string(p.of) +
           " — the previous pick was not seen ENTERING within the entry wait, "
           "which is a genuine refusal about that candidate and not a fact "
           "about the target";
}

std::vector<SessionToast> live_session_toasts(const FeedbackInputs &prev,
                                              const FeedbackInputs &cur) {
    const LiveStatus &p = prev.status;
    const LiveStatus &c = cur.status;
    std::vector<SessionToast> out;
    // A NEW refusal (last_err changed to non-empty). Also stays a banner in-pane
    // (D7): the toast is the notification, the banner is the record.
    if (!c.last_err.empty() && c.last_err != p.last_err)
        out.push_back({ToastKind::Error, "refused: " + c.last_err, ""});
    // A NEW fatal (the host died / a hard failure).
    if (!c.fatal.empty() && c.fatal != p.fatal)
        out.push_back({ToastKind::Error, c.fatal, ""});
    // A session completed (one more ended than we last saw).
    if (c.sessions_ended > p.sessions_ended)
        out.push_back({ToastKind::Success,
                       c.last_stop_reason.empty()
                           ? "session ended"
                           : "session ended: " + c.last_stop_reason,
                       ""});
    // A NEW skip (the tracer worked, nothing to report — Info, not Error).
    if (c.skip_code != 0 && c.skip_code != p.skip_code)
        out.push_back({ToastKind::Info, "skipped: " + c.skip_reason, ""});
    // A save landed on a NEW path (first save, or a save to a different file).
    // Carry an "Open in Loom" button only for an exact capture — a statistical
    // one has no Loom, so the toast says so and offers no button.
    if (cur.saved_ok && !cur.saved_path.empty() &&
        cur.saved_path != prev.saved_path)
        out.push_back({ToastKind::Success,
                       cur.saved_statistical
                           ? "saved (statistical — no Loom): " + cur.saved_path
                           : "saved: " + cur.saved_path,
                       cur.saved_statistical ? "" : cur.saved_path});
    // A save that FAILED (a torn capture, a bad path): saved_ok is false but the
    // status line changed to something non-empty. Surface it; it is also shown
    // verbatim in-pane.
    else if (!cur.saved_ok && !cur.save_status.empty() &&
             cur.save_status != prev.save_status)
        out.push_back({ToastKind::Error, cur.save_status, ""});
    return out;
}

} // namespace asmdesk
