// procinfo.cpp — decode `asmspy --info`'s wire body into asmdesk::ProcInfo,
// plus the two judgement calls (rate derivation, names verdict).
//
// The decode is mechanical `json` field reads with `.value(key, default)`;
// pc/sp/base/args are the one exception — each crosses procinfo_parse_hex,
// never json's number path. NOT because nlohmann itself would round them:
// it stores an unsigned JSON number as number_unsigned_t, a real uint64_t,
// and this file decodes ts_ns/io_read_bytes/io_write_bytes/peak_rss_kb
// exactly that way a few lines below, verified exact. The reason is the
// SCHEMA CONTRACT (docs/internal/gui/asmtrace-schema.md): pc/sp/base/args
// cross the wire as hex STRINGS specifically because a JSON number is a
// double in MANY OTHER readers, which would silently round a real 64-bit
// pointer — asmspy holds every consumer to that contract, this one
// included, so a bare number showing up in one of these four here would
// mean the WIRE broke its own promise, not that this reader could safely
// relax it. Every "absence" section (a thread's syscall_why, a refused
// mode's why, the attach_why) is read as the alternative branch of an
// if/else, never coerced into the same empty default a present-but-blank
// field would also produce.
#include "live/procinfo.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "live/session.h" // resolve_asmspy_path()

namespace asmdesk {

using nlohmann::json;

uint64_t procinfo_parse_hex(const std::string &s) {
    if (s.empty())
        return 0;
    return std::strtoull(s.c_str(), nullptr, 16); // base 16 accepts "0x…" too
}

namespace {

char first_char(const std::string &s, char dflt) {
    return s.empty() ? dflt : s[0];
}

std::vector<std::string> parse_str_array(const json &j, const char *key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const json &e : j[key])
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

PiThread parse_thread(const json &t) {
    PiThread pt;
    pt.tid = t.value("tid", 0L);
    pt.comm = t.value("comm", std::string());
    pt.state = first_char(t.value("state", std::string()), '?');
    pt.wchan = t.value("wchan", std::string());
    pt.cpu_jiffies = t.value("cpu_jiffies", (uint64_t)0);
    // Absence survives as absence: a thread either carries `syscall` (and
    // have_syscall becomes true) or it carries `syscall_why` — never both,
    // and never a have_syscall=true with the object silently missing.
    pt.have_syscall = t.contains("syscall") && t["syscall"].is_object();
    if (pt.have_syscall) {
        const json &sc = t["syscall"];
        pt.nr = sc.value("nr", -1L);
        pt.name = sc.value("name", std::string());
        if (sc.contains("args") && sc["args"].is_array())
            for (const json &a : sc["args"])
                if (a.is_string())
                    pt.args.push_back(procinfo_parse_hex(a.get<std::string>()));
        pt.pc = procinfo_parse_hex(sc.value("pc", std::string()));
        pt.sp = procinfo_parse_hex(sc.value("sp", std::string()));
        pt.pc_sym = sc.value("pc_sym", std::string());
    } else {
        pt.why = t.value("syscall_why", std::string());
    }
    return pt;
}

PiModule parse_module(const json &m) {
    PiModule pm;
    pm.name = m.value("name", std::string());
    pm.path = m.value("path", std::string());
    pm.base = procinfo_parse_hex(m.value("base", std::string()));
    pm.size = m.value("size", (uint64_t)0);
    pm.exec = m.value("exec", false);
    pm.syms = m.value("syms", (uint64_t)0);
    return pm;
}

PiMode parse_mode(const json &m) {
    PiMode pm;
    pm.mode = m.value("mode", std::string());
    pm.ok = m.value("ok", false);
    pm.why = m.value("why", std::string());
    return pm;
}

PiChild parse_child(const json &c) {
    PiChild pc;
    pc.pid = c.value("pid", 0L);
    pc.comm = c.value("comm", std::string());
    return pc;
}

} // namespace

ProcInfo procinfo_parse(const Recording &r) {
    ProcInfo p;
    auto it = r.by_kind.find("procinfo");
    if (it == r.by_kind.end() || it->second.empty()) {
        p.parse_error = "no procinfo event in this recording";
        return p;
    }
    const json &body = it->second.front().body;
    p.valid = true;

    if (body.contains("identity") && body["identity"].is_object()) {
        const json &id = body["identity"];
        p.pid = id.value("pid", 0L);
        p.ppid = id.value("ppid", 0L);
        p.pgid = id.value("pgid", 0L);
        p.sid = id.value("sid", 0L);
        p.uid = id.value("uid", 0L);
        p.euid = id.value("euid", 0L);
        p.gid = id.value("gid", 0L);
        p.egid = id.value("egid", 0L);
        p.user = id.value("user", std::string());
        p.euser = id.value("euser", std::string());
        p.comm = id.value("comm", std::string());
        p.argv = parse_str_array(id, "argv");
        p.argv_truncated = id.value("argv_truncated", false);
        p.exe = id.value("exe", std::string());
        p.exe_deleted = id.value("exe_deleted", false);
        p.cwd = id.value("cwd", std::string());
        p.state = first_char(id.value("state", std::string()), '?');
        p.start_ticks = id.value("start_ticks", (uint64_t)0);
        p.elapsed_s = id.value("elapsed_s", 0.0);
    }

    if (body.contains("runtime") && body["runtime"].is_object()) {
        const json &rt = body["runtime"];
        p.runtime = rt.value("runtime", std::string());
        p.evidence = rt.value("evidence", std::string());
        p.jitting = rt.value("jitting", false);
        p.elf_class = rt.value("elf_class", 0);
        p.pie = rt.value("pie", false);
        // The wire's key is "static" (asmspy_fingerprint_t::static_linked);
        // ProcInfo spells it out because `static` is a keyword.
        p.static_linked = rt.value("static", false);
        p.interp = rt.value("interp", std::string());
    }

    if (body.contains("counters") && body["counters"].is_object()) {
        const json &c = body["counters"];
        p.ts_ns = c.value("ts_ns", (uint64_t)0);
        p.utime = c.value("utime", (uint64_t)0);
        p.stime = c.value("stime", (uint64_t)0);
        p.clk_tck = c.value("clk_tck", (uint64_t)0);
        p.rss_kb = c.value("rss_kb", (uint64_t)0);
        p.vsize_kb = c.value("vsize_kb", (uint64_t)0);
        p.peak_rss_kb = c.value("peak_rss_kb", (uint64_t)0);
        p.io_read_bytes = c.value("io_read_bytes", (uint64_t)0);
        p.io_write_bytes = c.value("io_write_bytes", (uint64_t)0);
        p.io_readable = c.value("io_readable", false);
        p.fds = c.value("fds", 0);
        p.fds_readable = c.value("fds_readable", false);
        p.oom_score = c.value("oom_score", 0);
        p.nice = c.value("nice", 0);
        // This "threads" is the KERNEL's own count (asmspy_procinfo_t::threads),
        // distinct from the `threads` ARRAY at the body's top level below —
        // same wire word, two different things, deliberately not merged.
        p.n_threads = c.value("threads", 0);
    }

    if (body.contains("threads") && body["threads"].is_array())
        for (const json &t : body["threads"])
            p.threads.push_back(parse_thread(t));
    p.threads_truncated = body.value("threads_truncated", false);

    if (body.contains("code") && body["code"].is_object()) {
        const json &cd = body["code"];
        p.syms_total = cd.value("syms_total", (uint64_t)0);
        p.jit_methods = cd.value("jit_methods", (uint64_t)0);
        p.jit_source = cd.value("jit_source", std::string());
        p.anon_exec_bytes = cd.value("anon_exec_bytes", (uint64_t)0);
        // See ProcInfo::maps_readable for why the default is true rather
        // than the usual zero/false.
        p.maps_readable = cd.value("maps_readable", true);
    }

    if (body.contains("modules") && body["modules"].is_array())
        for (const json &m : body["modules"])
            p.modules.push_back(parse_module(m));
    p.modules_truncated = body.value("modules_truncated", false);

    if (body.contains("trace") && body["trace"].is_object()) {
        const json &tr = body["trace"];
        p.attachable = tr.value("attachable", -1);
        p.attach_why = tr.value("why", std::string());
        p.attach_remedy = tr.value("remedy", std::string());
        if (tr.contains("modes") && tr["modes"].is_array())
            for (const json &m : tr["modes"])
                p.modes.push_back(parse_mode(m));
    }

    if (body.contains("containment") && body["containment"].is_object()) {
        const json &ns = body["containment"];
        p.ns_pid = ns.value("ns_pid", (uint64_t)0);
        p.ns_net = ns.value("ns_net", (uint64_t)0);
        p.ns_mnt = ns.value("ns_mnt", (uint64_t)0);
        p.ns_user = ns.value("ns_user", (uint64_t)0);
        // Wire key is "differs"; the struct spells out WHAT differs
        // (ns_differs) since "differs" alone says nothing on its own.
        p.ns_differs = ns.value("differs", false);
        p.cgroup = ns.value("cgroup", std::string());
        p.seccomp = ns.value("seccomp", -1);
        p.no_new_privs = ns.value("no_new_privs", 0);
        p.dumpable = ns.value("dumpable", -1);
    }

    if (body.contains("children") && body["children"].is_array())
        for (const json &c : body["children"])
            p.children.push_back(parse_child(c));
    p.children_truncated = body.value("children_truncated", false);
    p.budget_exceeded = body.value("budget_exceeded", false);

    // `host` — the probing BINARY's own capability verdict (finding 8). Keyed
    // on PRESENCE, not on the value: a producer that never emitted this key has
    // no opinion, and defaulting host_perf_ok to false would manufacture a
    // refusal nobody measured — the same failure the maps_readable field above
    // documents, in the other direction.
    if (body.contains("host") && body["host"].is_object()) {
        const json &h = body["host"];
        if (h.contains("perf_ok")) {
            p.host_probed = true;
            p.host_perf_ok = h.value("perf_ok", false);
            p.host_perf_why = h.value("perf_why", std::string());
        }
    }

    return p;
}

ProcRates procinfo_rates(const ProcInfo &prev, const ProcInfo &cur) {
    ProcRates r;
    // Every guard below marks a case where the DIFFERENCE is meaningless.
    // Reporting 0 instead would claim a measurement that was never taken.
    if (!prev.valid || !cur.valid)
        return r;
    if (prev.pid != cur.pid || prev.start_ticks != cur.start_ticks)
        return r; // a different process (or a reused pid)
    if (cur.ts_ns <= prev.ts_ns)
        return r; // no positive interval: neither rate means anything
    const double dt = double(cur.ts_ns - prev.ts_ns) / 1e9;
    // io's readiness is independent of the CPU-only gates below (Task 7
    // review "cheap fix"): clk_tck==0 and "utime/stime went backwards" are
    // facts about the CPU counters, not about io_read_bytes/io_write_bytes.
    // Computing io_have/read_bps/write_bps here, before either CPU gate,
    // keeps a genuinely measurable io rate from being masked by an
    // unrelated CPU-side absence.
    if (cur.io_readable && prev.io_readable) {
        r.io_have = true;
        if (cur.io_read_bytes >= prev.io_read_bytes)
            r.read_bps = double(cur.io_read_bytes - prev.io_read_bytes) / dt;
        if (cur.io_write_bytes >= prev.io_write_bytes)
            r.write_bps = double(cur.io_write_bytes - prev.io_write_bytes) / dt;
    }
    if (cur.clk_tck == 0)
        return r; // cpu_pct's denominator; io_have/bps above already stand
    const uint64_t pj = prev.utime + prev.stime, cj = cur.utime + cur.stime;
    if (cj < pj)
        return r; // counters went backwards: not a rate
    r.have = true;
    r.cpu_pct = 100.0 * (double(cj - pj) / double(cur.clk_tck)) / dt;
    return r;
}

std::string procinfo_names_verdict(const ProcInfo &p) {
    // FIRST, ahead of even the budget caveat below: every number this
    // verdict is built from comes from /proc/<pid>/maps, directly or via
    // the symbol loader that reads it first. When that file could not be
    // opened, syms_total and jit_methods are zero because nothing was
    // read -- not because nothing is there -- and the "no symbols and no
    // JIT map ... raw addresses" sentence below would be a confident
    // conclusion drawn from a permission denial. This dominates the budget
    // caveat when both hold: "we never got to look" is more specific, and
    // more actionable, than "we ran out of time looking".
    if (!p.maps_readable)
        return "names verdict withheld: /proc/" + std::to_string(p.pid) +
               "/maps could not be read, so no symbol table, module list or "
               "JIT map was ever consulted — this is an absent measurement, "
               "not a code-free process. Re-run as that process's user, or "
               "with CAP_SYS_PTRACE";
    // A budget-truncated gather can abort the whole code-surface section
    // before syms_total/jit_methods are counted (leaving them at their zero
    // defaults), or bail MID-loop (leaving a partial, undercounted sum) —
    // Task 7 review IMPORTANT 4. Either way, "no symbols and no JIT map" or
    // a confident count would manufacture a verdict out of the truncation
    // itself; state the uncertainty instead of the conclusion. This must be
    // the first check: a budget-truncated zero is not the same fact as a
    // genuinely symbol-free target, and the caveat has to win regardless of
    // which branch below would otherwise fire.
    if (p.budget_exceeded)
        return "names verdict withheld: the gather budget ran out before "
               "symbols/JIT methods were fully counted, so syms_total (" +
               std::to_string(p.syms_total) + ") and jit_methods (" +
               std::to_string(p.jit_methods) +
               ") may be undercounts — re-run once the target is idle for a "
               "trustworthy answer";
    const uint64_t named = p.syms_total + p.jit_methods;
    if (named == 0)
        return "no symbols and no JIT map — a trace of this process will show "
               "raw addresses";
    std::string s = std::to_string(p.syms_total) + " symbols";
    if (p.jit_methods)
        s += " + " + std::to_string(p.jit_methods) + " JIT methods (" +
             (p.jit_source.empty() ? "unknown source" : p.jit_source) + ")";
    s += " — a trace of this process will show names";
    if (p.anon_exec_bytes && !p.jit_methods)
        s += ", except in " + std::to_string(p.anon_exec_bytes / 1024) +
             " KB of anonymous executable memory no symtab covers";
    return s;
}

// --- the runner: debounce, spawn, drain, cache, deadline ------------------
namespace {

// waitpid(2) can return early with EINTR (any signal handler installed
// without SA_RESTART, e.g. the GUI's own SIGALRM plumbing elsewhere in the
// app, does exactly this). A caller that treats that as "reaped" zeros
// child_pid while the kernel still holds a zombie under it — nothing ever
// retries, and the zombie is permanent. Every blocking wait in this file
// goes through here instead of a bare ::waitpid.
pid_t waitpid_retry(pid_t pid, int *status) {
    for (;;) {
        pid_t r = ::waitpid(pid, status, 0);
        if (r >= 0 || errno != EINTR)
            return r;
    }
}

// Kill and reap, unconditionally. Called on every path that abandons a child
// — a new selection, a timeout, destruction — because the alternative is a
// probe that outlives the reason it was started. Hiding the pane is NOT one
// of these paths: `visible` only gates a NEW spawn (procinfo_tick's gate,
// below); an in-flight probe keeps draining normally, to EOF or the
// deadline, even while hidden. That is safe only because the pane's owner
// keeps calling procinfo_tick once per frame regardless of draw outcome
// (Task 7's shell does this deliberately) — if ticking ever stopped while
// hidden, the child would simply be waiting, bounded by its own 2s deadline
// once ticking resumed, not left to run forever.
void reap(ProcInfoRunner &r) {
    if (r.child_fd >= 0) {
        ::close(r.child_fd);
        r.child_fd = -1;
    }
    if (r.child_pid > 0) {
        ::kill(r.child_pid, SIGKILL);
        int st = 0;
        waitpid_retry(r.child_pid, &st);
        r.child_pid = 0;
    }
    // clear() alone does not release the allocation; a single flood (I5)
    // would otherwise permanently pin max_buf_bytes of heap even after the
    // child is gone. Swap-with-empty actually frees it.
    std::string().swap(r.buf);
    r.in_flight_pid = 0;
}

// "2.0s" — a bare number in a timeout message reads as a bug report.
std::string fmt_secs(double s) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fs", s);
    return b;
}

// "16 B" / "4.0 MiB" — likewise for the over-cap message; tests pin
// max_buf_bytes small (bytes), the pane's default is MB-scale.
std::string fmt_bytes(size_t n) {
    char b[48];
    if (n >= 1024 * 1024)
        std::snprintf(b, sizeof b, "%.1f MiB", double(n) / (1024.0 * 1024.0));
    else if (n >= 1024)
        std::snprintf(b, sizeof b, "%.1f KiB", double(n) / 1024.0);
    else
        std::snprintf(b, sizeof b, "%zu B", n);
    return b;
}

// How long to wait before retrying a target that just failed, given how many
// times in a row it has now failed. fail_count is incremented BEFORE this is
// called, so backoff_s(1) is the FIRST real delay: 1s, 2s, 4s, 8s, capped at
// 10s — not 0.5s first, despite the internal starting value below.
// Without this, `due` (gated only on the SUCCESS clock, last_ok_at) stays
// true forever after any failure, and a target that never succeeds — a bad
// path, a permanent mismatch, a hang — gets a fork+exec every single frame.
double backoff_s(int fail_count) {
    double b = 0.5;
    for (int i = 0; i < fail_count && b < 10.0; i++)
        b *= 2.0;
    return b > 10.0 ? 10.0 : b;
}

// Cache under (pid, start_ticks). The second half is the pid-REUSE guard:
// without it a recycled pid serves the previous process's card, which is a
// wrong answer that looks entirely plausible. True LRU: touching an existing
// key erases and re-appends it (so a re-probed pid moves to the back, not
// just its original insertion point), and eviction always drops the front.
void cache_put(ProcInfoRunner &r, const ProcInfo &p, double now_s) {
    const auto key = std::make_pair(p.pid, p.start_ticks);
    for (auto it = r.cache.begin(); it != r.cache.end(); ++it)
        if (it->first == key) {
            r.cache.erase(it);
            break;
        }
    if (r.cache.size() >= ProcInfoRunner::kCacheCap)
        r.cache.erase(r.cache.begin());
    r.cache.emplace_back(key, ProcInfoCacheEntry{p, now_s});
}

// fork/exec `asmspy --info <pid> --json`, stdout on a NON-BLOCKING pipe: the
// UI thread polls it from the frame loop and never waits on the child.
//
// Three details here are load-bearing rather than incidental:
//
//  - O_CLOEXEC on the pipe (pipe2, not pipe). A GUI process holds a lot of
//    descriptors — GL, X11/Wayland, fontconfig, the serve host's own pipes —
//    and a child that inherited them would keep them alive past its parent's
//    intent. The write end is dup2'd (which clears CLOEXEC on the copy), so
//    the child still gets its stdout.
//  - The child's stderr goes to /dev/null. asmspy writes refusal prose there
//    and a windowed app has no terminal for it; inheriting would scribble
//    over whatever launched the GUI.
//  - The read end is NON-BLOCKING. procinfo_tick runs in the frame loop, so a
//    blocking read would freeze the UI for as long as the child took — the
//    exact failure this one-shot design exists to avoid.
bool spawn(ProcInfoRunner &r, long pid, double now_s) {
    std::string exe =
        r.asmspy_path.empty() ? resolve_asmspy_path() : r.asmspy_path;
    if (exe.empty()) {
        r.status = "no asmspy found on $PATH or at ./build/asmspy — build it "
                   "with `make cli`, or set the path in Connect";
        return false;
    }
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        r.status = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    pid_t child = ::fork();
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.status = std::string("fork: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        ::dup2(fds[1], STDOUT_FILENO); // the dup CLEARS O_CLOEXEC
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            ::dup2(devnull, STDERR_FILENO);
        char pidbuf[32];
        std::snprintf(pidbuf, sizeof pidbuf, "%ld", pid);
        // v1 probes LOCALLY even when an ssh host is configured: the Processes
        // table lists local /proc, so an ssh-prefixed probe would render an
        // unrelated remote pid's details. The pane states that mismatch.
        char *argv[] = {const_cast<char *>(exe.c_str()),
                        const_cast<char *>("--info"), pidbuf,
                        const_cast<char *>("--json"), nullptr};
        ::execvp(argv[0], argv);
        ::_exit(127); // exec failed; the empty read reports it as a failure
    }
    ::close(fds[1]);
    int fl = ::fcntl(fds[0], F_GETFL, 0);
    ::fcntl(fds[0], F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    r.child_pid = child;
    r.child_fd = fds[0];
    r.in_flight_pid = pid;
    r.spawned_at = now_s;
    r.buf.clear();
    r.spawns++;
    return true;
}

} // namespace

void procinfo_tick(ProcInfoRunner &r, long selected_pid, double now_s) {
    r.last_tick_s = now_s;
    // A path fix in Connect is an operator saying "try again now" — waiting
    // out the OLD path's backoff on the SAME pid would silently ignore that.
    if (r.asmspy_path != r.last_asmspy_path_seen) {
        r.last_asmspy_path_seen = r.asmspy_path;
        r.fail_count = 0;
        r.next_retry_at = -1;
        // A different binary is a different inode with different file
        // capabilities, and CAP_PERFMON is granted per inode — so the latched
        // verdict is about a binary that is no longer the one we would run
        // (finding 8). Back to "not asked", which blocks nothing.
        r.host_perf_probed = false;
        r.host_perf_ok = false;
        r.host_perf_why.clear();
    }
    if (selected_pid != r.want_pid) {
        r.want_pid = selected_pid;
        r.want_since = now_s;
        reap(r); // abandon the in-flight probe for a target nobody is viewing
        r.shown = ProcInfo{};
        r.prev = ProcInfo{};
        r.rates = ProcRates{};
        r.status = selected_pid > 0 ? "reading…" : "no process selected";
        // last_ok_at is the CURRENT target's own last success. A new
        // selection has none yet, so this resets to "never" rather than
        // inheriting the PREVIOUS target's timestamp — without this reset,
        // `due` below (gated on last_ok_at) stays false for up to a full
        // refresh_s after landing on a brand-new pid, so the first probe of
        // it waits behind someone else's refresh clock instead of the 250ms
        // debounce the whole design is built around.
        r.last_ok_at = -1;
        // Likewise the failure backoff is per-TARGET, not global: a run of
        // failures against the previous pid must not throttle the first
        // attempt at a freshly-selected one.
        r.fail_count = 0;
        r.next_retry_at = -1;
        // A cache hit renders immediately; a refresh may still follow. This
        // scans NEWEST-first (cache_put appends, so the back is the most
        // recently completed probe): a pid can legitimately carry TWO
        // entries across a reuse (old process, new process, different
        // start_ticks, same pid number), and the front-to-back scan this
        // used to be would always resolve to the OLDER — i.e. dead — one,
        // which is exactly the rendering bug the compound cache key exists
        // to prevent.
        for (auto it = r.cache.rbegin(); it != r.cache.rend(); ++it)
            if (it->first.first == selected_pid) {
                r.shown = it->second.info;
                // Freshness must reflect THIS entry's own read time, not
                // whatever last_ok_at was reset to above — see
                // ProcInfoCacheEntry::read_at's comment.
                r.last_ok_at = it->second.read_at;
                r.status = "cached";
                break;
            }
    }
    if (selected_pid <= 0)
        return;

    // --- drain / reap / parse the in-flight child -----------------------
    //
    // Four distinctions here are easy to collapse and each collapse is a bug:
    //
    //  - read() < 0 with EAGAIN/EWOULDBLOCK means "nothing YET", not failure.
    //    Treating it as EOF parses a half-written body every frame.
    //  - read() == 0 is real EOF, and only then is the body complete. The
    //    child may still be a zombie at that point, so the status comes from
    //    waitpid AFTER the EOF, not before.
    //  - An exec that failed (_exit(127)) produces a clean EOF with an EMPTY
    //    body. That is a missing-binary error, not a parse error, and saying
    //    "no procinfo event" there would send the operator hunting the wrong
    //    problem.
    //  - a child that never stops writing (asmspy_path pointed at the wrong
    //    binary in Connect) must not grow r.buf without bound: max_buf_bytes
    //    caps it and the child is killed rather than kept draining forever.
    if (r.child_fd >= 0) {
        char chunk[8192];
        bool eof = false, ioerr = false, over_cap = false;
        for (;;) {
            ssize_t n = ::read(r.child_fd, chunk, sizeof chunk);
            if (n > 0) {
                r.buf.append(chunk, static_cast<size_t>(n));
                if (r.buf.size() >= r.max_buf_bytes) {
                    over_cap = true;
                    break;
                }
                continue; // drain everything available this frame
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // nothing more yet; try again next frame
            if (errno == EINTR)
                continue;
            ioerr = true;
            break;
        }

        const bool overdue = now_s - r.spawned_at >= r.deadline_s;
        if (eof || ioerr || overdue || over_cap) {
            // Close the pipe first, then reap, so waitpid cannot block on a
            // child still holding the write end open.
            ::close(r.child_fd);
            r.child_fd = -1;
            int wstatus = 0;
            // Unconditional: eof/ioerr do NOT mean the child exited, only
            // that its copy of the pipe's write end closed (e.g. it closed
            // stdout and kept running) -- waiting on a still-alive child here
            // would block procinfo_tick, the one thing this whole design
            // exists to never do, for as long as that child keeps running.
            ::kill(r.child_pid, SIGKILL);
            waitpid_retry(r.child_pid, &wstatus);
            const int exited = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            r.child_pid = 0;
            const long probed = r.in_flight_pid;
            r.in_flight_pid = 0;

            // Tracked once for the whole branch below, rather than in each
            // arm, so nothing can fall through this chain without either
            // resetting or growing the failure backoff (C2): a target that
            // fails every time must be retried with growing delay, never at
            // frame rate.
            bool succeeded = false;

            if (over_cap) {
                r.status = "asmspy produced more than " +
                           fmt_bytes(r.max_buf_bytes) +
                           " without finishing — refusing to buffer more; "
                           "check the path in Connect";
            } else if (overdue && !eof) {
                r.status = "timed out after " + fmt_secs(r.deadline_s) +
                           " — the probe was killed";
            } else if (ioerr) {
                r.status = std::string("read failed: ") + std::strerror(errno);
            } else if (r.buf.empty()) {
                // 3 is `asmspy --info`'s own code for "no such process"
                // (ASMSPY_INFO_EXIT_NO_SUCH_PID, cli/libasmspy.h), given a
                // code of its own precisely so this arm can tell the
                // routine race -- the target exited between the Processes
                // table listing it and the debounce firing -- from a
                // broken probe. Reporting it as "produced no output" named
                // asmspy as the thing that failed, when nothing failed.
                r.status = exited == 3
                               ? "pid " + std::to_string(probed) + " exited"
                           : exited == 127
                               ? "could not run asmspy — check the path in "
                                 "Connect, or build it with `make cli`"
                               : "asmspy produced no output (exit " +
                                     std::to_string(exited) + ")";
            } else {
                std::istringstream in(r.buf);
                std::string err;
                std::optional<Recording> rec = load_recording(in, err);
                ProcInfo got = rec ? procinfo_parse(*rec) : ProcInfo{};
                if (!rec)
                    got.parse_error = "load_recording failed: " + err;
                if (!got.valid) {
                    r.status = got.parse_error;
                } else if (got.pid != probed) {
                    // The snapshot must be OF the process we asked about. A
                    // mismatch means a stale child's output arrived after a
                    // selection change; showing it under the new pid would be
                    // the worst kind of wrong — plausible and attributed.
                    r.status = "ignored a snapshot for pid " +
                               std::to_string(got.pid) + " while viewing " +
                               std::to_string(probed);
                } else {
                    r.prev = r.shown;
                    r.shown = got;
                    r.rates = procinfo_rates(r.prev, r.shown);
                    r.last_ok_at = now_s;
                    cache_put(r, got, now_s);
                    // LATCH the binary's own perf verdict (finding 8). Only
                    // when the producer actually carried one: an older asmspy
                    // emits no `host` key, and treating its silence as a
                    // refusal would grey a mode nobody measured.
                    if (got.host_probed) {
                        r.host_perf_probed = true;
                        r.host_perf_ok = got.host_perf_ok;
                        r.host_perf_why = got.host_perf_why;
                    }
                    r.status = "attach-free (no ptrace)";
                    succeeded = true;
                }
            }
            // See reap()'s comment: clear() alone would not release the
            // allocation a flood (I5) grew.
            std::string().swap(r.buf);

            if (succeeded) {
                r.fail_count = 0;
                r.next_retry_at = -1;
            } else {
                // Backoff, not a flat retry-after: a target that keeps
                // failing must be asked about LESS often over time, not
                // re-forked every single frame it stays broken.
                r.fail_count++;
                r.next_retry_at = now_s + backoff_s(r.fail_count);
            }
        }
    }

    const bool idle = r.child_pid == 0;
    const bool debounced =
        r.want_since >= 0 && now_s - r.want_since >= r.debounce_s;
    const bool due = r.last_ok_at < 0 || now_s - r.last_ok_at >= r.refresh_s;
    const bool retry_ok = r.next_retry_at < 0 || now_s >= r.next_retry_at;
    if (idle && debounced && r.visible && due && retry_ok &&
        !spawn(r, selected_pid, now_s)) {
        // A spawn that never happened is a failed probe like any other --
        // it left a stated reason in r.status and produced no snapshot.
        // Without the backoff, the three reasons spawn() can return false
        // for (no asmspy resolved, pipe2 failed, fork failed) are all
        // conditions that persist, so the runner retried EVERY FRAME for
        // as long as they lasted: fork() failing under EAGAIN was retried
        // at frame rate by the one code path best placed to make it worse.
        r.fail_count++;
        r.next_retry_at = now_s + backoff_s(r.fail_count);
    }
}

const ProcInfo &procinfo_current(const ProcInfoRunner &r) { return r.shown; }

const ProcRates &procinfo_current_rates(const ProcInfoRunner &r) {
    return r.rates;
}

std::string procinfo_status(const ProcInfoRunner &r) {
    // The freshness clause reads against last_tick_s (the clock the runner
    // itself last saw), never a wall-clock read taken here: this function is
    // const and takes no `now_s`, so a live clock read here would drift from
    // the frame that actually produced the shown snapshot.
    if (r.shown.valid && r.last_ok_at >= 0) {
        double age = r.last_tick_s - r.last_ok_at;
        if (age < 0)
            age = 0;
        return "read " + fmt_secs(age) + " ago · " + r.status;
    }
    return r.status.empty() ? "nothing yet" : r.status;
}

ProcInfoRunner::~ProcInfoRunner() { reap(*this); }

} // namespace asmdesk
