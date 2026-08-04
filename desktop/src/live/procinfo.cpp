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
    if (cur.ts_ns <= prev.ts_ns || cur.clk_tck == 0)
        return r;
    const double dt = double(cur.ts_ns - prev.ts_ns) / 1e9;
    const uint64_t pj = prev.utime + prev.stime, cj = cur.utime + cur.stime;
    if (cj < pj)
        return r; // counters went backwards: not a rate
    r.have = true;
    r.cpu_pct = 100.0 * (double(cj - pj) / double(cur.clk_tck)) / dt;
    if (cur.io_readable && prev.io_readable) {
        if (cur.io_read_bytes >= prev.io_read_bytes)
            r.read_bps = double(cur.io_read_bytes - prev.io_read_bytes) / dt;
        if (cur.io_write_bytes >= prev.io_write_bytes)
            r.write_bps = double(cur.io_write_bytes - prev.io_write_bytes) / dt;
    }
    return r;
}

std::string procinfo_names_verdict(const ProcInfo &p) {
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

// Kill and reap, unconditionally. Called on every path that abandons a child
// — a new selection, a timeout, the pane closing, destruction — because the
// alternative is a probe that outlives the reason it was started.
void reap(ProcInfoRunner &r) {
    if (r.child_fd >= 0) {
        ::close(r.child_fd);
        r.child_fd = -1;
    }
    if (r.child_pid > 0) {
        ::kill(r.child_pid, SIGKILL);
        int st = 0;
        ::waitpid(r.child_pid, &st, 0);
        r.child_pid = 0;
    }
    r.buf.clear();
    r.in_flight_pid = 0;
}

// "2.0s" — a bare number in a timeout message reads as a bug report.
std::string fmt_secs(double s) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1fs", s);
    return b;
}

// Cache under (pid, start_ticks). The second half is the pid-REUSE guard:
// without it a recycled pid serves the previous process's card, which is a
// wrong answer that looks entirely plausible. Bounded LRU — oldest out first.
void cache_put(ProcInfoRunner &r, const ProcInfo &p) {
    const auto key = std::make_pair(p.pid, p.start_ticks);
    for (auto &e : r.cache)
        if (e.first == key) {
            e.second = p;
            return;
        }
    if (r.cache.size() >= ProcInfoRunner::kCacheCap)
        r.cache.erase(r.cache.begin());
    r.cache.emplace_back(key, p);
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
    if (selected_pid != r.want_pid) {
        r.want_pid = selected_pid;
        r.want_since = now_s;
        reap(r); // abandon the in-flight probe for a target nobody is viewing
        r.shown = ProcInfo{};
        r.rates = ProcRates{};
        r.status = selected_pid > 0 ? "reading…" : "no process selected";
        // A cache hit renders immediately; a refresh may still follow.
        for (auto &e : r.cache)
            if (e.first.first == selected_pid) {
                r.shown = e.second;
                r.status = "cached";
                break;
            }
    }
    if (selected_pid <= 0)
        return;

    // --- drain / reap / parse the in-flight child -----------------------
    //
    // Three distinctions here are easy to collapse and each collapse is a bug:
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
    if (r.child_fd >= 0) {
        char chunk[8192];
        bool eof = false, ioerr = false;
        for (;;) {
            ssize_t n = ::read(r.child_fd, chunk, sizeof chunk);
            if (n > 0) {
                r.buf.append(chunk, static_cast<size_t>(n));
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
        if (eof || ioerr || overdue) {
            // Close the pipe first, then reap, so waitpid cannot block on a
            // child still holding the write end open.
            ::close(r.child_fd);
            r.child_fd = -1;
            int wstatus = 0;
            if (overdue && !eof)
                ::kill(r.child_pid, SIGKILL); // it never finished; stop it
            ::waitpid(r.child_pid, &wstatus, 0);
            const int exited = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            r.child_pid = 0;
            const long probed = r.in_flight_pid;
            r.in_flight_pid = 0;

            if (overdue && !eof) {
                r.status = "timed out after " + fmt_secs(r.deadline_s) +
                           " — the probe was killed";
            } else if (ioerr) {
                r.status = std::string("read failed: ") + std::strerror(errno);
            } else if (r.buf.empty()) {
                r.status = exited == 127
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
                    cache_put(r, got);
                    r.status = "attach-free (no ptrace)";
                }
            }
            r.buf.clear();
        }
    }

    const bool idle = r.child_pid == 0;
    const bool debounced =
        r.want_since >= 0 && now_s - r.want_since >= r.debounce_s;
    const bool due = r.last_ok_at < 0 || now_s - r.last_ok_at >= r.refresh_s;
    if (idle && debounced && r.visible && due)
        spawn(r, selected_pid, now_s);
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
