// crossing.cpp — the pure builder + dump of crossing.h. No ImGui, no GL, no
// I/O, no engine (D4).
#include "views/crossing.h"

#include <algorithm>
#include <cctype>
#include <map>

#include "space/locate.h"    // scene_locate_off — the ONE address route (50 T1)
#include "space/stepplace.h" // place_address — the shared plane arithmetic (T1)

namespace asmdesk {

namespace {

// The syscall NAME at the head of a payload-free `line`, or "" when none can
// be read. The engine prefixes a multi-threaded stream with "[tid] " (see this
// view's own tid note), so that prefix is skipped first.
std::string syscall_name_of(const std::string &line) {
    size_t i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        i++;
    if (i < line.size() && line[i] == '[') {
        const size_t close = line.find(']', i);
        if (close == std::string::npos)
            return std::string(); // malformed prefix: read no name at all
        i = close + 1;
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i])))
            i++;
    }
    const size_t start = i;
    while (i < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[i])) ||
            line[i] == '_'))
        i++;
    return line.substr(start, i - start);
}

// The DERIVED family table. Deliberately CONSERVATIVE: a name that is not
// listed lands in `Other`, the visible grey bucket, and is never folded into
// a neighbouring family on a guess (T2 step 3 — "never folded into a known
// class, never green-on-unknown"). Growing this table is a fidelity-neutral
// change; guessing from a prefix would not be.
space::SyscallClass class_of(const std::string &name) {
    using C = space::SyscallClass;
    static const std::map<std::string, C> kTable = {
        // file / descriptor I/O
        {"open", C::File},        {"openat", C::File},
        {"openat2", C::File},     {"close", C::File},
        {"read", C::File},        {"pread64", C::File},
        {"write", C::File},       {"pwrite64", C::File},
        {"readv", C::File},       {"writev", C::File},
        {"lseek", C::File},       {"stat", C::File},
        {"fstat", C::File},       {"lstat", C::File},
        {"newfstatat", C::File},  {"statx", C::File},
        {"access", C::File},      {"faccessat", C::File},
        {"unlink", C::File},      {"unlinkat", C::File},
        {"rename", C::File},      {"renameat", C::File},
        {"renameat2", C::File},   {"mkdir", C::File},
        {"mkdirat", C::File},     {"rmdir", C::File},
        {"getdents64", C::File},  {"fcntl", C::File},
        {"ioctl", C::File},       {"dup", C::File},
        {"dup2", C::File},        {"dup3", C::File},
        {"pipe", C::File},        {"pipe2", C::File},
        {"chdir", C::File},       {"fchdir", C::File},
        {"readlink", C::File},    {"readlinkat", C::File},
        {"truncate", C::File},    {"ftruncate", C::File},
        {"fsync", C::File},       {"fdatasync", C::File},
        {"chmod", C::File},       {"fchmod", C::File},
        {"fchmodat", C::File},    {"chown", C::File},
        {"fchown", C::File},      {"statfs", C::File},
        {"fstatfs", C::File},     {"sendfile", C::File},
        {"splice", C::File},
        // network
        {"socket", C::Net},       {"socketpair", C::Net},
        {"bind", C::Net},         {"listen", C::Net},
        {"accept", C::Net},       {"accept4", C::Net},
        {"connect", C::Net},      {"sendto", C::Net},
        {"recvfrom", C::Net},     {"sendmsg", C::Net},
        {"recvmsg", C::Net},      {"sendmmsg", C::Net},
        {"recvmmsg", C::Net},     {"shutdown", C::Net},
        {"getsockname", C::Net},  {"getpeername", C::Net},
        {"setsockopt", C::Net},   {"getsockopt", C::Net},
        // process / thread
        {"clone", C::Process},    {"clone3", C::Process},
        {"fork", C::Process},     {"vfork", C::Process},
        {"execve", C::Process},   {"execveat", C::Process},
        {"exit", C::Process},     {"exit_group", C::Process},
        {"wait4", C::Process},    {"waitid", C::Process},
        {"getpid", C::Process},   {"gettid", C::Process},
        {"getppid", C::Process},  {"prctl", C::Process},
        {"arch_prctl", C::Process},
        {"set_tid_address", C::Process},
        {"sched_yield", C::Process},
        {"futex", C::Process},
        // memory
        {"mmap", C::Memory},      {"munmap", C::Memory},
        {"mprotect", C::Memory},  {"mremap", C::Memory},
        {"brk", C::Memory},       {"madvise", C::Memory},
        {"mlock", C::Memory},     {"munlock", C::Memory},
        {"msync", C::Memory},     {"memfd_create", C::Memory},
        // signals
        {"rt_sigaction", C::Signal},
        {"rt_sigprocmask", C::Signal},
        {"rt_sigreturn", C::Signal},
        {"rt_sigsuspend", C::Signal},
        {"sigaltstack", C::Signal},
        {"signalfd4", C::Signal},  {"kill", C::Signal},
        {"tkill", C::Signal},      {"tgkill", C::Signal},
        // time
        {"clock_gettime", C::Time}, {"clock_getres", C::Time},
        {"clock_nanosleep", C::Time}, {"nanosleep", C::Time},
        {"gettimeofday", C::Time},  {"time", C::Time},
        {"timer_create", C::Time},  {"timerfd_create", C::Time},
        {"alarm", C::Time},
    };
    auto it = kTable.find(name);
    return it == kTable.end() ? C::Other : it->second;
}

// The return value's outcome, from the LAST " = " in the payload-free line.
// Anything that does not parse as a number — "= ?", a missing "=", a live
// "unfinished" line — is Unknown, the visible grey bucket. It is NEVER read as
// success: "we could not tell" and "it worked" are different facts.
space::SyscallOutcome outcome_of(const std::string &line) {
    const size_t eq = line.rfind(" = ");
    if (eq == std::string::npos)
        return space::SyscallOutcome::Unknown;
    size_t i = eq + 3;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        i++;
    if (i >= line.size())
        return space::SyscallOutcome::Unknown;
    const bool neg = line[i] == '-';
    if (neg || line[i] == '+')
        i++;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i])))
        return space::SyscallOutcome::Unknown;
    return neg ? space::SyscallOutcome::Error : space::SyscallOutcome::Ok;
}

// One offset-bearing `trace` event, with the per-tid vertex ordinal
// build_trajectories would assign it (`p.t = next_t[tid]++`,
// space/trajectory.cpp) — the SAME counter TrajPoint::t carries, so a spur
// hangs on the vertex the worldline really drew. Reproduced here rather than
// read back off a TrajectorySet because the trajectory model keeps no `seq`,
// and `seq` is the only thing that orders a syscall against an instruction.
struct TraceInsn {
    uint64_t seq = 0;
    uint64_t off = 0;
    int32_t tid = -1;
    uint64_t t = 0;
};

} // namespace

space::CrossingLayer build_crossing_layer(const SyscallView &v,
                                          const Recording &r,
                                          const space::Projection &proj) {
    space::CrossingLayer layer;

    std::vector<TraceInsn> insns;
    auto tr = r.by_kind.find("trace");
    if (tr != r.by_kind.end()) {
        std::map<int32_t, uint64_t> next_t;
        for (const Event &e : tr->second) {
            auto off = e.body.find("off");
            if (off == e.body.end() || !off->is_number())
                continue; // an offset-less trace event places no vertex
            TraceInsn in;
            in.seq = e.seq;
            in.off = off->get<uint64_t>();
            auto ti = e.body.find("tid");
            if (ti != e.body.end() && ti->is_number_integer())
                in.tid = ti->get<int32_t>();
            in.t = next_t[in.tid]++;
            insns.push_back(in);
        }
    }

    // --- self-gates (T2 step 5): no geometry, and always a stated reason ----
    if (insns.empty()) {
        layer.disabled_reason =
            "no `trace` worldline in this recording — there is no path to "
            "hang a kernel-crossing spur on, and this layer will not "
            "synthesise one to decorate";
        return layer;
    }
    if (!v.rows.empty() && !v.seq_present) {
        layer.disabled_reason =
            "this recording's syscall rows carry no stream position (`seq`, "
            "54 T3): there is no order to anchor a crossing by, and anchoring "
            "every call to the first instruction would be a fabrication";
        return layer;
    }
    layer.enabled = true;
    if (v.rows.empty())
        return layer; // enabled, and genuinely no crossings — not a refusal

    // The trace events are in stream order within their kind, so `seq` is
    // ascending: a binary search over it is exact, not an approximation.
    const bool ascending =
        std::is_sorted(insns.begin(), insns.end(),
                       [](const TraceInsn &a, const TraceInsn &b) {
                           return a.seq < b.seq;
                       });
    if (!ascending)
        std::stable_sort(insns.begin(), insns.end(),
                         [](const TraceInsn &a, const TraceInsn &b) {
                             return a.seq < b.seq;
                         });

    // A sparse or truncated trace makes even the NEAREST recorded instruction
    // a weaker claim than usual, so every anchor over such a recording draws
    // hollow. Read off the recording's own fidelity facts — a dropped or
    // capped stream is exactly "the instruction we anchored to may not be the
    // last one that really ran".
    const bool hollow = r.truncated() || r.dropped();

    // scene_locate_off (50 T1) is the ONE address route; memoised per distinct
    // offset because its ambiguity scan is O(trace events) per call and a
    // recording revisits few distinct anchor offsets.
    std::map<uint64_t, space::Located> placed;
    auto locate = [&](uint64_t off) -> const space::Located & {
        auto it = placed.find(off);
        if (it == placed.end())
            it = placed.emplace(off, space::scene_locate_off(proj, r, off))
                     .first;
        return it->second;
    };

    for (const SyscallRow &row : v.rows) {
        // The anchor: the LAST instruction with a SMALLER seq.
        const auto after = std::lower_bound(
            insns.begin(), insns.end(), row.seq,
            [](const TraceInsn &a, uint64_t s) { return a.seq < s; });
        if (after == insns.begin()) {
            // Before every recorded instruction. There is no earlier vertex,
            // and instruction 0 is NOT one: count it and draw nothing.
            layer.before_first_insn++;
            continue;
        }
        const TraceInsn &anchor = *(after - 1);
        const space::Located &aloc = locate(anchor.off);
        if (!aloc.ok) {
            layer.off_plane++;
            continue;
        }

        space::CrossingSpur sp;
        sp.row = row.index;
        sp.seq = row.seq;
        sp.anchor_addr = aloc.addr;
        sp.anchor_t = anchor.t;
        sp.anchor_tid = anchor.tid;
        sp.anchor_cell = aloc.cell;
        {
            const space::StepPlace pl = space::place_address(proj, aloc.addr);
            sp.anchor_u = pl.u;
            sp.anchor_v = pl.v;
        }

        // The resume vertex: the FIRST instruction with a GREATER seq. An
        // instruction at exactly row.seq cannot exist (one stream position,
        // one event), so `after` is already it.
        for (auto res = after; res != insns.end(); ++res) {
            if (res->seq <= row.seq)
                continue;
            const space::Located &rloc = locate(res->off);
            if (!rloc.ok)
                break; // an unplaceable resume draws no return spur
            sp.has_resume = true;
            sp.resume_addr = rloc.addr;
            sp.resume_t = res->t;
            const space::StepPlace pl = space::place_address(proj, rloc.addr);
            sp.resume_u = pl.u;
            sp.resume_v = pl.v;
            break;
        }

        // The ONE rail point both spurs terminate at — the midpoint of the two
        // vertices, or the anchor's own cell when the recording states no
        // resume. One point => zero along-rail extent => no duration implied.
        sp.rail_u = sp.has_resume ? 0.5f * (sp.anchor_u + sp.resume_u)
                                  : sp.anchor_u;
        sp.rail_v = sp.has_resume ? 0.5f * (sp.anchor_v + sp.resume_v)
                                  : sp.anchor_v;

        sp.cls = class_of(syscall_name_of(row.line));
        sp.outcome = outcome_of(row.line);
        sp.has_payload = row.has_payload;
        sp.payload_bytes = row.has_payload ? row.payload.size() : 0;
        sp.redacted = v.record_redacted;
        sp.hollow = hollow;
        layer.spurs.push_back(sp);
    }
    return layer;
}

std::string crossing_layer_dump(const space::CrossingLayer &layer) {
    std::string s;
    s += std::string("enabled=") + (layer.enabled ? "yes" : "no") + "\n";
    if (!layer.enabled) {
        s += "disabled_reason=" + layer.disabled_reason + "\n";
        return s;
    }
    s += "dwell=" + std::string(space::CrossingLayer::dwell_note()) + "\n";
    s += "anchor=" + std::string(space::CrossingLayer::anchor_label()) + "\n";
    s += "rail_span=" + std::to_string(space::CrossingLayer::rail_span()) +
         "\n";
    s += "before_first_insn=" + std::to_string(layer.before_first_insn) +
         " off_plane=" + std::to_string(layer.off_plane) + "\n";
    s += "spurs=" + std::to_string(layer.spurs.size()) + "\n";
    for (const space::CrossingSpur &sp : layer.spurs) {
        s += "  row=" + std::to_string(sp.row) +
             " seq=" + std::to_string(sp.seq) +
             " cls=" + space::syscall_class_name(sp.cls) +
             " ret=" + space::syscall_outcome_name(sp.outcome) +
             " anchor_t=" + std::to_string(sp.anchor_t) +
             " cell=" + std::to_string(sp.anchor_cell) +
             " resume=" + (sp.has_resume ? "yes" : "no") +
             " payload_bytes=" + std::to_string(sp.payload_bytes) +
             (sp.redacted ? std::string(" [") +
                                space::CrossingLayer::redacted_label() + "]"
                          : std::string()) +
             (sp.hollow ? " hollow" : "") + "\n";
    }
    return s;
}

} // namespace asmdesk
