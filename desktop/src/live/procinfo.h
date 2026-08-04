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

// --- the runner: automatic on selection, without a fork per row ----------
//
// The pane probes AUTOMATICALLY as the selection moves, which is only safe
// because `asmspy --info` never attaches. Three rules keep it cheap as well
// as safe, and all three are timing, so the clock is a PARAMETER rather than
// read internally — that is what makes them assertable with no sleeping:
//
//  - a 250 ms DEBOUNCE, so arrowing down a table probes the row you stop on,
//    not every row you pass;
//  - a cache keyed on (pid, start_ticks) — the second half is the pid-reuse
//    guard: without it, two cache entries for a recycled pid collapse into
//    one and the newest-first lookup (procinfo_tick's switch block) has
//    nothing left to prefer, so it silently falls back to whichever is
//    left — usually the dead process. The key does NOT by itself guarantee
//    a reused pid never renders the dead process's card: in the
//    SINGLE-entry case (the dead process is the only one ever cached),
//    re-selecting that pid still renders it, labelled "cached", until a
//    fresh probe corrects it — a real residual, not fixable at the lookup
//    without changing what a bare pid selection is allowed to assume;
//  - a 2 s DEADLINE, after which the child is killed and the pane says so.
//
// A fourth rule, added on review (gui-process-details Task 6, round 2): a
// FAILURE BACKOFF. Only a successful read used to reset the refresh clock, so
// a target that fails every time (a bad path, a mismatch, a hang) was
// retried at frame rate forever — a fork+exec per frame is not "cheap" by
// any definition. `next_retry_at`/`fail_count` below track that separately
// from `last_ok_at`, which stays reserved for "when did the shown data last
// come from a REAL success."
struct ProcInfoCacheEntry {
    ProcInfo info;
    // procinfo_tick's `now_s` at the moment THIS entry's probe completed.
    // Freshness for a cache HIT is computed from this, never from the
    // runner's global `last_ok_at` — that field is the CURRENT target's own
    // last success, a different process's timestamp for anything just
    // switched to, and using it here is exactly the "measured zero" lie the
    // rest of this model refuses to tell.
    double read_at = -1;
};

struct ProcInfoRunner {
    std::string asmspy_path; // "" -> resolve_asmspy_path()
    bool visible = true;     // the pane is shown AND the window is focused

    // Tunables, named so the test can pin them rather than guess.
    double debounce_s = 0.25;
    double refresh_s = 2.0;
    double deadline_s = 2.0;
    // Bytes buffered from one child before the pane gives up on it and says
    // so, rather than growing unboundedly against a runaway/wrong binary
    // (Connect lets asmspy_path be pointed at anything). Small in tests, on
    // the order of MB in the pane.
    size_t max_buf_bytes = 4 * 1024 * 1024;

    int spawns = 0; // lifetime count — the test's evidence of a fork

    // Everything below is internal state; the pane reads it through the
    // accessors, never directly.
    long want_pid = 0, in_flight_pid = 0;
    double want_since = -1, spawned_at = -1, last_ok_at = -1;
    // Failure backoff, tracked SEPARATELY from last_ok_at (which only ever
    // moves on a real success). A tick may spawn only once now_s >=
    // next_retry_at; each failure pushes next_retry_at further out.
    int fail_count = 0;
    double next_retry_at = -1;
    // The path last seen when the backoff above was (re)computed. An
    // operator who fixes asmspy_path in Connect after a run of failures
    // should not still wait out the OLD path's backoff — a path change is
    // detected at the top of procinfo_tick and clears fail_count/
    // next_retry_at, same as a selection change does.
    std::string last_asmspy_path_seen;
    // The last clock value procinfo_tick saw. procinfo_status is const and
    // takes no clock, so freshness ("read 0.4s ago") is computed against
    // this rather than against a wall-clock read inside the getter — which
    // would make the status drift from the frame that produced it.
    double last_tick_s = 0;
    int child_pid = 0, child_fd = -1;
    std::string buf, status;
    ProcInfo shown, prev;
    ProcRates rates;
    // Bounded LRU, true LRU (touching an existing key moves it to the back;
    // eviction always drops the front) — a re-probed pid must count as
    // recently used, not sit at its original insertion position forever.
    std::vector<std::pair<std::pair<long, uint64_t>, ProcInfoCacheEntry>> cache;
    static constexpr size_t kCacheCap = 32;

    ~ProcInfoRunner();
};

// Advance one frame. `selected_pid` is InspectState::selected_pid; `now_s` is
// a monotonic seconds clock (ImGui::GetTime() in the pane, a literal in the
// tests). Spawns, reads, reaps, expires and caches — the pane calls only this.
void procinfo_tick(ProcInfoRunner &r, long selected_pid, double now_s);

// The snapshot to draw (valid=false while nothing has arrived yet).
const ProcInfo &procinfo_current(const ProcInfoRunner &r);
// Rates against the previous snapshot of the SAME process (have=false until a
// second one lands).
const ProcRates &procinfo_current_rates(const ProcInfoRunner &r);
// A human line for the pane's header: how fresh, or what went wrong. Never
// empty — "nothing yet" is itself a state worth naming.
std::string procinfo_status(const ProcInfoRunner &r);

} // namespace asmdesk
#endif // ASMDESK_LIVE_PROCINFO_H
