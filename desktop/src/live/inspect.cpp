// inspect.cpp — the Inspect door's decisions. See inspect.h for why these are
// pure functions rather than UI code (07-serve-live-host.md T5).
#include "live/inspect.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace asmdesk {

using nlohmann::json;

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
        // the target opted in is not readable from outside — so an honest
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

std::vector<ProcRow> list_processes() {
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
        rows.push_back(std::move(r));
    }
    ::closedir(d);
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
    // pick we know how to present honestly.
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

std::string pick_evidence_label(const AutoPick &p) {
    if (!pick_is_weak_evidence(p)) {
        std::string s = "entry evidence: " + p.func +
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
    std::string s = "WEAKER EVIDENCE — residency, not entry: " + p.func +
                    " was observed EXECUTING";
    if (p.weight)
        s += " (" + std::to_string(p.weight) + " residency samples)";
    s += ". That is a different claim: a function entered once and never "
         "re-entered ranks top here, and the capture's entry breakpoint would "
         "never fire on it.";
    return s;
}

std::string pick_walk_note(const AutoPick &p) {
    if (p.attempt <= 1)
        return std::string();
    return "candidate " + std::to_string(p.attempt) + " of " +
           std::to_string(p.of) +
           " — the previous pick was not seen ENTERING within the entry wait, "
           "which is an honest refusal about that candidate and not a fact "
           "about the target";
}

} // namespace asmdesk
