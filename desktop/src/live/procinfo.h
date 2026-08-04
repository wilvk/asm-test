// procinfo.h — the Process details model (`asmspy --info`'s payload).
//
// The pane's facts arrive as ONE `procinfo` event in a one-event .asmtrace,
// so this file is a decode plus two derivations, and nothing else: no ImGui,
// no fork, no engine. That split is what lets the whole model be driven from
// checked-in fixtures with no live process.
//
// The one rule worth stating twice: the wire carries RAW counters and a
// monotonic stamp, never a rate. %CPU therefore EXISTS only as a difference
// between two snapshots, and a first snapshot must report that it has none —
// rendering 0% would claim a measurement that was never taken.
#ifndef ASMDESK_LIVE_PROCINFO_H
#define ASMDESK_LIVE_PROCINFO_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"

namespace asmdesk {

struct PiThread {
    long tid = 0;
    std::string comm;
    char state = '?';
    std::string wchan;
    uint64_t cpu_jiffies = 0;
    bool have_syscall = false;
    long nr = -1;
    std::string name;
    std::vector<uint64_t> args;
    uint64_t pc = 0, sp = 0;
    std::string pc_sym;
    std::string why; // non-empty exactly when have_syscall is false
};

struct PiModule {
    std::string name, path;
    uint64_t base = 0, size = 0;
    bool exec = false;
    uint64_t syms = 0;
    // No has_symtab / build_id: the producer does not emit them (see the
    // asmspy_pi_module_t note). A field the wire never carries must not exist
    // here either, or the pane renders a default as if it were measured.
};

struct PiChild {
    long pid = 0;
    std::string comm;
};

struct PiMode {
    std::string mode; // "log" | "stream" | ... — the CLI/serve spelling
    bool ok = false;
    std::string why; // non-empty exactly when ok is false
};

struct ProcInfo {
    bool valid = false;
    std::string parse_error; // non-empty exactly when valid is false

    // identity
    long pid = 0, ppid = 0, pgid = 0, sid = 0;
    long uid = 0, euid = 0, gid = 0, egid = 0;
    std::string user, euser, comm;
    std::vector<std::string> argv;
    bool argv_truncated = false;
    std::string exe;
    bool exe_deleted = false;
    std::string cwd;
    char state = '?';
    uint64_t start_ticks = 0; // the cache key's second half — pid reuse guard
    double elapsed_s = 0;

    // runtime
    std::string runtime, evidence, interp;
    bool jitting = false, pie = false, static_linked = false;
    int elf_class = 0;

    // counters — RAW
    uint64_t ts_ns = 0, utime = 0, stime = 0;
    uint64_t clk_tck = 0;
    uint64_t rss_kb = 0, vsize_kb = 0, peak_rss_kb = 0;
    uint64_t io_read_bytes = 0, io_write_bytes = 0;
    bool io_readable = false, fds_readable = false;
    int fds = 0, oom_score = 0, nice = 0;
    int n_threads = 0; // the KERNEL's count; `threads` below is the rows we got

    std::vector<PiThread> threads; // capped at 64; n_threads may exceed it
    bool threads_truncated = false;

    uint64_t syms_total = 0, jit_methods = 0, anon_exec_bytes = 0;
    std::string jit_source;

    std::vector<PiModule> modules;
    bool modules_truncated = false;

    int attachable = -1; // 1 yes, 0 no, -1 unknown
    std::string attach_why, attach_remedy;
    std::vector<PiMode> modes;

    uint64_t ns_pid = 0, ns_net = 0, ns_mnt = 0, ns_user = 0;
    bool ns_differs = false;
    std::string cgroup;
    int seccomp = -1, no_new_privs = 0, dumpable = -1;

    std::vector<PiChild> children;
    bool children_truncated = false;
    bool budget_exceeded = false;
};

// Decode the single `procinfo` event of `r`. A recording without one yields
// valid=false and a stated parse_error — never a blank ProcInfo, which would
// render as a real process with every field zero.
ProcInfo procinfo_parse(const Recording &r);

// "0x1f" / "1f" -> 0x1f. Addresses cross the wire as hex STRINGS so a 64-bit
// pointer is not rounded through JSON's double; this is the one decoder.
uint64_t procinfo_parse_hex(const std::string &s);

struct ProcRates {
    bool have = false; // false = not yet measurable, NOT "measured zero"
    double cpu_pct = 0;
    double read_bps = 0, write_bps = 0;
};

// Derive rates from two snapshots OF THE SAME PROCESS. Returns have=false for
// a first snapshot, a different pid or start_ticks (pid reuse), a non-positive
// interval, or a missing tick rate — each of which makes the difference
// meaningless rather than zero.
ProcRates procinfo_rates(const ProcInfo &prev, const ProcInfo &cur);

// "a trace of this process will show names" / "...will show raw addresses",
// with the counts that justify it. The single sentence the code-surface
// numbers exist to support.
std::string procinfo_names_verdict(const ProcInfo &p);

} // namespace asmdesk
#endif // ASMDESK_LIVE_PROCINFO_H
