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
#include <unordered_map> // pid -> row, for the process tree's parent lookup

#include <dirent.h>
#include <fcntl.h> // open/O_RDONLY/O_CLOEXEC — the attach-probe mem open
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
unsigned long long proc_cpu_at(const std::string &stat_path) {
    const std::string stat = read_file(stat_path);
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

unsigned long long proc_cpu(const std::string &pid) {
    return proc_cpu_at("/proc/" + pid + "/stat");
}

// The state letter out of /proc/.../stat — the first field after the comm's
// closing paren, so it is read the same way proc_cpu_at reads past it.
char proc_state_at(const std::string &stat_path) {
    const std::string stat = read_file(stat_path);
    const size_t rp = stat.rfind(')');
    if (rp == std::string::npos || rp + 2 >= stat.size())
        return '?';
    return stat[rp + 2];
}

// Every task of `pid`, LEADER FIRST then ascending by tid — the same order
// cli/asmspy_tidsort.h gives the details pane. The order matters for the same
// reason it does there: the leader is the row an operator looks for, and a row
// that moves while you read it is a row you cannot point at.
//
// Returns empty when the task dir cannot be read at all, which the caller must
// not confuse with a single-threaded process (see ProcRow::threads).
std::vector<ProcThread> read_threads(long pid, bool sample_cpu) {
    std::vector<ProcThread> out;
    const std::string base = "/proc/" + std::to_string(pid) + "/task";
    DIR *d = ::opendir(base.c_str());
    if (!d)
        return out;
    struct dirent *e;
    while ((e = ::readdir(d)) != nullptr) {
        if (!all_digits(e->d_name))
            continue;
        ProcThread t;
        t.tid = std::atol(e->d_name);
        const std::string tp = base + "/" + e->d_name;
        t.comm = read_file(tp + "/comm");
        while (!t.comm.empty() &&
               (t.comm.back() == '\n' || t.comm.back() == ' '))
            t.comm.pop_back();
        t.state = proc_state_at(tp + "/stat");
        if (sample_cpu)
            t.cpu = proc_cpu_at(tp + "/stat");
        out.push_back(std::move(t));
    }
    ::closedir(d);
    std::sort(
        out.begin(), out.end(),
        [](const ProcThread &a, const ProcThread &b) { return a.tid < b.tid; });
    // Leader first — a ROTATE, not a swap, so the rest stay ascending.
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].tid == pid) {
            std::rotate(out.begin(), out.begin() + static_cast<long>(i),
                        out.begin() + static_cast<long>(i) + 1);
            break;
        }
    return out;
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

    // Whether we may actually attach, MEASURED rather than inferred from the
    // Yama scope. open("/proc/<pid>/mem", O_RDONLY) runs the kernel's own
    // ptrace_may_access(PTRACE_MODE_ATTACH_FSCREDS) -- the same check
    // PTRACE_ATTACH runs -- and changes no state.
    //
    // NOT PTRACE_SEIZE: every DETACH after a SEIZE probe returns ESRCH (the
    // tracee is not in a ptrace-stop, which `man 2 ptrace` requires for it), so
    // the probe LEAVES THE TARGET SEIZED and one SIGCONT later it reads
    // "State: t (tracing stop)". Measured, on a real victim.
    //
    // A success under scope 1 means the target called PR_SET_PTRACER (or is our
    // descendant), which is exactly what target_opted_in names. This is the
    // branch that makes a browser's content processes reachable: 12 of 15 live
    // Firefox processes pass it today, unprivileged, at ptrace_scope=1.
    {
        int fd = ::open((base + "/mem").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            f.target_opted_in = true;
        }
    }

    // A kernel thread has no exe link (no mm). readlink failing for any other
    // reason also leaves us unable to read the ELF, so the class stays 0 =
    // unknown rather than being assumed 64. Discriminate on VmSize, which
    // /proc/<pid>/status carries for every process WITH an address space and
    // omits for every kernel thread -- and which is world-readable, so it
    // answers for processes we cannot otherwise read (readlink on /exe fails
    // with EACCES far more often than it fails because the target is a kernel
    // thread; the two send an operator to completely different places).
    char buf[512];
    ssize_t n = ::readlink((base + "/exe").c_str(), buf, sizeof buf - 1);
    if (n <= 0) {
        f.is_kthread = status_field(status, "VmSize").empty();
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
        // Lineage, from the SAME read as Uid above — the Processes pane nests
        // the table by it (proc_tree_layout). Unreadable leaves 0, which the
        // tree treats as a root; see ProcRow::ppid on why that is not conflated
        // with a ppid we read but cannot find.
        std::string ppid = status_field(status, "PPid");
        r.ppid = ppid.empty() ? 0 : std::atol(ppid.c_str());
        r.facts = probe_attach(r.pid, scope, our_uid, have_cap);
        r.verdict = attach_verdict(r.facts);
        // First CPU snapshot; the delta over the window below becomes ::cpu. Only
        // the activity sort asks for this, so an unsampled list never reads /stat
        // and never sleeps.
        if (sample_cpu)
            r.cpu = proc_cpu(e->d_name);
        // The tasks under this pid. Measured on a 604-process desktop: the
        // readdir alone is 2.5ms and the per-thread comm+stat 12ms, against
        // ~10ms for the process walk this rides on — so the tree can offer
        // threads without a second scan, a background job, or a flag.
        r.threads = read_threads(r.pid, sample_cpu);
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
            // The same difference per task. A thread that EXITED during the
            // window has no /stat to re-read, so proc_cpu_at returns 0 and the
            // subtraction floors to 0 rather than underflowing into a huge
            // bogus delta — the row still says what it measured, which is
            // nothing.
            const std::string tb = "/proc/" + std::to_string(r.pid) + "/task/";
            for (ProcThread &t : r.threads) {
                const unsigned long long t1 =
                    proc_cpu_at(tb + std::to_string(t.tid) + "/stat");
                t.cpu = (t1 > t.cpu) ? t1 - t.cpu : 0;
            }
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const ProcRow &a, const ProcRow &b) { return a.pid < b.pid; });
    return rows;
}

// ---------------------------------------------------------------------------
// the process tree
// ---------------------------------------------------------------------------

std::vector<ProcTreeRow> proc_tree_layout(const std::vector<ProcRow> &rows,
                                          const std::vector<char> &visible,
                                          const ProcTreeExpansion &exp,
                                          bool descending) {
    std::vector<ProcTreeRow> out;
    const size_t n = rows.size();
    if (n == 0)
        return out;
    // A short `visible` reads as "not visible", never as "shown by default":
    // a caller that got the mask wrong must under-draw, not leak rows its own
    // attachability gate meant to withhold.
    auto vis = [&](size_t i) { return i < visible.size() && visible[i] != 0; };

    // pid -> index over the WHOLE snapshot, visible or not. A duplicate pid
    // cannot come out of one /proc readdir; emplace keeps the first anyway,
    // so a malformed caller-built vector re-parents nothing silently.
    std::unordered_map<long, size_t> by_pid;
    by_pid.reserve(n * 2);
    for (size_t i = 0; i < n; ++i)
        by_pid.emplace(rows[i].pid, i);

    const size_t kNone = static_cast<size_t>(-1);
    std::vector<size_t> parent(n, kNone);
    for (size_t i = 0; i < n; ++i) {
        if (rows[i].ppid <= 0 || rows[i].ppid == rows[i].pid)
            continue;
        auto it = by_pid.find(rows[i].ppid);
        if (it != by_pid.end() && it->second != i)
            parent[i] = it->second;
    }
    // Cut cycles. /proc is a tree, but this snapshot is read pid by pid over
    // milliseconds, and a pid recycled mid-read can close a loop between two
    // rows that were never actually related. Left in, such a loop is not a
    // mis-drawn row: every member of it has a parent, so none of them lands
    // in `roots`, the pre-order walk below never reaches them, and they
    // VANISH from a table whose whole job is to list what is running. So
    // every row whose ancestor chain runs longer than the snapshot itself is
    // cut loose to a root. Applied per row, so every member of a cycle is cut
    // and the loop is genuinely broken rather than moved onto someone else.
    std::vector<size_t> cut;
    for (size_t i = 0; i < n; ++i) {
        size_t steps = 0, at = i;
        while (parent[at] != kNone && ++steps <= n)
            at = parent[at];
        if (steps > n)
            cut.push_back(i);
    }
    for (size_t i : cut)
        parent[i] = kNone;

    // Sibling groups, each ordered by pid in the requested direction. `rows`
    // arrives pid-sorted from list_processes, but this does not assume it —
    // the order is the pane's to choose, and a caller-built vector need not
    // be sorted at all.
    std::vector<std::vector<size_t>> kids(n);
    std::vector<size_t> roots;
    for (size_t i = 0; i < n; ++i)
        (parent[i] == kNone ? roots : kids[parent[i]]).push_back(i);
    auto by_pid_order = [&](size_t a, size_t b) {
        return descending ? rows[a].pid > rows[b].pid
                          : rows[a].pid < rows[b].pid;
    };
    std::sort(roots.begin(), roots.end(), by_pid_order);
    for (auto &k : kids)
        std::sort(k.begin(), k.end(), by_pid_order);

    // The └ vs ├ decision, made over the DRAWN siblings (see ProcTreeRow::
    // last_sibling) rather than the snapshot's: the last visible entry of
    // each group gets the └, and a group with none gets nothing.
    std::vector<char> is_last(n, 0);
    auto mark_last = [&](const std::vector<size_t> &group) {
        for (size_t k = group.size(); k-- > 0;)
            if (vis(group[k])) {
                is_last[group[k]] = 1;
                return;
            }
    };
    mark_last(roots);
    for (const auto &k : kids)
        mark_last(k);

    // What to SAY about each row's parent — computed from the snapshot and
    // the visibility mask, independent of the nesting above. A cycle-cut row
    // is drawn at the root but its ppid still names a real row in this table,
    // and reporting that honestly is better than calling it Unknown because
    // the nesting gave up on it.
    auto parent_kind = [&](size_t i) {
        if (rows[i].ppid <= 0 || rows[i].ppid == rows[i].pid)
            return ProcParent::None;
        auto it = by_pid.find(rows[i].ppid);
        if (it == by_pid.end())
            return ProcParent::Unknown;
        return vis(it->second) ? ProcParent::Shown : ProcParent::Hidden;
    };

    // Would opening THIS process reveal anything? Not "does it have children"
    // — a subtree whose every node the gate removed reveals nothing, and an
    // expander that opens onto no new row is a control that lies about what
    // it does. The answer is "is there a visible node on the frontier below
    // me", where the frontier stops at the first visible node on each path,
    // because an INVISIBLE child blocks nothing (there is no row to click) and
    // so its own descendants are on my frontier too.
    //
    // Computed in reverse pre-order: children always follow their parent in a
    // pre-order list, so walking it backwards has every child settled before
    // the parent asks about it — a post-order without a second traversal.
    std::vector<size_t> pre;
    pre.reserve(n);
    {
        std::vector<size_t> st(roots.rbegin(), roots.rend());
        while (!st.empty()) {
            size_t node = st.back();
            st.pop_back();
            pre.push_back(node);
            for (size_t j = kids[node].size(); j-- > 0;)
                st.push_back(kids[node][j]);
        }
    }
    std::vector<char> frontier(n, 0);
    for (size_t k = pre.size(); k-- > 0;) {
        const size_t node = pre[k];
        for (size_t c : kids[node])
            if (vis(c) || frontier[c]) {
                frontier[node] = 1;
                break;
            }
    }

    auto is_open = [&](long id) { return exp.open.find(id) != exp.open.end(); };
    // A multi-threaded process gets a threads group. A single-threaded one
    // does not: "1 thread" under every row is noise, and the one thread is the
    // process, already on screen.
    auto has_group = [&](size_t i) { return rows[i].threads.size() > 1; };

    // Pre-order walk of the FULL tree, emitting only the visible rows — which
    // is how a child keeps its true depth across a hidden ancestor. An
    // explicit stack rather than recursion: the depth here is data read from
    // another process, and a pathological snapshot must not blow ours.
    //
    // `blocked` is what collapse actually means. It propagates from a VISIBLE
    // CLOSED ancestor only: an invisible one contributes nothing, because the
    // operator has no row to open and its subtree would otherwise be stranded
    // behind a control that is not on screen.
    struct Frame {
        ProcNode kind;
        size_t node;
        size_t thread_at;
        int depth;
        bool blocked;
        bool last;
    };
    // Pushed in reverse so the stack pops them in sibling order.
    std::vector<Frame> todo;
    for (size_t k = roots.size(); k-- > 0;)
        todo.push_back({ProcNode::Process, roots[k], 0, 0, false, false});
    while (!todo.empty()) {
        Frame f = todo.back();
        todo.pop_back();

        if (f.kind == ProcNode::Thread) {
            ProcTreeRow t;
            t.kind = ProcNode::Thread;
            t.index = f.node;
            t.thread_at = f.thread_at;
            t.depth = f.depth;
            t.parent = ProcParent::Shown; // its group is right above it
            t.last_sibling = f.last;
            t.node_id = 0; // a thread opens nothing
            t.pid = rows[f.node].pid;
            t.tid = rows[f.node].threads[f.thread_at].tid;
            out.push_back(t);
            continue;
        }

        if (f.kind == ProcNode::ThreadGroup) {
            const long id = -rows[f.node].pid;
            ProcTreeRow t;
            t.kind = ProcNode::ThreadGroup;
            t.index = f.node;
            t.depth = f.depth;
            t.parent = ProcParent::Shown;
            t.last_sibling = f.last;
            t.expandable = true;
            t.expanded = is_open(id);
            t.node_id = id;
            t.pid = rows[f.node].pid;
            out.push_back(t);
            if (!t.expanded)
                continue;
            const std::vector<ProcThread> &th = rows[f.node].threads;
            for (size_t j = th.size(); j-- > 0;)
                todo.push_back({ProcNode::Thread, f.node, j, f.depth + 1, false,
                                j + 1 == th.size()});
            continue;
        }

        // A process.
        const bool shown = vis(f.node) && !f.blocked;
        const long id = rows[f.node].pid;
        const bool group = has_group(f.node);
        if (shown) {
            ProcTreeRow t;
            t.kind = ProcNode::Process;
            t.index = f.node;
            t.depth = f.depth;
            t.parent = parent_kind(f.node);
            t.last_sibling = is_last[f.node] != 0;
            t.expandable = frontier[f.node] != 0 || group;
            t.expanded = is_open(id);
            t.node_id = id;
            t.pid = id;
            out.push_back(t);
        }
        // Children are blocked by a VISIBLE, CLOSED ancestor — never by an
        // invisible one. show_all_processes lifts it for processes only (see
        // ProcTreeExpansion): a filter reveals what it matched, it does not
        // dump a hundred thread rows nobody typed a query for.
        const bool closes =
            !exp.show_all_processes && vis(f.node) && !is_open(id);
        const bool kids_blocked = f.blocked || closes;

        const std::vector<size_t> &k = kids[f.node];
        // Draw order under a process: its threads group FIRST (locality — it
        // describes THIS row, and burying it after a deep child subtree puts
        // it pages away from the process it belongs to), then the child
        // processes. So the last CHILD PROCESS carries the └, and the group
        // carries it only when there is no child process to follow it.
        size_t last_visible_kid = kids.size(); // sentinel: none
        for (size_t j = k.size(); j-- > 0;)
            if (vis(k[j])) {
                last_visible_kid = k[j];
                break;
            }
        for (size_t j = k.size(); j-- > 0;)
            todo.push_back({ProcNode::Process, k[j], 0, f.depth + 1,
                            kids_blocked, k[j] == last_visible_kid});
        // The group is drawn only when the process itself is on screen and
        // open — it is a child of that row, not of the snapshot.
        if (shown && group && is_open(id))
            todo.push_back({ProcNode::ThreadGroup, f.node, 0, f.depth + 1,
                            false, last_visible_kid == kids.size()});
    }
    return out;
}

std::vector<long> proc_tree_all_nodes(const std::vector<ProcRow> &rows,
                                      const std::vector<char> &visible) {
    // Deliberately NOT filtered by `visible`: "expand all" means nothing left
    // closed, and a node the gate hides today is one the operator may untick
    // the gate to see a moment later — leaving it shut would make Expand all
    // depend on the order the two controls were touched in.
    (void)visible;
    std::vector<long> out;
    out.reserve(rows.size() * 2);
    for (const ProcRow &r : rows) {
        out.push_back(r.pid);
        if (r.threads.size() > 1)
            out.push_back(-r.pid);
    }
    return out;
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

std::string pick_region_spec(const AutoPick &p) {
    // An idle window picked NOTHING; its zero base/len are the sentinel, not a
    // region. Handing them to a scoped leg would single-step address 0.
    if (pick_is_idle_window(p) || p.base == 0 || p.len == 0)
        return std::string();
    // The pick's OWN base+len, in parse_region_spec's grammar. Not the func
    // name: a name is re-resolved against the symbol table by the serve host,
    // and a duplicated static symbol would resolve to a different function than
    // the one the sampler actually watched.
    char buf[64];
    std::snprintf(buf, sizeof buf, "0x%llx:%llu", (unsigned long long)p.base,
                  (unsigned long long)p.len);
    return std::string(buf);
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
