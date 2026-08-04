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

#include <cstdlib>

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

} // namespace asmdesk
