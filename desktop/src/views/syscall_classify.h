// syscall_classify.h — the ONE parse of a payload-free syscall `line` into its
// derived family and outcome. Extracted verbatim from crossing.cpp so the
// crossing spurs (3D) and the session strip (2D) classify from the same table
// and can never drift. The rules are the crossing layer's, unchanged:
//   - a name that is not listed lands in SyscallClass::Other, the VISIBLE grey
//     bucket, and is never folded into a neighbouring family on a guess;
//   - an outcome that does not parse ("= ?", missing "=", "unfinished") is
//     Unknown and is NEVER read as success;
//   - the engine's "[tid] " line prefix is skipped before the name; a
//     malformed prefix reads NO name at all rather than a wrong one.
// Header-only (C++17 inline): links nowhere, so no mk/ churn and both
// consumers share the single function-local table instance.
#ifndef ASMDESK_VIEWS_SYSCALL_CLASSIFY_H
#define ASMDESK_VIEWS_SYSCALL_CLASSIFY_H

#include <cctype>
#include <map>
#include <string>

#include "space/crossing.h" // SyscallClass, SyscallOutcome

namespace asmdesk {

// The syscall NAME at the head of a payload-free `line`, or "" when none can
// be read. The engine prefixes a multi-threaded stream with "[tid] " (see this
// view's own tid note), so that prefix is skipped first.
inline std::string syscall_name_of(const std::string &line) {
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
inline space::SyscallClass syscall_class_of(const std::string &name) {
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
inline space::SyscallOutcome syscall_outcome_of(const std::string &line) {
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

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SYSCALL_CLASSIFY_H
